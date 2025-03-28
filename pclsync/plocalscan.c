/*
   Copyright (c) 2014 Anton Titov.

   Copyright (c) 2014 pCloud Ltd.  All rights reserved.

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
#include <pthread.h>
#include <string.h>

#include "pdevice.h"
#include "pfile.h"
#include "pfoldersync.h"
#include "plibs.h"
#include "plist.h"
#include "plocalnotify.h"
#include "plocalscan.h"
#include "ppath.h"
#include "ppathstatus.h"
#include "prun.h"
#include "prunthrottled.h"
#include "psettings.h"
#include "psql.h"
#include "pssl.h"
#include "pstatus.h"
#include "psys.h"
#include "ptask.h"
#include "ptimer.h"
#include "pupload.h"
#include "putil.h"


typedef struct {
  psync_list list;
  psync_folderid_t folderid;
  uint64_t deviceid;
  psync_syncid_t syncid;
  psync_synctype_t synctype;
  char localpath[];
} sync_list;

typedef struct {
  psync_list list;
  psync_fileorfolderid_t localid;
  psync_fileorfolderid_t remoteid;
  psync_folderid_t localparentfolderid;
  psync_folderid_t parentfolderid;
  uint64_t inode;
  uint64_t deviceid;
  uint64_t mtimenat;
  uint64_t size;
  psync_syncid_t syncid;
  psync_synctype_t synctype;
  uint8_t isfolder;
  char name[1];
} sync_folderlist;

typedef struct {
  psync_list list;
  uint64_t deviceid;
  psync_syncid_t syncid;
  uint64_t inode;
  char localpath[];
} sync_restat_list;

typedef struct {
  uint64_t deviceid;
  uint64_t inode;
} device_inode_t;

static device_inode_t *ignored_paths = NULL;
static uint32_t ign_paths_cnt = 0;
static uint32_t ign_paths_alloc = 0;
static time_t ign_last_check = 0;
static unsigned char ign_checksum[PSYNC_SHA256_DIGEST_LEN];

static pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scan_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t restat_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t scan_wakes = 0;
static uint32_t restart_scan = 0;
static uint32_t scan_stoppers = 0;

static const uint32_t requiredstatuses[] = {
    PSTATUS_COMBINE(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED),
    PSTATUS_COMBINE(PSTATUS_TYPE_RUN, PSTATUS_RUN_RUN | PSTATUS_RUN_PAUSE)};

#define SCAN_LIST_CNT 9

#define SCAN_LIST_NEWFILES 0
#define SCAN_LIST_DELFILES 1
#define SCAN_LIST_NEWFOLDERS 2
#define SCAN_LIST_DELFOLDERS 3
#define SCAN_LIST_MODFILES 4
#define SCAN_LIST_RENFILESFROM 5
#define SCAN_LIST_RENFILESTO 6
#define SCAN_LIST_RENFOLDERSROM 7
#define SCAN_LIST_RENFOLDERSTO 8

static psync_list scan_lists[SCAN_LIST_CNT];
static uint64_t localsleepperfolder;
static time_t starttime;
static unsigned long changes;
static int localnotify;
psync_list scan_folders_list;

static void scanner_set_syncs_to_list(psync_list *lst,
                                      psync_list *lst_deviceid_full) {
  psync_sql_res *res;
  psync_variant_row row;
  const char *lp;
  sync_list *l, *l_full_deviceid;
  char *syncmp;
  size_t lplen;
  struct stat st;
  uint64_t deviceid;
  uint64_t inodeid;

  psync_list_init(lst);
  psync_list_init(lst_deviceid_full);
  syncmp = psync_fs_getmountpoint();
  res = psql_query_rdlock(
      "SELECT id, folderid, localpath, synctype, deviceid, inode FROM "
      "syncfolder WHERE synctype&" NTO_STR(PSYNC_UPLOAD_ONLY) "=" NTO_STR(
          PSYNC_UPLOAD_ONLY));
  while ((row = psql_fetch(res))) {
    lp = psync_get_lstring(row[2], &lplen);
    if (unlikely(stat(lp, &st))) {
      pdbg_logf(D_WARNING, "could not stat local folder %s, ignoring sync", lp);
      continue;
    }
    if (unlikely(syncmp && !memcmp(syncmp, lp, strlen(syncmp)))) {
      pdbg_logf(D_WARNING,
            "folder %s is on pCloudDrive mounted as %s, ignoring sync", lp,
            syncmp);
      continue;
    }
    deviceid = psync_get_number(row[4]);
    inodeid = psync_get_number(row[5]);
    if (unlikely(deviceid != pfile_stat_device(&st) &&
                 inodeid != pfile_stat_inode(&st))) {
      pdbg_logf(D_WARNING, "folder %s deviceid is different, ignoring", lp);
      continue;
    }
    l = (sync_list *)malloc(offsetof(sync_list, localpath) + lplen + 1);
    l->folderid = psync_get_number(row[1]);
    l->deviceid = deviceid;
    l->syncid = psync_get_number(row[0]);
    l->synctype = psync_get_number(row[3]);
    memcpy(l->localpath, lp, lplen + 1);
    psync_list_add_tail(lst, &l->list);
    l_full_deviceid =
        (sync_list *)malloc(offsetof(sync_list, localpath) + lplen + 1);
    l_full_deviceid->folderid = psync_get_number(row[1]);
    l_full_deviceid->deviceid = pfile_stat_device_full(&st);
    l_full_deviceid->syncid = psync_get_number(row[0]);
    l_full_deviceid->synctype = psync_get_number(row[3]);
    memcpy(l_full_deviceid->localpath, lp, lplen + 1);
    psync_list_add_tail(lst_deviceid_full, &l_full_deviceid->list);
  }
  psql_free(res);
  free(syncmp);
}

static void add_ignored_dir(const char *path) {
  struct stat st;

  if (stat(path, &st))
    return;
  if (ign_paths_cnt >= ign_paths_alloc) {
    if (!ign_paths_alloc)
      ign_paths_alloc = 8;
    else
      ign_paths_alloc *= 2;
    ignored_paths = (device_inode_t *)realloc(
        ignored_paths, sizeof(device_inode_t) * ign_paths_alloc);
  }
  ignored_paths[ign_paths_cnt].deviceid = pfile_stat_device_full(&st);
  ignored_paths[ign_paths_cnt].inode = pfile_stat_inode(&st);
  ign_paths_cnt++;
}

static void reload_ignored_folders() {
  unsigned char checkcurr[PSYNC_SHA256_DIGEST_LEN];
  const char *ign, *start, *end, *next;
  char *dir, *home;
  size_t ignlen, dirlen, homelen;

  ign = psync_setting_get_string(_PS(ignorepaths));
  ignlen = strlen(ign);

  psync_sha256((const unsigned char *)ign, ignlen, checkcurr);

  if (!memcmp(ign_checksum, checkcurr, PSYNC_SHA256_DIGEST_LEN) &&
      ign_last_check + 3600 < ptimer_time())
    return;

  memcpy(ign_checksum, checkcurr, PSYNC_SHA256_DIGEST_LEN);
  ign_last_check = ptimer_time();
  ign_paths_cnt = 0;
  next = ign;
  home = NULL;
  homelen = 0;
  while (1) {
    start = next;
    while (isspace(*start))
      start++;
    if (!*start)
      break;
    end = start;
    while (*end && *end != ';' && *end != '\n')
      end++;
    if (*end)
      next = end + 1;
    else
      next = end;
    while (end > start && isspace(*(end - 1)))
      end--;
    dirlen = end - start;
    if (dirlen >= 5 && !memcmp(start, "$HOME", 5)) {
      if (!home) {
        home = ppath_home();
        if (home)
          homelen = strlen(home);
      }
      if (home) {
        dir = (char *)malloc(dirlen + homelen - 4);
        memcpy(dir, home, homelen);
        memcpy(dir + homelen, start + 5, dirlen - 5);
        dir[dirlen + homelen - 5] = 0;
      } else {
        dir = NULL;
      }
    } else {
      dir = (char *)malloc(dirlen + 1);
      memcpy(dir, start, dirlen);
      dir[dirlen] = 0;
    }
    if (dir)
      add_ignored_dir(dir);
    free(dir);
  }
  free(home);
}

static int is_path_to_ignore(uint64_t deviceid, uint64_t inode) {
  uint32_t i;
  for (i = 0; i < ign_paths_cnt; i++) {
    if (ignored_paths[i].deviceid == deviceid &&
        ignored_paths[i].inode == inode) {
      return 1;
    }
  }

  return 0;
}

static void scanner_local_entry_to_list(void *ptr, ppath_stat *st) {
  psync_list *lst;
  sync_folderlist *e;
  size_t l;

  if (is_path_to_ignore(pfile_stat_device_full(&st->stat),
                        pfile_stat_inode(&st->stat))) {
    return;
  }

  lst = (psync_list *)ptr;
  l = strlen(st->name) + 1;
  e = (sync_folderlist *)malloc(offsetof(sync_folderlist, name) + l);
  e->localid = 0;
  e->remoteid = 0;
  e->inode = pfile_stat_inode(&st->stat);
  e->deviceid = pfile_stat_device_full(&st->stat);
  e->mtimenat = pfile_stat_mtime_native(&st->stat);
  e->size = pfile_stat_size(&st->stat);
  e->isfolder = pfile_stat_isfolder(&st->stat);
  memcpy(e->name, st->name, l);
  psync_list_add_tail(lst, &e->list);
}

static int scanner_local_folder_to_list(const char *localpath,
                                        psync_list *lst) {
  psync_list_init(lst);

  return ppath_ls(localpath, scanner_local_entry_to_list, lst);
}

static void delete_local_folder_rec(psync_folderid_t localfolderid);

static void try_delete_localfolder(psync_folderid_t localfolderid) {
  if (psql_tryupgradeLock())
    return;
  psql_start();
  delete_local_folder_rec(localfolderid);
  psql_commit();
}

static void try_delete_localfile(psync_fileid_t localfileid) {
  psync_sql_res *res;
  if (psql_tryupgradeLock())
    return;
  res = psql_prepare("DELETE FROM localfile WHERE id=?");
  psql_bind_uint(res, 1, localfileid);
  psql_run_free(res);
}

static void scanner_db_folder_to_list(psync_syncid_t syncid,
                                      psync_folderid_t localfolderid,
                                      psync_list *lst) {
  psync_sql_res *res;
  psync_variant_row row;
  sync_folderlist *e;
  const char *name;
  size_t namelen;
  psync_list_init(lst);
  res = psql_query_rdlock(
      "SELECT id, folderid, inode, deviceid, mtimenative, name FROM "
      "localfolder WHERE localparentfolderid=? AND syncid=? AND mtimenative IS "
      "NOT NULL");
  psql_bind_uint(res, 1, localfolderid);
  psql_bind_uint(res, 2, syncid);
  while ((row = psql_fetch(res))) {
    name = psync_get_lstring(row[5], &namelen);
    if (unlikely(psync_is_lname_to_ignore(name, namelen))) {
      pdbg_logf(D_NOTICE,
            "found a name %s matching ignore pattern in localfolder, will try "
            "to delete",
            name);
      try_delete_localfolder(psync_get_number(row[0]));
      continue;
    }
    namelen++;
    e = (sync_folderlist *)malloc(offsetof(sync_folderlist, name) +
                                        namelen);
    e->localid = psync_get_number(row[0]);
    e->remoteid = psync_get_number_or_null(row[1]);
    e->inode = psync_get_number(row[2]);
    e->deviceid = psync_get_number(row[3]);
    e->mtimenat = psync_get_number(row[4]);
    e->size = 0;
    e->isfolder = 1;
    memcpy(e->name, name, namelen);
    psync_list_add_tail(lst, &e->list);
  }
  psql_free(res);
  res = psql_query_rdlock(
      "SELECT id, fileid, inode, mtimenative, size, name FROM localfile WHERE "
      "localparentfolderid=? AND syncid=?");
  psql_bind_uint(res, 1, localfolderid);
  psql_bind_uint(res, 2, syncid);
  while ((row = psql_fetch(res))) {
    name = psync_get_lstring(row[5], &namelen);
    if (unlikely(psync_is_lname_to_ignore(name, namelen))) {
      pdbg_logf(D_NOTICE,
            "found a name %s matching ignore pattern in localfile, will try to "
            "delete",
            name);
      try_delete_localfile(psync_get_number(row[0]));
      continue;
    }
    namelen++;
    e = (sync_folderlist *)malloc(offsetof(sync_folderlist, name) +
                                        namelen);
    e->localid = psync_get_number(row[0]);
    e->remoteid = psync_get_number_or_null(row[1]);
    e->inode = psync_get_number(row[2]);
    e->deviceid = 0;
    e->mtimenat = psync_get_number(row[3]);
    e->size = psync_get_number(row[4]);
    e->isfolder = 0;
    memcpy(e->name, name, namelen);
    psync_list_add_tail(lst, &e->list);
  }
  psql_free(res);
}

static int folderlist_cmp(const psync_list *l1, const psync_list *l2) {
  return strcmp(
      psync_list_element(l1, sync_folderlist, list)->name,
      psync_list_element(l2, sync_folderlist, list)->name);
}

static sync_folderlist *copy_folderlist_element(const sync_folderlist *e,
                                                psync_folderid_t folderid,
                                                psync_folderid_t localfolderid,
                                                psync_syncid_t syncid,
                                                psync_synctype_t synctype) {
  sync_folderlist *ret;
  size_t l;
  l = offsetof(sync_folderlist, name) + strlen(e->name) + 1;
  ret = (sync_folderlist *)malloc(l);
  memcpy(ret, e, l);
  ret->localparentfolderid = localfolderid;
  ret->parentfolderid = folderid;
  ret->syncid = syncid;
  ret->synctype = synctype;
  return ret;
}

static void add_element_to_scan_list(unsigned long id, sync_folderlist *e) {
  psync_list_add_tail(&scan_lists[id], &e->list);
  localsleepperfolder = 0;
  changes++;
}

static void add_new_element(const sync_folderlist *e, psync_folderid_t folderid,
                            psync_folderid_t localfolderid,
                            psync_syncid_t syncid, psync_synctype_t synctype,
                            uint64_t deviceid) {
  sync_folderlist *c;
  if (e->isfolder && e->deviceid != deviceid)
    return;
  if (psync_is_name_to_ignore(e->name))
    return;
  if (!psync_is_valid_utf8(e->name)) {
    pdbg_logf(D_WARNING, "ignoring %s with invalid UTF8 name %s",
          e->isfolder ? "folder" : "file", e->name);
    return;
  }
  pdbg_logf(D_NOTICE, "found new %s %s", e->isfolder ? "folder" : "file", e->name);
  c = copy_folderlist_element(e, folderid, localfolderid, syncid, synctype);
  if (e->isfolder)
    add_element_to_scan_list(SCAN_LIST_NEWFOLDERS, c);
  else
    add_element_to_scan_list(SCAN_LIST_NEWFILES, c);
}

static void add_deleted_element(const sync_folderlist *e,
                                psync_folderid_t folderid,
                                psync_folderid_t localfolderid,
                                psync_syncid_t syncid,
                                psync_synctype_t synctype) {
  sync_folderlist *c;
  pdbg_logf(D_NOTICE, "found deleted %s %s", e->isfolder ? "folder" : "file",
        e->name);
  c = copy_folderlist_element(e, folderid, localfolderid, syncid, synctype);

  if (e->isfolder) {
    add_element_to_scan_list(SCAN_LIST_DELFOLDERS, c);
  } else {
    // Send events only for backups, not for other syncs
    if (synctype == 7) {
      psync_send_backup_del_event(c->remoteid);
    }

    add_element_to_scan_list(SCAN_LIST_DELFILES, c);
  }
}

static void
add_modified_file(const sync_folderlist *e, const sync_folderlist *dbe,
                  psync_folderid_t folderid, psync_folderid_t localfolderid,
                  psync_syncid_t syncid, psync_synctype_t synctype) {
  pdbg_logf(D_NOTICE,
        "found modified file %s on disk: size=%llu mtime=%llu inode=%llu in "
        "db: size=%llu mtime=%llu inode=%llu",
        e->name, (long long unsigned)e->size, (long long unsigned)e->mtimenat,
        (long long unsigned)e->inode, (long long unsigned)dbe->size,
        (long long unsigned)dbe->mtimenat, (long long unsigned)dbe->inode);
  add_element_to_scan_list(
      SCAN_LIST_MODFILES,
      copy_folderlist_element(e, folderid, localfolderid, syncid, synctype));
}

static void
scanner_scan_folder(const char *localpath, psync_folderid_t folderid,
                    psync_folderid_t localfolderid, psync_syncid_t syncid,
                    psync_synctype_t synctype, uint64_t deviceid) {
  psync_list disklist, dblist, *ldisk, *ldb;
  sync_folderlist *l, *fdisk, *fdb;
  char *subpath;
  int cmp;
  // pdbg_logf(D_NOTICE, "scanning folder %s deviceid: %llu", localpath, deviceid);
  if (pdbg_unlikely(scanner_local_folder_to_list(localpath, &disklist))) {
    return;
  }

  scanner_db_folder_to_list(syncid, localfolderid, &dblist);

  psync_list_sort(&dblist, folderlist_cmp);
  psync_list_sort(&disklist, folderlist_cmp);

  ldisk = disklist.next;
  ldb = dblist.next;

  while (ldisk != &disklist && ldb != &dblist) {
    fdisk = psync_list_element(ldisk, sync_folderlist, list);
    fdb = psync_list_element(ldb, sync_folderlist, list);
    cmp = strcmp(fdisk->name, fdb->name);
    if (cmp == 0) {
      if (fdisk->isfolder == fdb->isfolder) {
        fdisk->localid = fdb->localid;
        fdisk->remoteid = fdb->remoteid;
        if (!fdisk->isfolder &&
            (fdisk->mtimenat != fdb->mtimenat || fdisk->size != fdb->size ||
             fdisk->inode != fdb->inode))
          add_modified_file(fdisk, fdb, folderid, localfolderid, syncid,
                            synctype);
        if (fdisk->isfolder &&
            pdevice_id_short(fdisk->deviceid) != fdb->deviceid &&
            fdisk->inode != fdb->inode) {
          if (fdisk->deviceid == deviceid) {
            pdbg_logf(D_NOTICE,
                  "deviceid of localfolder %s %lu is different, skipping",
                  fdisk->name, (unsigned long)fdisk->localid);
            fdisk->localid = 0;
          }
        }
      } else {
        add_deleted_element(fdb, folderid, localfolderid, syncid, synctype);
        add_new_element(fdisk, folderid, localfolderid, syncid, synctype,
                        deviceid);
      }
      ldisk = ldisk->next;
      ldb = ldb->next;
    } else if (cmp < 0) { // new element on disk
      add_new_element(fdisk, folderid, localfolderid, syncid, synctype,
                      deviceid);
      ldisk = ldisk->next;
    } else { // deleted element from disk
      add_deleted_element(fdb, folderid, localfolderid, syncid, synctype);
      ldb = ldb->next;
    }
  }

  while (ldisk != &disklist) {
    fdisk = psync_list_element(ldisk, sync_folderlist, list);
    add_new_element(fdisk, folderid, localfolderid, syncid, synctype, deviceid);
    ldisk = ldisk->next;
  }
  while (ldb != &dblist) {
    fdb = psync_list_element(ldb, sync_folderlist, list);
    add_deleted_element(fdb, folderid, localfolderid, syncid, synctype);
    ldb = ldb->next;
  }
  psync_list_for_each_element_call(&dblist, sync_folderlist, list, free);
  if (localsleepperfolder) {
    psys_sleep_milliseconds(localsleepperfolder);
    if (psync_current_time - starttime >=
        PSYNC_LOCALSCAN_SLEEPSEC_PER_SCAN * 3 / 2)
      localsleepperfolder = 0;
  }
  psync_list_for_each_element(l, &disklist, sync_folderlist,
                              list) if (l->isfolder && l->localid &&
                                        l->deviceid == deviceid) {
    subpath = psync_strcat(localpath, "/", l->name, NULL);
    scanner_scan_folder(subpath, l->remoteid, l->localid, syncid, synctype,
                        deviceid);
    free(subpath);
  }

  psync_list_for_each_element_call(&disklist, sync_folderlist, list, free);
}

static int compare_sizeinodemtime(const psync_list *l1, const psync_list *l2) {
  const sync_folderlist *f1, *f2;
  int64_t d;
  f1 = psync_list_element(l1, sync_folderlist, list);
  f2 = psync_list_element(l2, sync_folderlist, list);
  d = f1->size - f2->size;
  if (d < 0)
    return -1;
  else if (d > 0)
    return 1;
  d = f1->inode - f2->inode;
  if (d < 0)
    return -1;
  else if (d > 0)
    return 1;
  d = f1->mtimenat - f2->mtimenat;
  if (d < 0)
    return -1;
  else if (d > 0)
    return 1;
  else
    return 0;
}

static int compare_inode(const psync_list *l1, const psync_list *l2) {
  const sync_folderlist *f1, *f2;
  int64_t d;
  f1 = psync_list_element(l1, sync_folderlist, list);
  f2 = psync_list_element(l2, sync_folderlist, list);
  d = f1->inode - f2->inode;
  if (d < 0)
    return -1;
  else if (d > 0)
    return 1;
  else
    return 0;
}

static void scan_rename_file(sync_folderlist *rnfr, sync_folderlist *rnto) {
  psync_sql_res *res;
  psync_uint_row row;
  psync_folderid_t old_parentfolderid;
  psync_syncid_t old_syncid;
  int filetoupload;
  pdbg_logf(D_NOTICE, "file renamed from %s to %s", rnfr->name, rnto->name);
  res = psql_query_nolock(
      "SELECT syncid, localparentfolderid FROM localfile WHERE id=?");
  psql_bind_uint(res, 1, rnfr->localid);
  if ((row = psql_fetch_int(res))) {
    old_syncid = row[0];
    old_parentfolderid = row[1];
    psql_free(res);
    if (rnto->syncid != old_syncid ||
        rnto->localparentfolderid != old_parentfolderid) {
      res = psql_query_nolock("SELECT 1 FROM task WHERE type=" NTO_STR(
          PSYNC_UPLOAD_FILE) " AND localitemid=?");
      psql_bind_uint(res, 1, rnfr->localid);
      filetoupload = !!psql_fetch_int(res);
      psql_free(res);
    } else {
      filetoupload = 0;
    }
  } else {
    psql_free(res);
    return;
  }
  res = psql_prepare("UPDATE localfile SET localparentfolderid=?, "
                                 "syncid=?, name=? WHERE id=?");
  psql_bind_uint(res, 1, rnto->localparentfolderid);
  psql_bind_uint(res, 2, rnto->syncid);
  psql_bind_str(res, 3, rnto->name);
  psql_bind_uint(res, 4, rnfr->localid);
  psql_run_free(res);
  ptask_rfile_rename(rnfr->syncid, rnto->syncid, rnfr->localid,
                                rnto->localparentfolderid, rnto->name);
  if (filetoupload) {
    ppath_syncfldr_task_added_locked(rnto->syncid,
                                                    rnto->localparentfolderid);
    ppathstatus_syncfldr_task_completed(old_syncid,
                                                 old_parentfolderid);
  }
}

static void scan_upload_file(sync_folderlist *fl) {
  psync_sql_res *res;
  psync_fileid_t localfileid;
  pdbg_logf(D_NOTICE, "file created %s", fl->name);
  /* it is possible that files that are reported as new are already uploading
   * -- is it? when? how? and with what localid?
  pupload_del_tasks(fl->localid);
  */
  res = psql_prepare(
      "INSERT OR IGNORE INTO localfile (localparentfolderid, syncid, size, "
      "inode, mtime, mtimenative, name)"
      "VALUES (?, ?, ?, ?, ?, ?, ?)");
  psql_bind_uint(res, 1, fl->localparentfolderid);
  psql_bind_uint(res, 2, fl->syncid);
  psql_bind_uint(res, 3, fl->size);
  psql_bind_uint(res, 4, fl->inode);
  psql_bind_uint(res, 5, psys_native_to_mtime(fl->mtimenat));
  psql_bind_uint(res, 6, fl->mtimenat);
  psql_bind_str(res, 7, fl->name);
  psql_run_free(res);
  if (pdbg_unlikely(!psql_affected()))
    return;
  localfileid = psql_insertid();
  ptask_upload_q(fl->syncid, localfileid, fl->name);
  ppath_syncfldr_task_added(fl->syncid, fl->localparentfolderid);
}

static void scan_upload_modified_file(sync_folderlist *fl) {
  psync_sql_res *res;
  pdbg_logf(D_NOTICE, "file modified %s (%lu)", fl->name,
        (unsigned long)fl->localid);
  pupload_del_tasks(fl->localid);
  res = psql_prepare("UPDATE localfile SET size=?, inode=?, "
                                 "mtime=?, mtimenative=? WHERE id=?");
  psql_bind_uint(res, 1, fl->size);
  psql_bind_uint(res, 2, fl->inode);
  psql_bind_uint(res, 3, psys_native_to_mtime(fl->mtimenat));
  psql_bind_uint(res, 4, fl->mtimenat);
  psql_bind_uint(res, 5, fl->localid);
  psql_run_free(res);
  ptask_upload_q(fl->syncid, fl->localid, fl->name);
  ppath_syncfldr_task_added(fl->syncid, fl->localparentfolderid);
}

static void scan_delete_file(sync_folderlist *fl) {
  psync_sql_res *res;
  psync_uint_row row;
  psync_fileid_t fileid;
  psync_folderid_t localparentfolderid;
  psync_syncid_t syncid;
  pdbg_logf(D_NOTICE, "file deleted %s", fl->name);
  // it is also possible to use fl->remoteid, but the file might have just been
  // uploaded by the upload thread
  res = psql_query(
      "SELECT fileid, syncid, localparentfolderid FROM localfile WHERE id=?");
  psql_bind_uint(res, 1, fl->localid);
  if (pdbg_likely(row = psql_fetch_int(res))) {
    fileid = row[0];
    syncid = row[1];
    localparentfolderid = row[2];
  } else {
    psql_free(res);
    return;
  }
  psql_free(res);
  pupload_del_tasks(fl->localid);
  res = psql_prepare("DELETE FROM localfile WHERE id=?");
  psql_bind_uint(res, 1, fl->localid);
  psql_run_free(res);
  if (fileid)
    ptask_rfile_rm(fl->syncid, fileid);
  ppathstatus_syncfldr_task_completed(syncid, localparentfolderid);
}

static void scan_create_folder(sync_folderlist *fl) {
  psync_sql_res *res;
  psync_uint_row row;
  psync_folderid_t localfolderid;
  res = psql_prepare(
      "INSERT OR IGNORE INTO localfolder (localparentfolderid, syncid, inode, "
      "deviceid, mtime, mtimenative, flags, name) "
      "VALUES (?, ?, ?, ?, ?, ?, 0, ?)");
  psql_bind_uint(res, 1, fl->localparentfolderid);
  psql_bind_uint(res, 2, fl->syncid);
  psql_bind_uint(res, 3, fl->inode);
  psql_bind_uint(res, 4, pdevice_id_short(fl->deviceid));
  psql_bind_uint(res, 5, psys_native_to_mtime(fl->mtimenat));
  psql_bind_uint(res, 6, fl->mtimenat);
  psql_bind_str(res, 7, fl->name);
  psql_run_free(res);
  /* it is OK to use affected rows after run_free as we are in transaction */
  if (!psql_affected()) {
    res = psql_query("SELECT id FROM localfolder WHERE syncid=? AND "
                          "localparentfolderid=? AND name=?");
    psql_bind_uint(res, 1, fl->syncid);
    psql_bind_uint(res, 2, fl->localparentfolderid);
    psql_bind_str(res, 3, fl->name);
    if ((row = psql_fetch_int(res))) {
      localfolderid = row[0];
      pdbg_logf(D_NOTICE, "folder created %s, exists in localfolder,  localid %lu",
            fl->name, (unsigned long)localfolderid);
    } else
      pdbg_logf(D_NOTICE, "folder created %s, exists in localfolder", fl->name);
    psql_free(res);
    res = psql_prepare(
        "UPDATE localfolder SET inode=?, deviceid=?, mtime=?, mtimenative=?, "
        "flags=0 WHERE syncid=? AND localparentfolderid=? AND name=?");
    psql_bind_uint(res, 1, fl->inode);
    psql_bind_uint(res, 2, pdevice_id_short(fl->deviceid));
    psql_bind_uint(res, 3, psys_native_to_mtime(fl->mtimenat));
    psql_bind_uint(res, 4, fl->mtimenat);
    psql_bind_uint(res, 5, fl->syncid);
    psql_bind_uint(res, 6, fl->localparentfolderid);
    psql_bind_str(res, 7, fl->name);
    psql_run_free(res);
    goto hasfolder;
  }
  localfolderid = psql_insertid();
  pdbg_logf(D_NOTICE, "folder created %s localid %lu", fl->name,
        (unsigned long)localfolderid);
  fl->localid = localfolderid;
  res = psql_prepare("REPLACE INTO syncedfolder (syncid, "
                                 "localfolderid, synctype) VALUES (?, ?, ?)");
  psql_bind_uint(res, 1, fl->syncid);
  psql_bind_uint(res, 2, localfolderid);
  psql_bind_uint(res, 3, fl->synctype);
  psql_run_free(res);
  if (pdbg_unlikely(!psql_affected()))
    return;
  ptask_rdir_mk(fl->syncid, localfolderid, fl->name);
  return;
hasfolder:
  return;
}

static void scan_created_folder(sync_folderlist *fl) {
  char *localpath;
  if (fl->localid == 0) {
    pdbg_logf(D_WARNING, "local folder %s does not have localid", fl->name);
    return;
  }
  localpath = pfolder_lpath_lfldr(fl->localid, fl->syncid, NULL);
  if (pdbg_likely(localpath)) {
    pdbg_logf(D_NOTICE, "scanning just created folder %s localid %lu name %s",
          localpath, (unsigned long)fl->localid, fl->name);
    scanner_scan_folder(localpath, 0, fl->localid, fl->syncid, fl->synctype,
                        fl->deviceid);
    free(localpath);
  }
}

static void update_syncid_rec(psync_folderid_t localfolderid,
                              psync_syncid_t syncid) {
  psync_sql_res *res;
  psync_uint_row row;
  res = psql_prepare("UPDATE localfolder SET syncid=? WHERE id=?");
  psql_bind_uint(res, 1, syncid);
  psql_bind_uint(res, 2, localfolderid);
  psql_run_free(res);
  res = psql_prepare(
      "UPDATE syncedfolder SET syncid=? WHERE localfolderid=?");
  psql_bind_uint(res, 1, syncid);
  psql_bind_uint(res, 2, localfolderid);
  psql_run_free(res);
  res = psql_query_nolock(
      "SELECT id FROM localfolder WHERE localparentfolderid=?");
  psql_bind_uint(res, 1, localfolderid);
  while ((row = psql_fetch_int(res)))
    update_syncid_rec(row[0], syncid);
  psql_free(res);
}

static void scan_rename_folder(sync_folderlist *rnfr, sync_folderlist *rnto) {
  psync_sql_res *res;
  psync_uint_row row;
  //  char *localpath;
  pdbg_logf(D_NOTICE, "folder renamed from %s to %s", rnfr->name, rnto->name);
  res = psql_query_nolock(
      "SELECT syncid, localparentfolderid FROM localfolder WHERE id=?");
  psql_bind_uint(res, 1, rnfr->localid);
  if ((row = psql_fetch_int(res))) {
    ppathstatus_syncfldr_moved(
        rnfr->localid, row[0], row[1], rnto->syncid, rnto->localparentfolderid);
    psql_free(res);
  } else {
    psql_free(res);
    pdbg_logf(D_NOTICE, "localfolderid %u not found in localfolder",
          (unsigned)rnfr->localid);
    // This can prorably happen if we race with a task to delete the folder that
    // comes from the download thread. In any case it is safe not to do anything
    // as we are going to restart the scan anyway
    return;
  }
  res =
      psql_prepare("UPDATE localfolder SET localparentfolderid=?, "
                               "syncid=?, name=? WHERE id=?");
  psql_bind_uint(res, 1, rnto->localparentfolderid);
  psql_bind_uint(res, 2, rnto->syncid);
  psql_bind_str(res, 3, rnto->name);
  psql_bind_uint(res, 4, rnfr->localid);
  psql_run_free(res);
  res = psql_prepare(
      "UPDATE syncedfolder SET syncid=?, synctype=? WHERE localfolderid=?");
  psql_bind_uint(res, 1, rnto->syncid);
  psql_bind_uint(res, 2, rnto->synctype);
  psql_bind_uint(res, 3, rnfr->localid);
  psql_run_free(res);
  if (unlikely(rnfr->syncid != rnto->syncid)) {
    pdbg_logf(D_NOTICE, "folder %s moved from syncid %u to syncid %u", rnfr->name,
          (unsigned)rnfr->syncid, (unsigned)rnto->syncid);
    update_syncid_rec(rnfr->localid, rnto->syncid);
  }
  ptask_rdir_rename(rnfr->syncid, rnto->syncid, rnfr->localid,
                                  rnto->localparentfolderid, rnto->name);
}

static void delete_local_folder_rec(psync_folderid_t localfolderid) {
  psync_sql_res *res;
  psync_uint_row row;
  res =
      psql_query("SELECT id FROM localfolder WHERE localparentfolderid=?");
  psql_bind_uint(res, 1, localfolderid);
  while ((row = psql_fetch_int(res)))
    delete_local_folder_rec(row[0]);
  psql_free(res);
  res = psql_query("SELECT id FROM localfile WHERE localparentfolderid=?");
  psql_bind_uint(res, 1, localfolderid);
  while ((row = psql_fetch_int(res)))
    pupload_del_tasks(row[0]);
  psql_free(res);
  res = psql_prepare(
      "DELETE FROM localfile WHERE localparentfolderid=?");
  psql_bind_uint(res, 1, localfolderid);
  psql_run_free(res);
  res = psql_query("SELECT syncid FROM localfolder WHERE id=?");
  psql_bind_uint(res, 1, localfolderid);
  if (row)
    ppathstatus_syncfldr_deleted(row[0], localfolderid);
  psql_free(res);
  res = psql_prepare("DELETE FROM localfolder WHERE id=?");
  psql_bind_uint(res, 1, localfolderid);
  psql_run_free(res);
  res = psql_prepare(
      "DELETE FROM syncedfolder WHERE localfolderid=?");
  psql_bind_uint(res, 1, localfolderid);
  psql_run_free(res);
}

static void scan_delete_folder(sync_folderlist *fl) {
  psync_sql_res *res;
  psync_uint_row row;
  psync_folderid_t folderid;
  int tries;
  tries = 0;
retry:
  pdbg_logf(D_NOTICE, "folder deleted %s", fl->name);
  res = psql_query("SELECT folderid FROM localfolder WHERE id=?");
  psql_bind_uint(res, 1, fl->localid);
  if (pdbg_likely(row = psql_fetch_int(res)))
    folderid = row[0];
  else {
    psql_free(res);
    return;
  }
  psql_free(res);
  if (pdbg_unlikely(!folderid)) {
    /* folder is not yet created, folderid is not 0 but NULL actually */
    if (tries >= 50) {
      res = psql_query("DELETE FROM task WHERE type=" NTO_STR(
          PSYNC_CREATE_REMOTE_FOLDER) " AND syncid=? AND localitemid=?");
      psql_bind_uint(res, 1, fl->syncid);
      psql_bind_uint(res, 2, fl->localid);
      psql_run_free(res);
    } else {
      psql_commit();
      if (tries == 10)
        ptimer_notify_exception();
      tries++;
      psys_sleep_milliseconds(20 + tries * 20);
      psql_start();
      goto retry;
    }
  }
  delete_local_folder_rec(fl->localid);
  if (folderid)
    ptask_rdir_rm(fl->syncid, folderid);
}

#define check_for_query_cnt()                                                  \
  do {                                                                         \
    if (unlikely(++trn > 1000)) {                                              \
      trn = 0;                                                                 \
      psql_commit();                                          \
      psys_sleep_milliseconds(20);                                                     \
      psql_start();                                           \
    }                                                                          \
  } while (0)

static void scanner_scan(int first) {
  psync_list slist, slist_full_deviceid, newtmp, *l1, *l2;
  sync_folderlist *fl;
  sync_list *l;
  unsigned long i, w, trn, restartsleep;
  int movedfolders;
  if (first)
    localsleepperfolder = 0;
  else {
    i = psql_cellint("SELECT COUNT(*) FROM localfolder", 100);
    if (!i)
      i = 1;
    localsleepperfolder = PSYNC_LOCALSCAN_SLEEPSEC_PER_SCAN * 1000 / i;
    if (localsleepperfolder > 250)
      localsleepperfolder = 250;
    if (localsleepperfolder < 1)
      localsleepperfolder = 1;
  }
  starttime = psync_current_time;
  restartsleep = 1000;

restart:
  pthread_mutex_lock(&scan_mutex);
  while (scan_stoppers)
    pthread_cond_wait(&scan_cond, &scan_mutex);
  restart_scan = 0;
  pthread_mutex_unlock(&scan_mutex);
  if (!pstatus_ok_status_arr(requiredstatuses, ARRAY_SIZE(requiredstatuses)))
    return;
  reload_ignored_folders();
  for (i = 0; i < SCAN_LIST_CNT; i++)
    psync_list_init(&scan_lists[i]);
  scanner_set_syncs_to_list(&slist, &slist_full_deviceid);
  changes = 0;
  movedfolders = 0;

  psync_list_for_each_element(l, &slist, sync_list, list) {
    struct stat st;
    if (unlikely(stat(l->localpath, &st))) {
      pdbg_logf(D_WARNING,
            "could not stat local sync folder %s and will not scan it "
            "(recursively)",
            l->localpath);
      continue;
    }
    if (is_path_to_ignore(pfile_stat_device_full(&st), pfile_stat_inode(&st))) {
      pdbg_logf(D_NOTICE, "not syncing folder %s as it is in ignore list",
            l->localpath);
      continue;
    }
    scanner_scan_folder(l->localpath, l->folderid, 0, l->syncid, l->synctype,
                        pfile_stat_device_full(&st));
  }

  psync_list_for_each_element(l, &slist_full_deviceid, sync_list, list) {
    struct stat st;
    uint64_t deviceid;

    if (unlikely(stat(l->localpath, &st))) {
      pdbg_logf(D_NOTICE,
            "Can't stat sync folder %s. Was it deleted/unmounted while "
            "scanning? Will restart the local scan.",
            l->localpath);
      psync_restart_localscan();
      break;
    } else {
      deviceid = pfile_stat_device_full(&st);
    }
    if (l->deviceid != deviceid) {
      pdbg_logf(D_NOTICE,
            "The deviceid of sync folder '%s' has changed from %llu to %llu "
            "while scanning. Will restart the local scan.",
            l->localpath, (unsigned long long)l->deviceid,
            (unsigned long long)deviceid);
      psync_restart_localscan();
      break;
    }
  }
  psync_list_for_each_element_call(&slist, sync_list, list, free);
  psync_list_for_each_element_call(&slist_full_deviceid, sync_list, list, free);
  w = 0;

  do {
    pthread_mutex_lock(&scan_mutex);
    if (unlikely(restart_scan)) {
      pthread_mutex_unlock(&scan_mutex);
      for (i = 0; i < SCAN_LIST_CNT; i++)
        psync_list_for_each_element_call(&scan_lists[i], sync_folderlist, list, free);
      psys_sleep_milliseconds(restartsleep);
      if (restartsleep < 16000)
        restartsleep *= 2;
      goto restart;
    }
    pthread_mutex_unlock(&scan_mutex);
    pdbg_logf(D_NOTICE, "run checks");
    i = 0;
    psync_list_extract_repeating(
        &scan_lists[SCAN_LIST_DELFOLDERS], &scan_lists[SCAN_LIST_NEWFOLDERS],
        &scan_lists[SCAN_LIST_RENFOLDERSROM],
        &scan_lists[SCAN_LIST_RENFOLDERSTO], compare_inode);
    trn = 0;
    if (!psync_list_isempty(&scan_lists[SCAN_LIST_RENFOLDERSROM]) ||
        !psync_list_isempty(&scan_lists[SCAN_LIST_NEWFOLDERS])) {
      psql_start();
      l2 = &scan_lists[SCAN_LIST_RENFOLDERSTO];
      psync_list_for_each(l1, &scan_lists[SCAN_LIST_RENFOLDERSROM]) {
        l2 = l2->next;
        scan_rename_folder(psync_list_element(l1, sync_folderlist, list),
                           psync_list_element(l2, sync_folderlist, list));
        i++;
        w++;
        check_for_query_cnt();
      }
      psync_list_for_each_element_call(&scan_lists[SCAN_LIST_RENFOLDERSROM], sync_folderlist, list, free);
      psync_list_init(&scan_lists[SCAN_LIST_RENFOLDERSROM]);
      psync_list_for_each_element_call(&scan_lists[SCAN_LIST_RENFOLDERSTO], sync_folderlist, list, free);
      psync_list_init(&scan_lists[SCAN_LIST_RENFOLDERSTO]);
      psync_list_for_each_element(fl, &scan_lists[SCAN_LIST_NEWFOLDERS],
                                  sync_folderlist, list) {
        scan_create_folder(fl);
        i++;
        w++;
        check_for_query_cnt();
      }
      movedfolders = 1;
      psql_commit();
      psync_list_init(&newtmp);
      psync_list_for_each_safe(l1, l2, &scan_lists[SCAN_LIST_NEWFOLDERS]) {
        psync_list_del(l1);
        psync_list_add_tail(&newtmp, l1);
      }
      psync_list_for_each_element_call(&newtmp, sync_folderlist, list,
                                       scan_created_folder);
      psync_list_for_each_element_call(&newtmp, sync_folderlist, list, free);
    }
    if (changes) {
      i++;
      changes = 0;
    }
  } while (i);

  pthread_mutex_lock(&scan_mutex);
  if (unlikely(restart_scan)) {
    pthread_mutex_unlock(&scan_mutex);
    for (i = 0; i < SCAN_LIST_CNT; i++)
      psync_list_for_each_element_call(&scan_lists[i], sync_folderlist, list, free);
    psys_sleep_milliseconds(restartsleep);
    if (restartsleep < 16000)
      restartsleep *= 2;

    goto restart;
  }
  pthread_mutex_unlock(&scan_mutex);
  psync_list_extract_repeating(
      &scan_lists[SCAN_LIST_DELFILES], &scan_lists[SCAN_LIST_NEWFILES],
      &scan_lists[SCAN_LIST_RENFILESFROM], &scan_lists[SCAN_LIST_RENFILESTO],
      compare_sizeinodemtime);
  l2 = &scan_lists[SCAN_LIST_RENFILESTO];
  trn = 0;

  psql_start();
  psync_list_for_each(l1, &scan_lists[SCAN_LIST_RENFILESFROM]) {
    l2 = l2->next;
    scan_rename_file(psync_list_element(l1, sync_folderlist, list),
                     psync_list_element(l2, sync_folderlist, list));
    w++;
    check_for_query_cnt();
  }
  psync_list_for_each_element(fl, &scan_lists[SCAN_LIST_NEWFILES],
                              sync_folderlist, list) {
    scan_upload_file(fl);
    w++;
    check_for_query_cnt();
  }
  psync_list_for_each_element(fl, &scan_lists[SCAN_LIST_MODFILES],
                              sync_folderlist, list) {
    scan_upload_modified_file(fl);
    w++;
    check_for_query_cnt();
  }
  psync_list_for_each_element(fl, &scan_lists[SCAN_LIST_DELFILES],
                              sync_folderlist, list) {
    scan_delete_file(fl);
    w++;
    check_for_query_cnt();
  }
  psync_list_for_each_element(fl, &scan_lists[SCAN_LIST_DELFOLDERS],
                              sync_folderlist, list) {
    scan_delete_folder(fl);
    w++;
    check_for_query_cnt();
  }
  ppathstatus_clear_sync_cache();

  psql_commit();

  if (w) {
    pupload_wake();
    pstatus_upload_recalc_async();
  }
  for (i = 0; i < SCAN_LIST_CNT; i++)
    psync_list_for_each_element_call(&scan_lists[i], sync_folderlist, list, free);
  if (movedfolders) {
    starttime = psync_current_time;
    restartsleep = 1000;
    goto restart;
  }
}

static int scanner_wait() {
  struct timespec tm;
  int ret;
  if (localnotify == 0)
    tm.tv_sec = psync_current_time + PSYNC_LOCALSCAN_RESCAN_NOTIFY_SUPPORTED;
  else
    tm.tv_sec = psync_current_time + PSYNC_LOCALSCAN_RESCAN_INTERVAL;
  tm.tv_nsec = 0;
  pthread_mutex_lock(&scan_mutex);
  if (!scan_wakes)
    ret = !pthread_cond_timedwait(&scan_cond, &scan_mutex, &tm);
  else
    ret = 1;
  scan_wakes = 0;
  pthread_mutex_unlock(&scan_mutex);
  return ret;
}

static void scanner_thread() {
  time_t lastscan;
  int w;
  psys_sleep_milliseconds(1500);
  pstatus_wait_statuses_arr(requiredstatuses, ARRAY_SIZE(requiredstatuses));
  pstatus_wait(PSTATUS_TYPE_RUN, PSTATUS_RUN_RUN | PSTATUS_RUN_PAUSE);
  scanner_scan(1);
  pstatus_set(PSTATUS_TYPE_LOCALSCAN, PSTATUS_LOCALSCAN_READY);
  scanner_wait();
  w = 0;
  lastscan = 0;
  while (psync_do_run) {
    pstatus_wait_statuses_arr(requiredstatuses, ARRAY_SIZE(requiredstatuses));
    if (lastscan + 5 >= psync_current_time) {
      psys_sleep_milliseconds(2000);
      pthread_mutex_lock(&scan_mutex);
      scan_wakes = 0;
      pthread_mutex_unlock(&scan_mutex);
    }
    lastscan = psync_current_time;
    scanner_scan(w);
    w = scanner_wait();
  }
}

static void psync_do_wake_localscan() {
  localsleepperfolder = 0;
  pthread_mutex_lock(&scan_mutex);
  if (!scan_wakes++)
    pthread_cond_signal(&scan_cond);
  pthread_mutex_unlock(&scan_mutex);
  localsleepperfolder = 0;
}

void psync_wake_localscan() {
  prun_throttled("wake localscan", psync_do_wake_localscan,
                        PSYNC_LOCALSCAN_MIN_INTERVAL, 0);
}

void psync_restart_localscan() {
  pthread_mutex_lock(&scan_mutex);
  restart_scan = 1;
  pthread_mutex_unlock(&scan_mutex);
}

void psync_stop_localscan() {
  pthread_mutex_lock(&scan_mutex);
  restart_scan = 1;
  scan_stoppers++;
  pthread_mutex_unlock(&scan_mutex);
}

void psync_resume_localscan() {
  pthread_mutex_lock(&scan_mutex);
  scan_stoppers--;
  if (!scan_stoppers)
    pthread_cond_signal(&scan_cond);
  pthread_mutex_unlock(&scan_mutex);
}

static void psync_wake_localscan_noscan() {
  pthread_mutex_lock(&scan_mutex);
  pthread_cond_signal(&scan_cond);
  pthread_mutex_unlock(&scan_mutex);
}

void psync_restat_sync_folders_add(psync_syncid_t syncid,
                                   const char *localpath) {
  sync_restat_list *l;
  struct stat st;
  size_t lplen = strlen(localpath);
  l = (sync_restat_list *)malloc(offsetof(sync_restat_list, localpath) +
                                       lplen + 1);
  l->syncid = syncid;
  memcpy(l->localpath, localpath, lplen + 1);
  if (stat(l->localpath, &st)) {
    pdbg_logf(D_NOTICE,
          "Can't stat sync folder '%s'. Putting zeros for inode and deviceid",
          l->localpath);
    l->inode = 0;
    l->deviceid = 0;
  } else {
    l->inode = pfile_stat_inode(&st);
    l->deviceid = pfile_stat_device_full(&st);
  }
  pthread_mutex_lock(&restat_mutex);
  psync_list_add_tail(&scan_folders_list, &l->list);
  pthread_mutex_unlock(&restat_mutex);
}

void psync_restat_sync_folders_del(psync_syncid_t syncid) {
  sync_restat_list *l, *to_del = NULL;
  pthread_mutex_lock(&restat_mutex);
  psync_list_for_each_element(l, &scan_folders_list, sync_restat_list, list) {
    if (l->syncid == syncid) {
      to_del = l;
      break;
    }
  }
  if (to_del) {
    psync_list_del(&to_del->list);
    free(to_del);
  }
  pthread_mutex_unlock(&restat_mutex);
}

void psync_restat_sync_folders() {
  sync_restat_list *l;
  int has_changes = 0;
  struct stat st;
  uint64_t deviceid;
  uint64_t inode;
  pthread_mutex_lock(&restat_mutex);
  psync_list_for_each_element(l, &scan_folders_list, sync_restat_list, list) {
    if (stat(l->localpath, &st)) {
      pdbg_logf(D_NOTICE,
            "Can't stat sync folder '%s'. Setting deviceid and inode to zero.",
            l->localpath);
      deviceid = 0;
      inode = 0;
    } else {
      deviceid = pfile_stat_device_full(&st);
      inode = pfile_stat_inode(&st);
    }
    if (l->deviceid != deviceid || l->inode != inode) {
      l->deviceid = deviceid;
      l->inode = inode;
      psync_localnotify_del_sync(l->syncid);
      if (l->deviceid)
        psync_localnotify_add_sync(l->syncid);
      has_changes = 1;
    }
  }
  pthread_mutex_unlock(&restat_mutex);
  if (has_changes)
    psync_wake_localscan();
}

void psync_localscan_init() {
  psync_sql_res *res;
  psync_variant_row row;
  const char *localpath;
  psync_syncid_t syncid;
  psync_list_init(&scan_folders_list);
  ptimer_exception_handler(psync_wake_localscan_noscan);
  prun_thread("localscan", scanner_thread);
  localnotify = psync_localnotify_init();
  res = psql_query_rdlock(
      "SELECT id, localpath FROM syncfolder WHERE synctype&" NTO_STR(
          PSYNC_UPLOAD_ONLY) "=" NTO_STR(PSYNC_UPLOAD_ONLY));
  while ((row = psql_fetch(res))) {
    syncid = psync_get_number(row[0]);
    localpath = psync_get_string(row[1]);
    psync_localnotify_add_sync(syncid);
    psync_restat_sync_folders_add(syncid, localpath);
  }
  psql_free(res);
}
