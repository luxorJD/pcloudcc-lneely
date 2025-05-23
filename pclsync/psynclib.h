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

#ifndef _PSYNC_LIB_H
#define _PSYNC_LIB_H

/* All paths are in UTF-8 regardless of the OS.
 * All functions with int return type unless specified otherwise return 0 for
 * success and -1 for failure.
 */

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "paccountevents.h"
#include "ptools.h"
#include "pstatus.h"
#include "pfoldersync.h"
#include "publiclinks.h"

typedef uint64_t psync_userid_t;
typedef uint64_t psync_shareid_t;
typedef uint64_t psync_sharerequestid_t;
typedef uint64_t psync_teamid_t;
typedef uint32_t psync_eventtype_t;

typedef struct {
  const char *label;
  const char *api;
  const char *binapi;
  uint32_t locationid;
} apiserver_info_t;

typedef struct {
  size_t serverscnt;
  apiserver_info_t entries[];
} apiservers_list_t;

// Backend API errors constants
#define BEAPI_ERR_F_NOT_EXISTS 2005     // folder does not exist, skip
#define BEAPI_ERR_MOVE_ROOT 2042        // moving root, should not happen
#define BEAPI_ERR_ACCESS_DENIED 2003    // access denied, skip
#define BEAPI_ERR_INVALID_NAME 2001     // invalid name, should not happen
#define BEAPI_ERR_OVERQUOTA 2008        // overquota
#define BEAPI_ERR_MOVE_IN_SHARED_F 2023 // moving into shared folder
#define BEAPI_ERR_MOVE_INTO_SELF 2043   // into itself or child
#define BEAPI_ERR_NO_SHARE_IN_PUB                                              \
  2282 // public folder can't contain shared folder
#define BEAPI_ERR_NO_UP_LINK_IN_PUB                                            \
  2283 // public folder can't contain upload link
#define BEAPI_ERR_NO_DL_LINK_IN_PUB                                            \
  2284 // public folder can't contain download link
#define BEAPI_ERR_NO_PUB_F_IN_SHARE                                            \
  2285 // shared folder can't contain public folder
#define BEAPI_ERR_NO_PUB_F_IN_BUP                                              \
  2340 // backup folders can't contain shared folders
#define BEAPI_ERR_NO_UPLINK_IN_BUP                                             \
  2342 // backup folders can't contain upload links
#define BEAPI_ERR_NO_DL_LINK_IN_BUP                                            \
  2343 // backup folders can't contain download links
#define BEAPI_ERR_NOT_ALLOWED_IN_BUP                                           \
  2346 // you can't place this item in backup folder
#define BEAPI_ERR_DEST_F_EXISTS 2004 // destination folder already exists
#define BEAPI_ERR_MV_TOO_MANY_IN_SHA                                           \
  2352 // Too many objects moved at once in shared folder

/* PEVENT_LOCAL_FOLDER_CREATED means that a folder was created in remotely and
 * this action was replicated locally, not the other way around. Accordingly
 * PEVENT_REMOTE_FOLDER_CREATED is fired when locally created folder is
 * replicated to the server.
 */

#define PEVENT_TYPE_LOCAL (0 << 0)
#define PEVENT_TYPE_REMOTE (1 << 0)
#define PEVENT_TYPE_FILE (0 << 1)
#define PEVENT_TYPE_FOLDER (1 << 1)
#define PEVENT_TYPE_CREATE (0 << 2)
#define PEVENT_TYPE_DELETE (1 << 2)
#define PEVENT_TYPE_RENAME (2 << 2)
#define PEVENT_TYPE_START (0 << 5)
#define PEVENT_TYPE_FINISH (1 << 5)
#define PEVENT_TYPE_SUCCESS (0 << 6)
#define PEVENT_TYPE_FAIL (1 << 6)

#define PEVENT_FIRST_USER_EVENT (1 << 30)
#define PEVENT_FIRST_SHARE_EVENT (PEVENT_FIRST_USER_EVENT + 200)
#define PEVENT_FIRST_BACKUP_EVENT (PEVENT_FIRST_SHARE_EVENT + 200)

#define PEVENT_LOCAL_FOLDER_CREATED                                            \
  (PEVENT_TYPE_LOCAL + PEVENT_TYPE_FOLDER + PEVENT_TYPE_CREATE)
#define PEVENT_REMOTE_FOLDER_CREATED                                           \
  (PEVENT_TYPE_REMOTE + PEVENT_TYPE_FOLDER + PEVENT_TYPE_CREATE)
#define PEVENT_FILE_DOWNLOAD_STARTED                                           \
  (PEVENT_TYPE_LOCAL + PEVENT_TYPE_FILE + PEVENT_TYPE_CREATE +                 \
   PEVENT_TYPE_START)
#define PEVENT_FILE_DOWNLOAD_FINISHED                                          \
  (PEVENT_TYPE_LOCAL + PEVENT_TYPE_FILE + PEVENT_TYPE_CREATE +                 \
   PEVENT_TYPE_FINISH + PEVENT_TYPE_SUCCESS)
#define PEVENT_FILE_DOWNLOAD_FAILED                                            \
  (PEVENT_TYPE_LOCAL + PEVENT_TYPE_FILE + PEVENT_TYPE_CREATE +                 \
   PEVENT_TYPE_FINISH + PEVENT_TYPE_FAIL)
#define PEVENT_FILE_UPLOAD_STARTED                                             \
  (PEVENT_TYPE_REMOTE + PEVENT_TYPE_FILE + PEVENT_TYPE_CREATE +                \
   PEVENT_TYPE_START)
#define PEVENT_FILE_UPLOAD_FINISHED                                            \
  (PEVENT_TYPE_REMOTE + PEVENT_TYPE_FILE + PEVENT_TYPE_CREATE +                \
   PEVENT_TYPE_FINISH + PEVENT_TYPE_SUCCESS)
#define PEVENT_FILE_UPLOAD_FAILED                                              \
  (PEVENT_TYPE_REMOTE + PEVENT_TYPE_FILE + PEVENT_TYPE_CREATE +                \
   PEVENT_TYPE_FINISH + PEVENT_TYPE_FAIL)
#define PEVENT_LOCAL_FOLDER_DELETED                                            \
  (PEVENT_TYPE_LOCAL + PEVENT_TYPE_FOLDER + PEVENT_TYPE_DELETE)
#define PEVENT_REMOTE_FOLDER_DELETED                                           \
  (PEVENT_TYPE_REMOTE + PEVENT_TYPE_FOLDER + PEVENT_TYPE_DELETE)
#define PEVENT_LOCAL_FILE_DELETED                                              \
  (PEVENT_TYPE_LOCAL + PEVENT_TYPE_FILE + PEVENT_TYPE_DELETE)
#define PEVENT_REMOTE_FILE_DELETED                                             \
  (PEVENT_TYPE_REMOTE + PEVENT_TYPE_FILE + PEVENT_TYPE_DELETE)
#define PEVENT_LOCAL_FOLDER_RENAMED                                            \
  (PEVENT_TYPE_LOCAL + PEVENT_TYPE_FOLDER + PEVENT_TYPE_RENAME)

#define PEVENT_USERINFO_CHANGED PEVENT_FIRST_USER_EVENT
#define PEVENT_USEDQUOTA_CHANGED (PEVENT_FIRST_USER_EVENT + 1)

#define PEVENT_SHARE_REQUESTIN PEVENT_FIRST_SHARE_EVENT
#define PEVENT_SHARE_REQUESTOUT (PEVENT_FIRST_SHARE_EVENT + 1)
#define PEVENT_SHARE_ACCEPTIN (PEVENT_FIRST_SHARE_EVENT + 2)
#define PEVENT_SHARE_ACCEPTOUT (PEVENT_FIRST_SHARE_EVENT + 3)
#define PEVENT_SHARE_DECLINEIN (PEVENT_FIRST_SHARE_EVENT + 4)
#define PEVENT_SHARE_DECLINEOUT (PEVENT_FIRST_SHARE_EVENT + 5)
#define PEVENT_SHARE_CANCELIN (PEVENT_FIRST_SHARE_EVENT + 6)
#define PEVENT_SHARE_CANCELOUT (PEVENT_FIRST_SHARE_EVENT + 7)
#define PEVENT_SHARE_REMOVEIN (PEVENT_FIRST_SHARE_EVENT + 8)
#define PEVENT_SHARE_REMOVEOUT (PEVENT_FIRST_SHARE_EVENT + 9)
#define PEVENT_SHARE_MODIFYIN (PEVENT_FIRST_SHARE_EVENT + 10)
#define PEVENT_SHARE_MODIFYOUT (PEVENT_FIRST_SHARE_EVENT + 11)

#define PEVENT_SHARE_RENAME_F (PEVENT_FIRST_SHARE_EVENT + 12)

#define PEVENT_BACKUP_STOP PEVENT_FIRST_BACKUP_EVENT
#define PEVENT_BKUP_F_DEL_SYNCED (PEVENT_FIRST_BACKUP_EVENT + 1)
#define PEVENT_BKUP_F_DEL_NOTSYNCED (PEVENT_FIRST_BACKUP_EVENT + 2)
#define PEVENT_BKUP_F_DEL_DRIVE (PEVENT_FIRST_BACKUP_EVENT + 3)

#define PNOTIFICATION_ACTION_NONE 0
#define PNOTIFICATION_ACTION_GO_TO_FOLDER 1
#define PNOTIFICATION_ACTION_GO_TO_URL 2
#define PNOTIFICATION_ACTION_SHARE_REQUEST 3

// Sync constants
#define PSYNC_DOWNLOAD_ONLY 1
#define PSYNC_UPLOAD_ONLY 2
#define PSYNC_FULL 3
#define PSYNC_BACKUPS 7

#define PSYNC_STR_DOWNLOAD_ONLY "1"
#define PSYNC_STR_UPLOAD_ONLY "2"
#define PSYNC_STR_FULL "3"
#define PSYNC_STR_ALLSYNCS "1,2,3"
#define PSYNC_STR_BACKUPS "7"

#define PSYNC_SYNCTYPE_MIN 1
#define PSYNC_SYNCTYPE_MAX 7
// Sync constants end

#define PERROR_LOCAL_FOLDER_NOT_FOUND 1
#define PERROR_REMOTE_FOLDER_NOT_FOUND 2
#define PERROR_DATABASE_OPEN 3
#define PERROR_NO_HOMEDIR 4
#define PERROR_SSL_INIT_FAILED 5
#define PERROR_DATABASE_ERROR 6
#define PERROR_LOCAL_FOLDER_ACC_DENIED 7
#define PERROR_REMOTE_FOLDER_ACC_DENIED 8
#define PERROR_FOLDER_ALREADY_SYNCING 9
#define PERROR_INVALID_SYNCTYPE 10
#define PERROR_OFFLINE 11
#define PERROR_INVALID_SYNCID 12
#define PERROR_PARENT_OR_SUBFOLDER_ALREADY_SYNCING 13
#define PERROR_LOCAL_IS_ON_PDRIVE 14
#define PERROR_NO_MEMORY 15
#define PERROR_NET_ERROR 16
#define PERROR_PARENT_IS_IGNORED 17

#define PERROR_CACHE_MOVE_NOT_EMPTY 1
#define PERROR_CACHE_MOVE_NO_WRITE_ACCESS 2
#define PERROR_CACHE_MOVE_DRIVE_HAS_TASKS                                      \
  3 // this error is also returned when the path is on pCloudDrive

#define PLIST_FILES 1
#define PLIST_FOLDERS 2
#define PLIST_ALL 3

#define PSYNC_PERM_READ 1
#define PSYNC_PERM_CREATE 2
#define PSYNC_PERM_MODIFY 4
#define PSYNC_PERM_DELETE 8
#define PSYNC_PERM_MANAGE 16

#define PSYNC_PERM_ALL                                                         \
  (PSYNC_PERM_READ | PSYNC_PERM_CREATE | PSYNC_PERM_MODIFY | PSYNC_PERM_DELETE)
#define PSYNC_PERM_WRITE                                                       \
  (PSYNC_PERM_CREATE | PSYNC_PERM_MODIFY | PSYNC_PERM_DELETE)

#define PSYNC_CRYPTO_SETUP_SUCCESS 0
#define PSYNC_CRYPTO_SETUP_NOT_SUPPORTED -1
#define PSYNC_CRYPTO_SETUP_KEYGEN_FAILED 1
#define PSYNC_CRYPTO_SETUP_CANT_CONNECT 2
#define PSYNC_CRYPTO_SETUP_NOT_LOGGED_IN 3
#define PSYNC_CRYPTO_SETUP_ALREADY_SETUP 4
#define PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR 5

#define PSYNC_CRYPTO_START_SUCCESS 0
#define PSYNC_CRYPTO_START_NOT_SUPPORTED -1
#define PSYNC_CRYPTO_START_ALREADY_STARTED 1
#define PSYNC_CRYPTO_START_CANT_CONNECT 2
#define PSYNC_CRYPTO_START_NOT_LOGGED_IN 3
#define PSYNC_CRYPTO_START_NOT_SETUP 4
#define PSYNC_CRYPTO_START_UNKNOWN_KEY_FORMAT 5
#define PSYNC_CRYPTO_START_BAD_PASSWORD 6
#define PSYNC_CRYPTO_START_KEYS_DONT_MATCH 7
#define PSYNC_CRYPTO_START_UNKNOWN_ERROR 8

#define PSYNC_CRYPTO_STOP_SUCCESS 0
#define PSYNC_CRYPTO_STOP_NOT_SUPPORTED -1
#define PSYNC_CRYPTO_STOP_NOT_STARTED 1

#define PSYNC_CRYPTO_HINT_SUCCESS 0
#define PSYNC_CRYPTO_HINT_NOT_SUPPORTED -1
#define PSYNC_CRYPTO_HINT_NOT_PROVIDED 1
#define PSYNC_CRYPTO_HINT_CANT_CONNECT 2
#define PSYNC_CRYPTO_HINT_NOT_LOGGED_IN 3
#define PSYNC_CRYPTO_HINT_UNKNOWN_ERROR 4

#define PSYNC_CRYPTO_RESET_SUCCESS 0
#define PSYNC_CRYPTO_RESET_CRYPTO_IS_STARTED 1
#define PSYNC_CRYPTO_RESET_CANT_CONNECT 2
#define PSYNC_CRYPTO_RESET_NOT_LOGGED_IN 3
#define PSYNC_CRYPTO_RESET_NOT_SETUP 4
#define PSYNC_CRYPTO_RESET_UNKNOWN_ERROR 5

#define PSYNC_CRYPTO_SUCCESS 0
#define PSYNC_CRYPTO_NOT_STARTED -1
#define PSYNC_CRYPTO_RSA_ERROR -2
#define PSYNC_CRYPTO_FOLDER_NOT_FOUND -3
#define PSYNC_CRYPTO_FILE_NOT_FOUND -4
#define PSYNC_CRYPTO_INVALID_KEY -5
#define PSYNC_CRYPTO_CANT_CONNECT -6
#define PSYNC_CRYPTO_FOLDER_NOT_ENCRYPTED -7
#define PSYNC_CRYPTO_INTERNAL_ERROR -8
#define PSYNC_CRYPTO_BAD_PASSPHRASE -9
#define PSYNC_CRYPTO_BAD_KEY -10
#define PSYNC_CRYPTO_NULL_PTR -11

#define PSYNC_CRYPTO_STATUS_NEW 1
#define PSYNC_CRYPTO_STATUS_TRIAL 2
#define PSYNC_CRYPTO_STATUS_EXPIRED 3
#define PSYNC_CRYPTO_STATUS_ACTIVE 4
#define PSYNC_CRYPTO_STATUS_SETUP 5

#define PSYNC_CRYPTO_INVALID_FOLDERID ((psync_folderid_t) - 1)

#define PSYNC_CRYPTO_FLAG_TEMP_PASS 1

#ifndef DEFAULT_FUSE_VOLUME_NAME
#define DEFAULT_FUSE_VOLUME_NAME "pCloud Drive"
#endif

#ifndef DEFAULT_FUSE_MOUNT_POINT
#define DEFAULT_FUSE_MOUNT_POINT "pCloud"
#endif

// Lib error codes

// Backup errors
#define PSYNC_BACKUP_PATH_EMPTY_ERR 11001
#define PSYNC_BACKUP_PATH_EMPTY_MSG "Cannot backup an empty path."
// Backup errors end

// Lib error codes end

typedef struct {
  psync_fileid_t fileid;
  const char *name;
  const char *localpath;
  const char *remotepath;
  psync_syncid_t syncid;
} psync_file_event_t;

typedef struct {
  psync_fileid_t folderid;
  const char *name;
  const char *localpath;
  const char *remotepath;
  psync_syncid_t syncid;
} psync_folder_event_t;

typedef struct {
  psync_folderid_t folderid;
  const char *sharename;
  const char *toemail;
  const char *fromemail;
  const char *message;
  psync_userid_t userid;
  psync_shareid_t shareid;
  psync_sharerequestid_t sharerequestid;
  time_t created;
  unsigned char canread;
  unsigned char cancreate;
  unsigned char canmodify;
  unsigned char candelete;
  unsigned char canmanage;
} psync_share_event_t;

typedef union {
  psync_file_event_t *file;
  psync_folder_event_t *folder;
  psync_share_event_t *share;
  void *ptr;
} psync_eventdata_t;

typedef struct {
  psync_sharerequestid_t sharerequestid;
  psync_folderid_t folderid;
  time_t created;
  psync_userid_t userid;
  const char *email;
  const char *sharename;
  const char *message;
  unsigned char permissions;
  unsigned char canread;
  unsigned char cancreate;
  unsigned char canmodify;
  unsigned char candelete;
  unsigned char isba;
} psync_sharerequest_t;

typedef struct {
  size_t sharerequestcnt;
  psync_sharerequest_t sharerequests[];
} psync_sharerequest_list_t;

typedef struct {
  psync_shareid_t shareid;
  psync_folderid_t folderid;
  time_t created;
  psync_userid_t userid;
  const char *fromemail;
  const char *toemail;
  const char *sharename;
  unsigned char permissions;
  unsigned char canread;
  unsigned char cancreate;
  unsigned char canmodify;
  unsigned char candelete;
  unsigned char canmanage;
  unsigned char isba;
  unsigned char isteam;
} psync_share_t;

typedef struct {
  size_t sharecnt;
  psync_share_t shares[];
} psync_share_list_t;

typedef struct {
  const char *url;
  const char *notes;
  const char *versionstr;
  const char *localpath;
  unsigned long version;
  uint64_t updatesize;
} psync_new_version_t;

typedef union {
  psync_folderid_t folderid;
  const char *url;
  psync_sharerequestid_t sharerequestid;
} psync_notification_action_t;

typedef struct {
  const char *text;
  const char *thumb;
  time_t mtime;
  psync_notification_action_t actiondata;
  uint32_t notificationid;
  uint8_t actionid;
  uint8_t isnew;
  uint8_t iconid;
} psync_notification_t;

typedef struct {
  uint32_t notificationcnt;
  uint32_t newnotificationcnt;
  psync_notification_t notifications[];
} psync_notification_list_t;


typedef enum {
  Dev_Types_UsbRemovableDisk = 1,
  Dev_Types_UsbFixedDisk,
  Dev_Types_CDRomMedia,
  Dev_Types_CameraDevice,
  Dev_Types_AndroidDevice,
  Dev_Types_Unknown
} pdevice_types;

typedef enum { Dev_Event_arrival = 1, Dev_Event_removed } device_event;

typedef struct {
  pdevice_types type;
  const char *device_id;
  int isextended;
  const char *filesystem_path;
  const char *vendor;
  const char *product;
  int enabled;
  int connected;
} pdevice_item_t;

typedef struct {
  uint32_t entrycnt;
  pdevice_item_t entries[];
} pdevice_item_list_t;

typedef void (*device_event_callback)(device_event event, void *device_info_);

typedef struct {
  uint32_t type;
  const char *name;
} plogged_device_t;

typedef struct {
  uint32_t entrycnt;
  plogged_device_t devices[];
} plogged_device_list_t;

typedef struct {
  uint64_t teamid;
  const char *mail;
  const char *name;
  uint32_t type;
} contact_info_t;

typedef struct {
  size_t entrycnt;
  contact_info_t entries[];
} pcontacts_list_t;

typedef struct {
  const char *email;
  const char *currency;
  const char *language;
  uint8_t cryptosetup;
  uint8_t cryptosubscription;
  uint8_t cryptolifetime;
  uint8_t efh; // optional
  uint8_t emailverified;
  uint8_t usedpublinkbranding;
  uint8_t haspassword;
  uint8_t premium;
  uint8_t premiumlifetime;
  uint8_t business;
  uint8_t haspaidrelocation;
  uint32_t trashrevretentiondays;
  uint32_t plan;
  uint32_t result;
  uint64_t premiumexpires; // optional
  uint64_t publiclinkquota;
  uint64_t userid;
  uint64_t quota;
  uint64_t usedquota;
  uint64_t freequota;
  uint64_t registered;
} userinfo_t;

#define PSYNC_INVALID_SYNCID (psync_syncid_t) - 1

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*psync_malloc_t)(size_t);
typedef void *(*psync_realloc_t)(void *, size_t);
typedef void (*psync_free_t)(void *);

typedef void (*psync_generic_callback_t)();

/* Event callback is called every time a download/upload is started/finished,
 * quota is changed, folder is shared or similar. Look at the PEVENT_ constants
 * for a list of possible events.
 *
 * The type of data parameter is specific to eventtype. That is for
 * PEVENT_FILE_* or PEVENT_*_FILE_* events the type is psync_folder_event_t, for
 * PEVENT_*_FOLDER_* events it is psync_file_event_t, for PEVENT_SHARE_* events
 * is psync_share_event_t. For PEVENT_USERINFO_CHANGED and
 * PEVENT_USEDQUOTA_CHANGED data is NULL. Observe the changes by calling
 * psync_get_*_value() functions.
 *
 * It is unsafe to use pointers to strings that are passed in the data
 * structure, if you need to use them this way, strdup() will do the job. Event
 * callbacks will not overlap.
 *
 * Do not expect localpath to exist after receiving PEVENT_FILE_DOWNLOAD_STARTED
 * as the file will be created with alternative name first and renamed when
 * download is finished.
 *
 */

typedef void (*pevent_callback_t)(psync_eventtype_t event,
                                  psync_eventdata_t data);

/* Notifications callback is called every time new notificaion arrives (well,
 * with some throttling). List of notifications is always sorted from latest to
 * oldest. Every notification has the following fields: text - the text to
 * display thumb - if available, set to a local path for a thumb, NULL if no
 * thumbnail is available mtime - the date/time of notification as UNIX
 * timestamp notificationid - id of the notification actionid - action to be
 * taken when clicked, one of: PNOTIFICATION_ACTION_NONE - do nothing
 *     PNOTIFICATION_ACTION_GO_TO_FOLDER - go to a folderid set in
 * actiondata.folderid isnew - if true, notificaion is new (not seen) iconid -
 * id of the icon to display (when thumb is not available)
 */

typedef void (*pnotification_callback_t)(uint32_t notificationcnt,
                                         uint32_t newnotificationcnt);

/* psync_init inits the sync library. No network or local scan operations are
 * initiated by this call, call psync_start_sync to start those. However listing
 * remote folders, listing and editing syncs is supported.
 *
 * Returns 0 on success and -1 otherwise.
 *
 * psync_start_sync starts remote sync, both callbacks can be NULL, but most of
 * the time setting at least status_callback will make sense. Applications
 * should expect immediate status_callback with status of PSTATUS_LOGIN_REQUIRED
 * after first run of psync_start_sync().
 *
 * psync_set_notification_callback - sets callback for new notifications. Should
 * be called before psync_start_sync if at all. thumbsize should be string in
 * "WxH" format (e.g. "64x64"). If NULL no thumbs will be included in listings.
 *
 * psync_download_state is to be called after psync_init but before/instead of
 * psync_start_sync. This function downloads the directory structure into the
 * local state in foreground (e.g. it can take time to complete). It returns one
 * of PSTATUS_-es, specifically PSTATUS_READY, PSTATUS_OFFLINE or one of
 * login-related statuses. After a successful call to this function remote
 * folder listing can be preformed.
 *
 * psync_destroy is to be called before application exit. This is not
 * neccessary. In any case psync_destroy will return relatively fast, regardless
 * of blocked network calls and other potentially slow to finish tasks.
 *
 * psync_set_alloc can set the allocator to be used by the library. To be called
 * BEFORE psync_init if ever. If allocator is provided, its free() function is
 * to be used to free any memory that is said to be freed when returned by the
 * library.
 *
 * psync_set_database_path can set a full path to database file. If it does not
 * exists it will be created. The function should be only called before
 * psync_init. If database path is not set, appropriate location for the given
 * OS will be chosen. The library will make it's own copy of the path, so the
 * memory can be free()d/reused after the function returns. The path is not
 * checked by the function itself, if it is invalid (directory does not exist,
 * it is not writable, etc) psync_init will return -1 and the error code will be
 * PERROR_DATABASE_OPEN. In this condition it is safe to call
 * psync_set_database_path and psync_init again. A special value of ":memory:"
 * for databasepath will create in-memory database that will not be preserved
 * between runs. An empty string will create the database in a temporary file,
 * the net effect being similar to the ":memory:" option with less pressure on
 * the required memory. The underlying database is in fact SQLite, so any other
 * options that work for SQLite will work here.
 *
 */

void psync_set_database_path(const char *databasepath);
void psync_set_alloc(psync_malloc_t malloc_call, psync_realloc_t realloc_call,
                     psync_free_t free_call);
int psync_init();
void psync_start_sync(pstatus_change_callback_t status_callback,
                      pevent_callback_t event_callback);
void psync_set_notification_callback(
    pnotification_callback_t notification_callback, const char *thumbsize);
psync_notification_list_t *psync_get_notifications();
int psync_mark_notificaitons_read(uint32_t notificationid);
uint32_t psync_download_state();
void psync_destroy();

/* returns current status.
 */

void psync_get_status(pstatus_t *status);

/* psync_set_user_pass and psync_set_auth functions can be used for initial
 * login (PSTATUS_LOGIN_REQUIRED) and when PSTATUS_BAD_LOGIN_DATA error is
 * returned, however if the username do not match previously logged in user,
 * PSTATUS_USER_MISMATCH event will be generated. Preferably on
 * PSTATUS_BAD_LOGIN_DATA the user should be only prompted for new password and
 * psync_set_pass should be called. To change the current user, psync_unlink is
 * to be called first and then the new user may log in.
 *
 * The pointer returned by psync_get_username() is to be free()d.
 */

char *psync_get_username();
void psync_set_user_pass(const char *username, const char *password, int save);
void psync_set_pass(const char *password, int save);
void psync_set_auth(const char *auth, int save);
void psync_logout(uint32_t auth_status, int doinvauth);
void psync_unlink();

/* Upon seein a status of PSTATUS_TFA_REQUIRED the application is supposed to
 * let the user know that two factor authentication is enabled and provide the
 * user with three choices 1) get code by SMS. After that the application calls
 * psync_tfa_send_sms(), possibly display the returned phone number, wait for
 * code input and call psync_tfa_set_code(code, trusted, 0) 2) get code by
 * notification at any other logged in device. Application calls
 * psync_tfa_send_nofification(), possibly display logged devices list, wait for
 * code input and call psync_tfa_set_code(code, trusted, 0) 3) use recovery
 * code, in this case the user provides the recovery code and application calls
 * psync_tfa_set_code(code, trusted, 1)
 *
 * psync_tfa_has_devices() - can be called after PSTATUS_TFA_REQUIRED is
 * received and returns true if the user has other devices logged in
 *
 * psync_tfa_type() - can be called after PSTATUS_TFA_REQUIRED is received and
 * returns TFA type. 1 - msisdn 2 - google authenticator
 *
 * psync_tfa_send_sms() - sends SMS with two factor authentication code to the
 * phone number on file. If parameters country_code and phone_number are not
 * NULL, those are filled with user's phone number (split in two parts). In this
 * case the caller needs to free the returned values. Returns -1 in case of
 * network error or one of the positive error codes listed at
 * https://docsqa2.pcloud.com/methods/auth/tfa_sendcodeviasms.html
 *
 * psync_tfa_send_nofification() - sends notification with two factor
 * authentication code to all already logged in devices. If devices_list is not
 * null it is filled in with list of user's devices. In this case the caller is
 * supposed to free the list with a single call to free(). Returns -1 in
 * case of network error or one of the positive error codes listed at
 * https://docsqa2.pcloud.com/methods/auth/tfa_sendcodeviasysnotification.html
 *
 * psync_tfa_set_code() - sets the two factor code that is to be used for
 * logging in. If "trusted" is set, this device will be marked trusted and will
 * not require two factor authentication in the future. The "is_recovery"
 * parameter is supposed to be set if a recovery code is used and be zero in
 * case of SMS or notification code. Note that the function is void. Bad code
 * will be signalled with status change to PSTATUS_BAD_LOGIN_DATA. In that case
 * the login procedure can either be retried from the beggining or from
 * sending/providing two factor authentication code.
 *
 */

int psync_tfa_has_devices();
int psync_tfa_type();
int psync_tfa_send_sms(char **country_code, char **phone_number);
int psync_tfa_send_nofification(plogged_device_list_t **devices_list);
plogged_device_list_t *psync_tfa_send_nofification_res();
void psync_tfa_set_code(const char *code, int trusted, int is_recovery);

/* psync_add_sync_by_path and psync_add_sync_by_folderid are to be used to add a
 * folder to be synced, on success syncid is returned, on error
 * PSYNC_INVALID_SYNCID. The value of synctype should always be one of
 * PSYNC_DOWNLOAD_ONLY, PSYNC_UPLOAD_ONLY or PSYNC_FULL.
 *
 * psync_add_sync_by_path_delayed generally works in a way similar to
 * psync_add_sync_by_path, but with few differences: 1) it can be called just
 * after psync_init() and before psync_start_sync(), even before logging in and
 * downloading account state 2) actual creation of the sync will be delayed
 * until login and state download 3) if remotepath does not exist, it will be
 * created if possible (it is generally possible to create any path unless some
 * prefix of the path is a mounted share with no create privileges) 4) in rare
 * cases when remote path does not exists and could not be created, whole
 * psync_add_sync_by_path_delayed request will be silently discarded 5)
 * psync_add_sync_by_path_delayed does not return syncid, and can only fail if
 * there is some problem with localpath
 *
 * psync_change_synctype changes the sync type, on success returns 0 and -1 on
 * error.
 *
 * psync_delete_sync deletes the sync relationship between folders, on success
 * returns 0 and -1 on error (it is only likely to fail if syncid is invalid).
 * No files or folders are deleted either in the cloud or locally.
 *
 * psync_get_sync_list returns all folders that are set for sync. On error
 * returns NULL. On success the returned pointer is to be free()d.
 *
 */

psync_syncid_t psync_add_sync_by_path(const char *localpath,
                                      const char *remotepath,
                                      psync_synctype_t synctype);
psync_syncid_t psync_add_sync_by_folderid(const char *localpath,
                                          psync_folderid_t folderid,
                                          psync_synctype_t synctype);
int psync_add_sync_by_path_delayed(const char *localpath,
                                   const char *remotepath,
                                   psync_synctype_t synctype);
int psync_change_synctype(psync_syncid_t syncid, psync_synctype_t synctype);
int psync_delete_sync(psync_syncid_t syncid);
psync_folder_list_t *psync_get_sync_list();

psuggested_folders_t *psync_get_sync_suggestions();

// Backups
int psync_is_folder_syncable(char *localPath, char **errMsg);

// Gets a list of local syncs, based on their type. Type empty string means all.
// Accepts comma separated list of types example: 1,2,3
psync_folder_list_t *psync_get_syncs_bytype(const char *syncType);

// Create a backup for the local folder defined by path parameter.
int psync_create_backup(char *path, char **err);

// Deletes a local sync with the id passed in syncId. Stops the backup in the
// backen with the coresponding folderId.
int psync_delete_backup(psync_syncid_t syncId, char **errMsg);

// Stop all the backups of the device. Passing 0 for folderId stops the current
// device.
void psync_stop_device(psync_folderid_t folderId, char **errMsg);

// Returns the local machine name.
char *get_pc_name();

// Returns the name of the root backup folder of the machine.
char *get_backup_root_name();

// Asynchronous delete of local sync in thred, if a local sync with the folder
// id passed in fId parameter exists in the local DB
int psync_delete_sync_by_folderid(psync_folderid_t fId);

// Called when stop device is exceuted in the web, will delete the localy stored
// device id in order to create new one if backup is started again.
int psync_delete_backup_device(psync_folderid_t fId);

// Backup events
void psync_send_backup_del_event(psync_fileorfolderid_t remoteFId);

// Send async event
void psync_async_ui_callback(void *ptr);

// Backups

/* Use the following functions to list local or remote folders.
 * For local folders fileid and folderid will be set to a value that
 * should in general uniquely identify the entry (e.g. inode number).
 * Remote paths use slashes (/) and start with one.
 * In case of success the returned folder list is to be freed with a
 * single call to free(). In case of error NULL is returned. Parameter
 * listtype should be one of PLIST_FILES, PLIST_FOLDERS or PLIST_ALL.
 *
 * Folders do not contain "." or ".." entries.
 *
 * All files/folders are listed regardless if they are to be ignored
 * based on 'ignorepatterns' setting. If needed, pass the names to
 * psync_is_name_to_ignore that returns 1 for files that are to be
 * ignored and 0 for others.
 *
 * Remote root folder has 0 folderid.
 */

pfolder_list_t *psync_list_local_folder_by_path(const char *localpath,
                                                psync_listtype_t listtype);
pfolder_list_t *psync_list_remote_folder_by_path(const char *remotepath,
                                                 psync_listtype_t listtype);
pfolder_list_t *psync_list_remote_folder_by_folderid(psync_folderid_t folderid,
                                                     psync_listtype_t listtype);
pentry_t *psync_stat_path(const char *remotepath);
psync_folderid_t psync_get_fsfolderid_by_path(const char *path,
                                              uint32_t *pflags,
                                              uint32_t *pPerm);
uint32_t psync_get_fsfolderflags_by_id(psync_folderid_t folderid,
                                       uint32_t *pPerm);
int psync_is_lname_to_ignore(const char *name, size_t namelen);
int psync_is_name_to_ignore(const char *name);

/* Returns the code of the last error that occured when calling psync_*
 * functions in the given thread. The error is one of PERROR_* constants.
 */

uint32_t psync_get_last_error();

/* Pause stops the sync, but both local and remote directories are still
 * monitored for updates and status updates are still received with updated
 * filestoupload/filestodownload and others.
 *
 * Stop stops all the actions of the library. No network traffic and no local
 * scans are to be expected after this call. No status updates callback except
 * the one setting PSTATUS_STOPPED status.
 *
 * Resume will restart all operations in both paused and stopped state.
 */

int psync_pause();
int psync_stop();
int psync_resume();

/* Forces rescan of local files and folders. You generally don't need to call
 * this function. It can be useful only as an option for the user of the program
 * to force local re-scan.
 */

void psync_run_localscan();

/* Registers a new user account. email is user e-mail address which will also be
 * the username after successful registration. Password is user's chosen
 * password implementations are advised to have the user verify the password by
 * typing it twice. The termsaccepted field should only be set to true if the
 * user actually indicated acceptance of pCloud terms and conditions.
 *
 * binapi is the binapi selected in the registration form and locationid is the
 * one coresponding to that binapi.
 *
 * Returns zero on success, -1 if network error occurs or a positive error code
 * from this list: https://docs.pcloud.com/methods/auth/register.html In case of
 * error.
 *
 * If err is not NULL in all cases of non-zero return it will be set to point to
 * a psync_malloc-allocated buffer with English language error text, suitable to
 * display to the user. This buffer must be freed by the application.
 *
 */

int psync_register(const char *email, const char *password, int termsaccepted,
                   const char *binapi, unsigned int locationid, char **err);

/* Sends email verification mail to the user, return value and err are the same
 * as with registering.
 */

int psync_verify_email(char **err);

/* Upon seeing a status of PSTATUS_VERIFY_REQUIRED an error should appear in the
 * client, showing that access is restricted with "Cancel" and "Verify" options.
 *
 * psync_verify_email_restricted() - Sends verification email to the user.
 * Returns zero on success, -1 if network error occurs or a positive error code
 * from this list:
 * https://docsqa2.pcloud.com/methods/auth/sendverificationemail.html
 * In case of error.
 *
 * If err is not NULL in all cases of non-zero return it will be set to point to
 * a psync_malloc-allocated buffer with English language error text, suitable to
 * display to the user. This buffer must be freed by the application.
 */

int psync_verify_email_restricted(char **err);

/* Sends email with link to reset password to the user with specified email,
 * return value and err are the same as with registering.
 */

int psync_lost_password(const char *email, char **err);

/* Changes the password of the user, return value and err are the same as with
 * registering.
 */

int psync_change_password(const char *currentpass, const char *newpass,
                          char **err);

int psync_create_remote_folder_by_path(const char *path, char **err);

int psync_create_remote_folder(psync_folderid_t parentfolderid,
                               const char *name, char **err);

/* Returns auth string of the current user. Do not free the returned string
 * (which should be obvious from the const anyway).
 */

const char *psync_get_auth_string();

/*
 * List of settings:
 * usessl (bool) - use SSL connections to remote servers
 * maxdownloadspeed (int) - maximum download speed in bytes per second, 0 for
 * auto-shaper, -1 for no limit maxuploadspeed (int) - maximum upload speed in
 * bytes per second, 0 for auto-shaper, -1 for no limit minlocalfreespace (uint)
 * - minimum free space on local drives to run downloads ignorepatterns (string)
 * - patterns of files and folders to be ignored when syncing, separated by ";"
 * supported widcards are
 *                          * - matches any number of characters (even zero)
 *                          ? - matches exactly one character
 * p2psync (bool) - use or not peer to peer downloads
 *
 * fscachesize (uint) - size of filesystem cache, in bytes, sane minimum of few
 * tens of Mb or even hundreds is advised fsroot (string) - where to mount the
 * filesystem autostartfs (bool) - if set starts the fs on app startup
 * sleepstopcrypto (bool) - if set, stops crypto when computer wakes up from
 * sleep
 *
 *
 * The following functions operate on settings. The value of
 * psync_get_string_setting does not have to be freed, however if you are going
 * to store it rather than use it right away, you should strdup() it.
 *
 * psync_set_*_setting functions return 0 on success and -1 on failure. Setting
 * a setting may fail if you mismatch the type or give invalid setting name.
 *
 * psync_get_string_setting returns empty string on failure (type mismatch or
 * non-existing setting), all other psync_get_*_setting return zero on failure.
 *
 * psync_reset_setting resets setting to default value. Returns 0 on success and
 * -1 on error. Currently only "ignorepatterns" and "ignorepaths" are supported.
 *
 * All settings are reset to default values on unlink.
 *
 * int and uint are interchangeable and are considered same type.
 *
 */

int psync_get_bool_setting(const char *settingname);
int psync_set_bool_setting(const char *settingname, int value);
int64_t psync_get_int_setting(const char *settingname);
int psync_set_int_setting(const char *settingname, int64_t value);
uint64_t psync_get_uint_setting(const char *settingname);
int psync_set_uint_setting(const char *settingname, uint64_t value);
const char *psync_get_string_setting(const char *settingname);
int psync_set_string_setting(const char *settingname, const char *value);
int psync_reset_setting(const char *settingname);

/*
 * Values are like settings, except that you can store and retrieve any
 * key-value pair you want. There are some library-polpulated values that you
 * are not supposed to change. There are no type mismatch for values, instead
 * they are converted to requested representation.
 *
 * The pointer returned by psync_get_string_value is to be freed by the
 * application. This function returns NULL when value does not exist (as opposed
 * to psync_get_string_setting).
 *
 * The application can store values even when there is no user logged in.
 * However all values are cleared on unlink.
 *
 * Library-populated values are:
 * dbversion (uint) - version of the database
 * runstatus (uint) - one of (1, 2, 4) for current run status of (run, pause,
 * stop) saveauth  (bool) - indicates whether user've chosen to save the
 * password or not userid    (uint) - userid of the logged in user username
 * (string)- username/email of the logged in user emailverified (bool)-
 * indicates whether the user's email is verified premium   (bool) - true if
 * user is paid account premiumexpires (uint) - if premium is true, the expire
 * date of the premium account in unix timestamp language  (string) - two
 * letter, lowercase ISO 639-1 code of the user's language preference quota
 * (uint) - user's quota in bytes usedquota (uint) - used space of the quota in
 * bytes freequota (uint) - maximum quota that free user can obtain diffid
 * (uint) - diffid of the user status, see
 * https://docs.pcloud.com/methods/general/diff.html, can be used to detect
 * account changes auth (string) - user's auth token (if the user is logged in
 * and saveauth is true), see
 * https://docs.pcloud.com/methods/intro/authentication.html plan (uint) -
 * user's plan id business (bool) - If true the user is part of a business
 * account. Always provided vivapcloud (bool) - If it's Viva pCloud user it will
 * be true otherwise the parameter won't be provided premiumlifetime (bool) - If
 * true user's premium is life time. Always provided owner (bool) - True if the
 * user is owner of a family plan. Presented only for user with family plan.
 * hasactivesubscription (bool) - True if the user has active subscription.
 */

int psync_has_value(const char *valuename);
int psync_get_bool_value(const char *valuename);
void psync_set_bool_value(const char *valuename, int value);
int64_t psync_get_int_value(const char *valuename);
void psync_set_int_value(const char *valuename, int64_t value);
uint64_t psync_get_uint_value(const char *valuename);
void psync_set_uint_value(const char *valuename, uint64_t value);
char *psync_get_string_value(const char *valuename);
void psync_set_string_value(const char *valuename, const char *value);

/* If your application has a way to detect network change (e.g. wireless access
 * point change), you should subscribe for such notifications and call
 * psync_network_exception() in those cases. There is no harm in calling it too
 * often.
 *
 */

void psync_network_exception();

/* The following functions return lists of pending sharerequests in
 * psync_list_sharerequests and list of shared folders in case of
 * psync_list_shares. Memory is to be freed with a single free().
 *
 * These functions do not return errors. However, if no user is logged in, they
 * will return empty lists.
 *
 * Pass 1 as parameter to list incoming sharerequests/shares and 0 for outgoing.
 *
 * Listing shares/sharerequests do not require active network connection.
 *
 */

psync_sharerequest_list_t *psync_list_sharerequests(int incoming);
psync_share_list_t *psync_list_shares(int incoming);

/* psync_share_folder shares a folder with the user "mail". The "permissions"
 * parameter is bitwise or of PSYNC_PERM_READ, PSYNC_PERM_CREATE,
 * PSYNC_PERM_MODIFY and PSYNC_PERM_DELETE (PSYNC_PERM_READ is actually ignored
 * and always set).
 *
 * On success returns 0, otherwise returns API error number (or -1 on network
 * error) and sets err to a string error message if it is not NULL. This string
 * should be freed if the return value is not 0 and err is not NULL.
 *
 * It is NOT guaranteed that upon successful return psync_list_sharerequests(0)
 * will return the newly created share request. Windows showing list of
 * sharerequests/shares are supposed to requery shares/request upon receiving of
 * PEVENT_SHARE_* event. That is true for all share management functions.
 *
 */

int psync_share_folder(psync_folderid_t folderid, const char *name,
                       const char *mail, const char *message,
                       uint32_t permissions, char **err);

/* psync_crypto_share_folder shares a crypto folder with the user "mail". The
 * "permissions" parameter is bitwise or of PSYNC_PERM_READ, PSYNC_PERM_CREATE,
 * PSYNC_PERM_MODIFY and PSYNC_PERM_DELETE (PSYNC_PERM_READ is actually ignored
 * and always set). The "temppass" parameter is used to create a temporary
 * crypto pass for the provided user if the user doesn't have activated crypto
 * an if left empty no attempt to create a temporary pass is made by the
 * function.
 *
 * On success returns 0, otherwise returns API error number (or -1 on network
 * error) and sets err to a string error message if it is not NULL. This string
 * should be freed if the return value is not 0 and err is not NULL. The
 * function can also return PSYNC_CRYPTO_NOT_STARTED or PERROR_NO_MEMORY errors.
 *
 * It is NOT guaranteed that upon successful return psync_list_sharerequests(0)
 * will return the newly created share request. Windows showing list of
 * sharerequests/shares are supposed to requery shares/request upon receiving of
 * PEVENT_SHARE_* event. That is true for all share management functions.
 *
 */
int psync_crypto_share_folder(psync_folderid_t folderid, const char *name,
                              const char *mail, const char *message,
                              uint32_t permissions, char *hint, char *temppass,
                              char **err);

/* Cancels a share request (this is to be called for outgoing requests).
 *
 * Return value same as psync_share_folder.
 */

int psync_cancel_share_request(psync_sharerequestid_t requestid, char **err);

/* Declines a share request (this is to be called for incoming requests).
 *
 * Return value same as psync_share_folder.
 */

int psync_decline_share_request(psync_sharerequestid_t requestid, char **err);

/* Accepts a share request to a folder "tofolderid" under a name "name". If
 * "name" is NULL then the original share name is used.
 *
 * Return value same as psync_share_folder.
 */

int psync_accept_share_request(psync_sharerequestid_t requestid,
                               psync_folderid_t tofolderid, const char *name,
                               char **err);

/* Removes established share. Can be called by both receiving and sharing user.
 *
 * Return value same as psync_share_folder.
 */

int psync_remove_share(psync_shareid_t shareid, char **err);

/* Removes established business account share.
 *
 * Return value same as psync_share_folder.
 */
int psync_account_stopshare(psync_shareid_t shareid, char **err);

/* Removes established share. Can be called by both receiving and sharing user.
 *
 * Return value same as psync_share_folder.
 */

int psync_modify_share(psync_shareid_t shareid, uint32_t permissions,
                       char **err);

/* The following function check for new version of the application. Return NULL
 * if there is no new version or psync_new_version_t structure if a new version
 * is available. Returned value is to be freed with a single free(). The os
 * parameter is one of the following: WIN WIN_XP MAC LINUX32 LINUX64
 *
 * String versions are in format "a.b.c", equivalen numeric version is
 * a*10000+b*100+c
 *
 * The _download version also downloads (and is potentially slow) the update and
 * stores it in (returned value)->localpath.
 *
 * Function psync_run_new_version actually runs the update. On success it exists
 * and does not return.
 *
 * Applications are expected to run psync_check_new_version and upon non-NULL
 * return to seek user confirmation for updating and if the user confirms run
 * psync_run_new_version
 *
 */

psync_new_version_t *psync_check_new_version_str(const char *os,
                                                 const char *currentversion);
psync_new_version_t *psync_check_new_version(const char *os,
                                             unsigned long currentversion);
psync_new_version_t *
psync_check_new_version_download_str(const char *os,
                                     const char *currentversion);
psync_new_version_t *
psync_check_new_version_download(const char *os, unsigned long currentversion);
void psync_run_new_version(psync_new_version_t *ver);

/* The following functions provide simplified interface to file upload. While no
 * actual limit is enforced, they are targeted for immediate upload of
 * relatively small files (up to few tens of megabytes). These functions:
 * - do not obey upload speed limits
 * - do not obey stopped/paused state
 * - do not increase/touch the number of "files to upload", therefore the status
 * may be "in sync" while these functions actually upload data
 * - just try to instantly upload the file regardless of number of files queued
 * up by either/both sync or drive
 * - do not support resume of failed uploads
 * - overwrite already existing target files
 * - functions that work with local files allocate memory equal to the file size
 * (to deal with race conditions of changing files)
 *
 * All functions return 0 upon success, -1 upon network error, -2 if the local
 * file can not be read or a positive API error code (see
 * https://docs.pcloud.com/methods/file/uploadfile.html).
 */

int psync_upload_data(psync_folderid_t folderid, const char *remote_filename,
                      const void *data, size_t length, psync_fileid_t *fileid);
int psync_upload_data_as(const char *remote_path, const char *remote_filename,
                         const void *data, size_t length,
                         psync_fileid_t *fileid);
int psync_upload_file(psync_folderid_t folderid, const char *remote_filename,
                      const char *local_path, psync_fileid_t *fileid);
int psync_upload_file_as(const char *remote_path, const char *remote_filename,
                         const char *local_path, psync_fileid_t *fileid);

/* Filesystem functions.
 *
 * psync_fs_start() - starts the filesystem
 * psync_fs_isstarted() - returns 1 if the filesystem is started and 0 otherwise
 * psync_fs_stop() - stops the filesystem
 * psync_fs_getmountpoint() - returns current mountpoint of the filesystem, or
 * NULL if the filesystem is not mounted, you are supposed to free the returned
 * pointer psync_fs_register_start_callback() - registers a callback that will
 * be called once the drive is started psync_fs_get_path_by_folderid() - returns
 * full path (including mountpoint) of a given folderid on the filesystem or
 *                            NULL if it is not mounted or folder could not be
 * found. You are supposed to free the returned pointer.
 * pfolder_file_path() - returns path (without mountpoint) of a given
 * fileid on the filesystem or NULL if it is not mounted or parent folder could
 * not be found. You are supposed to free the returned pointer.
 *
 * psync_fs_clean_read_cache() - cleans the filesystem read cache. This function
 * does not fail. The general expectation is that the function takes some
 * moderate time to execute - maybe 0.5-2 seconds depending on the system and
 * it's load. In the unlikely case of cache flush or cache garbage collection
 * operations are in progress, it may take more time (~10 seconds maybe) to
 * clean the read cache. During the cleaning almost all library functions and
 * filesystem operations will hang and wait for the process to finish, so please
 * design the UI accordingly.
 *
 * psync_fs_move_cache()      - cleans filesystem read cache and moves both read
 * cache and write queue to specified directory. The function has the same
 * overall complexity as psync_fs_clean_read_cache(). Returns 0 on success,
 * PERROR_CACHE_MOVE_NOT_EMPTY if the target directory already has a read cache
 * file in it, PERROR_CACHE_MOVE_NO_WRITE_ACCESS if request to open a file for
 * writing in the provided directory fails or PERROR_CACHE_MOVE_DRIVE_HAS_TASKS
 * if Drive's task queue is not empty. Putting the cache on a remote drive is
 * generally not a good idea.
 *
 */

int psync_fs_start();
int psync_fs_isstarted();
void psync_fs_stop();
char *psync_fs_getmountpoint();
void psync_fs_register_start_callback(psync_generic_callback_t callback);
char *psync_fs_get_path_by_folderid(psync_folderid_t folderid);

void psync_fs_clean_read_cache();
int psync_fs_move_cache(const char *path);

/* psync_password_quality estimates password quality, returns one of:
 *   0 - weak
 *   1 - moderate
 *   2 - strong
 */

int psync_password_quality(const char *password);

/* psync_password_quality10000 works the same way as psync_password_quality but
 * for each password strength also return a range, returns integer in one of the
 * following (inclusive) intervals 0     to  9999 - weak password 10000 to 19999
 * - moderate password 20000 to 29999 - strong password
 *
 * integer division of the result of psync_password_quality10000 by 10000 will
 * give the same result as psync_password_quality()
 *
 */

int psync_password_quality10000(const char *password);

/* psync_derive_password_from_passphrase() - derives API password from
 * passphrase of cases where single password login is desired. Returned string
 * is to be free()-d.
 *
 */
char *psync_derive_password_from_passphrase(const char *username,
                                            const char *passphrase);

/*
 * Crypto functions.
 *
 * psync_crypto_setup() - setups crypto with a given password, on error returns
 one of PSYNC_CRYPTO_SETUP_* errors
 * psync_crypto_get_hint() - if successful sets *hint to point to a string with
 the user's password hint. In this case
 *                        *hint is to be free-d. On error one of
 PSYNC_CRYPTO_HINT_* codes is returned and *hint is not
 *                        set.
 * psync_crypto_start() - starts crypto with a given password, on error returns
 one of PSYNC_CRYPTO_START_* errors
 * psync_crypto_stop() - stops crypto, on error returns one of
 PSYNC_CRYPTO_STOP_* errors
 * psync_crypto_isstarted() - returns 1 if crypto is started and 0 otherwise
 * psync_crypto_mkdir() - creates encrypted folder with name in folderid. If the
 parent
 *                        folder is not encrypted itself the folder name will be
 stored in plaintext
 *                        and only the contents will be encrypted. Returns 0 for
 success and sets *newfolderid (if newfolderid is
 *                        non-NULL) to the id of the new folder, or non-zero on
 error. Negative error values are local and positive
 *                        error  values are API error codes. If err is not null
 it is set to point to an static error string
 *                        message that you do NOT have to free.
 * psync_crypto_issetup() - returns 1 if crypto is set up or 0 otherwise
 * psync_crypto_hassubscription() - returns 1 if the user have active payment
 subscription for crypto or 0 otherwise
 * psync_crypto_isexpired() - returns 1 if the users crypto service is expired
 or 0 otherwise. Note that it also returns
 *                        0 when the user never set up crypto and is therefore
 eligible for a trial account.
 * psync_crypto_expires() - returns unix timestamp with the date of current
 crypto service expiration. The returned value
 *                        may be in the past, meaning expired service. If crypto
 has never been setup for this account
 *                        this functions returns 0.
 * psync_crypto_reset() - reset user's crypto, which means that all encrypted
 files and folders get deleted. This function
 *                        does not directly reset user's account, a confirmation
 email is first sent to the user.
 * psync_crypto_folderid() - returns the id of the first encrypted folder it
 finds. If no encrypted folder is found the function returns
 *                        PSYNC_CRYPTO_INVALID_FOLDERID.
 * psync_crypto_folderids() - returns array of the ids of all encrypted folders
 (but not their subfolders). Last element of the array is
 *                        always PSYNC_CRYPTO_INVALID_FOLDERID. You need to free
 the memory returned by this function.
 * int psync_crypto_crypto_send_change_user_private() - Request sending of code
 for changing the private key password. Possible erroe codes are:
 *                        PSYNC_CRYPTO_SETUP_CANT_CONNECT,
 PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR.
 * psync_crypto_change_crypto_pass() - Re-encodes the private key with the new
 pasword provided and makes an API call to upload it.
 *                        On success returns PSYNC_CRYPTO_SETUP_SUCCESS.
 Possible errors are PSYNC_CRYPTO_BAD_PASSPHRASE, PERROR_NET_ERROR,
 *                        PERROR_NO_MEMORY, PSYNC_CRYPTO_BAD_KEY,
 PSYNC_CRYPTO_SETUP_CANT_CONNECT and PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR.
 *                        This function does not care whether the crypto is
 locked or unlocked.
 *                        Note: This function doesn't care if we are
 authenticated.
 * psync_crypto_change_crypto_pass_unlocked() - Re-encodes the private key
 loaded in the memory with the new pasword provided and makes an
 *                        API call to upload it. On success returns
 PSYNC_CRYPTO_SETUP_SUCCESS. Possible errors are PSYNC_CRYPTO_NOT_STARTED,
 *                        PSYNC_CRYPTO_BAD_PASSPHRASE, PERROR_NET_ERROR,
 PERROR_NO_MEMORY, PSYNC_CRYPTO_BAD_KEY and PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR.
 *                        In order to work, this function requires the crypto to
 be unlocked.
 * psync_crypto_priv_key_flags() - Read private key flags from the DB. The only
 possible flag for the moment is PSYNC_CRYPTO_FLAG_TEMP_PASS

 */

int psync_crypto_get_hint(char **hint);
int psync_crypto_mkdir(psync_folderid_t folderid, const char *name,
                       const char **err, psync_folderid_t *newfolderid);
int psync_crypto_hassubscription();
int psync_crypto_isexpired();
time_t psync_crypto_expires();
int psync_crypto_reset();
psync_folderid_t psync_crypto_folderid();
psync_folderid_t *psync_crypto_folderids();
int psync_crypto_crypto_send_change_user_private();
int psync_crypto_change_crypto_pass(const char *oldpass, const char *newpass,
                                    const char *hint, const char *code);
int psync_crypto_change_crypto_pass_unlocked(const char *newpass,
                                             const char *hint,
                                             const char *code);
uint64_t psync_crypto_priv_key_flags();
/*
 * Status functions.
 *
 * All status functions take path and return corresponding file or folder
 * status. Possible statuses are  INSYNC means everything is OK, INPROG -
 * synchronization in progress, NOSYNC - file or folder not synced.
 *
 * psync_status_file() returns the status of a file in pCloud drive. Path is
 * given from the mount point of the drive. psync_status_folder() returns the
 * status of a folder in pCloud drive. Path is given from the mount point of the
 * drive. psync_filesystem_status() returns the status of a folder or a folder
 * in pCloud drive of file system. Path is the absolute path including mount
 * point of the drive and/or drive letter. Can be used for synced folders. For
 * files and folders not in drive or sync folder INSYNC is returned.
 */

external_status_t psync_filesystem_status(const char *path);
external_status_t psync_status_file(const char *path);
external_status_t psync_status_folder(const char *path);

/*
 * Publik links API functions.
 *
 * psync_file_public_link() creates public link for a file. Returns link id or
 * negative error number. The path parameter is pcloud drive path. The link is
 * pointer where generated link is returned. The err is parameter where
 * printable text of api error if any is returned.
 *
 *
 *
 * psync_folder_updownlink_link() creates upload and download public link for a
 * folder. Returns link id or negative error number. The folderid is id of the
 * folder that is shared. The err is parameter where printable text of api error
 * if any is returned.
 *
 *
 * ptree_public_link() creates public link for a tree. Tree is define by
 * root folder and arrays of folders and file paths. Each entry in the arrays
 *  describes a path to file or folder. Number of entries in the arrays is
 * passed separately. The API constructs a virtual folder of this files and
 * folders and if root is passed it will serve as root folder for this virtual
 * folder so name is mandatory. you can omit any of the other parameters.
 *  Returns link id or negative error number.
 *  The link is pointer where generated link is returned.
 *  The err is parameter where printable text of api error if any is returned.
 *
 *
 * psync_delete_link() Deletes a public link by linkid or returns negative
 * number and upon API failure a string representation of the error.
 *
 * psync_list_links() Lists all public links in the account or returns negative
 * number and upon API failure a string representation of the error. Same
 * structure used for listing public and upload links only comment and maxspace
 * are set to 0 in public links list.
 *
 * psync_upload_link() Creates upload link to given folder. Comment is mandatory
 * parameter as it's the only information the user sees.
 *
 * psync_delete_upload_link(uploadlinkid) Deletes a upload link by uploadlinkid
 * or returns negative number and upon API failure a string representation of
 * the error.
 *
 * psync_list_upload_links() Lists all public links in the account or returns
 * negative number and upon API failure a string representation of the error.
 *   Same structure used for listing public and upload links only comment and
 * maxspace are set to 0 in public links list. Space parameter is filled in
 * traffic and files in downloads.
 *
 * psync_sow_link() Lists link contents. Returns list of contents for folders
 * and virtial folders or empty pointer and err is filled with string
 * representation of the error.
 *
 * psync_delete_all_links_folder() Deletes all link for given folderid. Stops on
 * first error and returns error msg. psync_delete_all_links_file() Deletes all
 * link for given fileid.  Stops on first error and returns error msg.
 *
 * REMINDER. You have to free the out parameters passed as pointers to the
 * library as it reserves memory for them but does not cleans it. You will have
 * to iterate though entire entires[] array and free all codes and names and
 * comments if not empty before feeing entire info with separate call.
 *
 */

int64_t psync_file_public_link(const char *path, char **link /*OUT*/,
                               char **err /*OUT*/);
int64_t psync_folder_public_link(const char *path, char **link /*OUT*/,
                                 char **err /*OUT*/);
int64_t psync_folder_public_link_full(const char *path, char **link /*OUT*/,
                                      char **err /*OUT*/,
                                      unsigned long long expire,
                                      int maxdownloads, int maxtraffic,
                                      const char *password);
int64_t psync_folder_updownlink_link(int canupload, unsigned long long folderid,
                                     const char *mail, char **err /*OUT*/);
int64_t ptree_public_link(const char *linkname, const char *root,
                               char **folders, int numfolders, char **files,
                               int numfiles, char **link /*OUT*/,
                               char **err /*OUT*/);
plink_info_list_t *psync_list_links(char **err /*OUT*/);
plink_contents_t *psync_show_link(const char *link, char **err /*OUT*/);
int psync_delete_link(int64_t linkid, char **err /*OUT*/);
int psync_change_link(unsigned long long linkid, unsigned long long expire,
                      int delete_expire, const char *linkpassword,
                      int delete_password, unsigned long long maxtraffic,
                      unsigned long long maxdownloads,
                      int enableuploadforeveryone,
                      int enableuploadforchosenusers, int disableupload,
                      char **err);
int64_t psync_upload_link(const char *path, const char *comment,
                          char **link /*OUT*/, char **err /*OUT*/);
int psync_delete_upload_link(int64_t uploadlinkid, char **err /*OUT*/);

int psync_delete_all_links_folder(psync_folderid_t folderid, char **err);
int psync_delete_all_links_file(psync_fileid_t fileid, char **err);

void psync_cache_links_all();
/*
 * Creates download link for newly uploaded screenshot and sets the expiration
 * to current date plus delay seconds. If hasdelay equals 0 no expiration is
 * set. If hasdelay and delay is 0 expiration is for one mount
 */
int64_t psync_screenshot_public_link(const char *path, int hasdelay,
                                     int64_t delay, char **link /*OUT*/,
                                     char **err /*OUT*/);

/*
 * psync_list_email_with_access List email and recieverid for each user who can
 * upload to the speciefied link. linkid the id of the link for which the users
 * with upload rights are listed.
 *
 * psync_link_add_access Add upload access to link specified by linkid
 * psync_link_remove_access Remove upload access of user specified by recieverid
 * to link specified by linkid psync_psync_change_link Changes settings of
 * download link
 *
 * psync_cache_bookmarks Returns list of bookmarked links.
 * psync_remove_bookmark Removes bookmark be given code and locationid of the
 * link.
 *
 * psync_change_link_expire Change expire date of link. Delete expire date if
 * expire equals 0. psync_change_link_password Change password of link. Delete
 * password date if password equals NULL. psync_change_link_enable_upload Allows
 * upload to a download link. If enableuploadforchosenusers is more than 0
 * upload is allowed for specified mails. If enableuploadforchosenusers is 0 and
 * enableuploadforeveryone is more than 0 upload is allowed for everyone. If
 * enableuploadforchosenusers and enableuploadforeveryone are 0 upload for the
 * link is disabled.
 */
preciever_list_t *psync_list_email_with_access(unsigned long long linkid,
                                               char **err);

int psync_link_add_access(unsigned long long linkid, const char *mail,
                          char **err);

int psync_link_remove_access(unsigned long long linkid,
                             unsigned long long receiverid, char **err);

bookmarks_list_t *psync_cache_bookmarks(char **err);
int psync_remove_bookmark(const char *code, int locationid, char **err);
int psync_change_bookmark(const char *code, int locationid, const char *name,
                          const char *description, char **err);

int psync_psync_change_link(unsigned long long linkid,
                            unsigned long long expire, int delete_expire,
                            const char *linkpassword, int delete_password,
                            unsigned long long maxtraffic,
                            unsigned long long maxdownloads,
                            int enableuploadforeveryone,
                            int enableuploadforchosenusers, int disableupload,
                            char **err);

int psync_change_link_expire(unsigned long long linkid,
                             unsigned long long expire, char **err);
int psync_change_link_password(unsigned long long linkid, const char *password,
                               char **err);
int psync_change_link_enable_upload(unsigned long long linkid,
                                    int enableuploadforeveryone,
                                    int enableuploadforchosenusers, char **err);

/*
 * Publik contacts API functions.
 *
 * psync_list_contacts() Lists cached contacts emails from the buissiness
 * account and team names.
 *
 * psync_list_myteams() Lists cached teams that you are member of. Returns same
 * structure like psync_list_contacts only type 3 records filled.
 * */

pcontacts_list_t *psync_list_contacts();

pcontacts_list_t *psync_list_myteams();

/* account_teamshare shares a folder with business account team a. The
 * "permissions" parameter is bitwise or of PSYNC_PERM_READ, PSYNC_PERM_CREATE,
 * PSYNC_PERM_MODIFY and PSYNC_PERM_DELETE (PSYNC_PERM_READ is actually ignored
 * and always set) and PSYNC_PERM_MANAGE.
 *
 * On success returns 0, otherwise returns API error number (or -1 on network
 * error) and sets err to a string error message if it is not NULL. This string
 * should be freed if the return value is not 0 and err is not NULL.
 *
 * It is NOT guaranteed that upon successful return psync_list_sharerequests(0)
 * will return the newly created share request. Windows showing list of
 * sharerequests/shares are supposed to requery shares/request upon receiving of
 * PEVENT_SHARE_* event. That is true for all share management functions.
 *
 */

int psync_account_teamshare(psync_folderid_t folderid, const char *name,
                            psync_teamid_t teamid, const char *message,
                            uint32_t permissions, char **err);

/* account_teamshare shares a folder with business account team a. The
 * "permissions" parameter is bitwise or of PSYNC_PERM_READ, PSYNC_PERM_CREATE,
 * PSYNC_PERM_MODIFY and PSYNC_PERM_DELETE (PSYNC_PERM_READ is actually ignored
 * and always set) and PSYNC_PERM_MANAGE. The "temppass" parameter is used to
 * create a temporary crypto pass for the users in team without activated crypto
 * if left empty no attempt to create a temporary pass is made by the function.
 *
 * On success returns 0, otherwise returns API error number (or -1 on network
 * error) and sets err to a string error message if it is not NULL. This string
 * should be freed if the return value is not 0 and err is not NULL.
 *
 * It is NOT guaranteed that upon successful return psync_list_sharerequests(0)
 * will return the newly created share request. Windows showing list of
 * sharerequests/shares are supposed to requery shares/request upon receiving of
 * PEVENT_SHARE_* event. That is true for all share management functions.
 *
 */
int psync_crypto_account_teamshare(psync_folderid_t folderid, const char *name,
                                   psync_teamid_t teamid, const char *message,
                                   uint32_t permissions, char *hint,
                                   char *temppass, char **err);

/* psync_register_account_events_callback Registers a callback to be notified
 * upon invalidation of the account cache information. Different notifications
 * are: Links, team, team users emails, contacts or all.
 */
void psync_register_account_events_callback(paccount_cache_callback_t callback);

void psync_register_backup_events_callback(pevent_callback_t callback);

void psync_get_current_userid(psync_userid_t * /*OUT*/ ret);
void psync_get_folder_ownerid(psync_folderid_t folderid,
                              psync_userid_t * /*OUT*/ ret);

/* Registers file manager extension callback that will be called when packet
 * with id equals to the give one had arrived from extension. The id must be
 * over or equal to 20 or -1 will be returned. There is a hard coded maximum of
 * menu items on some OS-s so maximum of 15 ids are available. Value of -2 is
 * returned when id grater then 35 and 0 returned on success.
 *
 * WARNING this functions are not thread-safe. Use them in single thread or
 * synchronize.
 */

int psync_setlanguage(const char *language, char **err);

// Update crypto status information from userinfo.
void psync_update_cryptostatus();

// Checks and creates new folder with write permissions on it and adds suffix to
// the name if necessary i.e. New Folder (1) etc..
psync_folderid_t psync_check_and_create_folder(const char *path);

char *psync_get_token();

/*Devices monitoring functions
 */

// Adds device monitoring callback which is invoked every time a new not
// disabled device arrives.
//  void psync_add_device_monitor_callback(device_event_callback callback);
// Lists all stored devices
//  pdevice_item_list_t * psync_list_devices(char **err /*OUT*/);
// Enables device. This info is stored in the database so will be present after
// restart.
//  void penable_device(const char* device_id);
// Disable device
//  void pdisable_device(const char* device_id);
// Remove db information about device
//  void premove_device(const char* device_id);

/* Checks for promotions.
 * If url is empty there is no ptomotion.
 * Returns -1 in case of network error or
 * one of the positive error codes
 */
int psync_get_promo(char **url, uint64_t *width, uint64_t *height);
/*
 * Checks if the user has any crypto folders.
 */
int psync_has_crypto_folders();
void set_tfa_flag(int value);
/*
 *
 */
apiservers_list_t *psync_get_apiservers(char **err /*OUT*/);
void psync_set_apiserver(const char *binapi, uint32_t locationid);

/*send_publink send a download link via email
 * code - code of the downloadlink to be send
 * mail - email of the recipient of the download link
 * message - optional message for the recipient
 * err - out parameter. Returns the error message if any.
 */
int psync_send_publink(const char *code, const char *mail, const char *message,
                       char **err /*OUT*/);

userinfo_t *psync_get_userinfo();

/*
 * category - string representing category of the event
 * action - string representing action of the event
 * label - string representing label of the event
 * eventParams - list of binparams(optional)
 */
int psync_ptools_create_backend_event(const char *category, const char *action,
                               const char *label, eventParams params,
                               char *err);

/*
 * ptr - pointer to the data event handling function.
 */
void psync_init_data_event_handler(void *ptr);
#ifdef __cplusplus
}
#endif

// moved from pdiff
void psync_delete_cached_crypto_keys();


#endif
