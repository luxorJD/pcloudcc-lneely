/*
   Copyright (c) 2013-2014 Anton Titov.

   Copyright (c) 2013-2014 pCloud Ltd.  All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met: Redistributions of source code must retain the above
   copyright notice, this list of conditions and the following
   disclaimer.  Redistributions in binary form must reproduce the
   above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided
   with the distribution.  Neither the name of pCloud Ltd nor the
   names of its contributors may be used to endorse or promote
   products derived from this software without specific prior written
   permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
   FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL pCloud
   Ltd BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
   OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
   DAMAGE.
*/

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>

#include "papi.h"
#include "pcache.h"
#include "pdevice.h"
#include "pfile.h"
#include "plibs.h"
#include "pnetlibs.h"
#include "ppath.h"
#include "prun.h"
#include "psettings.h"
#include "psql.h"
#include "pssl.h"
#include "pstatus.h"
#include "psys.h"
#include "ptimer.h"
#include "ptree.h"

// required by psync_send_debug
extern PSYNC_THREAD const char *psync_thread_name; 

struct time_bytes {
  time_t tm;
  unsigned long bytes;
};

struct _psync_file_lock_t {
  psync_tree tree;
  char filename[];
};

typedef struct {
  unsigned char mbedtls_sha1[PSYNC_SHA1_DIGEST_LEN];
  uint32_t adler;
} psync_block_checksum;

typedef struct {
  uint64_t filesize;
  uint32_t blocksize;
  uint32_t blockcnt;
  uint32_t *next;
  psync_block_checksum blocks[];
} psync_file_checksums;

typedef struct {
  unsigned long elementcnt;
  uint32_t elements[];
} psync_file_checksum_hash;

typedef struct {
  uint64_t filesize;
  uint32_t blocksize;
  unsigned char _reserved[12];
} psync_block_checksum_header;

typedef struct {
  uint64_t off;
  uint32_t idx;
  uint32_t type;
} psync_block_action;

static time_t current_download_sec = 0;
static unsigned long download_bytes_this_sec = 0;
static unsigned long download_bytes_off = 0;
static unsigned long download_speed = 0;

static time_t current_upload_sec = 0;
static unsigned long upload_bytes_this_sec = 0;
static unsigned long upload_bytes_off = 0;
static unsigned long upload_speed = 0;
static unsigned long dyn_upload_speed = PSYNC_UPL_AUTO_SHAPER_INITIAL;

static psync_tree *file_lock_tree = PSYNC_TREE_EMPTY;
static pthread_mutex_t file_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct time_bytes download_bytes_sec[PSYNC_SPEED_CALC_AVERAGE_SEC],
    upload_bytes_sec[PSYNC_SPEED_CALC_AVERAGE_SEC];

static sem_t api_pool_sem;

char apiserver[64] = PSYNC_API_HOST;
static char apikey[68] = "API:" PSYNC_API_HOST;

static uint32_t hash_func(const char *key) {
  uint32_t c, hash;
  hash = 0;
  while ((c = (uint32_t)*key++))
    hash = c + (hash << 5) + hash;
  return hash;
}

static psock_t *psync_get_api() {
  psock_t *sock;
  sem_wait(&api_pool_sem);
  pdbg_logf(D_NOTICE, "connecting to %s", apiserver);
  sock = papi_connect(apiserver, psync_setting_get_bool(_PS(usessl)));
  if (sock)
    sock->misc = hash_func(apiserver);
  return sock;
}

static void psync_ret_api(void *ptr) {
  sem_post(&api_pool_sem);
  pdbg_logf(D_NOTICE, "closing connection to api");
  psock_close((psock_t *)ptr);
  pdbg_logf(D_NOTICE, "closed connection to api");
}

static int api_sock_is_broken(psock_t *ret) {
  if (pdbg_unlikely(psock_is_broken(ret->sock) ||
                   psock_is_ssl(ret) !=
                       psync_setting_get_bool(_PS(usessl)))) {
    pdbg_logf(D_NOTICE, "got broken socket from cache");
    psync_ret_api(ret);
    return 1;
  } else {
    if (psock_readable(ret)) {
      char buff[16];
      int rd;
      rd = psock_read_noblock(ret, buff, sizeof(buff));
      if (rd <= 0) {
        psync_ret_api(ret);
        pdbg_logf(D_NOTICE, "got broken socket from cache");
      } else {
        buff[sizeof(buff) - 1] = 0;
        pdbg_logf(D_ERROR,
              "got socket with pending data to read from cache, read %d bytes: "
              "%s",
              rd, buff);
        psync_ret_api(ret);
        if (IS_DEBUG)
          abort();
      }
      return 1;
    } else {
      pdbg_logf(D_NOTICE, "got api connection from cache, key %s", apikey);
      return 0;
    }
  }
}

void psync_apipool_set_server(const char *binapi) {
  size_t len;
  len = strlen(binapi) + 1;
  if (len <= sizeof(apiserver)) {
    memcpy(apiserver, binapi, len);
    memcpy(apikey + 4, binapi, len);
    pdbg_logf(D_NOTICE, "set %s as best api server", apiserver);
  }
}

psock_t *psync_apipool_get() {
  psock_t *ret;
  while (1) {
    ret = (psock_t *)pcache_get(apikey);
    if (!ret)
      break;
    if (!api_sock_is_broken(ret))
      return ret;
  }
  ret = psync_get_api();
  if (pdbg_unlikely(!ret))
    ptimer_notify_exception();
  return ret;
}

psock_t *psync_apipool_get_from_cache() {
  psock_t *ret;
  while (1) {
    ret = (psock_t *)pcache_get(apikey);
    if (!ret)
      break;
    if (!api_sock_is_broken(ret))
      return ret;
  }
  return NULL;
}

void psync_apipool_prepare() {
  if (pcache_has(apikey))
    return;
  else {
    psock_t *ret;
    ret = psync_get_api();
    if (pdbg_unlikely(!ret))
      ptimer_notify_exception();
    else {
      pdbg_logf(D_NOTICE, "prepared api connection");
      psync_apipool_release(ret);
    }
  }
}

binresult *psync_do_api_run_command(const char *command, size_t cmdlen,
                                    const binparam *params, size_t paramcnt) {
  psock_t *api;
  binresult *ret;
  int tries;
  tries = 0;

  do {
    api = psync_apipool_get();
    if (unlikely(!api))
      break;
    if (likely(
            papi_send(api, command, cmdlen, params, paramcnt, -1, 0))) {
      // something useful can be done here as we will wait a while
      ret = papi_result(api);
      if (likely(ret)) {
        psync_apipool_release(api);
        return ret;
      }
    }
    psync_apipool_release_bad(api);
  } while (++tries <= PSYNC_RETRY_REQUEST);

  ptimer_notify_exception();

  return NULL;
}

#if IS_DEBUG

void pident(int ident) {
  VAR_ARRAY(b, char, ident + 1);
  memset(b, '\t', ident);
  b[ident] = 0;
  fputs(b, stdout);
}

static void print_tree(const binresult *tree, int ident) {
  int i;
  if (tree->type == PARAM_STR)
    printf("string(%u)\"%s\"", tree->length, tree->str);
  else if (tree->type == PARAM_NUM)
    printf("number %llu", (unsigned long long)tree->num);
  else if (tree->type == PARAM_DATA)
    printf("data %llu", (unsigned long long)tree->num);
  else if (tree->type == PARAM_BOOL)
    printf("bool %s", tree->num ? "true" : "false");
  else if (tree->type == PARAM_HASH) {
    printf("hash (%u){\n", tree->length);
    if (tree->length) {
      pident(ident + 1);
      printf("\"%s\" = ", tree->hash[0].key);
      print_tree(tree->hash[0].value, ident + 1);
      for (i = 1; i < tree->length; i++) {
        printf(",\n");
        pident(ident + 1);
        printf("\"%s\" = ", tree->hash[i].key);
        print_tree(tree->hash[i].value, ident + 1);
      }
    }
    printf("\n");
    pident(ident);
    printf("}");
  } else if (tree->type == PARAM_ARRAY) {
    printf("array (%u)[\n", tree->length);
    if (tree->length) {
      pident(ident + 1);
      print_tree(tree->array[0], ident + 1);
      for (i = 1; i < tree->length; i++) {
        printf(",\n");
        pident(ident + 1);
        print_tree(tree->array[i], ident + 1);
      }
    }
    printf("\n");
    pident(ident);
    printf("]");
  }
}

PSYNC_NOINLINE static void psync_apipool_dump_socket(psock_t *api) {
  binresult *res;
  res = papi_result(api);
  psync_apipool_release_bad(api);
  if (!res) {
    pdbg_logf(D_NOTICE, "could not read result from socket, it is probably broken");
    return;
  }
  pdbg_logf(D_WARNING, "read result from released socket, dumping and aborting");
  print_tree(res, 0);
  free(res);
  abort();
}

#endif

void psync_apipool_release(psock_t *api) {
#if IS_DEBUG
  if (unlikely(psock_readable(api))) {
    pdbg_logf(D_WARNING, "released socket with pending data to read");
    psync_apipool_dump_socket(api);
    return;
  }
#endif
  if (hash_func(apiserver) == api->misc)
    pcache_add(apikey, api, PSYNC_APIPOOL_MAXIDLESEC, psync_ret_api,
                    PSYNC_APIPOOL_MAXIDLE);
  else
    psync_ret_api(api);
}

void psync_apipool_release_bad(psock_t *api) { psync_ret_api(api); }

static void rm_all(void *vpath, ppath_stat *st) {
  char *path;
  path = psync_strcat((char *)vpath, "/", st->name, NULL);
  if (pfile_stat_isfolder(&st->stat)) {
    ppath_ls(path, rm_all, path);
    rmdir(path);
  } else
    pfile_delete(path);
  free(path);
}

static void rm_ign(void *vpath, ppath_stat *st) {
  char *path;
  int ign;
  ign = psync_is_name_to_ignore(st->name);
  path = psync_strcat((char *)vpath, "/", st->name, NULL);
  if (pfile_stat_isfolder(&st->stat)) {
    if (ign)
      ppath_ls(path, rm_all, path);
    else
      ppath_ls(path, rm_ign, path);
    rmdir(path);
  } else if (ign)
    pfile_delete(path);
  free(path);
}

int psync_rmdir_with_trashes(const char *path) {
  if (!rmdir(path))
    return 0;
  if (errno != ENOTEMPTY && errno != EEXIST)
    return -1;
  if (ppath_ls(path, rm_ign, (void *)path))
    return -1;
  return rmdir(path);
}

int psync_rmdir_recursive(const char *path) {
  if (ppath_ls(path, rm_all, (void *)path))
    return -1;
  return rmdir(path);
}

void psync_set_local_full(int over) {
  static int isover = 0;
  if (over != isover) {
    isover = over;
    if (isover) {
      pstatus_set(PSTATUS_TYPE_DISKFULL, PSTATUS_DISKFULL_FULL);
    } else
      pstatus_set(PSTATUS_TYPE_DISKFULL, PSTATUS_DISKFULL_OK);
  }
}

int psync_handle_api_result(uint64_t result) {
  if (result == 2000) {
    pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_BADLOGIN);
    ptimer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  } else if (result == 2003 || result == 2009 || result == 2005 ||
             result == 2029 || result == 2067 || result == 5002)
    return PSYNC_NET_PERMFAIL;
  else if (result == 2007) {
    pdbg_logf(D_ERROR, "trying to delete root folder");
    return PSYNC_NET_PERMFAIL;
  } else
    return PSYNC_NET_TEMPFAIL;
}

int psync_get_remote_file_checksum(psync_fileid_t fileid, unsigned char *hexsum,
                                   uint64_t *fsize, uint64_t *hash) {
  binresult *res;
  const binresult *meta, *checksum;
  psync_sql_res *sres;
  psync_variant_row row;
  uint64_t result, h;
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_NUM("fileid", fileid)};
  sres = psql_query_rdlock(
      "SELECT h.checksum, f.size, f.hash FROM hashchecksum h, file f WHERE "
      "f.id=? AND f.hash=h.hash AND f.size=h.size");
  psql_bind_uint(sres, 1, fileid);
  row = psql_fetch(sres);
  if (row) {
    pdbg_assertw(row[0].length == PSYNC_HASH_DIGEST_HEXLEN);
    memcpy(hexsum, psync_get_string(row[0]), PSYNC_HASH_DIGEST_HEXLEN);
    if (fsize)
      *fsize = psync_get_number(row[1]);
    if (hash)
      *hash = psync_get_number(row[2]);
    psql_free(sres);
    return PSYNC_NET_OK;
  }
  psql_free(sres);
  res = psync_api_run_command("checksumfile", params);
  if (!res)
    return PSYNC_NET_TEMPFAIL;
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    pdbg_logf(D_WARNING, "checksumfile returned error %lu", (unsigned long)result);
    free(res);
    return psync_handle_api_result(result);
  }
  meta = papi_find_result2(res, "metadata", PARAM_HASH);
  checksum = papi_find_result2(res, PSYNC_CHECKSUM, PARAM_STR);
  result = papi_find_result2(meta, "size", PARAM_NUM)->num;
  h = papi_find_result2(meta, "hash", PARAM_NUM)->num;
  if (fsize)
    *fsize = result;
  if (hash)
    *hash = h;
  sres = psql_prepare(
      "REPLACE INTO hashchecksum (hash, size, checksum) VALUES (?, ?, ?)");
  psql_bind_uint(sres, 1, h);
  psql_bind_uint(sres, 2, result);
  psql_bind_lstr(sres, 3, checksum->str, checksum->length);
  psql_run_free(sres);
  memcpy(hexsum, checksum->str, checksum->length);
  free(res);
  return PSYNC_NET_OK;
}

static int file_changed(struct stat *st1, struct stat *st2) {
  return pfile_stat_size(st1) != pfile_stat_size(st2) ||
         pfile_stat_mtime_native(st1) != pfile_stat_mtime_native(st2);
}

int psync_get_local_file_checksum(const char *restrict filename,
                                  unsigned char *restrict hexsum,
                                  uint64_t *restrict fsize) {
  struct stat st, st2;
  psync_hash_ctx hctx;
  uint64_t rsz;
  void *buff;
  size_t rs;
  ssize_t rrs;
  unsigned long cnt;
  int fd;
  unsigned char hashbin[PSYNC_HASH_DIGEST_LEN];
  fd = pfile_open(filename, O_RDONLY, 0);
  if (fd == INVALID_HANDLE_VALUE)
    return PSYNC_NET_PERMFAIL;
  buff = malloc(PSYNC_COPY_BUFFER_SIZE);
retry:
  if (pdbg_unlikely(fstat(fd, &st)))
    goto err1;
  psync_hash_init(&hctx);
  rsz = pfile_stat_size(&st);
  cnt = 0;
  while (rsz) {
    if (rsz > PSYNC_COPY_BUFFER_SIZE)
      rs = PSYNC_COPY_BUFFER_SIZE;
    else
      rs = rsz;
    rrs = pfile_read(fd, buff, rs);
    if (unlikely(rrs <= 0)) {
      if (rrs == 0 && !fstat(fd, &st2) && file_changed(&st, &st2)) {
        pdbg_logf(D_NOTICE,
              "file %s changed while calculating checksum, restarting",
              filename);
        psync_hash_final(hashbin, &hctx);
        psys_sleep_milliseconds(PSYNC_SLEEP_FILE_CHANGE);
        pfile_seek(fd, 0, SEEK_SET);
        goto retry;
      }
      goto err1;
    }
    psync_hash_update(&hctx, buff, rrs);
    rsz -= rrs;
    if (++cnt % 16 == 0)
      psys_sleep_milliseconds(5);
  }
  if (pdbg_unlikely(fstat(fd, &st2)))
    goto err1;
  if (unlikely(file_changed(&st, &st2))) {
    pdbg_logf(D_NOTICE, "file %s changed while calculating checksum, restarting",
          filename);
    psync_hash_final(hashbin, &hctx);
    psys_sleep_milliseconds(PSYNC_SLEEP_FILE_CHANGE);
    pfile_seek(fd, 0, SEEK_SET);
    goto retry;
  }
  free(buff);
  pfile_close(fd);
  psync_hash_final(hashbin, &hctx);
  psync_binhex(hexsum, hashbin, PSYNC_HASH_DIGEST_LEN);
  if (fsize)
    *fsize = pfile_stat_size(&st);
  return PSYNC_NET_OK;
err1:
  free(buff);
  pfile_close(fd);
  return PSYNC_NET_PERMFAIL;
}

int psync_get_local_file_checksum_part(const char *restrict filename,
                                       unsigned char *restrict hexsum,
                                       uint64_t *restrict fsize,
                                       unsigned char *restrict phexsum,
                                       uint64_t pfsize) {
  struct stat st;
  psync_hash_ctx hctx, hctxp;
  uint64_t rsz;
  void *buff;
  size_t rs;
  ssize_t rrs;
  unsigned long cnt;
  int fd;
  unsigned char hashbin[PSYNC_HASH_DIGEST_LEN];
  fd = pfile_open(filename, O_RDONLY, 0);
  if (fd == INVALID_HANDLE_VALUE)
    return PSYNC_NET_PERMFAIL;
  if (pdbg_unlikely(fstat(fd, &st)))
    goto err1;
  buff = malloc(PSYNC_COPY_BUFFER_SIZE);
  psync_hash_init(&hctx);
  psync_hash_init(&hctxp);
  rsz = pfile_stat_size(&st);
  cnt = 0;
  while (rsz) {
    if (rsz > PSYNC_COPY_BUFFER_SIZE)
      rs = PSYNC_COPY_BUFFER_SIZE;
    else
      rs = rsz;
    rrs = pfile_read(fd, buff, rs);
    if (rrs <= 0)
      goto err2;
    psync_hash_update(&hctx, buff, rrs);
    if (pfsize) {
      if (pfsize < rrs) {
        psync_hash_update(&hctxp, buff, pfsize);
        pfsize = 0;
      } else {
        psync_hash_update(&hctxp, buff, rrs);
        pfsize -= rrs;
      }
    }
    rsz -= rrs;
    if (++cnt % 16 == 0)
      psys_sleep_milliseconds(5);
  }
  free(buff);
  pfile_close(fd);
  psync_hash_final(hashbin, &hctx);
  psync_binhex(hexsum, hashbin, PSYNC_HASH_DIGEST_LEN);
  psync_hash_final(hashbin, &hctxp);
  psync_binhex(phexsum, hashbin, PSYNC_HASH_DIGEST_LEN);
  if (fsize)
    *fsize = pfile_stat_size(&st);
  return PSYNC_NET_OK;
err2:
  free(buff);
err1:
  pfile_close(fd);
  return PSYNC_NET_PERMFAIL;
}

int psync_file_writeall_checkoverquota(int fd, const void *buf,
                                       size_t count) {
  ssize_t wr;
  while (count) {
    wr = pfile_write(fd, buf, count);
    if (wr == count) {
      psync_set_local_full(0);
      return 0;
    } else if (wr == -1) {
      if (errno == ENOSPC || errno == EDQUOT) {
        psync_set_local_full(1);
        psys_sleep_milliseconds(PSYNC_SLEEP_ON_DISK_FULL);
      }
      return -1;
    }
    buf = (unsigned char *)buf + wr;
    count -= wr;
  }
  return 0;
}

int psync_copy_local_file_if_checksum_matches(const char *source,
                                              const char *destination,
                                              const unsigned char *hexsum,
                                              uint64_t fsize) {
  int sfd, dfd;
  psync_hash_ctx hctx;
  void *buff;
  size_t rrd;
  ssize_t rd;
  unsigned char hashbin[PSYNC_HASH_DIGEST_LEN];
  char hashhex[PSYNC_HASH_DIGEST_HEXLEN];
  sfd = pfile_open(source, O_RDONLY, 0);
  if (pdbg_unlikely(sfd == INVALID_HANDLE_VALUE))
    goto err0;
  if (pdbg_unlikely(pfile_size(sfd) != fsize))
    goto err1;
  dfd = pfile_open(destination, O_WRONLY, O_CREAT | O_TRUNC);
  if (pdbg_unlikely(dfd == INVALID_HANDLE_VALUE))
    goto err1;
  psync_hash_init(&hctx);
  buff = malloc(PSYNC_COPY_BUFFER_SIZE);
  while (fsize) {
    if (fsize > PSYNC_COPY_BUFFER_SIZE)
      rrd = PSYNC_COPY_BUFFER_SIZE;
    else
      rrd = fsize;
    rd = pfile_read(sfd, buff, rrd);
    if (pdbg_unlikely(rd <= 0))
      goto err2;
    if (pdbg_unlikely(psync_file_writeall_checkoverquota(dfd, buff, rd)))
      goto err2;
    sched_yield();
    psync_hash_update(&hctx, buff, rd);
    fsize -= rd;
  }
  psync_hash_final(hashbin, &hctx);
  psync_binhex(hashhex, hashbin, PSYNC_HASH_DIGEST_LEN);
  if (pdbg_unlikely(memcmp(hexsum, hashhex, PSYNC_HASH_DIGEST_HEXLEN)) ||
      pdbg_unlikely(pfile_sync(dfd)))
    goto err2;
  free(buff);
  if (pdbg_unlikely(pfile_close(dfd)))
    goto err1;
  pfile_close(sfd);
  return PSYNC_NET_OK;
err2:
  free(buff);
  pfile_close(dfd);
  pfile_delete(destination);
err1:
  pfile_close(sfd);
err0:
  return PSYNC_NET_PERMFAIL;
}

psock_t *psync_socket_connect_download(const char *host, int unsigned port,
                                            int usessl) {
  psock_t *sock;
  int64_t dwlspeed;
  sock = psock_connect(host, port, usessl);
  if (sock) {
    dwlspeed = psync_setting_get_int(_PS(maxdownloadspeed));
    if (dwlspeed != -1 && dwlspeed < PSYNC_MAX_SPEED_RECV_BUFFER) {
      if (dwlspeed == 0)
        dwlspeed = PSYNC_RECV_BUFFER_SHAPED;
      psock_set_recvbuf(sock, (uint32_t)dwlspeed);
    }
  }
  return sock;
}

psock_t *psync_api_connect_download() {
  psock_t *sock;
  int64_t dwlspeed;
  sock = papi_connect(apiserver, psync_setting_get_bool(_PS(usessl)));
  if (sock) {
    dwlspeed = psync_setting_get_int(_PS(maxdownloadspeed));
    if (dwlspeed != -1 && dwlspeed < PSYNC_MAX_SPEED_RECV_BUFFER) {
      if (dwlspeed == 0)
        dwlspeed = PSYNC_RECV_BUFFER_SHAPED;
      psock_set_recvbuf(sock, (uint32_t)dwlspeed);
    }
  }
  return sock;
}

void psync_socket_close_download(psock_t *sock) {
  psock_close(sock);
}

/* generally this should be protected by mutex as downloading is multi threaded,
 * but it is not so important to have that accurate download speed
 */

void psync_account_downloaded_bytes(int unsigned bytes) {
  if (current_download_sec == psync_current_time)
    download_bytes_this_sec += bytes;
  else {
    uint64_t sum;
    unsigned long i;
    download_bytes_sec[download_bytes_off].tm = current_download_sec;
    download_bytes_sec[download_bytes_off].bytes = download_bytes_this_sec;
    current_download_sec = psync_current_time;
    download_bytes_this_sec = bytes;
    download_bytes_off =
        (download_bytes_off + 1) % PSYNC_SPEED_CALC_AVERAGE_SEC;
    sum = 0;
    for (i = 0; i < PSYNC_SPEED_CALC_AVERAGE_SEC; i++)
      if (download_bytes_sec[i].tm >=
          psync_current_time - PSYNC_SPEED_CALC_AVERAGE_SEC)
        sum += download_bytes_sec[i].bytes;
    download_speed = sum / PSYNC_SPEED_CALC_AVERAGE_SEC;
    pstatus_download_set_speed(download_speed);
  }
}

static unsigned long get_download_bytes_this_sec() {
  if (current_download_sec == psync_current_time)
    return download_bytes_this_sec;
  else
    return 0;
}

static int psync_socket_readall_download_th(psock_t *sock, void *buff,
                                            int num, int th) {
  long dwlspeed, readbytes, pending, lpending, rd, rrd;
  unsigned long thissec, ds;
  dwlspeed = psync_setting_get_int(_PS(maxdownloadspeed));
  if (dwlspeed == 0) {
    if (th)
      lpending = psock_pendingdata_buf_thread(sock);
    else
      lpending = psock_pendingdata_buf(sock);
    if (download_speed > 100 * 1024)
      ds = download_speed / 1024;
    else
      ds = 100;
    while (1) {
      psys_sleep_milliseconds(PSYNC_SLEEP_AUTO_SHAPER * 100 / ds);
      if (th)
        pending = psock_pendingdata_buf_thread(sock);
      else
        pending = psock_pendingdata_buf(sock);
      if (pending == lpending)
        break;
      else
        lpending = pending;
    }
    if (pending > 0)
      sock->pending = 1;
  } else if (dwlspeed > 0) {
    readbytes = 0;
    while (num) {
      while ((thissec = get_download_bytes_this_sec()) >= dwlspeed)
        ptimer_wait_next_sec();
      if (num > dwlspeed - thissec)
        rrd = dwlspeed - thissec;
      else
        rrd = num;
      if (th)
        rd = psock_read_thread(sock, buff, rrd);
      else
        rd = psock_read(sock, buff, rrd);
      if (rd <= 0)
        return readbytes ? readbytes : rd;
      num -= rd;
      buff = (char *)buff + rd;
      readbytes += rd;
      psync_account_downloaded_bytes(rd);
    }
    return readbytes;
  }
  if (th)
    readbytes = psock_readall_thread(sock, buff, num);
  else
    readbytes = psock_readall(sock, buff, num);
  if (readbytes > 0)
    psync_account_downloaded_bytes(readbytes);
  return readbytes;
}

int psync_socket_readall_download(psock_t *sock, void *buff, int num) {
  return psync_socket_readall_download_th(sock, buff, num, 0);
}

int psync_socket_readall_download_thread(psock_t *sock, void *buff,
                                         int num) {
  return psync_socket_readall_download_th(sock, buff, num, 1);
}

static void account_uploaded_bytes(int unsigned bytes) {
  if (current_upload_sec == psync_current_time)
    upload_bytes_this_sec += bytes;
  else {
    uint64_t sum;
    unsigned long i;
    upload_bytes_sec[upload_bytes_off].tm = current_upload_sec;
    upload_bytes_sec[upload_bytes_off].bytes = upload_bytes_this_sec;
    upload_bytes_off = (upload_bytes_off + 1) % PSYNC_SPEED_CALC_AVERAGE_SEC;
    current_upload_sec = psync_current_time;
    upload_bytes_this_sec = bytes;
    sum = 0;
    for (i = 0; i < PSYNC_SPEED_CALC_AVERAGE_SEC; i++)
      if (upload_bytes_sec[i].tm >=
          psync_current_time - PSYNC_SPEED_CALC_AVERAGE_SEC)
        sum += upload_bytes_sec[i].bytes;
    upload_speed = sum / PSYNC_SPEED_CALC_AVERAGE_SEC;
    pstatus_upload_set_speed(upload_speed);
  }
}

static unsigned long get_upload_bytes_this_sec() {
  if (current_upload_sec == psync_current_time)
    return upload_bytes_this_sec;
  else
    return 0;
}

// static void set_send_buf(psock_t *sock){
//   psock_set_sendbuf(sock,
//   dyn_upload_speed*PSYNC_UPL_AUTO_SHAPER_BUF_PER/100);
// }

int psync_set_default_sendbuf(psock_t *sock) {
  //  return psock_set_sendbuf(sock, PSYNC_DEFAULT_SEND_BUFF);
  return 0;
}

long psync_socket_writeall_upload(psock_t *sock, const void *buff,
                                         int num) {
  long uplspeed, writebytes, wr, wwr;
  unsigned long thissec;
  uplspeed = psync_setting_get_int(_PS(maxuploadspeed));
  if (uplspeed == 0) {
    writebytes = 0;
    while (num) {
      while ((thissec = get_upload_bytes_this_sec()) >= dyn_upload_speed) {
        dyn_upload_speed =
            (dyn_upload_speed * PSYNC_UPL_AUTO_SHAPER_INC_PER) / 100;
        //        set_send_buf(sock);
        ptimer_wait_next_sec();
      }
      pdbg_logf(D_NOTICE, "dyn_upload_speed=%lu", dyn_upload_speed);
      if (num > dyn_upload_speed - thissec)
        wwr = dyn_upload_speed - thissec;
      else
        wwr = num;
      if (!psock_writable(sock)) {
        dyn_upload_speed =
            (dyn_upload_speed * PSYNC_UPL_AUTO_SHAPER_DEC_PER) / 100;
        if (dyn_upload_speed < PSYNC_UPL_AUTO_SHAPER_MIN)
          dyn_upload_speed = PSYNC_UPL_AUTO_SHAPER_MIN;
        //        set_send_buf(sock);
        psys_sleep_milliseconds(1000);
      }
      wr = psock_write(sock, buff, wwr);
      if (wr == -1)
        return writebytes ? writebytes : wr;
      num -= wr;
      buff = (char *)buff + wr;
      writebytes += wr;
      account_uploaded_bytes(wr);
    }
    return writebytes;
  } else if (uplspeed > 0) {
    writebytes = 0;
    while (num) {
      while ((thissec = get_upload_bytes_this_sec()) >= uplspeed)
        ptimer_wait_next_sec();
      if (num > uplspeed - thissec)
        wwr = uplspeed - thissec;
      else
        wwr = num;
      wr = psock_write(sock, buff, wwr);
      if (wr == -1)
        return writebytes ? writebytes : wr;
      num -= wr;
      buff = (char *)buff + wr;
      writebytes += wr;
      account_uploaded_bytes(wr);
    }
    return writebytes;
  }
  writebytes = psock_writeall(sock, buff, num);
  if (writebytes > 0)
    account_uploaded_bytes(writebytes);
  return writebytes;
}

psync_http_socket *psync_http_connect(const char *host, const char *path,
                                      uint64_t from, uint64_t to,
                                      const char *addhdr) {
  psock_t *sock;
  psync_http_socket *hsock;
  char *readbuff, *ptr, *end, *key, *val;
  int64_t clen;
  uint32_t keepalive;
  int usessl, rl, rb, isval, cl;
  char ch, lch;
  char cachekey[256];
  usessl = psync_setting_get_bool(_PS(usessl));
  cl = snprintf(cachekey, sizeof(cachekey) - 1, "HTTP%d-%s", usessl, host) + 1;
  cachekey[sizeof(cachekey) - 1] = 0;
  sock = (psock_t *)pcache_get(cachekey);
  if (!sock) {
    sock = psync_socket_connect_download(host, usessl ? 443 : 80, usessl);
    if (!sock)
      goto err0;
  } else
    pdbg_logf(D_NOTICE, "got connection to %s from cache", host);
  readbuff = malloc(PSYNC_HTTP_RESP_BUFFER);
  if (!addhdr)
    addhdr = "";
  if (from || to) {
    if (to)
      rl = snprintf(
          readbuff, PSYNC_HTTP_RESP_BUFFER,
          "GET %s HTTP/1.1\015\012Host: %s\015\012Range: bytes=%" PRIu64
          "-%" PRIu64 "\015\012Connection: Keep-Alive\015\012%s\015\012",
          path, host, from, to, addhdr);
    else
      rl = snprintf(
          readbuff, PSYNC_HTTP_RESP_BUFFER,
          "GET %s HTTP/1.1\015\012Host: %s\015\012Range: bytes=%" PRIu64
          "-\015\012Connection: Keep-Alive\015\012%s\015\012",
          path, host, from, addhdr);
  } else
    rl = snprintf(readbuff, PSYNC_HTTP_RESP_BUFFER,
                  "GET %s HTTP/1.1\015\012Host: %s\015\012Connection: "
                  "Keep-Alive\015\012%s\015\012",
                  path, host, addhdr);
  if (psock_writeall(sock, readbuff, rl) != rl ||
      (rb = psock_read(sock, readbuff, PSYNC_HTTP_RESP_BUFFER - 1)) <= 0)
    goto err1;
  readbuff[rb] = 0;
  ptr = readbuff;
  while (*ptr && !isspace(*ptr))
    ptr++;
  while (*ptr && isspace(*ptr))
    ptr++;
  if (!isdigit(*ptr) || atoi(ptr) / 10 != 20)
    goto err1;
  while (*ptr && *ptr != '\012')
    ptr++;
  if (!*ptr)
    goto err1;
  ptr++;
  end = readbuff + rb;
  lch = 0;
  isval = 0;
  keepalive = 0;
  clen = -1;
  key = val = ptr;
cont:
  for (; ptr < end; ptr++) {
    ch = *ptr;
    if (ch == '\015') {
      *ptr = 0;
      continue;
    } else if (ch == '\012') {
      if (lch == '\012') {
        ptr++;
        goto ex;
      }
      *ptr = 0;
      /*      pdbg_logf(D_NOTICE, "key=%s, value=%s", key, val);*/
      if (!memcmp(key, "content-length", 14))
        clen = psync_ato64(val);
      else if (!memcmp(key, "keep-alive", 10) && !memcmp(val, "timeout=", 8))
        keepalive = psync_ato32(val + 8);
      key = ptr + 1;
      isval = 0;
    } else if (ch == ':' && !isval) {
      *ptr++ = 0;
      while (isspace(*ptr))
        ptr++;
      val = ptr;
      *ptr = tolower(*ptr);
      isval = 1;
    } else
      *ptr = tolower(ch);
    lch = ch;
  }
  if (rb == PSYNC_HTTP_RESP_BUFFER)
    goto err1;
  rl = psock_read(sock, readbuff + rb, PSYNC_HTTP_RESP_BUFFER - rb);
  if (rl <= 0)
    goto err1;
  rb += rl;
  end = readbuff + rb;
  goto cont;
ex:
  rl = ptr - readbuff;
  if (rl == rb) {
    free(readbuff);
    readbuff = NULL;
  }
  hsock = (psync_http_socket *)malloc(
      offsetof(psync_http_socket, cachekey) + cl);
  hsock->sock = sock;
  hsock->readbuff = readbuff;
  hsock->contentlength = clen;
  hsock->readbytes = 0;
  hsock->keepalive = keepalive;
  hsock->readbuffoff = rl;
  hsock->readbuffsize = rb;
  memcpy(hsock->cachekey, cachekey, cl);
  return hsock;
err1:
  free(readbuff);
  psync_socket_close_download(sock);
err0:
  return NULL;
}

void psync_http_close(psync_http_socket *http) {
  if (http->keepalive > 5 && http->readbytes == http->contentlength) {
    //    pdbg_logf(D_NOTICE, "caching socket %s keepalive=%u, readbytes=%lu,
    //    contentlength=%lu", http->cachekey, (unsigned)http->keepalive,
    //                    (unsigned long)http->readbytes, (unsigned
    //                    long)http->contentlength);
    pcache_add(http->cachekey, http->sock, http->keepalive - 5,
                    (pcache_free_cb)psync_socket_close_download,
                    PSYNC_MAX_IDLE_HTTP_CONNS);
  } else {
    pdbg_logf(D_NOTICE,
          "closing socket %s keepalive=%u, readbytes=%lu, contentlength=%lu",
          http->cachekey, (unsigned)http->keepalive,
          (unsigned long)http->readbytes, (unsigned long)http->contentlength);
    psync_socket_close_download(http->sock);
  }
  if (http->readbuff)
    free(http->readbuff);
  free(http);
}

int psync_http_readall(psync_http_socket *http, void *buff, int num) {
  if (http->contentlength != -1) {
    if ((uint64_t)num > (uint64_t)http->contentlength - http->readbytes)
      num = http->contentlength - http->readbytes;
    if (!num)
      return num;
  }
  if (http->readbuff) {
    int cp;
    if (num < http->readbuffsize - http->readbuffoff)
      cp = num;
    else
      cp = http->readbuffsize - http->readbuffoff;
    memcpy(buff, (unsigned char *)http->readbuff + http->readbuffoff, cp);
    http->readbuffoff += cp;
    http->readbytes += cp;
    if (http->readbuffoff >= http->readbuffsize) {
      free(http->readbuff);
      http->readbuff = NULL;
    }
    if (cp == num)
      return cp;
    num = psync_socket_readall_download(http->sock, (unsigned char *)buff + cp,
                                        num - cp);
    if (num <= 0)
      return cp;
    else {
      http->readbytes += num;
      return cp + num;
    }
  } else {
    num = psync_socket_readall_download(http->sock, buff, num);
    if (num > 0)
      http->readbytes += num;
    return num;
  }
}

typedef struct {
  psync_tree tree;
  pthread_cond_t *cond;
  psock_t **res;
  int usessl;
  int haswaiter;
  int ready;
  char host[];
} connect_cache_tree_node_t;

pthread_mutex_t connect_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
psync_tree *connect_cache_tree = PSYNC_TREE_EMPTY;

static void connect_cache_thread(void *ptr) {
  connect_cache_tree_node_t *node;
  psock_t *sock;
  node = (connect_cache_tree_node_t *)ptr;
  sock =
      psock_connect(node->host, node->usessl ? 443 : 80, node->usessl);
  pthread_mutex_lock(&connect_cache_mutex);
  ptree_del(&connect_cache_tree, &node->tree);
  if (node->haswaiter) {
    *node->res = sock;
    node->ready = 1;
    pdbg_logf(D_NOTICE, "passing connection to %s to waiter", node->host);
    pthread_cond_signal(node->cond);
    pthread_mutex_unlock(&connect_cache_mutex);
  } else {
    if (sock) {
      char cachekey[256];
      snprintf(cachekey, sizeof(cachekey) - 1, "HTTP%d-%s", node->usessl,
               node->host);
      cachekey[sizeof(cachekey) - 1] = 0;
      pcache_add(cachekey, sock, 25,
                      (pcache_free_cb)psync_socket_close_download,
                      PSYNC_MAX_IDLE_HTTP_CONNS);
    }
    pthread_mutex_unlock(&connect_cache_mutex);
    if (sock)
      pdbg_logf(D_NOTICE, "added connection to %s to cache", node->host);
    free(node);
  }
}

connect_cache_tree_node_t *connect_cache_create_node(const char *host) {
  connect_cache_tree_node_t *res;
  size_t len;
  len = strlen(host) + 1;
  res = (connect_cache_tree_node_t *)malloc(
      offsetof(connect_cache_tree_node_t, host) + len);
  res->usessl = psync_setting_get_bool(_PS(usessl));
  res->haswaiter = 0;
  res->ready = 0;
  memcpy(res->host, host, len);
  return res;
}

static connect_cache_tree_node_t *
connect_cache_neighbour_is_free_duplicate(psync_tree *e, const char *host) {
  connect_cache_tree_node_t *node;
  psync_tree *n;
  n = ptree_get_next(e);
  while (n) {
    node = ptree_element(n, connect_cache_tree_node_t, tree);
    if (strcmp(node->host, host))
      break;
    if (!node->haswaiter)
      return node;
    n = ptree_get_next(n);
  }
  n = ptree_get_prev(e);
  while (n) {
    node = ptree_element(n, connect_cache_tree_node_t, tree);
    if (strcmp(node->host, host))
      break;
    if (!node->haswaiter)
      return node;
    n = ptree_get_prev(n);
  }
  return NULL;
}

void psync_http_connect_and_cache_host(const char *host) {
  connect_cache_tree_node_t *node;
  psync_tree *e;
  int c;
  pdbg_logf(D_NOTICE, "creating connection to host %s for cache", host);
  pthread_mutex_lock(&connect_cache_mutex);
  if (!connect_cache_tree) {
    node = connect_cache_create_node(host);
    ptree_add_after(&connect_cache_tree, NULL, &node->tree);
  } else {
    e = connect_cache_tree;
    while (1) {
      node = ptree_element(e, connect_cache_tree_node_t, tree);
      c = strcmp(host, node->host);
      if (c < 0) {
        if (e->left)
          e = e->left;
        else {
          node = connect_cache_create_node(host);
          ptree_add_before(&connect_cache_tree, e, &node->tree);
          break;
        }
      } else if (c > 0) {
        if (e->right)
          e = e->right;
        else {
          node = connect_cache_create_node(host);
          ptree_add_after(&connect_cache_tree, e, &node->tree);
          break;
        }
      } else {
        if (!node->haswaiter ||
            connect_cache_neighbour_is_free_duplicate(e, host)) {
          node = NULL;
          break;
        } else {
          node = connect_cache_create_node(host);
          ptree_add_after(&connect_cache_tree, e, &node->tree);
          break;
        }
      }
    }
  }
  pthread_mutex_unlock(&connect_cache_mutex);
  if (node)
    prun_thread1("connect http cache", connect_cache_thread, node);
  else
    pdbg_logf(D_NOTICE, "connection for %s is already in progress", host);
}

psock_t *connect_cache_wait_for_http_connection(const char *host,
                                                     int usessl) {
  connect_cache_tree_node_t *node;
  psync_tree *e;
  int c;
  pthread_mutex_lock(&connect_cache_mutex);
  if (connect_cache_tree) {
    e = connect_cache_tree;
    while (1) {
      node = ptree_element(e, connect_cache_tree_node_t, tree);
      c = strcmp(host, node->host);
      if (c < 0) {
        if (e->left)
          e = e->left;
        else
          break;
      } else if (c > 0) {
        if (e->right)
          e = e->right;
        else
          break;
      } else {
        if ((!node->haswaiter ||
             (node = connect_cache_neighbour_is_free_duplicate(e, host))) &&
            node->usessl == usessl) {
          pthread_cond_t cond;
          psock_t *sock;
          pthread_cond_init(&cond, NULL);
          node->haswaiter = 1;
          node->cond = &cond;
          node->res = &sock;
          do {
            pthread_cond_wait(&cond, &connect_cache_mutex);
          } while (!node->ready);
          pthread_mutex_unlock(&connect_cache_mutex);
          free(node);
          if (sock)
            pdbg_logf(D_NOTICE, "waited successfully for connection to %s", host);
          return sock;
        }
        break;
      }
    }
  }
  pthread_mutex_unlock(&connect_cache_mutex);
  return NULL;
}

psync_http_socket *psync_http_connect_multihost(const binresult *hosts,
                                                const char **host) {
  psock_t *sock;
  psync_http_socket *hsock;
  uint32_t i;
  int usessl, cl;
  char cachekey[256];
  usessl = psync_setting_get_bool(_PS(usessl));
  sock = NULL;
  for (i = 0; i < hosts->length; i++) {
    cl = snprintf(cachekey, sizeof(cachekey) - 1, "HTTP%d-%s", usessl,
                  hosts->array[i]->str) +
         1;
    cachekey[sizeof(cachekey) - 1] = 0;
    sock = (psock_t *)pcache_get(cachekey);
    if (sock) {
      if (pdbg_unlikely(psock_is_broken(sock->sock))) {
        psock_close_bad(sock);
        sock = NULL;
      } else {
        pdbg_logf(D_NOTICE, "got socket to %s from cache", hosts->array[i]->str);
        *host = hosts->array[i]->str;
        break;
      }
    }
  }
  if (!sock) {
    for (i = 0; i < hosts->length; i++)
      if ((sock = connect_cache_wait_for_http_connection(hosts->array[i]->str,
                                                         usessl))) {
        cl = snprintf(cachekey, sizeof(cachekey) - 1, "HTTP%d-%s", usessl,
                      hosts->array[i]->str) +
             1;
        cachekey[sizeof(cachekey) - 1] = 0;
        *host = hosts->array[i]->str;
        break;
      }
    if (!sock) {
      for (i = 0; i < hosts->length; i++) {
        sock = psock_connect(hosts->array[i]->str, usessl ? 443 : 80,
                                    usessl);
        if (sock) {
          cl = snprintf(cachekey, sizeof(cachekey) - 1, "HTTP%d-%s", usessl,
                        hosts->array[i]->str) +
               1;
          cachekey[sizeof(cachekey) - 1] = 0;
          *host = hosts->array[i]->str;
          break;
        }
      }
      if (!sock) {
        ptimer_notify_exception();
        return NULL;
      }
    }
  }
  hsock = (psync_http_socket *)malloc(
      offsetof(psync_http_socket, cachekey) + cl);
  hsock->sock = sock;
  hsock->readbuff = malloc(PSYNC_HTTP_RESP_BUFFER);
  ;
  hsock->contentlength = -1;
  hsock->readbytes = 0;
  hsock->keepalive = 0;
  hsock->readbuffoff = 0;
  hsock->readbuffsize = 0;
  memcpy(hsock->cachekey, cachekey, cl);
  return hsock;
}

psync_http_socket *
psync_http_connect_multihost_from_cache(const binresult *hosts,
                                        const char **host) {
  psock_t *sock;
  psync_http_socket *hsock;
  uint32_t i;
  int usessl, cl;
  char cachekey[256];
  usessl = psync_setting_get_bool(_PS(usessl));
  sock = NULL;
  for (i = 0; i < hosts->length; i++) {
    cl = snprintf(cachekey, sizeof(cachekey) - 1, "HTTP%d-%s", usessl,
                  hosts->array[i]->str) +
         1;
    cachekey[sizeof(cachekey) - 1] = 0;
    sock = (psock_t *)pcache_get(cachekey);
    if (sock) {
      if (pdbg_unlikely(psock_is_broken(sock->sock))) {
        psock_close_bad(sock);
        sock = NULL;
      } else {
        pdbg_logf(D_NOTICE, "got socket to %s from cache", hosts->array[i]->str);
        *host = hosts->array[i]->str;
        break;
      }
    }
  }
  if (!sock)
    return NULL;
  hsock = (psync_http_socket *)malloc(
      offsetof(psync_http_socket, cachekey) + cl);
  hsock->sock = sock;
  hsock->readbuff = malloc(PSYNC_HTTP_RESP_BUFFER);
  ;
  hsock->contentlength = -1;
  hsock->readbytes = 0;
  hsock->keepalive = 0;
  hsock->readbuffoff = 0;
  hsock->readbuffsize = 0;
  memcpy(hsock->cachekey, cachekey, cl);
  return hsock;
}

int psync_http_request_range_additional(psync_http_socket *sock,
                                        const char *host, const char *path,
                                        uint64_t from, uint64_t to,
                                        const char *addhdr) {
  int rl;
  if (unlikely(!addhdr))
    return psync_http_request(sock, host, path, from, to, NULL);
  rl = snprintf(
      sock->readbuff, PSYNC_HTTP_RESP_BUFFER,
      "GET %s HTTP/1.1\015\012Host: %s\015\012Range: bytes=%" PRIu64
      "-%" PRIu64 "\015\012Connection: Keep-Alive\015\012%s\015\012",
      path, host, from, to, addhdr);
  if (unlikely(rl >= PSYNC_HTTP_RESP_BUFFER - 1))
    return psync_http_request(sock, host, path, from, to, NULL);
  return psock_writeall(sock->sock, sock->readbuff, rl) == rl ? 0 : -1;
}

int psync_http_request(psync_http_socket *sock, const char *host,
                       const char *path, uint64_t from, uint64_t to,
                       const char *addhdr) {
  int rl;
  if (!addhdr)
    addhdr = "";
  if (from || to) {
    if (to)
      rl = snprintf(
          sock->readbuff, PSYNC_HTTP_RESP_BUFFER,
          "GET %s HTTP/1.1\015\012Host: %s\015\012Range: bytes=%" PRIu64
          "-%" PRIu64 "\015\012Connection: Keep-Alive\015\012%s\015\012",
          path, host, from, to, addhdr);
    else
      rl = snprintf(
          sock->readbuff, PSYNC_HTTP_RESP_BUFFER,
          "GET %s HTTP/1.1\015\012Host: %s\015\012Range: bytes=%" PRIu64
          "-\015\012Connection: Keep-Alive\015\012%s\015\012",
          path, host, from, addhdr);
  } else
    rl = snprintf(sock->readbuff, PSYNC_HTTP_RESP_BUFFER,
                  "GET %s HTTP/1.1\015\012Host: %s\015\012Connection: "
                  "Keep-Alive\015\012%s\015\012",
                  path, host, addhdr);
  return psock_writeall(sock->sock, sock->readbuff, rl) == rl ? 0 : -1;
}

int psync_http_next_request(psync_http_socket *sock) {
  char *ptr, *end, *key, *val;
  int64_t clen;
  uint32_t keepalive;
  int rl, rb, isval;
  char ch, lch;
  if (unlikely((rb = psock_read(sock->sock, sock->readbuff,
                                       PSYNC_HTTP_RESP_BUFFER - 1)) <= 0)) {
    pdbg_logf(D_WARNING, "read from socket for %d bytes returned %d",
          (int)(PSYNC_HTTP_RESP_BUFFER - 1), rb);
    goto err0;
  }
  sock->readbuff[rb] = 0;
  ptr = sock->readbuff;
  while (*ptr && !isspace(*ptr))
    ptr++;
  while (*ptr && isspace(*ptr))
    ptr++;
  if (pdbg_unlikely(!isdigit(*ptr))) {
    pdbg_logf(D_NOTICE, "got %s", sock->readbuff);
    goto err0;
  }
  rl = atoi(ptr);
  if (pdbg_unlikely(rl / 10 != 20)) {
    if (pdbg_unlikely(rl == 0))
      return -1;
    else
      return rl;
  }
  while (*ptr && *ptr != '\012')
    ptr++;
  if (pdbg_unlikely(!*ptr))
    goto err0;
  ptr++;
  end = sock->readbuff + rb;
  lch = 0;
  isval = 0;
  keepalive = 0;
  clen = -1;
  key = val = ptr;
cont:
  for (; ptr < end; ptr++) {
    ch = *ptr;
    if (ch == '\015') {
      *ptr = 0;
      continue;
    } else if (ch == '\012') {
      if (lch == '\012') {
        ptr++;
        goto ex;
      }
      *ptr = 0;
      /*      pdbg_logf(D_NOTICE, "key=%s, value=%s", key, val);*/
      if (!memcmp(key, "content-length", 14))
        clen = psync_ato64(val);
      else if (!memcmp(key, "keep-alive", 10) && !memcmp(val, "timeout=", 8))
        keepalive = psync_ato32(val + 8);
      key = ptr + 1;
      isval = 0;
    } else if (ch == ':' && !isval) {
      *ptr++ = 0;
      while (isspace(*ptr))
        ptr++;
      val = ptr;
      *ptr = tolower(*ptr);
      isval = 1;
    } else
      *ptr = tolower(ch);
    lch = ch;
  }
  if (pdbg_unlikely(rb == PSYNC_HTTP_RESP_BUFFER))
    goto err0;
  rl = psock_read(sock->sock, sock->readbuff + rb,
                         PSYNC_HTTP_RESP_BUFFER - rb);
  if (pdbg_unlikely(rl <= 0))
    goto err0;
  rb += rl;
  end = sock->readbuff + rb;
  goto cont;
ex:
  rl = ptr - sock->readbuff;
  sock->contentlength = clen;
  sock->readbytes = 0;
  sock->keepalive = keepalive;
  sock->readbuffoff = rl;
  sock->readbuffsize = rb;
  return 0;
err0:
  return -1;
}

int psync_http_request_readall(psync_http_socket *http, void *buff, int num) {
  int rb;
  if (http->contentlength != -1) {
    if ((uint64_t)num > (uint64_t)http->contentlength - http->readbytes)
      num = http->contentlength - http->readbytes;
    if (!num)
      return num;
  }
  if (http->readbuff) {
    int cp;
    if (num < http->readbuffsize - http->readbuffoff)
      cp = num;
    else
      cp = http->readbuffsize - http->readbuffoff;
    memcpy(buff, (unsigned char *)http->readbuff + http->readbuffoff, cp);
    http->readbuffoff += cp;
    http->readbytes += cp;
    if (cp == num)
      return cp;
    rb = psock_readall(http->sock, (unsigned char *)buff + cp, num - cp);
    if (rb <= 0)
      return -1;
    else {
      http->readbytes += rb;
      if (rb != num - cp && http->contentlength != -1)
        return -1;
      else
        return cp + rb;
    }
  } else {
    rb = psock_readall(http->sock, buff, num);
    if (rb > 0)
      http->readbytes += num;
    if (rb != num && http->contentlength != -1)
      return -1;
    else
      return rb;
  }
}

char *psync_url_decode(const char *s) {
  char *ret, *p;
  size_t slen;
  char unsigned ch1, ch2;
  slen = strlen(s);
  ret = p = (char *)malloc(slen + 1);
  while (slen--) {
    if (*s == '+')
      *p = ' ';
    else if (*s == '%' && slen >= 2 && isxdigit(ch1 = tolower(*(s + 1))) &&
             isxdigit(ch2 = tolower(*(s + 2)))) {
      *p = (char)(((ch1 >= 'a' && ch1 <= 'f' ? ch1 - 'a' + 10 : ch1 - '0')
                   << 4) +
                  (ch2 >= 'a' && ch2 <= 'f' ? ch2 - 'a' + 10 : ch2 - '0'));
      s += 2;
      slen -= 2;
    } else
      *p = *s;
    s++;
    p++;
  }
  *p = 0;
  return ret;
}

static int psync_net_get_checksums(psock_t *api, psync_fileid_t fileid,
                                   uint64_t hash,
                                   psync_file_checksums **checksums) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_NUM("fileid", fileid),
                       PAPI_NUM("hash", hash)};
  binresult *res;
  const binresult *hosts;
  const char *requestpath;
  psync_http_socket *http;
  psync_file_checksums *cs;
  psync_block_checksum_header hdr;
  uint64_t result;
  uint64_t i;
  char cookie[128];
  *checksums = NULL; /* gcc is not smart enough to notice that initialization is
                        not needed */
  if (api)
    res = papi_send2(api, "getchecksumlink", params);
  else {
    api = psync_apipool_get();
    if (unlikely(!api))
      return PSYNC_NET_TEMPFAIL;
    res = papi_send2(api, "getchecksumlink", params);
    if (res)
      psync_apipool_release(api);
    else
      psync_apipool_release_bad(api);
  }
  if (pdbg_unlikely(!res)) {
    ptimer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  }
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    pdbg_logf(D_ERROR, "getchecksumlink returned error %lu", (unsigned long)result);
    free(res);
    return psync_handle_api_result(result);
  }
  hosts = papi_find_result2(res, "hosts", PARAM_ARRAY);
  requestpath = papi_find_result2(res, "path", PARAM_STR)->str;
  psync_slprintf(cookie, sizeof(cookie), "Cookie: dwltag=%s\015\012",
                 papi_find_result2(res, "dwltag", PARAM_STR)->str);
  http = NULL;
  for (i = 0; i < hosts->length; i++)
    if ((http = psync_http_connect(hosts->array[i]->str, requestpath, 0, 0,
                                   cookie)))
      break;
  free(res);
  if (pdbg_unlikely(!http))
    return PSYNC_NET_TEMPFAIL;
  if (pdbg_unlikely(psync_http_readall(http, &hdr, sizeof(hdr)) != sizeof(hdr)))
    goto err0;
  i = (hdr.filesize + (uint64_t)hdr.blocksize - 1) / (uint64_t)hdr.blocksize;
  if ((sizeof(psync_block_checksum) + sizeof(uint32_t)) * i >=
      PSYNC_MAX_CHECKSUMS_SIZE) {
    pdbg_logf(
        D_WARNING, "checksums too large %lu",
        (unsigned long)((sizeof(psync_block_checksum) + sizeof(uint32_t)) * i));
    psync_http_close(http);
    return PSYNC_NET_OK;
  }
  cs = (psync_file_checksums *)malloc(
      offsetof(psync_file_checksums, blocks) +
      (sizeof(psync_block_checksum) + sizeof(uint32_t)) * i);
  cs->filesize = hdr.filesize;
  cs->blocksize = hdr.blocksize;
  cs->blockcnt = i;
  cs->next =
      (uint32_t *)(((char *)cs) + offsetof(psync_file_checksums, blocks) +
                   sizeof(psync_block_checksum) * i);
  if (pdbg_unlikely(psync_http_readall(http, cs->blocks,
                                      sizeof(psync_block_checksum) * i) !=
                   sizeof(psync_block_checksum) * i))
    goto err1;
  psync_http_close(http);
  memset(cs->next, 0, sizeof(uint32_t) * i);
  *checksums = cs;
  pdbg_logf(D_NOTICE, "checksums downloaded");
  return PSYNC_NET_OK;
err1:
  free(cs);
err0:
  psync_http_close(http);
  return PSYNC_NET_TEMPFAIL;
}

static int psync_net_get_upload_checksums(psock_t *api,
                                          psync_uploadid_t uploadid,
                                          psync_file_checksums **checksums) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("uploadid", uploadid)};
  binresult *res;
  psync_file_checksums *cs;
  psync_block_checksum_header hdr;
  uint64_t result;
  uint32_t i;
  *checksums = NULL;
  res = papi_send2(api, "upload_blockchecksums", params);
  if (pdbg_unlikely(!res)) {
    ptimer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  }
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    pdbg_logf(D_ERROR, "upload_blockchecksums returned error %lu",
          (unsigned long)result);
    free(res);
    return psync_handle_api_result(result);
  }
  free(res);
  if (pdbg_unlikely(psync_socket_readall_download(api, &hdr, sizeof(hdr)) !=
                   sizeof(hdr)))
    goto err0;
  i = (hdr.filesize + hdr.blocksize - 1) / hdr.blocksize;
  if ((sizeof(psync_block_checksum) + sizeof(uint32_t)) * i >=
      PSYNC_MAX_CHECKSUMS_SIZE) {
    pdbg_logf(
        D_WARNING, "checksums too large %lu",
        (unsigned long)((sizeof(psync_block_checksum) + sizeof(uint32_t)) * i));
    // should we delete the uploadid from db as well so we don't loop
    // constantly?
    return PSYNC_NET_TEMPFAIL;
  }
  if (!hdr.filesize)
    return PSYNC_NET_PERMFAIL;
  cs = (psync_file_checksums *)malloc(
      offsetof(psync_file_checksums, blocks) +
      (sizeof(psync_block_checksum) + sizeof(uint32_t)) * i);
  cs->filesize = hdr.filesize;
  cs->blocksize = hdr.blocksize;
  cs->blockcnt = i;
  cs->next =
      (uint32_t *)(((char *)cs) + offsetof(psync_file_checksums, blocks) +
                   sizeof(psync_block_checksum) * i);
  if (pdbg_unlikely(psync_socket_readall_download(
                       api, cs->blocks, sizeof(psync_block_checksum) * i) !=
                   sizeof(psync_block_checksum) * i))
    goto err1;
  memset(cs->next, 0, sizeof(uint32_t) * i);
  *checksums = cs;
  return PSYNC_NET_OK;
err1:
  free(cs);
err0:
  return PSYNC_NET_TEMPFAIL;
}

/*
static int psync_sha1_cmp(const void *p1, const void *p2){
  psync_block_checksum **b1=(psync_block_checksum **)p1;
  psync_block_checksum **b2=(psync_block_checksum **)p2;
  return memcmp((*b1)->mbedtls_sha1, (*b2)->mbedtls_sha1,
PSYNC_SHA1_DIGEST_LEN);
}

static psync_block_checksum
**psync_net_get_sorted_checksums(psync_file_checksums *checksums){
  psync_block_checksum **ret;
  uint32_t i;
  ret=malloc(sizeof(psync_block_checksum *) * checksums->blockcnt);
  for (i=0; i<checksums->blockcnt; i++)
    ret[i]=&checksums->blocks[i];
  qsort(ret, checksums->blockcnt, sizeof(psync_block_checksum *),
psync_sha1_cmp); return ret;
}*/

static int psync_is_prime(unsigned long num) {
  unsigned long i;
  for (i = 5; i * i <= num; i += 2)
    if (num % i == 0)
      return 0;
  return 1;
}

#define MAX_ADLER_COLL 64

/* Since it is fairly easy to generate adler32 collisions, a file can be crafted
 * to contain many colliding blocks. Our hash will drop entries if more than
 * MAX_ADLER_COLL collisions are detected (actually we just don't travel more
 * than MAX_ADLER_COLL from our "perfect" position in the hash).
 */

static psync_file_checksum_hash *
psync_net_create_hash(const psync_file_checksums *checksums) {
  psync_file_checksum_hash *h;
  unsigned long cnt, col;
  uint32_t i, o;
  cnt = ((checksums->blockcnt + 1) / 2) * 6 + 1;
  while (1) {
    if (psync_is_prime(cnt))
      break;
    cnt += 4;
    if (psync_is_prime(cnt))
      break;
    cnt += 2;
  }
  h = (psync_file_checksum_hash *)malloc(
      offsetof(psync_file_checksum_hash, elements) + sizeof(uint32_t) * cnt);
  h->elementcnt = cnt;
  memset(h->elements, 0, sizeof(uint32_t) * cnt);
  for (i = 0; i < checksums->blockcnt; i++) {
    o = checksums->blocks[i].adler % cnt;
    if (h->elements[o]) {
      col = 0;
      do {
        if (!memcmp(checksums->blocks[i].mbedtls_sha1,
                    checksums->blocks[h->elements[o] - 1].mbedtls_sha1,
                    PSYNC_SHA1_DIGEST_LEN)) {
          checksums->next[i] = h->elements[o];
          break;
        }
        if (++o >= cnt)
          o = 0;
        if (++col > MAX_ADLER_COLL)
          break;
      } while (h->elements[o]);
      if (col > MAX_ADLER_COLL) {
        pdbg_logf(D_WARNING, "too many collisions, ignoring a checksum %u",
              (unsigned)checksums->blocks[i].adler);
        continue;
      }
    }
    h->elements[o] = i + 1;
  }
  return h;
}

static void psync_net_hash_remove(psync_file_checksum_hash *restrict hash,
                                  psync_file_checksums *restrict checksums,
                                  uint32_t adler,
                                  const unsigned char *mbedtls_sha1) {
  uint32_t idx, zeroidx, o, bp;
  o = adler % hash->elementcnt;
  while (1) {
    idx = hash->elements[o];
    if (pdbg_unlikely(!idx))
      return;
    else if (checksums->blocks[idx - 1].adler == adler &&
             !memcmp(checksums->blocks[idx - 1].mbedtls_sha1, mbedtls_sha1,
                     PSYNC_SHA1_DIGEST_LEN))
      break;
    else if (++o >= hash->elementcnt)
      o = 0;
  }
  hash->elements[o] = 0;
  zeroidx = o;
  while (1) {
    if (++o >= hash->elementcnt)
      o = 0;
    idx = hash->elements[o];
    if (!idx)
      return;
    bp = checksums->blocks[idx - 1].adler % hash->elementcnt;
    if (bp != o) {
      while (1) {
        if (bp == zeroidx) {
          hash->elements[bp] = idx;
          hash->elements[o] = 0;
          zeroidx = o;
          break;
        } else if (bp == o)
          break;
        else if (++bp >= hash->elementcnt)
          bp = 0;
      }
    }
  }
}

static void
psync_net_block_match_found(psync_file_checksum_hash *restrict hash,
                            psync_file_checksums *restrict checksums,
                            psync_block_action *restrict blockactions,
                            uint32_t idx, uint32_t fileidx,
                            uint64_t fileoffset) {
  uint32_t cidx;
  idx--;
  if (blockactions[idx].type != PSYNC_RANGE_TRANSFER)
    return;
  cidx = idx;
  while (1) {
    blockactions[cidx].type = PSYNC_RANGE_COPY;
    blockactions[cidx].idx = fileidx;
    blockactions[cidx].off = fileoffset;
    cidx = checksums->next[cidx];
    if (cidx)
      cidx--;
    else
      break;
  }
  psync_net_hash_remove(hash, checksums, checksums->blocks[idx].adler,
                        checksums->blocks[idx].mbedtls_sha1);
}

static int psync_net_hash_has_adler(const psync_file_checksum_hash *hash,
                                    const psync_file_checksums *checksums,
                                    uint32_t adler) {
  uint32_t idx, o;
  o = adler % hash->elementcnt;
  while (1) {
    idx = hash->elements[o];
    if (!idx)
      return 0;
    else if (checksums->blocks[idx - 1].adler == adler)
      return 1;
    else if (++o >= hash->elementcnt)
      o = 0;
  }
}

static uint32_t psync_net_hash_has_adler_and_sha1(
    const psync_file_checksum_hash *hash, const psync_file_checksums *checksums,
    uint32_t adler, const unsigned char *mbedtls_sha1) {
  uint32_t idx, o;
  o = adler % hash->elementcnt;
  while (1) {
    idx = hash->elements[o];
    if (!idx)
      return 0;
    else if (checksums->blocks[idx - 1].adler == adler &&
             !memcmp(checksums->blocks[idx - 1].mbedtls_sha1, mbedtls_sha1,
                     PSYNC_SHA1_DIGEST_LEN))
      return idx;
    else if (++o >= hash->elementcnt)
      o = 0;
  }
}

#define ADLER32_1(o)                                                           \
  adler += buff[o];                                                            \
  sum += adler
#define ADLER32_2(o)                                                           \
  ADLER32_1(o);                                                                \
  ADLER32_1(o + 1)
#define ADLER32_4(o)                                                           \
  ADLER32_2(o);                                                                \
  ADLER32_2(o + 2)
#define ADLER32_8(o)                                                           \
  ADLER32_4(o);                                                                \
  ADLER32_4(o + 4)
#define ADLER32_16()                                                           \
  do {                                                                         \
    ADLER32_8(0);                                                              \
    ADLER32_8(8);                                                              \
  } while (0)

#define ADLER32_BASE 65521U
#define ADLER32_NMAX 5552U
#define ADLER32_INITIAL 1U

static uint32_t adler32(uint32_t adler, const unsigned char *buff, size_t len) {
  uint32_t sum, i;
  sum = adler >> 16;
  adler &= 0xffff;
  while (len >= ADLER32_NMAX) {
    len -= ADLER32_NMAX;
    for (i = 0; i < ADLER32_NMAX / 16; i++) {
      ADLER32_16();
      buff += 16;
    }
    adler %= ADLER32_BASE;
    sum %= ADLER32_BASE;
  }
  while (len >= 16) {
    len -= 16;
    ADLER32_16();
    buff += 16;
  }
  while (len--) {
    adler += *buff++;
    sum += adler;
  }
  adler %= ADLER32_BASE;
  sum %= ADLER32_BASE;
  return adler | (sum << 16);
}

static uint32_t adler32_roll(uint32_t adler, unsigned char byteout,
                             unsigned char bytein, uint32_t len) {
  uint32_t sum;
  sum = adler >> 16;
  adler &= 0xffff;
  adler += ADLER32_BASE + bytein - byteout;
  sum = (ADLER32_BASE * ADLER32_BASE + sum - len * byteout - ADLER32_INITIAL +
         adler) %
        ADLER32_BASE;
  /* dividing after sum calculation gives processor a chance to run the
   * divisions in parallel */
  adler %= ADLER32_BASE;
  return adler | (sum << 16);
}

static void psync_net_check_file_for_blocks(
    const char *name, psync_file_checksums *restrict checksums,
    psync_file_checksum_hash *restrict hash,
    psync_block_action *restrict blockactions, uint32_t fileidx) {
  unsigned char *buff;
  uint64_t buffoff;
  unsigned long buffersize, hbuffersize, bufferlen, inbyteoff, outbyteoff,
      blockmask;
  ssize_t rd;
  int fd;
  uint32_t adler, off;
  psync_sha1_ctx ctx;
  unsigned char sha1bin[PSYNC_SHA1_DIGEST_LEN];
  pdbg_logf(D_NOTICE, "scanning file %s for blocks", name);
  fd = pfile_open(name, O_RDONLY, 0);
  if (fd == INVALID_HANDLE_VALUE)
    return;
  if (checksums->blocksize * 2 > PSYNC_COPY_BUFFER_SIZE)
    buffersize = checksums->blocksize * 2;
  else
    buffersize = PSYNC_COPY_BUFFER_SIZE;
  hbuffersize = buffersize / 2;
  buff = malloc(buffersize);
  rd = pfile_read(fd, buff, hbuffersize);
  if (unlikely(rd < (ssize_t)hbuffersize)) {
    if (rd < (ssize_t)checksums->blocksize) {
      free(buff);
      pfile_close(fd);
      return;
    }
    bufferlen = (rd + checksums->blocksize - 1) / checksums->blocksize *
                checksums->blocksize;
    memset(buff + rd, 0, bufferlen - rd);
  } else
    bufferlen = buffersize;
  adler = adler32(ADLER32_INITIAL, buff, checksums->blocksize);
  outbyteoff = 0;
  buffoff = 0;
  inbyteoff = checksums->blocksize;
  blockmask = checksums->blocksize - 1;
  while (1) {
    if (psync_net_hash_has_adler(hash, checksums, adler)) {
      if (outbyteoff < inbyteoff)
        psync_sha1(buff + outbyteoff, checksums->blocksize, sha1bin);
      else {
        psync_sha1_init(&ctx);
        psync_sha1_update(&ctx, buff + outbyteoff, buffersize - outbyteoff);
        psync_sha1_update(&ctx, buff, inbyteoff);
        psync_sha1_final(sha1bin, &ctx);
      }
      off = psync_net_hash_has_adler_and_sha1(hash, checksums, adler, sha1bin);
      if (off)
        psync_net_block_match_found(hash, checksums, blockactions, off, fileidx,
                                    buffoff + outbyteoff);
    }
    if (unlikely((inbyteoff & blockmask) == 0)) {
      if (outbyteoff >= bufferlen) {
        outbyteoff = 0;
        buffoff += buffersize;
      }
      if (inbyteoff == bufferlen) { /* not >=, bufferlen might be lower than
                                       current inbyteoff */
        if (bufferlen != buffersize)
          break;
        inbyteoff = 0;
        rd = pfile_read(fd, buff, hbuffersize);
        if (unlikely(rd != hbuffersize)) {
          if (rd <= 0)
            break;
          else {
            bufferlen = (rd + checksums->blocksize - 1) / checksums->blocksize *
                        checksums->blocksize;
            memset(buff + rd, 0, bufferlen - rd);
          }
        }
      } else if (inbyteoff == hbuffersize) {
        rd = pfile_read(fd, buff + hbuffersize, hbuffersize);
        if (unlikely(rd != hbuffersize)) {
          if (rd <= 0)
            break;
          else {
            bufferlen = (rd + checksums->blocksize - 1) / checksums->blocksize *
                        checksums->blocksize;
            memset(buff + hbuffersize + rd, 0, bufferlen - rd);
            bufferlen += hbuffersize;
          }
        }
      }
    }
    adler = adler32_roll(adler, buff[outbyteoff++], buff[inbyteoff++],
                         checksums->blocksize);
  }
  free(buff);
  pfile_close(fd);
}

int psync_net_download_ranges(psync_list *ranges, psync_fileid_t fileid,
                              uint64_t filehash, uint64_t filesize,
                              char *const *files, uint32_t filecnt) {
  psync_range_list_t *range;
  psync_file_checksums *checksums;
  psync_file_checksum_hash *hash;
  psync_block_action *blockactions;
  uint32_t i, bs;
  int rt;
  if (!filecnt)
    goto fulldownload;
  rt = psync_net_get_checksums(NULL, fileid, filehash, &checksums);
  if (pdbg_unlikely(rt == PSYNC_NET_PERMFAIL))
    goto fulldownload;
  else if (pdbg_unlikely(rt == PSYNC_NET_TEMPFAIL))
    return PSYNC_NET_TEMPFAIL;
  if (pdbg_unlikely(checksums->filesize != filesize)) {
    free(checksums);
    return PSYNC_NET_TEMPFAIL;
  }
  hash = psync_net_create_hash(checksums);
  blockactions = malloc(sizeof(psync_block_action) * checksums->blockcnt);
  memset(blockactions, 0, sizeof(psync_block_action) * checksums->blockcnt);
  for (i = 0; i < filecnt; i++)
    psync_net_check_file_for_blocks(files[i], checksums, hash, blockactions, i);
  free(hash);
  range = malloc(sizeof(psync_range_list_t));
  range->len = checksums->blocksize;
  range->type = blockactions[0].type;
  if (range->type == PSYNC_RANGE_COPY) {
    range->off = blockactions[0].off;
    range->filename = files[blockactions[0].idx];
  } else
    range->off = 0;
  psync_list_add_tail(ranges, &range->list);
  for (i = 1; i < checksums->blockcnt; i++) {
    if (i == checksums->blockcnt - 1) {
      bs = checksums->filesize % checksums->blocksize;
      if (!bs)
        bs = checksums->blocksize;
    } else
      bs = checksums->blocksize;
    if (blockactions[i].type != range->type ||
        (range->type == PSYNC_RANGE_COPY &&
         (range->filename != files[blockactions[i].idx] ||
          range->off + range->len != blockactions[i].off))) {
      range = malloc(sizeof(psync_range_list_t));
      range->len = bs;
      range->type = blockactions[i].type;
      if (range->type == PSYNC_RANGE_COPY) {
        range->off = blockactions[i].off;
        range->filename = files[blockactions[i].idx];
      } else
        range->off = (uint64_t)i * checksums->blocksize;
      psync_list_add_tail(ranges, &range->list);
    } else
      range->len += bs;
  }
  free(checksums);
  free(blockactions);
  return PSYNC_NET_OK;
fulldownload:
  range = malloc(sizeof(psync_range_list_t));
  range->off = 0;
  range->len = filesize;
  range->type = PSYNC_RANGE_TRANSFER;
  psync_list_add_tail(ranges, &range->list);
  return PSYNC_NET_OK;
}

static int check_range_for_blocks(psync_file_checksums *checksums,
                                  psync_file_checksum_hash *hash, uint64_t off,
                                  uint64_t len, int fd,
                                  psync_list *nr) {
  unsigned char *buff;
  psync_upload_range_list_t *ur;
  uint64_t buffoff, blen;
  unsigned long buffersize, hbuffersize, bufferlen, inbyteoff, outbyteoff,
      blockmask;
  ssize_t rd;
  uint32_t adler, blockidx;
  int32_t skipbytes;
  psync_sha1_ctx ctx;
  unsigned char sha1bin[PSYNC_SHA1_DIGEST_LEN];
  if (pdbg_unlikely(pfile_seek(fd, off, SEEK_SET) == -1))
    return PSYNC_NET_TEMPFAIL;
  pdbg_logf(D_NOTICE, "scanning in range starting %lu, length %lu, blocksize %u",
        (unsigned long)off, (unsigned long)len, (unsigned)checksums->blocksize);
  if (checksums->blocksize * 2 > PSYNC_COPY_BUFFER_SIZE ||
      len < PSYNC_COPY_BUFFER_SIZE)
    buffersize = checksums->blocksize * 2;
  else
    buffersize = PSYNC_COPY_BUFFER_SIZE;
  hbuffersize = buffersize / 2;
  buff = malloc(buffersize);
  rd = pfile_read(fd, buff, hbuffersize);
  if (unlikely(rd < (ssize_t)hbuffersize)) {
    free(buff);
    return PSYNC_NET_OK;
  } else
    bufferlen = buffersize;
  adler = adler32(ADLER32_INITIAL, buff, checksums->blocksize);
  outbyteoff = 0;
  buffoff = 0;
  inbyteoff = checksums->blocksize;
  blockmask = checksums->blocksize - 1;
  ur = NULL;
  skipbytes = -1;
  while (buffoff + outbyteoff < len) {
    if (psync_net_hash_has_adler(hash, checksums, adler)) {
      if (outbyteoff < inbyteoff)
        psync_sha1(buff + outbyteoff, checksums->blocksize, sha1bin);
      else {
        psync_sha1_init(&ctx);
        psync_sha1_update(&ctx, buff + outbyteoff, buffersize - outbyteoff);
        psync_sha1_update(&ctx, buff, inbyteoff);
        psync_sha1_final(sha1bin, &ctx);
      }
      blockidx =
          psync_net_hash_has_adler_and_sha1(hash, checksums, adler, sha1bin);
      if (blockidx) {
        // pdbg_logf(D_NOTICE, "got block, buffoff+outbyteoff=%lu, off=%lu,
        // blockidx=%u", buffoff+outbyteoff, off, (unsigned)(blockidx-1));
        if (buffoff + outbyteoff + checksums->blocksize <= len)
          blen = checksums->blocksize;
        else
          blen = len - buffoff - outbyteoff;
        if (ur &&
            ur->off + ur->len ==
                (uint64_t)(blockidx - 1) * checksums->blocksize &&
            ur->uploadoffset + ur->len == off + buffoff + outbyteoff)
          ur->len += blen;
        else {
          ur = malloc(sizeof(psync_upload_range_list_t));
          ur->uploadoffset = off + buffoff + outbyteoff;
          ur->off = (uint64_t)(blockidx - 1) * checksums->blocksize;
          ur->len = blen;
          psync_list_add_tail(nr, &ur->list);
        }
        if (blen != checksums->blocksize)
          break;
        blockidx = checksums->blocksize - (inbyteoff & blockmask);
        if (blockidx == checksums->blocksize)
          blockidx = 0;
        skipbytes = checksums->blocksize - blockidx;
        inbyteoff += blockidx;
        outbyteoff += blockidx;
      }
    }
    if (unlikely((inbyteoff & blockmask) == 0)) {
      if (outbyteoff >= buffersize) {
        outbyteoff -= buffersize;
        buffoff += buffersize;
      }
      if (inbyteoff == bufferlen) { /* not >=, bufferlen might be lower than
                                       current inbyteoff */
        if (bufferlen != buffersize)
          break;
        inbyteoff = 0;
        rd = pfile_read(fd, buff, hbuffersize);
        if (unlikely(rd != hbuffersize)) {
          if (rd <= 0)
            break;
          else {
            pdbg_assert(checksums->blocksize != 0);
            bufferlen = (rd + checksums->blocksize - 1) / checksums->blocksize *
                        checksums->blocksize;
            memset(buff + rd, 0, bufferlen - rd);
          }
        }
      } else if (inbyteoff == hbuffersize) {
        rd = pfile_read(fd, buff + hbuffersize, hbuffersize);
        if (unlikely(rd != hbuffersize)) {
          if (rd <= 0)
            break;
          else {
            bufferlen = (rd + checksums->blocksize - 1) / checksums->blocksize *
                        checksums->blocksize;
            memset(buff + hbuffersize + rd, 0, bufferlen - rd);
            bufferlen += hbuffersize;
          }
        }
      }
      if (skipbytes != -1) {
        inbyteoff += skipbytes;
        outbyteoff += skipbytes;
        skipbytes = -1;
        if (outbyteoff < inbyteoff)
          adler =
              adler32(ADLER32_INITIAL, buff + outbyteoff, checksums->blocksize);
        else {
          adler = adler32(ADLER32_INITIAL, buff + outbyteoff,
                          buffersize - outbyteoff);
          adler = adler32(adler, buff, inbyteoff);
        }
        continue;
      }
    }
    adler = adler32_roll(adler, buff[outbyteoff++], buff[inbyteoff++],
                         checksums->blocksize);
  }
  free(buff);
  return PSYNC_NET_OK;
}

static void merge_list_to_element(psync_upload_range_list_t *le,
                                  psync_list *rlist) {
  psync_list *l, *lb;
  psync_upload_range_list_t *ur, *n;
  psync_list_for_each_safe(l, lb, rlist) {
    ur = psync_list_element(l, psync_upload_range_list_t, list);
    psync_list_del(l);
    pdbg_assertw(ur->len <= le->len);
    pdbg_assertw(ur->uploadoffset >= le->uploadoffset);
    pdbg_assertw(ur->uploadoffset + ur->len <= le->uploadoffset + le->len);
    if (IS_DEBUG &&
        (!(ur->len <= le->len) || !(ur->uploadoffset >= le->uploadoffset) ||
         !(ur->uploadoffset + ur->len <= le->uploadoffset + le->len)))
      pdbg_logf(D_ERROR,
            "ur->len=%lu, le->len=%lu, ur->uploadoffset=%lu, "
            "le->uploadoffset=%lu",
            (unsigned long)ur->len, (unsigned long)le->len,
            (unsigned long)ur->uploadoffset, (unsigned long)le->uploadoffset);
    if (ur->len == le->len) {
      pdbg_assertw(ur->uploadoffset == le->uploadoffset);
      pdbg_assertw(psync_list_isempty(rlist));
      psync_list_add_after(&le->list, &ur->list);
      psync_list_del(&le->list);
      free(le);
    } else if (ur->uploadoffset == le->uploadoffset) {
      psync_list_add_before(&le->list, &ur->list);
      le->uploadoffset += ur->len;
      le->off += ur->len;
      le->len -= ur->len;
    } else if (ur->uploadoffset + ur->len == le->uploadoffset + le->len) {
      psync_list_add_after(&le->list, &ur->list);
      le->len -= ur->len;
    } else {
      n = malloc(sizeof(psync_upload_range_list_t));
      n->uploadoffset = n->off = ur->uploadoffset + ur->len;
      pdbg_assertw(le->len > ur->uploadoffset - le->uploadoffset + ur->len);
      n->len = le->len - (ur->uploadoffset - le->uploadoffset) - ur->len;
      n->type = PSYNC_URANGE_UPLOAD;
      le->len = ur->uploadoffset - le->uploadoffset;
      psync_list_add_after(&le->list, &ur->list);
      psync_list_add_after(&ur->list, &n->list);
      le = n;
    }
  }
}

int psync_net_scan_file_for_blocks(psock_t *api, psync_list *rlist,
                                   psync_fileid_t fileid, uint64_t filehash,
                                   int fd) {
  psync_file_checksums *checksums;
  psync_file_checksum_hash *hash;
  psync_list *l, *lb;
  psync_upload_range_list_t *ur, *le;
  psync_list nr;
  int rt;
  pdbg_logf(D_NOTICE, "scanning fileid %lu hash %lu for blocks",
        (unsigned long)fileid, (unsigned long)filehash);
  rt = psync_net_get_checksums(api, fileid, filehash, &checksums);
  if (pdbg_unlikely(rt == PSYNC_NET_PERMFAIL))
    return PSYNC_NET_OK;
  else if (pdbg_unlikely(rt == PSYNC_NET_TEMPFAIL))
    return PSYNC_NET_TEMPFAIL;
  hash = psync_net_create_hash(checksums);
  psync_list_for_each_safe(l, lb, rlist) {
    ur = psync_list_element(l, psync_upload_range_list_t, list);
    if (ur->len < checksums->blocksize || ur->type != PSYNC_URANGE_UPLOAD)
      continue;
    psync_list_init(&nr);
    if (check_range_for_blocks(checksums, hash, ur->off, ur->len, fd, &nr) ==
        PSYNC_NET_TEMPFAIL) {
      free(hash);
      free(checksums);
      return PSYNC_NET_TEMPFAIL;
    }
    if (!psync_list_isempty(&nr)) {
      psync_list_for_each_element(le, &nr, psync_upload_range_list_t, list) {
        le->type = PSYNC_URANGE_COPY_FILE;
        le->file.fileid = fileid;
        le->file.hash = filehash;
      }
      merge_list_to_element(ur, &nr);
    }
  }
  free(hash);
  free(checksums);
  return PSYNC_NET_OK;
}

int psync_net_scan_upload_for_blocks(psock_t *api, psync_list *rlist,
                                     psync_uploadid_t uploadid,
                                     int fd) {
  psync_file_checksums *checksums;
  psync_file_checksum_hash *hash;
  psync_list *l, *lb;
  psync_upload_range_list_t *ur, *le;
  psync_list nr;
  int rt;
  pdbg_logf(D_NOTICE, "scanning uploadid %lu for blocks", (unsigned long)uploadid);
  rt = psync_net_get_upload_checksums(api, uploadid, &checksums);
  if (pdbg_unlikely(rt == PSYNC_NET_PERMFAIL))
    return PSYNC_NET_OK;
  else if (pdbg_unlikely(rt == PSYNC_NET_TEMPFAIL))
    return PSYNC_NET_TEMPFAIL;
  hash = psync_net_create_hash(checksums);
  psync_list_for_each_safe(l, lb, rlist) {
    ur = psync_list_element(l, psync_upload_range_list_t, list);
    if (ur->len < checksums->blocksize || ur->type != PSYNC_URANGE_UPLOAD)
      continue;
    psync_list_init(&nr);
    if (check_range_for_blocks(checksums, hash, ur->off, ur->len, fd, &nr) ==
        PSYNC_NET_TEMPFAIL) {
      free(hash);
      free(checksums);
      return PSYNC_NET_TEMPFAIL;
    }
    if (!psync_list_isempty(&nr)) {
      psync_list_for_each_element(le, &nr, psync_upload_range_list_t, list) {
        le->type = PSYNC_URANGE_COPY_UPLOAD;
        le->uploadid = uploadid;
      }
      merge_list_to_element(ur, &nr);
    }
  }
  free(hash);
  free(checksums);
  return PSYNC_NET_OK;
}

static int is_revision_local(const unsigned char *localhashhex,
                             uint64_t filesize, psync_fileid_t fileid) {
  psync_sql_res *res;
  psync_uint_row row;
  // listrevisions does not return zero sized revisions, so do we
  if (filesize == 0)
    return 1;
  res = psql_query_rdlock(
      "SELECT f.fileid FROM filerevision f, hashchecksum h WHERE f.fileid=? "
      "AND f.hash=h.hash AND h.size=? AND h.checksum=?");
  psql_bind_uint(res, 1, fileid);
  psql_bind_uint(res, 2, filesize);
  psql_bind_lstr(res, 3, (const char *)localhashhex,
                         PSYNC_HASH_DIGEST_HEXLEN);
  row = psql_fetch_int(res);
  psql_free(res);
  return row ? 1 : 0;
}

static int download_file_revisions(psync_fileid_t fileid) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_NUM("fileid", fileid),
                       PAPI_BOOL("showchecksums", 1),
                       PAPI_STR("timeformat", "timestamp")};
  psock_t *api;
  binresult *res;
  const binresult *revs, *meta;
  psync_sql_res *fr, *hc;
  uint64_t result, hash, size;
  uint32_t i;
  api = psync_apipool_get();
  if (unlikely(!api))
    return PSYNC_NET_TEMPFAIL;
  res = papi_send2(api, "listrevisions", params);
  if (pdbg_unlikely(!res)) {
    psync_apipool_release_bad(api);
    ptimer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  }
  psync_apipool_release(api);
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    pdbg_logf(D_ERROR, "listrevisions returned error %lu", (unsigned long)result);
    free(res);
    return psync_handle_api_result(result);
  }
  revs = papi_find_result2(res, "revisions", PARAM_ARRAY);
  meta = papi_find_result2(res, "metadata", PARAM_HASH);
  psql_start();
  fr = psql_prepare("REPLACE INTO filerevision (fileid, hash, "
                                "ctime, size) VALUES (?, ?, ?, ?)");
  hc = psql_prepare(
      "REPLACE INTO hashchecksum (hash, size, checksum) VALUES (?, ?, ?)");
  for (i = 0; i < revs->length; i++) {
    hash = papi_find_result2(revs->array[i], "hash", PARAM_NUM)->num;
    size = papi_find_result2(revs->array[i], "size", PARAM_NUM)->num;
    psql_bind_uint(fr, 1, fileid);
    psql_bind_uint(fr, 2, hash);
    psql_bind_uint(
        fr, 3, papi_find_result2(revs->array[i], "created", PARAM_NUM)->num);
    psql_bind_uint(fr, 4, size);
    psql_run(fr);
    psql_bind_uint(hc, 1, hash);
    psql_bind_uint(hc, 2, size);
    psql_bind_lstr(
        hc, 3,
        papi_find_result2(revs->array[i], PSYNC_CHECKSUM, PARAM_STR)->str,
        PSYNC_HASH_DIGEST_HEXLEN);
    psql_run(hc);
  }
  hash = papi_find_result2(meta, "hash", PARAM_NUM)->num;
  psql_bind_uint(fr, 1, fileid);
  psql_bind_uint(fr, 2, hash);
  psql_bind_uint(fr, 3,
                      papi_find_result2(meta, "modified", PARAM_NUM)->num);
  psql_bind_uint(fr, 4, papi_find_result2(meta, "size", PARAM_NUM)->num);
  psql_run_free(fr);
  psql_bind_uint(hc, 1, hash);
  psql_bind_uint(hc, 2, papi_find_result2(meta, "size", PARAM_NUM)->num);
  psql_bind_lstr(hc, 3,
                         papi_find_result2(res, PSYNC_CHECKSUM, PARAM_STR)->str,
                         PSYNC_HASH_DIGEST_HEXLEN);
  psql_run_free(hc);
  psql_commit();
  free(res);
  return PSYNC_NET_OK;
}

int psync_is_revision_of_file(const unsigned char *localhashhex,
                              uint64_t filesize, psync_fileid_t fileid,
                              int *isrev) {
  int ret;
  if (is_revision_local(localhashhex, filesize, fileid)) {
    *isrev = 1;
    return PSYNC_NET_OK;
  }
  ret = download_file_revisions(fileid);
  if (ret != PSYNC_NET_OK)
    return ret;
  if (is_revision_local(localhashhex, filesize, fileid))
    *isrev = 1;
  else
    *isrev = 0;
  return PSYNC_NET_OK;
}

psync_file_lock_t *psync_lock_file(const char *path) {
  psync_file_lock_t *lock;
  psync_tree *tr, **at;
  size_t len;
  int cmp;
  len = strlen(path) + 1;
  lock = malloc(offsetof(psync_file_lock_t, filename) + len);
  memcpy(lock->filename, path, len);
  pthread_mutex_lock(&file_lock_mutex);
  tr = file_lock_tree;
  at = &file_lock_tree;
  while (tr) {
    cmp = strcmp(
        path, ptree_element(tr, psync_file_lock_t, tree)->filename);
    if (cmp < 0) {
      if (tr->left)
        tr = tr->left;
      else {
        at = &tr->left;
        break;
      }
    } else if (cmp > 0) {
      if (tr->right)
        tr = tr->right;
      else {
        at = &tr->right;
        break;
      }
    } else {
      pthread_mutex_unlock(&file_lock_mutex);
      free(lock);
      return NULL;
    }
  }
  *at = &lock->tree;
  ptree_added_at(&file_lock_tree, tr, &lock->tree);
  pthread_mutex_unlock(&file_lock_mutex);
  return lock;
}

void psync_unlock_file(psync_file_lock_t *lock) {
  pthread_mutex_lock(&file_lock_mutex);
  ptree_del(&file_lock_tree, &lock->tree);
  pthread_mutex_unlock(&file_lock_mutex);
  free(lock);
}

int psync_get_upload_checksum(psync_uploadid_t uploadid, unsigned char *uhash,
                              uint64_t *usize) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("uploadid", uploadid)};
  psock_t *api;
  binresult *res;
  api = psync_apipool_get();
  if (unlikely(!api))
    return PSYNC_NET_TEMPFAIL;
  res = papi_send2(api, "upload_info", params);
  if (pdbg_unlikely(!res)) {
    psync_apipool_release_bad(api);
    ptimer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  }
  psync_apipool_release(api);
  if (papi_find_result2(res, "result", PARAM_NUM)->num) {
    free(res);
    return PSYNC_NET_PERMFAIL;
  }
  *usize = papi_find_result2(res, "size", PARAM_NUM)->num;
  memcpy(uhash, papi_find_result2(res, PSYNC_CHECKSUM, PARAM_STR)->str,
         PSYNC_HASH_DIGEST_HEXLEN);
  free(res);
  return PSYNC_NET_OK;
}

static void proc_logout() { 
  psync_logout(PSTATUS_AUTH_BADTOKEN, 0); 
}

// this is called when ANY api call returns non zero result
void psync_process_api_error(uint64_t result) {
  if (result == 2000)
    prun_thread("logout from process_api_error", proc_logout);
}

static void psync_netlibs_timer(psync_timer_t timer, void *ptr) {
  psync_account_downloaded_bytes(0);
  account_uploaded_bytes(0);
}

static void psync_send_debug_thread(void *ptr) {
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  static char *last = NULL;
  char *str = (char *)ptr;
  pthread_mutex_lock(&m);
  if (!last || strcmp(last, str)) {
    binparam params[] = {PAPI_STR("report", str),
                         PAPI_NUM("userid", psync_my_userid)};
    binresult *res;
    pdbg_logf(D_NOTICE, "sending debug %s", str);
    res = psync_api_run_command("senddebug", params);
    if (res) {
      free(res);
      free(last);
      last = str;
    } else
      free(str);
  } else
    free(str);
  pthread_mutex_unlock(&m);
}

int psync_send_pdbg_logf(int thread, const char *file, const char *function,
                     int unsigned line, const char *fmt, ...) {
  char format[1024];
  va_list ap;
  char *ret;
  int sz, l;
  ret = pdevice_id();
  snprintf(format, sizeof(format), "%s %s: %s:%u (function %s): %s\n", ret,
           psync_thread_name, file, line, function, fmt);
  free(ret);
  format[sizeof(format) - 1] = 0;
  ret = NULL;
  l = 511;
  do {
    sz = l + 1;
    ret = (char *)realloc(ret, sz);
    va_start(ap, fmt);
    l = vsnprintf(ret, sz, format, ap);
    va_end(ap);
  } while (l >= sz);
  if (l > 0) {
    if (thread)
      prun_thread1("send debug", psync_send_debug_thread, ret);
    else
      psync_send_debug_thread(ret);
  } else
    free(ret);
  return 1;
}

void psync_netlibs_init() {
  ptimer_register(psync_netlibs_timer, 1, NULL);
  sem_init(&api_pool_sem, 0, PSYNC_APIPOOL_MAXACTIVE);
}

int psync_do_run_command_res(const char *cmd, size_t cmdlen,
                             const binparam *params, size_t paramscnt,
                             char **err) {
  psock_t *api;
  binresult *res;
  uint64_t result;
  int tries;
  tries = 0;
  while (1) {
    api = psync_apipool_get();
    if (unlikely(!api))
      goto neterr;
    res = papi_send(api, cmd, cmdlen, params, paramscnt, -1, 1);
    if (likely(res)) {
      psync_apipool_release(api);
      break;
    } else {
      psync_apipool_release_bad(api);
      if (++tries >= 5)
        goto neterr;
    }
  }
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    pdbg_logf(D_WARNING, "command %s returned code %u", cmd, (unsigned)result);
    if (err)
      *err = psync_strdup(papi_find_result2(res, "error", PARAM_STR)->str);
    psync_process_api_error(result);
  }
  free(res);
  return (int)result;
neterr:
  if (err)
    *err = psync_strdup("Could not connect to the server.");
  return -1;
}
