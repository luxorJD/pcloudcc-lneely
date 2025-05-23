#include <dirent.h>
#include <pwd.h>
#include <stddef.h>

#include <sys/statvfs.h>

#include "pcompiler.h"
#include "pfile.h"
#include "plibs.h"
#include "ppath.h"
#include "psettings.h"
#include "psql.h"

char *ppath_default_db() {
  char *pcdir, *dbp, *home, *oldp;
  struct stat st;

  pcdir = ppath_pcloud();
  if (!pcdir) {
    return NULL;
  }

  dbp = psync_strcat(pcdir, "/", PSYNC_DEFAULT_DB_NAME, NULL);
  free(pcdir);

  if (stat(dbp, &st)) {
    // Inline the old database path function here
    if ((home = ppath_home())) {
      oldp = psync_strcat(home, "/", PSYNC_DEFAULT_POSIX_DBNAME, NULL);
      free(home);

      if (oldp) {
        if (!stat(oldp, &st)) {
          if (psql_reopen(oldp)) {
            free(dbp);
            return oldp;
          } else {
            pfile_rename(oldp, dbp);
          }
        }
        free(oldp);
      }
    }
  }

  return dbp;
}

int64_t ppath_free_space(const char *path) {
  struct statvfs buf;
  if (pdbg_unlikely(statvfs(path, &buf)))
    return -1;
  else
    return (int64_t)buf.f_bavail * (int64_t)buf.f_frsize;
}

char *ppath_home() {
  struct stat st;
  const char *dir;
  dir = getenv("HOME");
  if (pdbg_unlikely(!dir) || pdbg_unlikely(stat(dir, &st)) ||
      pdbg_unlikely(!pfile_stat_mode_ok(&st, 7))) {
    struct passwd pwd;
    struct passwd *result;
    char buff[4096];
    if (pdbg_unlikely(getpwuid_r(getuid(), &pwd, buff, sizeof(buff), &result)) ||
        pdbg_unlikely(stat(result->pw_dir, &st)) ||
        pdbg_unlikely(!pfile_stat_mode_ok(&st, 7)))
      return NULL;
    dir = result->pw_dir;
  }
  return psync_strdup(dir);
}

int ppath_ls(const char *path, ppath_ls_cb callback, void *ptr) {
  ppath_stat pst;
  DIR *dh;
  char *cpath;
  size_t pl;
  struct dirent *de;

  dh = opendir(path);
  if (unlikely(!dh)) {
    pdbg_logf(D_WARNING, "could not open directory %s", path);
    psync_error = PERROR_LOCAL_FOLDER_NOT_FOUND;
    return -1;
  }

  pl = strlen(path);
  cpath = (char *)malloc(pl + NAME_MAX + 2);
  memcpy(cpath, path, pl);
  if (!pl || cpath[pl - 1] != '/')
    cpath[pl++] = '/';

  pst.path = cpath;

  while ((de = readdir(dh))) {
    // Skip . and .. entries
    if (de->d_name[0] == '.' &&
        (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
      continue;

    psync_strlcpy(cpath + pl, de->d_name, NAME_MAX + 1);

    if (pdbg_likely(!lstat(cpath, &pst.stat)) &&
        (S_ISREG(pst.stat.st_mode) || S_ISDIR(pst.stat.st_mode))) {
      pst.name = de->d_name;
      callback(ptr, &pst);
    }
  }

  free(cpath);
  closedir(dh);
  return 0;
}

int ppath_ls_fast(const char *path, ppath_ls_fast_cb callback, void *ptr) {
  ppath_fast_stat pst;
  struct stat st;
  DIR *dh;
  char *cpath;
  size_t pl;
  struct dirent *de;

  dh = opendir(path);
  if (pdbg_unlikely(!dh)) {
    psync_error = PERROR_LOCAL_FOLDER_NOT_FOUND;
    return -1;
  }

  pl = strlen(path);
  cpath = (char *)malloc(pl + NAME_MAX + 2);
  memcpy(cpath, path, pl);
  if (!pl || cpath[pl - 1] != '/')
    cpath[pl++] = '/';

  while ((de = readdir(dh))) {
    // Skip . and .. entries
    if (de->d_name[0] == '.' &&
        (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
      continue;

    pst.name = de->d_name;

    if (de->d_type == DT_DIR) {
      pst.isfolder = 1;
      callback(ptr, &pst);
    } else if (de->d_type == DT_REG) {
      pst.isfolder = 0;
      callback(ptr, &pst);
    } else if (de->d_type == DT_UNKNOWN) {
      // Fall back to lstat for unknown file types
      psync_strlcpy(cpath + pl, de->d_name, NAME_MAX + 1);
      if (!pdbg_unlikely(lstat(cpath, &st))) {
        pst.isfolder = S_ISDIR(st.st_mode);
        callback(ptr, &pst);
      }
    }
    // Ignore other file types
  }

  free(cpath);
  closedir(dh);
  return 0;
}

char *ppath_pcloud() {
  char *homedir, *path;
  struct stat st;

  if (!(homedir = ppath_home())) {
    return NULL;
  }

  path = psync_strcat(homedir, "/", PSYNC_DEFAULT_POSIX_DIR, NULL);
  free(homedir);
  if (pdbg_unlikely(!path)) {
    return NULL;
  }

  if (stat(path, &st) &&
      pdbg_unlikely(mkdir(path, PSYNC_DEFAULT_POSIX_FOLDER_MODE))) {
    free(path);
    return NULL;
  }
  return path;
}

char *ppath_private(char *name) {
  char *path, *rpath;
  struct stat st;
  path = ppath_pcloud();
  if (!path)
    return NULL;
  rpath = psync_strcat(path, "/", name, NULL);
  if (stat(rpath, &st) && mkdir(path, PSYNC_DEFAULT_POSIX_FOLDER_MODE)) {
    free(rpath);
    free(path);
    return NULL;
  }
  free(path);
  return rpath;
}

char *ppath_private_tmp() { return ppath_private(PSYNC_DEFAULT_TMP_DIR); }
