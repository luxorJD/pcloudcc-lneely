/*
  Copyright (c) 2013-2015 pCloud Ltd.  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met: Redistributions of source code must retain the above
  copyright notice, this list of conditions and the following
  disclaimer.  Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided with
  the distribution.  Neither the name of pCloud Ltd nor the names of
  its contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "pbusinessaccount.h"
#include "papi.h"
#include "pfoldersync.h"
#include "plibs.h"
#include "pnetlibs.h"
#include "psys.h"
#include "psql.h"

#include <stdio.h>

typedef struct _email_vis_params {
  char **email;
  size_t *length;
} email_visitor_params;

typedef struct _team_vis_params {
  char **name;
  size_t *length;
} team_visitor_params;

#define FOLDERID_ENTRY_SIZE 18
#define INVALID_SHAREDID_RESULT 2025

static void init_param_str(binparam *t, const char *name, const char *val) {
  //{PARAM_STR, strlen(name), strlen(val), (name),
  //{(uint64_t)((uintptr_t)(val))}}
  t->paramtype = PARAM_STR;
  t->paramnamelen = strlen(name);
  t->opts = strlen(val);
  t->paramname = name;
  t->str = val;
}
/*
static void init_param_num(binparam* t, const char *name, uint64_t val) {
  //{PARAM_NUM, strlen(name), 0, (name), {(val)}}
  t->paramtype  = PARAM_NUM;
  t->paramnamelen = strlen(name);
  t->opts = 0;
  t->paramname = name;
  t->num = val;
}*/

static int handle_result(const binresult *bres, uint64_t result, char **err) {
  const char *errorret = 0;
  errorret = papi_find_result2(bres, "error", PARAM_STR)->str;
  if (strlen(errorret) == 0)
    errorret = papi_find_result2(bres, "message", PARAM_STR)->str;

  *err = psync_strndup(errorret, strlen(errorret));
  pdbg_logf(D_WARNING, "command gettreepublink returned error code %u message %s",
        (unsigned)result, errorret);
  psync_process_api_error(result);
  if (psync_handle_api_result(result) == PSYNC_NET_TEMPFAIL)
    return -result;
  else {
    *err = psync_strndup("Connection error.", 17);
    return -1;
  }
}

int do_psync_account_stopshare(psync_shareid_t usershareids[], int nusershareid,
                               psync_shareid_t teamshareids[], int nteamshareid,
                               char **err) {
  psock_t *api;
  binresult *bres;
  uint64_t result, userresult, teamresult;
  char *ids1 = NULL;
  char *ids2 = NULL;
  char *idsp = 0;
  int i, pind = 1, numparam = 1, k;
  binparam *t;
  const binresult *userres, *teamres, *statres;
  *err = 0;

  numparam += !!nusershareid + !!nteamshareid;
  if (unlikely(numparam == 1))
    return -3;

  t = (binparam *)malloc(numparam * sizeof(binparam));

  init_param_str(t, "auth", psync_my_auth);

  if (nusershareid) {
    ids1 = (char *)malloc(nusershareid * FOLDERID_ENTRY_SIZE);
    idsp = ids1;
    for (i = 0; i < nusershareid; ++i) {
      k = sprintf(idsp, "%lld", (long long)usershareids[i]);
      if (unlikely(k <= 0))
        break;
      idsp[k] = ',';
      idsp = idsp + k + 1;
    }
    if (i > 0)
      *(idsp - 1) = '\0';
    // pdbg_logf(D_NOTICE, "usershareids %s",ids1);
    init_param_str(t + pind++, "usershareids", ids1);
  }

  if (nteamshareid) {
    ids2 = (char *)malloc(nteamshareid * FOLDERID_ENTRY_SIZE);
    idsp = ids2;
    for (i = 0; i < nteamshareid; ++i) {
      k = sprintf(idsp, "%lld", (long long)teamshareids[i]);
      if (unlikely(k <= 0))
        break;
      idsp[k] = ',';
      idsp = idsp + k + 1;
    }
    if (i > 0)
      *(idsp - 1) = '\0';
    // pdbg_logf(D_NOTICE, "teamshareids %s",ids2);
    init_param_str(t + pind++, "teamshareids", ids2);
  }

  api = psync_apipool_get();
  if (unlikely(!api)) {
    pdbg_logf(D_WARNING, "Can't gat api from the pool. No pool ?\n");
    return -2;
  }

  bres = papi_send(api, "account_stopshare",
                         sizeof("account_stopshare") - 1, t, pind, -1, 1);

  if (likely(bres))
    psync_apipool_release(api);
  else {
    psync_apipool_release_bad(api);
    pdbg_logf(D_WARNING, "Send command returned in valid result.\n");
    return -2;
  }

  result = papi_find_result2(bres, "result", PARAM_NUM)->num;
  if (unlikely(result))
    return handle_result(bres, result, err);

  statres = papi_find_result2(bres, "status", PARAM_HASH);
  teamres = papi_find_result2(statres, "team", PARAM_ARRAY)->array[0];
  teamresult = papi_find_result2(teamres, "result", PARAM_NUM)->num;
  userres = papi_find_result2(statres, "user", PARAM_ARRAY)->array[0];
  userresult = papi_find_result2(userres, "result", PARAM_NUM)->num;
  if (!userresult || !teamresult)
    result = 0;
  else {
    if (userresult == INVALID_SHAREDID_RESULT &&
        teamresult == INVALID_SHAREDID_RESULT)
      result = handle_result(userres, userresult, err);
    else if (userresult)
      result = handle_result(userres, userresult, err);
    else
      result = handle_result(teamres, teamresult, err);
  }

  if (ids1)
    free(ids1);
  if (ids2)
    free(ids2);

  free(bres);
  free(t);

  return result;
}

int do_psync_account_modifyshare(psync_shareid_t usrshrids[], uint32_t uperms[],
                                 int nushid, psync_shareid_t tmshrids[],
                                 uint32_t tperms[], int ntmshid, char **err) {
  psock_t *api;
  binresult *bres;
  uint64_t result, userresult, teamresult;
  char *ids1 = NULL;
  char *ids2 = NULL;
  char *perms1 = NULL;
  char *perms2 = NULL;
  char *idsp = 0;
  char *permsp = 0;
  int i, pind = 1, numparam = 1, k;
  binparam *t;
  const binresult *userres, *teamres, *statres;
  *err = 0;

  numparam += 2 * (!!nushid) + 2 * (!!ntmshid);
  if (unlikely(numparam == 1))
    return -3;

  t = (binparam *)malloc(numparam * sizeof(binparam));

  init_param_str(t, "auth", psync_my_auth);

  if (nushid) {
    ids1 = (char *)malloc(nushid * FOLDERID_ENTRY_SIZE);
    idsp = ids1;
    perms1 = (char *)malloc(nushid * FOLDERID_ENTRY_SIZE);
    permsp = perms1;
    for (i = 0; i < nushid; ++i) {
      k = sprintf(idsp, "%lld", (long long)usrshrids[i]);
      if (unlikely(k <= 0))
        break;
      idsp[k] = ',';
      idsp = idsp + k + 1;

      k = sprintf(permsp, "%lld", (long long)uperms[i]);
      if (unlikely(k <= 0))
        break;
      permsp[k] = ',';
      permsp = permsp + k + 1;
    }
    if (i > 0) {
      *(idsp - 1) = '\0';
      *(permsp - 1) = '\0';
    }
    // pdbg_logf(D_NOTICE, "usershareids %s, userpermissions %s",ids1, perms1);
    init_param_str(t + pind++, "usershareids", ids1);
    init_param_str(t + pind++, "userpermissions", perms1);
  }

  if (ntmshid) {
    ids2 = (char *)malloc(ntmshid * FOLDERID_ENTRY_SIZE);
    idsp = ids2;
    perms2 = (char *)malloc(ntmshid * FOLDERID_ENTRY_SIZE);
    permsp = perms2;

    for (i = 0; i < ntmshid; ++i) {
      k = sprintf(idsp, "%lld", (long long)tmshrids[i]);
      if (unlikely(k <= 0))
        break;
      idsp[k] = ',';
      idsp = idsp + k + 1;

      k = sprintf(permsp, "%lld", (long long)uperms[i]);
      if (unlikely(k <= 0))
        break;
      permsp[k] = ',';
      permsp = permsp + k + 1;
    }
    if (i > 0) {
      *(idsp - 1) = '\0';
      *(permsp - 1) = '\0';
    }
    // pdbg_logf(D_NOTICE, "teamshareids %s teampermissions %s",ids2, perms2);
    init_param_str(t + pind++, "teamshareids", ids2);
    init_param_str(t + pind++, "teampermissions", perms2);
  }

  api = psync_apipool_get();
  if (unlikely(!api)) {
    pdbg_logf(D_WARNING, "Can't gat api from the pool. No pool ?\n");
    return -2;
  }

  bres = papi_send(api, "account_modifyshare",
                         sizeof("account_modifyshare") - 1, t, pind, -1, 1);

  if (likely(bres))
    psync_apipool_release(api);
  else {
    psync_apipool_release_bad(api);
    pdbg_logf(D_WARNING, "Send command returned in valid result.\n");
    return -2;
  }

  result = papi_find_result2(bres, "result", PARAM_NUM)->num;
  if (unlikely(result))
    return handle_result(bres, result, err);

  statres = papi_find_result2(bres, "status", PARAM_HASH);
  teamres = papi_find_result2(statres, "team", PARAM_ARRAY)->array[0];
  teamresult = papi_find_result2(teamres, "result", PARAM_NUM)->num;
  userres = papi_find_result2(statres, "user", PARAM_ARRAY)->array[0];
  userresult = papi_find_result2(userres, "result", PARAM_NUM)->num;
  if (!userresult || !teamresult)
    result = 0;
  else {
    if (userresult == INVALID_SHAREDID_RESULT &&
        teamresult == INVALID_SHAREDID_RESULT)
      result = handle_result(userres, userresult, err);
    else if (userresult)
      result = handle_result(userres, userresult, err);
    else
      result = handle_result(teamres, teamresult, err);
  }

  if (ids1)
    free(ids1);
  if (ids2)
    free(ids2);
  if (perms1)
    free(perms1);
  if (perms2)
    free(perms2);

  free(bres);
  free(t);

  return result;
}

void get_ba_member_email(uint64_t userid, char **email /*OUT*/,
                         size_t *length /*OUT*/) {
  psync_sql_res *res;
  psync_variant_row row;
  const char *cstr;
  *length = 0;
  res = psql_query("SELECT mail FROM baccountemail WHERE id=?");
  psql_bind_uint(res, 1, userid);
  if ((row = psql_fetch(res))) {
    cstr = psync_get_lstring(row[0], length);
    *email = (char *)malloc(*length);
    memcpy(*email, cstr, *length);
    psql_free(res);
    return;
  } else
    psql_free(res);

  {
    binresult *bres;
    const binresult *users;
    const char *fname, *lname;

    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("timeformat", "timestamp"),
                         PAPI_NUM("userids", userid)};
    bres = psync_api_run_command("account_users", params);

    if (!bres) {
      pdbg_logf(D_NOTICE, "Account users command failed! \n");
      return;
    }

    if (api_error_result(bres))
      return;

    users = papi_find_result2(bres, "users", PARAM_ARRAY);
    if (!users->length) {
      free(bres);
      pdbg_logf(D_WARNING, "Account_users returned empty result!\n");
      return;
    } else {
      const char *resret =
          papi_find_result2(users->array[0], "email", PARAM_STR)->str;
      *length = strlen(resret);
      *email = psync_strndup(resret, *length);
      fname = papi_find_result2(users->array[0], "firstname", PARAM_STR)->str;
      lname = papi_find_result2(users->array[0], "lastname", PARAM_STR)->str;
    }
    free(bres);

    if (*length) {
      psync_sql_res *q;
      q = psql_prepare("REPLACE INTO baccountemail  (id, mail, "
                                   "firstname, lastname) VALUES (?, ?, ?, ?)");
      psql_bind_uint(q, 1, userid);
      psql_bind_lstr(q, 2, *email, *length);
      psql_bind_lstr(q, 3, fname, strlen(fname));
      psql_bind_lstr(q, 4, lname, strlen(lname));
      psql_run_free(q);
    }
  }
}

void get_ba_team_name(uint64_t teamid, char **name /*OUT*/,
                      size_t *length /*OUT*/) {
  psync_sql_res *res;
  psync_variant_row row;
  const char *cstr;

  binresult *bres;
  const binresult *teams;

  res = psql_query("SELECT name FROM baccountteam WHERE id=?");
  psql_bind_uint(res, 1, teamid);
  if ((row = psql_fetch(res))) {
    cstr = psync_get_lstring(row[0], length);
    *name = (char *)malloc(*length);
    memcpy(*name, cstr, *length);
    psql_free(res);
    return;
  } else
    psql_free(res);

  // pdbg_logf(D_NOTICE, "Account_teams numids %d\n", nids);
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_STR("timeformat", "timestamp"),
                       PAPI_NUM("teamids", teamid), PAPI_STR("showeveryone", "1")};
  bres = psync_api_run_command("account_teams", params);

  if (!bres) {
    pdbg_logf(D_WARNING, "Send command returned in valid result.\n");
    return;
  }

  if (api_error_result(bres))
    return;

  teams = papi_find_result2(bres, "teams", PARAM_ARRAY);

  // pdbg_logf(D_NOTICE, "Result contains %d teams\n", users->length);

  if (!teams->length) {
    free(bres);
    pdbg_logf(D_WARNING, "Account_teams returned empty result!\n");
    return;
  } else {
    const char *teamret =
        papi_find_result2(teams->array[0], "name", PARAM_STR)->str;
    *length = strlen(teamret);
    *name = psync_strndup(teamret, *length);
  }
  free(bres);

  psync_sql_res *q;
  q = psql_prepare(
      "REPLACE INTO baccountteam  (id, name) VALUES (?, ?)");
  psql_bind_uint(q, 1, teamid);
  psql_bind_lstr(q, 2, *name, *length);
  psql_run_free(q);

  return;
}

void cache_account_emails() {
  binresult *bres;
  int i;
  const binresult *users;

  if (psync_my_auth[0]) {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("timeformat", "timestamp")};
    bres = psync_api_run_command("account_users", params);
  } else if (psync_my_user && psync_my_pass) {
    binparam params[] = {PAPI_STR("username", psync_my_user),
                         PAPI_STR("password", psync_my_pass),
                         PAPI_STR("timeformat", "timestamp")};
    bres = psync_api_run_command("account_users", params);
  } else
    return;

  if (!bres) {
    pdbg_logf(D_WARNING, "Send command returned invalid result.\n");
    return;
  }

  if (api_error_result(bres))
    return;

  users = papi_find_result2(bres, "users", PARAM_ARRAY);

  if (!users->length) {
    pdbg_logf(D_WARNING, "Account_users returned empty result!\n");
    goto end_close;
  } else {
    psync_sql_res *q;
    psql_start();
    q = psql_prepare("DELETE FROM baccountemail");
    if (unlikely(psql_run_free(q))) {
      psql_rollback();
      goto end_close;
    }

    for (i = 0; i < users->length; ++i) {
      const char *nameret = 0;
      const binresult *user = users->array[i];
      uint64_t userid = 0;
      psync_sql_res *res;
      const char *fname, *lname;
      int active = 0;
      int frozen = 0;

      active = papi_find_result2(user, "active", PARAM_BOOL)->num;
      frozen = papi_find_result2(user, "frozen", PARAM_BOOL)->num;
      nameret = papi_find_result2(user, "email", PARAM_STR)->str;
      userid = papi_find_result2(user, "id", PARAM_NUM)->num;
      fname = papi_find_result2(user, "firstname", PARAM_STR)->str;
      lname = papi_find_result2(user, "lastname", PARAM_STR)->str;

      if (userid && (active || frozen)) {
        res = psql_prepare(
            "INSERT INTO baccountemail  (id, mail, firstname, lastname) VALUES "
            "(?, ?, ?, ?)");
        psql_bind_uint(res, 1, userid);
        psql_bind_lstr(res, 2, nameret, strlen(nameret));
        psql_bind_lstr(res, 3, fname, strlen(fname));
        psql_bind_lstr(res, 4, lname, strlen(lname));
        if (unlikely(psql_run_free(res))) {
          psql_rollback();
          goto end_close;
        }
      }
    }
    psql_commit();
  }
end_close:
  free(bres);
}

void cache_account_teams() {
  binresult *bres;
  int i;
  const binresult *users;

  if (psync_my_auth[0]) {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("timeformat", "timestamp"),
                         PAPI_STR("showeveryone", "1")};
    bres = psync_api_run_command("account_teams", params);
  } else if (psync_my_user && psync_my_pass) {
    binparam params[] = {
        PAPI_STR("username", psync_my_user), PAPI_STR("password", psync_my_pass),
        PAPI_STR("timeformat", "timestamp"), PAPI_STR("showeveryone", "1")};
    bres = psync_api_run_command("account_teams", params);
  } else
    return;

  if (!bres) {
    pdbg_logf(D_WARNING, "Send command returned in valid result.\n");
    return;
  }

  if (api_error_result(bres))
    return;

  users = papi_find_result2(bres, "teams", PARAM_ARRAY);

  // pdbg_logf(D_NOTICE, "Result contains %d teams\n", users->length);

  if (!users->length) {
    free(bres);
    pdbg_logf(D_WARNING, "Account_teams returned empty result!\n");
    return;
  } else {
    psync_sql_res *q;
    psql_start();
    q = psql_prepare("DELETE FROM baccountteam");
    if (unlikely(psql_run_free(q)))
      psql_rollback();

    for (i = 0; i < users->length; ++i) {
      const char *nameret = 0;
      nameret = papi_find_result2(users->array[i], "name", PARAM_STR)->str;
      uint64_t teamid = 0;
      psync_sql_res *res;

      teamid = papi_find_result2(users->array[i], "id", PARAM_NUM)->num;
      // pdbg_logf(D_NOTICE, "Team name %s team id %lld\n", nameret,(long
      // long)teamid);
      res = psql_prepare(
          "INSERT INTO baccountteam  (id, name) VALUES (?, ?)");
      psql_bind_uint(res, 1, teamid);
      psql_bind_lstr(res, 2, nameret, strlen(nameret));
      if (unlikely(psql_run_free(res)))
        psql_rollback();
    }
    psql_commit();
  }
  free(bres);
  return;
}

static void cache_my_team(const binresult *team1) {
  const char *nameret = 0;
  const binresult *team;
  uint64_t teamid = 0;
  psync_sql_res *q;

  team = papi_find_result2(team1, "team", PARAM_HASH);

  nameret = papi_find_result2(team, "name", PARAM_STR)->str;
  teamid = papi_find_result2(team, "id", PARAM_NUM)->num;
  // pdbg_logf(D_NOTICE, "My Team name %s team id %lld\n", nameret,(long
  // long)teamid);
  q = psql_prepare("INSERT INTO myteams  (id, name) VALUES (?, ?)");
  psql_bind_uint(q, 1, teamid);
  psql_bind_lstr(q, 2, nameret, strlen(nameret));
  psql_run_free(q);
}

void cache_ba_my_teams() {
  binresult *bres;
  const binresult *users;
  const binresult *user;
  const binresult *teams;
  psync_sql_res *q;
  int i;

  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_STR("timeformat", "timestamp"), PAPI_STR("userids", "me"),
                       PAPI_STR("showteams", "1"), PAPI_STR("showeveryone", "1")};
  bres = psync_api_run_command("account_users", params);
  if (!bres) {
    pdbg_logf(D_WARNING, "Send command returned invalid result.\n");
    return;
  }

  if (api_error_result(bres))
    return;

  users = papi_find_result2(bres, "users", PARAM_ARRAY);

  if (!users->length) {
    free(bres);
    pdbg_logf(D_WARNING, "Account_users returned empty result!\n");
    return;
  }

  psql_lock();
  q = psql_prepare("DELETE FROM myteams");
  psql_run_free(q);
  user = users->array[0];
  teams = papi_find_result2(user, "teams", PARAM_ARRAY);
  for (i = 0; i < teams->length; i++)
    cache_my_team(teams->array[i]);
  free(bres);
  psql_unlock();
}

int api_error_result(binresult *res) {
  uint64_t result;
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    free(res);
    psync_process_api_error(result);
    return 1;
  }
  return 0;
}

void psync_update_cryptostatus() {
  if (psync_my_auth[0]) {
    binresult *res;
    const binresult *cres;
    psync_sql_res *q;
    uint64_t u, crexp, crsub = 0, is_business = 0;
    int crst = 0, crstat;

    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("timeformat", "timestamp")};
    res = psync_api_run_command("userinfo", params);
    if (!res) {
      pdbg_logf(D_WARNING, "Send command returned invalid result.\n");
      return;
    }

    if (api_error_result(res))
      return;

    q = psql_prepare(
        "REPLACE INTO setting (id, value) VALUES (?, ?)");

    is_business = papi_find_result2(res, "business", PARAM_BOOL)->num;

    u = papi_find_result2(res, "cryptosetup", PARAM_BOOL)->num;
    psql_bind_str(q, 1, "cryptosetup");
    psql_bind_uint(q, 2, u);
    psql_run(q);
    if (u)
      crst = 1;
    psql_bind_str(q, 1, "cryptosubscription");
    crsub = papi_find_result2(res, "cryptosubscription", PARAM_BOOL)->num;
    psql_bind_uint(q, 2, crsub);
    psql_run(q);

    cres = papi_check_result2(res, "cryptoexpires", PARAM_NUM);
    crexp = cres ? cres->num : 0;
    psql_bind_str(q, 1, "cryptoexpires");
    psql_bind_uint(q, 2, crexp);
    psql_run(q);

    if (is_business || crsub) {
      if (crst)
        crstat = 5;
      else
        crstat = 4;
    } else {
      if (!crst)
        crstat = 1;
      else {
        if (psys_time_seconds() > crexp)
          crstat = 3;
        else
          crstat = 2;
      }
    }
    psql_bind_str(q, 1, "cryptostatus");
    psql_bind_uint(q, 2, crstat);
    psql_run(q);
    psql_free(q);
  }
}

static int check_write_permissions(psync_folderid_t folderid) {
  psync_sql_res *res;
  psync_uint_row row;
  int ret = 0;

  res =
      psql_query("SELECT permissions, flags, name FROM folder WHERE id=?");
  psql_bind_uint(res, 1, folderid);
  row = psql_fetch_int(res);
  if (unlikely(!row))
    pdbg_logf(D_ERROR, "could not find folder of folderid %lu",
          (unsigned long)folderid);
  else if (/*(((row[1]) & 3) != O_RDONLY) &&*/ ((row[0] & PSYNC_PERM_MODIFY) &&
                                                (row[0] & PSYNC_PERM_CREATE)))
    ret = 1;

  psql_free(res);
  return ret;
}
static psync_folderid_t create_index_folder(const char *path) {
  char *buff = NULL;
  uint32_t bufflen;
  int ind = 1;
  char *err;
  psync_folderid_t folderid;

  while (ind < 100) {
    bufflen = strlen(path) + 1 /*zero char*/ + 3 /*parenthesis*/ +
              3 /*up to 3 digit index*/;
    buff = (char *)malloc(bufflen);
    snprintf(buff, bufflen - 1, "%s (%d)", path, ind);
    if (psync_create_remote_folder_by_path(buff, &err) != 0)
      pdbg_logf(D_NOTICE, "Unable to create folder %s error is %s.", buff, err);
    folderid = pfolder_id(buff);
    if ((folderid != PSYNC_INVALID_FOLDERID) &&
        check_write_permissions(folderid)) {
      free(buff);
      break;
    }
    ++ind;
    if (err)
      free(err);
    free(buff);
  }
  return folderid;
}
psync_folderid_t psync_check_and_create_folder(const char *path) {
  psync_folderid_t folderid = pfolder_id(path);
  char *err;

  if (folderid == PSYNC_INVALID_FOLDERID) {
    if (psync_create_remote_folder_by_path(path, &err) != 0) {
      pdbg_logf(D_NOTICE, "Unable to create folder %s error is %s.", path, err);
      free(err);
      folderid = create_index_folder(path);
    }
  } else if (!check_write_permissions(folderid))
    folderid = create_index_folder(path);

  return folderid;
}
