/*
   Copyright (c) 2013 Anton Titov.

   Copyright (c) 2013 pCloud Ltd.  All rights reserved.

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

#include "pfs.h"
#include "plibs.h"
#include "plocalscan.h"
#include "pp2p.h"
#include "ppagecache.h"
#include "ppath.h"
#include "psettings.h"
#include "psql.h"
#include "ptimer.h"

typedef void (*setting_callback)();
typedef void (*filter_callback)(void *);

typedef struct {
  const char *name;
  setting_callback change_callback;
  filter_callback fix_callback;
  union {
    int64_t snum;
    uint64_t num;
    char *str;
    int boolean;
  };
  unsigned long type;
} psync_setting_t;

static void lower_patterns(void *ptr);

static void fsroot_change() { psync_fs_remount(); }

#define PSYNC_SETTING_owneremail 16

// unused but may be important later
// #define PSYNC_SETTING_cryptosetup      17

static psync_setting_t settings[] = {
    {"usessl",
     ptimer_do_notify_exception,
     NULL,
     {PSYNC_USE_SSL_DEFAULT},
     PSYNC_TBOOL},
    {"saveauth", NULL, NULL, {1}, PSYNC_TBOOL},
    {"maxdownloadspeed", NULL, NULL, {PSYNC_DWL_SHAPER_DEFAULT}, PSYNC_TNUMBER},
    {"maxuploadspeed", NULL, NULL, {PSYNC_UPL_SHAPER_DEFAULT}, PSYNC_TNUMBER},
    {"ignorepatterns", NULL, lower_patterns, {0}, PSYNC_TSTRING},
    {"minlocalfreespace",
     NULL,
     NULL,
     {PSYNC_MIN_LOCAL_FREE_SPACE},
     PSYNC_TNUMBER},
    {"p2psync", pp2p_change, NULL, {PSYNC_P2P_SYNC_DEFAULT}, PSYNC_TBOOL},
    {"fsroot", fsroot_change, NULL, {0}, PSYNC_TSTRING},
    {"autostartfs", NULL, NULL, {PSYNC_AUTOSTARTFS_DEFAULT}, PSYNC_TBOOL},
    {"fscachesize",
     ppagecache_resize,
     NULL,
     {PSYNC_FS_DEFAULT_CACHE_SIZE},
     PSYNC_TNUMBER},
    {"fscachepath", NULL, NULL, {0}, PSYNC_TSTRING},
    {"sleepstopcrypto",
     NULL,
     NULL,
     {PSYNC_CRYPTO_DEFAULT_STOP_ON_SLEEP},
     PSYNC_TBOOL},
    {"companyname", NULL, NULL, {0}, PSYNC_TSTRING},
    {"owneruserid", NULL, NULL, {PSYNC_BACC_OWNERUSERID}, PSYNC_TNUMBER},
    {"ownerfirstname", NULL, NULL, {0}, PSYNC_TSTRING},
    {"ownerlastname", NULL, NULL, {0}, PSYNC_TSTRING},
    {"owneremail", NULL, NULL, {0}, PSYNC_TSTRING},
    {"owner_cryptosetup",
     NULL,
     NULL,
     {PSYNC_BACC_OWNER_CRYPTOSETUP},
     PSYNC_TBOOL},
    {"cryptov2isactive", NULL, NULL, {PSYNC_BACC_V2}, PSYNC_TBOOL},
    {"hasactivesubscription", NULL, NULL, {0}, PSYNC_TBOOL},
    {"api_server", NULL, NULL, {0}, PSYNC_TSTRING},
    {"location_id", NULL, NULL, {PSYNC_LOCATIONID_DEFAULT}, PSYNC_TNUMBER},
    {"ignorepaths", psync_wake_localscan, NULL, {0}, PSYNC_TSTRING}};

void psync_settings_reset() {
  char *home, *defaultfs, *defaultcache;
  psync_settingid_t i;
  home = ppath_home();
  defaultfs = psync_strcat(home, "/",
                           PSYNC_DEFAULT_FS_FOLDER, NULL);
  free(home);
  home = ppath_pcloud();
  defaultcache = psync_strcat(home, "/",
                              PSYNC_DEFAULT_CACHE_FOLDER, NULL);
  free(home);
  for (i = 0; i < ARRAY_SIZE(settings); i++)
    if (settings[i].type == PSYNC_TSTRING)
      psync_free_after_sec(settings[i].str, 60);
  settings[_PS(usessl)].boolean = PSYNC_USE_SSL_DEFAULT;
  settings[_PS(saveauth)].boolean = 1;
  settings[_PS(maxdownloadspeed)].snum = PSYNC_DWL_SHAPER_DEFAULT;
  settings[_PS(maxuploadspeed)].snum = PSYNC_UPL_SHAPER_DEFAULT;
  settings[_PS(ignorepatterns)].str = PSYNC_IGNORE_PATTERNS_DEFAULT;
  settings[_PS(minlocalfreespace)].num = PSYNC_MIN_LOCAL_FREE_SPACE;
  settings[_PS(p2psync)].boolean = PSYNC_P2P_SYNC_DEFAULT;
  settings[_PS(fsroot)].str = defaultfs;
  settings[_PS(fscachesize)].num = PSYNC_FS_DEFAULT_CACHE_SIZE;
  settings[_PS(fscachepath)].str = defaultcache;
  settings[_PS(sleepstopcrypto)].num = PSYNC_CRYPTO_DEFAULT_STOP_ON_SLEEP;
  settings[_PS(companyname)].str = PSYNC_BACC_COMPANYNAME;
  settings[_PS(owneruserid)].num = PSYNC_BACC_OWNERUSERID;
  settings[_PS(ownerfirstname)].str = PSYNC_BACC_OWNERFIRSTNAME;
  settings[_PS(ownerlastname)].str = PSYNC_BACC_OWNERLASTNAME;
  settings[_PS(owneremail)].str = PSYNC_BACC_OWNEREMAIL;
  settings[_PS(owner_cryptosetup)].num = PSYNC_BACC_OWNER_CRYPTOSETUP;
  settings[_PS(cryptov2isactive)].num = PSYNC_BACC_V2;
  settings[_PS(hasactivesubscription)].boolean = 0;
  settings[_PS(api_server)].str = PSYNC_API_HOST;
  settings[_PS(location_id)].num = PSYNC_LOCATIONID_DEFAULT;
  settings[_PS(ignorepaths)].str = PSYNC_IGNORE_PATHS_DEFAULT;
  for (i = 0; i < ARRAY_SIZE(settings); i++) {
    if (settings[i].type == PSYNC_TSTRING) {
      settings[i].str = psync_strdup(settings[i].str);
      if (settings[i].fix_callback)
        settings[i].fix_callback(&settings[i].str);
    } else if (settings[i].fix_callback) {
      if (settings[i].type == PSYNC_TNUMBER)
        settings[i].fix_callback(&settings[i].num);
      else if (settings[i].type == PSYNC_TBOOL)
        settings[i].fix_callback(&settings[i].boolean);
    }
  }
  free(defaultfs);
  free(defaultcache);
}

void psync_settings_init() {
  psync_sql_res *res;
  psync_str_row row;
  const char *name;
  char *home, *defaultfs, *defaultcache;
  psync_settingid_t i;
  home = ppath_home();
  defaultfs = psync_strcat(home, "/",
                           PSYNC_DEFAULT_FS_FOLDER, NULL);
  free(home);
  home = ppath_pcloud();
  defaultcache = psync_strcat(home, "/",
                              PSYNC_DEFAULT_CACHE_FOLDER, NULL);
  free(home);
  settings[_PS(ignorepatterns)].str = PSYNC_IGNORE_PATTERNS_DEFAULT;
  settings[_PS(fsroot)].str = defaultfs;
  settings[_PS(fscachepath)].str = defaultcache;
  settings[_PS(companyname)].str = PSYNC_BACC_COMPANYNAME;
  settings[_PS(owneruserid)].num = PSYNC_BACC_OWNERUSERID;
  settings[_PS(ownerfirstname)].str = PSYNC_BACC_OWNERFIRSTNAME;
  settings[_PS(ownerlastname)].str = PSYNC_BACC_OWNERLASTNAME;
  settings[_PS(owneremail)].str = PSYNC_BACC_OWNEREMAIL;
  settings[_PS(owner_cryptosetup)].num = PSYNC_BACC_OWNER_CRYPTOSETUP;
  settings[_PS(cryptov2isactive)].num = PSYNC_BACC_V2;
  settings[_PS(hasactivesubscription)].boolean = 0;
  settings[_PS(api_server)].str = PSYNC_API_HOST;
  settings[_PS(location_id)].num = PSYNC_LOCATIONID_DEFAULT;
  settings[_PS(ignorepaths)].str = PSYNC_IGNORE_PATHS_DEFAULT;
  for (i = 0; i < ARRAY_SIZE(settings); i++) {
    if (settings[i].type == PSYNC_TSTRING) {
      settings[i].str = psync_strdup(settings[i].str);
      if (settings[i].fix_callback)
        settings[i].fix_callback(&settings[i].str);
    } else if (settings[i].fix_callback) {
      if (settings[i].type == PSYNC_TNUMBER)
        settings[i].fix_callback(&settings[i].num);
      else if (settings[i].type == PSYNC_TBOOL)
        settings[i].fix_callback(&settings[i].boolean);
    }
  }
  free(defaultfs);
  free(defaultcache);
  res = psql_query("SELECT id, value FROM setting");
  while ((row = psql_fetch_str(res))) {
    name = row[0];
    for (i = 0; i < ARRAY_SIZE(settings); i++)
      if (!strcmp(name, settings[i].name)) {
        if (settings[i].type == PSYNC_TSTRING) {
          free(settings[i].str);
          settings[i].str = psync_strdup(row[1]);
          if (settings[i].fix_callback)
            settings[i].fix_callback(&settings[i].str);
        } else if (settings[i].type == PSYNC_TNUMBER) {
          settings[i].num = atoll(row[1]);
          if (settings[i].fix_callback)
            settings[i].fix_callback(&settings[i].num);
        } else if (settings[i].type == PSYNC_TBOOL) {
          settings[i].boolean = atoll(row[1]) ? 1 : 0;
          if (settings[i].fix_callback)
            settings[i].fix_callback(&settings[i].boolean);
        } else
          pdbg_logf(D_BUG,
                "bad setting type for settingid %d (%s) expected %lu", i,
                name, settings[i].type);
      }
  }
  psql_free(res);
}

psync_settingid_t psync_setting_getid(const char *name) {
  psync_settingid_t i;
  for (i = 0; i < ARRAY_SIZE(settings); i++)
    if (!strcmp(name, settings[i].name))
      return i;
  pdbg_logf(D_BUG, "setting witn name %s not found", name);
  return PSYNC_INVALID_SETTINGID;
}

#define CHECK_SETTINGID_AND_TYPE(ret, stype)                                   \
  do {                                                                         \
    if (unlikely(settingid < 0 || settingid >= ARRAY_SIZE(settings))) {        \
      pdbg_logf(D_BUG, "invalid settingid %d", settingid);                         \
      return ret;                                                              \
    }                                                                          \
    if (unlikely(settings[settingid].type != stype)) {                         \
      pdbg_logf(D_BUG, "invalid setting type requested for settingid %d (%s)",     \
            settingid, settings[settingid].name);                              \
      return ret;                                                              \
    }                                                                          \
  } while (0)

int psync_setting_get_bool(psync_settingid_t settingid) {
  CHECK_SETTINGID_AND_TYPE(0, PSYNC_TBOOL);
  return settings[settingid].boolean;
}

int psync_setting_set_bool(psync_settingid_t settingid, int value) {
  psync_sql_res *res;
  CHECK_SETTINGID_AND_TYPE(-1, PSYNC_TBOOL);
  if (value)
    value = 1;
  else
    value = 0;
  if (settings[settingid].fix_callback)
    settings[settingid].fix_callback(&value);
  settings[settingid].boolean = value;
  res = psql_prepare(
      "REPLACE INTO setting (id, value) VALUES (?, ?)");
  psql_bind_str(res, 1, settings[settingid].name);
  psql_bind_uint(res, 2, value);
  psql_run_free(res);
  if (settings[settingid].change_callback)
    settings[settingid].change_callback();
  return 0;
}

uint64_t psync_setting_get_uint(psync_settingid_t settingid) {
  CHECK_SETTINGID_AND_TYPE(0, PSYNC_TNUMBER);
  return settings[settingid].num;
}

int psync_setting_set_uint(psync_settingid_t settingid, uint64_t value) {
  psync_sql_res *res;
  CHECK_SETTINGID_AND_TYPE(-1, PSYNC_TNUMBER);
  if (settings[settingid].fix_callback)
    settings[settingid].fix_callback(&value);
  settings[settingid].num = value;
  res = psql_prepare(
      "REPLACE INTO setting (id, value) VALUES (?, ?)");
  psql_bind_str(res, 1, settings[settingid].name);
  psql_bind_uint(res, 2, value);
  psql_run_free(res);
  if (settings[settingid].change_callback)
    settings[settingid].change_callback();
  return 0;
}

int64_t psync_setting_get_int(psync_settingid_t settingid) {
  CHECK_SETTINGID_AND_TYPE(0, PSYNC_TNUMBER);
  return settings[settingid].snum;
}

int psync_setting_set_int(psync_settingid_t settingid, int64_t value) {
  psync_sql_res *res;
  CHECK_SETTINGID_AND_TYPE(-1, PSYNC_TNUMBER);
  if (settings[settingid].fix_callback)
    settings[settingid].fix_callback(&value);
  settings[settingid].snum = value;
  res = psql_prepare(
      "REPLACE INTO setting (id, value) VALUES (?, ?)");
  psql_bind_str(res, 1, settings[settingid].name);
  psql_bind_int(res, 2, value);
  psql_run_free(res);
  if (settings[settingid].change_callback)
    settings[settingid].change_callback();
  return 0;
}

const char *psync_setting_get_string(psync_settingid_t settingid) {
  CHECK_SETTINGID_AND_TYPE("", PSYNC_TSTRING);
  return settings[settingid].str;
}

int psync_setting_set_string(psync_settingid_t settingid, const char *value) {
  psync_sql_res *res;
  char *oldval, *newval;
  CHECK_SETTINGID_AND_TYPE(-1, PSYNC_TSTRING);
  oldval = settings[settingid].str;
  newval = psync_strdup(value);
  if (settings[settingid].fix_callback)
    settings[settingid].fix_callback(&newval);
  settings[settingid].str = newval;
  res = psql_prepare(
      "REPLACE INTO setting (id, value) VALUES (?, ?)");
  psql_bind_str(res, 1, settings[settingid].name);
  psql_bind_str(res, 2, value);
  psql_run_free(res);
  if (settings[settingid].change_callback)
    settings[settingid].change_callback();
  psync_free_after_sec(oldval, 600);
  return 0;
}

int psync_setting_reset(psync_settingid_t settingid) {
  switch (settingid) {
  case _PS(ignorepatterns):
    return psync_setting_set_string(_PS(ignorepatterns),
                                    PSYNC_IGNORE_PATTERNS_DEFAULT);
  case _PS(ignorepaths):
    return psync_setting_set_string(_PS(ignorepaths),
                                    PSYNC_IGNORE_PATHS_DEFAULT);
  default:
    return -1;
  }
  return 0;
}

static void lower_patterns(void *ptr) {
  unsigned char *str;
  str = *((unsigned char **)ptr);
  while (*str) {
    *str = tolower(*str);
    str++;
  }
}
