/*
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include "InstalldNativeService.h"

#include <dirent.h>
#include <errno.h>
#include <fts.h>
#include <inttypes.h>
#include <linux/fsverity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <thread>
#include <unordered_set>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/ashmem.h>
#include <cutils/fs.h>
#include <cutils/misc.h>
#include <cutils/properties.h>
#include <cutils/sched_policy.h>
#include <linux/quota.h>
#include <log/log.h>               // TODO: Move everything to base/logging.
#include <logwrap/logwrap.h>
#include <private/android_filesystem_config.h>
#include <private/android_projectid_config.h>
#include <selinux/android.h>
#include <selinux/selinux.h>
#include <system/thread_defs.h>
#include <utils/Trace.h>

#include "dexopt.h"
#include "globals.h"
#include "installd_deps.h"
#include "utils.h"

#include "CacheTracker.h"
#include "CrateManager.h"
#include "MatchExtensionGen.h"
#include "QuotaUtils.h"
#include "SysTrace.h"

#include <android_installd_flags.h>

#ifndef LOG_TAG
#define LOG_TAG "installd"
#endif

#define GRANULAR_LOCKS

using android::base::Fdopendir;
using android::base::ParseUint;
using android::base::Split;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::os::ParcelFileDescriptor;
using std::endl;
namespace flags = android::installd::flags;

namespace android {
namespace installd {

// An uuid used in unit tests.
static constexpr const char* kTestUuid = "TEST";

static constexpr const mode_t kRollbackFolderMode = 0700;

static constexpr const char* kCpPath = "/system/bin/cp";
static constexpr const char* kXattrDefault = "user.default";

static constexpr const char* kDataMirrorCePath = "/data_mirror/data_ce";
static constexpr const char* kDataMirrorDePath = "/data_mirror/data_de";
static constexpr const char* kMiscMirrorCePath = "/data_mirror/misc_ce";
static constexpr const char* kMiscMirrorDePath = "/data_mirror/misc_de";

static constexpr const int MIN_RESTRICTED_HOME_SDK_VERSION = 24; // > M

static constexpr const char* PKG_LIB_POSTFIX = "/lib";
static constexpr const char* CACHE_DIR_POSTFIX = "/cache";
static constexpr const char* CODE_CACHE_DIR_POSTFIX = "/code_cache";

static constexpr const char* kFuseProp = "persist.sys.fuse";

/**
 * Property to control if app data isolation is enabled.
 */
static constexpr const char* kAppDataIsolationEnabledProperty = "persist.zygote.app_data_isolation";
static constexpr const char* kMntSdcardfs = "/mnt/runtime/default/";

static std::atomic<bool> sAppDataIsolationEnabled(false);

/**
 * Flag to control if project ids are supported for internal storage
 */
static std::atomic<bool> sUsingProjectIdsFlag(false);
static std::once_flag flag;

namespace {

static binder::Status ok() {
    return binder::Status::ok();
}

static binder::Status exception(uint32_t code, const std::string& msg) {
    LOG(ERROR) << msg << " (" << code << ")";
    return binder::Status::fromExceptionCode(code, String8(msg.c_str()));
}

static binder::Status error() {
    return binder::Status::fromServiceSpecificError(errno);
}

static binder::Status error(const std::string& msg) {
    PLOG(ERROR) << msg;
    return binder::Status::fromServiceSpecificError(errno, String8(msg.c_str()));
}

static binder::Status error(uint32_t code, const std::string& msg) {
    LOG(ERROR) << msg << " (" << code << ")";
    return binder::Status::fromServiceSpecificError(code, String8(msg.c_str()));
}

binder::Status checkUid(uid_t expectedUid) {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid == expectedUid || uid == AID_ROOT) {
        return ok();
    } else {
        return exception(binder::Status::EX_SECURITY,
                StringPrintf("UID %d is not expected UID %d", uid, expectedUid));
    }
}

binder::Status checkArgumentUuid(const std::optional<std::string>& uuid) {
    if (!uuid || is_valid_filename(*uuid)) {
        return ok();
    } else {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                StringPrintf("UUID %s is malformed", uuid->c_str()));
    }
}

binder::Status checkArgumentUuidTestOrNull(const std::optional<std::string>& uuid) {
    if (!uuid || strcmp(uuid->c_str(), kTestUuid) == 0) {
        return ok();
    } else {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                StringPrintf("UUID must be null or \"%s\", got: %s", kTestUuid, uuid->c_str()));
    }
}

binder::Status checkArgumentPackageName(const std::string& packageName) {
    if (is_valid_package_name(packageName)) {
        return ok();
    } else {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                StringPrintf("Package name %s is malformed", packageName.c_str()));
    }
}

binder::Status checkArgumentPath(const std::string& path) {
    if (path.empty()) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT, "Missing path");
    }
    if (path[0] != '/') {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                StringPrintf("Path %s is relative", path.c_str()));
    }
    if ((path + '/').find("/../") != std::string::npos) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                StringPrintf("Path %s is shady", path.c_str()));
    }
    for (const char& c : path) {
        if (c == '\0' || c == '\n') {
            return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                    StringPrintf("Path %s is malformed", path.c_str()));
        }
    }
    return ok();
}

binder::Status checkArgumentPath(const std::optional<std::string>& path) {
    if (path) {
        return checkArgumentPath(*path);
    } else {
        return ok();
    }
}

binder::Status checkArgumentFileName(const std::string& path) {
    if (path.empty()) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT, "Missing name");
    }
    for (const char& c : path) {
        if (c == '\0' || c == '\n' || c == '/') {
            return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                             StringPrintf("Name %s is malformed", path.c_str()));
        }
    }
    return ok();
}

binder::Status checkArgumentAppId(int32_t appId) {
    if (FIRST_APPLICATION_UID <= appId && appId <= LAST_APPLICATION_UID) {
        return ok();
    }
    return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                     StringPrintf("appId %d is outside of the range", appId));
}

binder::Status checkArgumentAppIdsPccIds(const std::vector<int32_t>& appIds,
                                         const std::vector<int32_t>& pccIds) {
    if (appIds.size() != pccIds.size()) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                         "appIds and pccIds are not of the same length");
    }
    return ok();
}

#define ENFORCE_UID(uid) {                                  \
    binder::Status status = checkUid((uid));                \
    if (!status.isOk()) {                                   \
        return status;                                      \
    }                                                       \
}

// we could have tighter checks, but this is only to avoid hard errors. Negative values are defined
// in UserHandle.java and carry specific meanings that may not be handled by certain APIs here.
#define ENFORCE_VALID_USER(userId)                                                               \
    {                                                                                            \
        if (static_cast<uid_t>(userId) >= std::numeric_limits<uid_t>::max() / AID_USER_OFFSET) { \
            return error("userId invalid: " + std::to_string(userId));                           \
        }                                                                                        \
    }

#define ENFORCE_VALID_USER_OR_NULL(userId)             \
    {                                                  \
        if (static_cast<uid_t>(userId) != USER_NULL) { \
            ENFORCE_VALID_USER(userId);                \
        }                                              \
    }

#define CHECK_ARGUMENT_UUID(uuid) {                         \
    binder::Status status = checkArgumentUuid((uuid));      \
    if (!status.isOk()) {                                   \
        return status;                                      \
    }                                                       \
}

#define CHECK_ARGUMENT_UUID_IS_TEST_OR_NULL(uuid) {         \
    auto status = checkArgumentUuidTestOrNull(uuid);        \
    if (!status.isOk()) {                                   \
        return status;                                      \
    }                                                       \
}                                                           \

#define CHECK_ARGUMENT_PACKAGE_NAME(packageName) {          \
    binder::Status status =                                 \
            checkArgumentPackageName((packageName));        \
    if (!status.isOk()) {                                   \
        return status;                                      \
    }                                                       \
}

#define CHECK_ARGUMENT_PATH(path) {                         \
    binder::Status status = checkArgumentPath((path));      \
    if (!status.isOk()) {                                   \
        return status;                                      \
    }                                                       \
}

#define CHECK_ARGUMENT_FILE_NAME(path)                         \
    {                                                          \
        binder::Status status = checkArgumentFileName((path)); \
        if (!status.isOk()) {                                  \
            return status;                                     \
        }                                                      \
    }

#define CHECK_ARGUMENT_APP_ID(appId)                         \
    {                                                        \
        binder::Status status = checkArgumentAppId((appId)); \
        if (!status.isOk()) {                                \
            return status;                                   \
        }                                                    \
    }

#define CHECK_ARGUMENT_APP_IDS_PCC_IDS(appIds, pccIds)                     \
    {                                                                      \
        binder::Status status = checkArgumentAppIdsPccIds(appIds, pccIds); \
        if (!status.isOk()) {                                              \
            return status;                                                 \
        }                                                                  \
    }

#ifdef GRANULAR_LOCKS

/**
 * This class obtains in constructor and keeps the local strong pointer to the RefLock.
 * On destruction, it checks if there are any other strong pointers, and remove the map entry if
 * this was the last one.
 */
template <class Key, class Mutex>
struct LocalLockHolder {
    using WeakPointer = std::weak_ptr<Mutex>;
    using StrongPointer = std::shared_ptr<Mutex>;
    using Map = std::unordered_map<Key, WeakPointer>;
    using MapLock = std::recursive_mutex;

    LocalLockHolder(Key key, Map& map, MapLock& mapLock)
          : mKey(std::move(key)), mMap(map), mMapLock(mapLock) {
        std::lock_guard lock(mMapLock);
        auto& weakPtr = mMap[mKey];

        // Check if the RefLock is still alive.
        mRefLock = weakPtr.lock();
        if (!mRefLock) {
            // Create a new lock.
            mRefLock = std::make_shared<Mutex>();
            weakPtr = mRefLock;
        }
    }
    LocalLockHolder(LocalLockHolder&& other) noexcept
          : mKey(std::move(other.mKey)),
            mMap(other.mMap),
            mMapLock(other.mMapLock),
            mRefLock(std::move(other.mRefLock)) {
        other.mRefLock.reset();
    }
    ~LocalLockHolder() {
        if (!mRefLock) {
            return;
        }

        std::lock_guard lock(mMapLock);
        // Clear the strong pointer.
        mRefLock.reset();
        auto found = mMap.find(mKey);
        if (found == mMap.end()) {
            return;
        }
        const auto& weakPtr = found->second;
        // If this was the last pointer then it's ok to remove the map entry.
        if (weakPtr.expired()) {
            mMap.erase(found);
        }
    }

    void lock() { mRefLock->lock(); }
    void unlock() { mRefLock->unlock(); }
    void lock_shared() { mRefLock->lock_shared(); }
    void unlock_shared() { mRefLock->unlock_shared(); }

private:
    Key mKey;
    Map& mMap;
    MapLock& mMapLock;
    StrongPointer mRefLock;
};

using UserLock = LocalLockHolder<userid_t, std::shared_mutex>;
using UserWriteLockGuard = std::unique_lock<UserLock>;
using UserReadLockGuard = std::shared_lock<UserLock>;

using PackageLock = LocalLockHolder<std::string, std::recursive_mutex>;
using PackageLockGuard = std::lock_guard<PackageLock>;

#define LOCK_USER()                                     \
    UserLock localUserLock(userId, mUserIdLock, mLock); \
    UserWriteLockGuard userLock(localUserLock)

#define LOCK_USER_READ()                                \
    UserLock localUserLock(userId, mUserIdLock, mLock); \
    UserReadLockGuard userLock(localUserLock)

#define LOCK_PACKAGE()                                                  \
    PackageLock localPackageLock(packageName, mPackageNameLock, mLock); \
    PackageLockGuard packageLock(localPackageLock)

#define LOCK_PACKAGE_USER() \
    LOCK_USER_READ();       \
    LOCK_PACKAGE()

#else

#define LOCK_USER() std::lock_guard lock(mLock)
#define LOCK_PACKAGE() std::lock_guard lock(mLock)
#define LOCK_PACKAGE_USER() \
    (void)userId;           \
    std::lock_guard lock(mLock)

#endif // GRANULAR_LOCKS

}  // namespace

binder::Status InstalldNativeService::FsveritySetupAuthToken::authenticate(
        const ParcelFileDescriptor& authFd, int32_t uid) {
    int open_flags = fcntl(authFd.get(), F_GETFL);
    if (open_flags < 0) {
        return exception(binder::Status::EX_SERVICE_SPECIFIC, "fcntl failed");
    }
    if ((open_flags & O_ACCMODE) != O_WRONLY && (open_flags & O_ACCMODE) != O_RDWR) {
        return exception(binder::Status::EX_SECURITY, "Received FD with unexpected open flag");
    }
    if (fstat(authFd.get(), &this->mStatFromAuthFd) < 0) {
        return exception(binder::Status::EX_SERVICE_SPECIFIC, "fstat failed");
    }
    if (!S_ISREG(this->mStatFromAuthFd.st_mode)) {
        return exception(binder::Status::EX_SECURITY, "Not a regular file");
    }
    // Don't accept a file owned by a different app.
    if (this->mStatFromAuthFd.st_uid != (uid_t)uid) {
        return exception(binder::Status::EX_SERVICE_SPECIFIC, "File not owned by uid");
    }
    return ok();
}

bool InstalldNativeService::FsveritySetupAuthToken::isSameStat(const struct stat& st) const {
    return memcmp(&st, &mStatFromAuthFd, sizeof(st)) == 0;
}

status_t InstalldNativeService::start() {
    IPCThreadState::self()->disableBackgroundScheduling(true);
    status_t ret = BinderService<InstalldNativeService>::publish();
    if (ret != android::OK) {
        return ret;
    }
    sp<ProcessState> ps(ProcessState::self());
    ps->startThreadPool();
    ps->giveThreadPoolName();
    sAppDataIsolationEnabled = android::base::GetBoolProperty(
            kAppDataIsolationEnabledProperty, true);
    return android::OK;
}

status_t InstalldNativeService::dump(int fd, const Vector<String16>& /* args */) {
    {
        std::lock_guard<std::recursive_mutex> lock(mMountsLock);
        dprintf(fd, "Storage mounts:\n");
        for (const auto& n : mStorageMounts) {
            dprintf(fd, "    %s = %s\n", n.first.c_str(), n.second.c_str());
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lock(mQuotasLock);
        dprintf(fd, "Per-UID cache quotas:\n");
        for (const auto& n : mCacheQuotas) {
            dprintf(fd, "    %d = %" PRId64 "\n", n.first, n.second);
        }
    }

    dprintf(fd, "is_dexopt_blocked:%d\n", android::installd::is_dexopt_blocked());

    return NO_ERROR;
}

constexpr const char kXattrRestoreconInProgress[] = "user.restorecon_in_progress";

static std::string lgetfilecon(const std::string& path) {
    char* context;
    if (::lgetfilecon(path.c_str(), &context) < 0) {
        PLOG(ERROR) << "Failed to lgetfilecon for " << path;
        return {};
    }
    std::string result{context};
    free(context);
    return result;
}

static bool getRestoreconInProgress(const std::string& path) {
    bool inProgress = false;
    if (getxattr(path.c_str(), kXattrRestoreconInProgress, &inProgress, sizeof(inProgress)) !=
        sizeof(inProgress)) {
        if (errno != ENODATA) {
            PLOG(ERROR) << "Failed to check in-progress restorecon for " << path;
        }
        return false;
    }
    return inProgress;
}

struct RestoreconInProgress {
    explicit RestoreconInProgress(const std::string& path) : mPath(path) {
        bool inProgress = true;
        if (setxattr(mPath.c_str(), kXattrRestoreconInProgress, &inProgress, sizeof(inProgress),
                     0) != 0) {
            PLOG(ERROR) << "Failed to set in-progress restorecon for " << path;
        }
    }
    ~RestoreconInProgress() {
        if (removexattr(mPath.c_str(), kXattrRestoreconInProgress) < 0) {
            PLOG(ERROR) << "Failed to clear in-progress restorecon for " << mPath;
        }
    }

private:
    const std::string& mPath;
};

/**
 * Perform restorecon of the given path, but only perform recursive restorecon
 * if the label of that top-level file actually changed.  This can save us
 * significant time by avoiding no-op traversals of large filesystem trees.
 */
static int restorecon_app_data_lazy(const std::string& path, const std::string& seInfo, uid_t uid,
        bool existing) {
    ScopedTrace tracer("restorecon-lazy");
    if (!existing) {
        ScopedTrace tracer("new-path");
        if (selinux_android_restorecon_pkgdir(path.c_str(), seInfo.c_str(), uid,
                SELINUX_ANDROID_RESTORECON_RECURSE) < 0) {
            PLOG(ERROR) << "Failed recursive restorecon for " << path;
            return -1;
        }
        return 0;
    }

    // Note that SELINUX_ANDROID_RESTORECON_DATADATA flag is set by libselinux. Not needed here.

    // Check to see if there was an interrupted operation.
    bool inProgress = getRestoreconInProgress(path);
    std::string before, after;
    if (!inProgress) {
        if (before = lgetfilecon(path); before.empty()) {
            PLOG(ERROR) << "Failed before getfilecon for " << path;
            return -1;
        }
        if (selinux_android_restorecon_pkgdir(path.c_str(), seInfo.c_str(), uid, 0) < 0) {
            PLOG(ERROR) << "Failed top-level restorecon for " << path;
            return -1;
        }
        if (after = lgetfilecon(path); after.empty()) {
            PLOG(ERROR) << "Failed after getfilecon for " << path;
            return -1;
        }
    }

    // If the initial top-level restorecon above changed the label, then go
    // back and restorecon everything recursively
    if (inProgress || before != after) {
        if (existing) {
            LOG(DEBUG) << "Detected label change from " << before << " to " << after << " at "
                       << path << "; running recursive restorecon";
        }

        auto restorecon = [path, seInfo, uid]() {
            ScopedTrace tracer("label-change");

            // Temporary mark the folder as "in-progress" to resume in case of reboot/other failure.
            RestoreconInProgress fence(path);

            if (selinux_android_restorecon_pkgdir(path.c_str(), seInfo.c_str(), uid,
                                                  SELINUX_ANDROID_RESTORECON_RECURSE) < 0) {
                PLOG(ERROR) << "Failed recursive restorecon for " << path;
                return -1;
            }
            return 0;
        };
        if (inProgress) {
            // The previous restorecon was interrupted. It's either crashed (unlikely), or the phone
            // was rebooted. Possibly because it took too much time. This time let's move it to a
            // separate thread - so it won't block the rest of the OS.
            std::thread(restorecon).detach();
        } else {
            if (int result = restorecon(); result) {
                return result;
            }
        }
    }

    return 0;
}
static bool internal_storage_has_project_id() {
    // The following path is populated in setFirstBoot, so if this file is present
    // then project ids can be used. Using call once to cache the result of this check
    // to avoid having to check the file presence again and again.
    std::call_once(flag, []() {
        auto using_project_ids =
                StringPrintf("%smisc/installd/using_project_ids", android_data_dir.c_str());
        sUsingProjectIdsFlag = access(using_project_ids.c_str(), F_OK) == 0;
    });
    // return sUsingProjectIdsFlag;
    return false;
}

static int prepare_app_dir(const std::string& path, mode_t target_mode, uid_t uid, gid_t gid,
                           long project_id) {
    {
        ScopedTrace tracer("prepare-dir");
        if (fs_prepare_dir_strict(path.c_str(), target_mode, uid, gid) != 0) {
            PLOG(ERROR) << "Failed to prepare " << path;
            return -1;
        }
    }
    if (internal_storage_has_project_id()) {
        ScopedTrace tracer("set-quota");
        return set_quota_project_id(path, project_id, true);
    }
    return 0;
}

static int prepare_app_cache_dir(const std::string& parent, const char* name, mode_t target_mode,
                                 uid_t uid, gid_t gid, long project_id) {
    auto path = StringPrintf("%s/%s", parent.c_str(), name);
    int ret;
    {
        ScopedTrace tracer("prepare-cache-dir");
        ret = prepare_app_cache_dir(parent, name, target_mode, uid, gid);
    }
    if (ret == 0 && internal_storage_has_project_id()) {
        ScopedTrace tracer("set-quota-cache-dir");
        return set_quota_project_id(path, project_id, true);
    }
    return ret;
}

static bool prepare_app_profile_dir(const std::string& packageName, int32_t appId, int32_t userId) {
    ScopedTrace tracer("prepare-app-profile");
    int32_t uid = multiuser_get_uid(userId, appId);
    int shared_app_gid = multiuser_get_shared_gid(userId, appId);
    if (shared_app_gid == -1) {
        // TODO(calin): this should no longer be possible but do not continue if we don't get
        // a valid shared gid.
        PLOG(WARNING) << "Invalid shared_app_gid for " << packageName;
        return true;
    }

    const std::string profile_dir =
            create_primary_current_profile_package_dir_path(userId, packageName);
    // read-write-execute only for the app user.
    if (fs_prepare_dir_strict(profile_dir.c_str(), 0700, uid, uid) != 0) {
        PLOG(ERROR) << "Failed to prepare " << profile_dir;
        return false;
    }
    if (selinux_android_restorecon(profile_dir.c_str(), 0)) {
        PLOG(ERROR) << "Failed to restorecon " << profile_dir;
        return false;
    }

    const std::string ref_profile_path =
            create_primary_reference_profile_package_dir_path(packageName);

    // Prepare the reference profile directory. Note that we use the non strict version of
    // fs_prepare_dir. This will fix the permission and the ownership to the correct values.
    // This is particularly important given that in O there were some fixes for how the
    // shared_app_gid is computed.
    //
    // Note that by the time we get here we know that we are using a correct uid (otherwise
    // prepare_app_dir and the above fs_prepare_file_strict which check the uid). So we
    // are sure that the gid being used belongs to the owning app and not someone else.
    //
    // dex2oat/profman runs under the shared app gid and it needs to read/write reference profiles.
    if (fs_prepare_dir(ref_profile_path.c_str(), 0770, AID_SYSTEM, shared_app_gid) != 0) {
        PLOG(ERROR) << "Failed to prepare " << ref_profile_path;
        return false;
    }

    return true;
}

static bool chown_app_dir(const std::string& path, uid_t uid, uid_t previousUid, gid_t cacheGid) {
    FTS* fts;
    char *argv[] = { (char*) path.c_str(), nullptr };
    if (!(fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr))) {
        return false;
    }
    for (FTSENT* p; (p = fts_read(fts)) != nullptr;) {
        if (p->fts_info == FTS_D && p->fts_level == 1
            && (strcmp(p->fts_name, "cache") == 0
                || strcmp(p->fts_name, "code_cache") == 0)) {
            // Mark cache dirs
            p->fts_number = 1;
        } else {
            // Inherit parent's number
            p->fts_number = p->fts_parent->fts_number;
        }

        switch (p->fts_info) {
        case FTS_D:
        case FTS_F:
        case FTS_SL:
        case FTS_SLNONE:
            if (p->fts_statp->st_uid == previousUid) {
                if (lchown(p->fts_path, uid, p->fts_number ? cacheGid : uid) != 0) {
                    PLOG(WARNING) << "Failed to lchown " << p->fts_path;
                }
            } else {
                LOG(WARNING) << "Ignoring " << p->fts_path << " with unexpected UID "
                        << p->fts_statp->st_uid << " instead of " << previousUid;
            }
            break;
        }
    }
    fts_close(fts);
    return true;
}

static void chown_app_profile_dir(const std::string &packageName, int32_t appId, int32_t userId) {
    uid_t uid = multiuser_get_uid(userId, appId);
    gid_t sharedGid = multiuser_get_shared_gid(userId, appId);

    const std::string profile_dir =
            create_primary_current_profile_package_dir_path(userId, packageName);
    char *argv[] = { (char*) profile_dir.c_str(), nullptr };
    if (FTS* fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr)) {
        for (FTSENT* p; (p = fts_read(fts)) != nullptr;) {
            switch (p->fts_info) {
            case FTS_D:
            case FTS_F:
            case FTS_SL:
            case FTS_SLNONE:
                if (lchown(p->fts_path, uid, uid) != 0) {
                    PLOG(WARNING) << "Failed to lchown " << p->fts_path;
                }
                break;
            }
        }
        fts_close(fts);
    }

    const std::string ref_profile_path =
            create_primary_reference_profile_package_dir_path(packageName);
    argv[0] = (char *) ref_profile_path.c_str();
    if (FTS* fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr)) {
        for (FTSENT* p; (p = fts_read(fts)) != nullptr;) {
            if (p->fts_info == FTS_D && p->fts_level == 0) {
                if (chown(p->fts_path, AID_SYSTEM, sharedGid) != 0) {
                    PLOG(WARNING) << "Failed to chown " << p->fts_path;
                }
                continue;
            }
            switch (p->fts_info) {
            case FTS_D:
            case FTS_F:
            case FTS_SL:
            case FTS_SLNONE:
                if (lchown(p->fts_path, sharedGid, sharedGid) != 0) {
                    PLOG(WARNING) << "Failed to lchown " << p->fts_path;
                }
                break;
            }
        }
        fts_close(fts);
    }
}

static binder::Status createAppDataDirs(const std::string& path, int32_t uid, int32_t gid,
                                        int32_t previousUid, int32_t cacheGid,
                                        const std::string& seInfo, mode_t targetMode,
                                        long projectIdApp, long projectIdCache) {
    ScopedTrace tracer("create-dirs");
    struct stat st{};
    bool parent_dir_exists = (stat(path.c_str(), &st) == 0);

    auto cache_path = StringPrintf("%s/%s", path.c_str(), "cache");
    auto code_cache_path = StringPrintf("%s/%s", path.c_str(), "code_cache");
    bool cache_exists = (access(cache_path.c_str(), F_OK) == 0);
    bool code_cache_exists = (access(code_cache_path.c_str(), F_OK) == 0);

    if (parent_dir_exists) {
        if (previousUid > 0 && previousUid != uid) {
            if (!chown_app_dir(path, uid, previousUid, cacheGid)) {
                return error("Failed to chown " + path);
            }
        }
    }

    // Prepare only the parent app directory
    if (prepare_app_dir(path, targetMode, uid, gid, projectIdApp) ||
        prepare_app_cache_dir(path, "cache", 02771, uid, cacheGid, projectIdCache) ||
        prepare_app_cache_dir(path, "code_cache", 02771, uid, cacheGid, projectIdCache)) {
        return error("Failed to prepare " + path);
    }

    // Consider restorecon over contents if label changed
    if (restorecon_app_data_lazy(path, seInfo, uid, parent_dir_exists)) {
        return error("Failed to restorecon " + path);
    }

    // If the parent dir exists, the restorecon would already have been done
    // as a part of the recursive restorecon above
    if (parent_dir_exists && !cache_exists
            && restorecon_app_data_lazy(cache_path, seInfo, uid, false)) {
        return error("Failed to restorecon " + cache_path);
    }

    // If the parent dir exists, the restorecon would already have been done
    // as a part of the recursive restorecon above
    if (parent_dir_exists && !code_cache_exists
            && restorecon_app_data_lazy(code_cache_path, seInfo, uid, false)) {
        return error("Failed to restorecon " + code_cache_path);
    }
    return ok();
}

binder::Status InstalldNativeService::createAppDataLocked(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t flags, int32_t appId, int32_t previousAppId, const std::string& seInfo,
        int32_t targetSdkVersion, int64_t* ceDataInode, int64_t* deDataInode,
        int64_t* pccCeDataInode, int64_t* pccDeDataInode, int32_t pccId, int32_t previousPccId) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();

    // Assume invalid inode unless filled in below
    if (ceDataInode != nullptr) *ceDataInode = -1;
    if (deDataInode != nullptr) *deDataInode = -1;
    if (pccCeDataInode != nullptr) *pccCeDataInode = -1;
    if (pccDeDataInode != nullptr) *pccDeDataInode = -1;

    int32_t uid = multiuser_get_uid(userId, appId);

    // If previousAppId > 0, an app is changing its app ID
    int32_t previousUid =
            previousAppId > 0 ? (int32_t)multiuser_get_uid(userId, previousAppId) : -1;

    int32_t cacheGid = multiuser_get_cache_gid(userId, appId);
    mode_t targetMode = targetSdkVersion >= MIN_RESTRICTED_HOME_SDK_VERSION ? 0700 : 0751;

    // If UID doesn't have a specific cache GID, use UID value
    if (cacheGid == -1) {
        cacheGid = uid;
    }

    long projectIdApp = get_project_id(uid, PROJECT_ID_APP_START);
    long projectIdCache = get_project_id(uid, PROJECT_ID_APP_CACHE_START);

    if (flags & FLAG_STORAGE_CE) {
        ScopedTrace tracer("ce");
        auto path = create_data_user_ce_package_path(uuid_, userId, pkgname);

        binder::Status status;
        {
            ScopedTrace tracer_dirs("create_ce_app_data_dirs");
            status = createAppDataDirs(path, uid, uid, previousUid, cacheGid, seInfo, targetMode,
                                       projectIdApp, projectIdCache);
        }
        if (!status.isOk()) {
            return status;
        }

        {
            ScopedTrace tracer_inodes("write_ce_path_inodes");
            // Remember inode numbers of cache directories so that we can clear
            // contents while CE storage is locked
            if (write_path_inode(path, "cache", kXattrInodeCache) ||
                    write_path_inode(path, "code_cache", kXattrInodeCodeCache)) {
                return error("Failed to write_path_inode for " + path);
            }

            // And return the CE inode of the top-level data directory so we can
            // clear contents while CE storage is locked
            if (ceDataInode != nullptr) {
                ino_t result;
                if (get_path_inode(path, &result) != 0) {
                    return error("Failed to get_path_inode for " + path);
                }
                *ceDataInode = static_cast<uint64_t>(result);
            }
        }

        {
            ScopedTrace tracer_pcc("create_pcc_ce_dir");
            // Prepare the PCC sibling directory.
            status = createOrDeletePccDirectoryLocked(uuid_, userId, pkgname, pccId, previousPccId,
                                                      seInfo, targetMode, /* isCeStorage */ true,
                                                      pccCeDataInode);

            if (!status.isOk()) {
                return status;
            }
        }
    }
    if (flags & FLAG_STORAGE_DE) {
        ScopedTrace tracer("de");
        auto path = create_data_user_de_package_path(uuid_, userId, pkgname);

        binder::Status status;
        {
            ScopedTrace tracer_dirs("create_de_app_data_dirs");
            status = createAppDataDirs(path, uid, uid, previousUid, cacheGid, seInfo, targetMode,
                                       projectIdApp, projectIdCache);
        }
        if (!status.isOk()) {
            return status;
        }
        if (previousUid > 0 && previousUid != uid) {
            ScopedTrace tracer_chown("chown_app_profile_dir");
            chown_app_profile_dir(packageName, appId, userId);
        }

        #if APPLY_HARD_QUOTAS
        if (flags::enable_set_inode_quotas() && appId >= FIRST_APPLICATION_UID) {
            ScopedTrace tracer_quota("prepare_app_inode_quota");
            if (!PrepareAppInodeQuota(uuid ? uuid->c_str() : "", uid)) {
                PLOG(ERROR) << "Failed to set hard quota " + path;
            }
        }
        #endif

        {
            ScopedTrace tracer_profile("prepare_app_profile_dir");
            if (!prepare_app_profile_dir(packageName, appId, userId)) {
                return error("Failed to prepare profiles for " + packageName);
            }
        }

        {
            ScopedTrace tracer_pcc("create_pcc_de_dir");
            status = createOrDeletePccDirectoryLocked(uuid_, userId, pkgname, pccId, previousPccId,
                                                      seInfo, targetMode, /* isCeStorage */ false,
                                                      pccDeDataInode);
            if (!status.isOk()) {
                return status;
            }
        }

        if (deDataInode != nullptr) {
            ScopedTrace tracer_inodes("get_de_path_inode");
            ino_t result;
            if (get_path_inode(path, &result) != 0) {
                return error("Failed to get_path_inode for " + path);
            }
            *deDataInode = static_cast<uint64_t>(result);
        }
    }

    if (flags & FLAG_STORAGE_SDK) {
        ScopedTrace tracer("sdk");
        // Safe to ignore status since we can retry creating this by calling reconcileSdkData
        auto ignore = createSdkSandboxDataPackageDirectory(uuid, packageName, userId, appId, flags);
        if (!ignore.isOk()) {
            PLOG(WARNING) << "Failed to create sdk data package directory for " << packageName;
        }
    } else {
        ScopedTrace tracer("destroy-sdk");
        // Package does not need sdk storage. Remove it.
        destroySdkSandboxDataPackageDirectory(uuid, packageName, userId, flags);
    }

    return ok();
}

/**
 * Responsible for creating /data/misc_{ce|de}/user/0/sdksandbox/<package-name> directory and other
 * app level sub directories, such as ./shared
 */
binder::Status InstalldNativeService::createSdkSandboxDataPackageDirectory(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t appId, int32_t flags) {
    ENFORCE_VALID_USER(userId);

    int32_t sdkSandboxUid = multiuser_get_sdk_sandbox_uid(userId, appId);
    if (sdkSandboxUid == -1) {
        // There no valid sdk sandbox process for this app. Skip creation of data directory
        return ok();
    }

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;

    constexpr int storageFlags[2] = {FLAG_STORAGE_CE, FLAG_STORAGE_DE};
    for (int currentFlag : storageFlags) {
        if ((flags & currentFlag) == 0) {
            continue;
        }
        bool isCeData = (currentFlag == FLAG_STORAGE_CE);

        // /data/misc_{ce,de}/<user-id>/sdksandbox directory gets created by vold
        // during user creation

        // Prepare the package directory
        auto packagePath = create_data_misc_sdk_sandbox_package_path(uuid_, isCeData, userId,
                                                                     packageName.c_str());
#if SDK_DEBUG
        LOG(DEBUG) << "Creating app-level sdk data directory: " << packagePath;
#endif

        if (prepare_app_dir(packagePath, 0751, AID_SYSTEM, AID_SYSTEM, 0)) {
            return error("Failed to prepare " + packagePath);
        }
    }

    return ok();
}

binder::Status InstalldNativeService::createAppData(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t flags, int32_t appId, int32_t previousAppId, const std::string& seInfo,
        int32_t targetSdkVersion, int64_t* ceDataInode, int64_t* deDataInode,
        int64_t* pccCeDataInode, int64_t* pccDeDataInode, int32_t pccId, int32_t previousPccId) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    auto trace_name = StringPrintf("createAppData pkg=%s userId=%d", packageName.c_str(), userId);
    ScopedTrace tracer(trace_name.c_str());

    return createAppDataLocked(uuid, packageName, userId, flags, appId, previousAppId, seInfo,
                               targetSdkVersion, ceDataInode, deDataInode, pccCeDataInode,
                               pccDeDataInode, pccId, previousPccId);
}

binder::Status InstalldNativeService::createAppData(
        const android::os::CreateAppDataArgs& args,
        android::os::CreateAppDataResult* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(args.userId);
    // Locking is performed depeer in the callstack.

    int64_t ceDataInode = -1;
    int64_t deDataInode = -1;
    int64_t pccCeDataInode = -1;
    int64_t pccDeDataInode = -1;
    auto status = createAppData(args.uuid, args.packageName, args.userId, args.flags, args.appId,
                                args.previousAppId, args.seInfo, args.targetSdkVersion,
                                &ceDataInode, &deDataInode, &pccCeDataInode, &pccDeDataInode,
                                args.pccId, args.previousPccId);
    _aidl_return->ceDataInode = ceDataInode;
    _aidl_return->deDataInode = deDataInode;
    _aidl_return->pccCeDataInode = pccCeDataInode;
    _aidl_return->pccDeDataInode = pccDeDataInode;
    _aidl_return->exceptionCode = status.exceptionCode();
    _aidl_return->exceptionMessage = status.exceptionMessage();
    return ok();
}

binder::Status InstalldNativeService::createAppDataBatched(
        const std::vector<android::os::CreateAppDataArgs>& args,
        std::vector<android::os::CreateAppDataResult>* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    for (const auto& arg : args) {
        ENFORCE_VALID_USER(arg.userId);
    }

    // Locking is performed depeer in the callstack.
    auto trace_name = StringPrintf("createAppDataBatched size=%zu", args.size());
    ScopedTrace tracer(trace_name.c_str());

    std::vector<android::os::CreateAppDataResult> results;
    for (const auto &arg : args) {
        android::os::CreateAppDataResult result;
        createAppData(arg, &result);
        results.push_back(result);
    }
    *_aidl_return = results;
    return ok();
}

binder::Status InstalldNativeService::reconcileSdkData(
        const android::os::ReconcileSdkDataArgs& args) {
    ENFORCE_VALID_USER(args.userId);
    // Locking is performed depeer in the callstack.

    return reconcileSdkData(args.uuid, args.packageName, args.subDirNames, args.userId, args.appId,
                            args.previousAppId, args.seInfo, args.flags);
}

/**
 * Reconciles per-sdk directory under app-level sdk data directory.

 * E.g. `/data/misc_ce/0/sdksandbox/<package-name>/<sdkPackageName>-<randomSuffix>
 *
 * - If the sdk data package directory is missing, we create it first.
 * - If sdkPackageNames is empty, we delete sdk package directory since it's not needed anymore.
 * - If a sdk level directory we need to prepare already exist, we skip creating it again. This
 *   is to avoid having same per-sdk directory with different suffix.
 * - If a sdk level directory exist which is absent from sdkPackageNames, we remove it.
 */
binder::Status InstalldNativeService::reconcileSdkData(const std::optional<std::string>& uuid,
                                                       const std::string& packageName,
                                                       const std::vector<std::string>& subDirNames,
                                                       int userId, int appId, int previousAppId,
                                                       const std::string& seInfo, int flags) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

#if SDK_DEBUG
    LOG(DEBUG) << "Creating per sdk data directory for: " << packageName;
#endif

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;

    // Prepare the sdk package directory in case it's missing
    const auto status =
            createSdkSandboxDataPackageDirectory(uuid, packageName, userId, appId, flags);
    if (!status.isOk()) {
        return status;
    }

    auto res = ok();
    // We have to create sdk data for CE and DE storage
    const int storageFlags[2] = {FLAG_STORAGE_CE, FLAG_STORAGE_DE};
    for (int currentFlag : storageFlags) {
        if ((flags & currentFlag) == 0) {
            continue;
        }
        const bool isCeData = (currentFlag == FLAG_STORAGE_CE);

        const auto packagePath = create_data_misc_sdk_sandbox_package_path(uuid_, isCeData, userId,
                                                                           packageName.c_str());

        // Remove existing sub-directories not referred in subDirNames
        const std::unordered_set<std::string> expectedSubDirNames(subDirNames.begin(),
                                                                  subDirNames.end());
        const auto subDirHandler = [&packagePath, &expectedSubDirNames,
                                    &res](const std::string& subDirName) {
            // Remove the per-sdk directory if it is not referred in
            // expectedSubDirNames
            if (expectedSubDirNames.find(subDirName) == expectedSubDirNames.end()) {
                auto path = packagePath + "/" + subDirName;
                if (delete_dir_contents_and_dir(path) != 0) {
                    res = error("Failed to delete " + path);
                    return;
                }
            }
        };
        const int ec = foreach_subdir(packagePath, subDirHandler);
        if (ec != 0) {
            res = error("Failed to process subdirs for " + packagePath);
            continue;
        }

        // Now create the subDirNames
        for (const auto& subDirName : subDirNames) {
            const std::string path =
                    create_data_misc_sdk_sandbox_sdk_path(uuid_, isCeData, userId,
                                                          packageName.c_str(), subDirName.c_str());

            // Create the directory along with cache and code_cache
            const int32_t cacheGid = multiuser_get_cache_gid(userId, appId);
            if (cacheGid == -1) {
                return exception(binder::Status::EX_ILLEGAL_STATE,
                                 StringPrintf("cacheGid cannot be -1 for sdk data"));
            }
            const int32_t sandboxUid = multiuser_get_sdk_sandbox_uid(userId, appId);
            int32_t previousSandboxUid = multiuser_get_sdk_sandbox_uid(userId, previousAppId);
            int32_t appUid = multiuser_get_uid(userId, appId);
            long projectIdApp = get_project_id(appUid, PROJECT_ID_APP_START);
            long projectIdCache = get_project_id(appUid, PROJECT_ID_APP_CACHE_START);
            auto status =
                    createAppDataDirs(path, sandboxUid, AID_NOBODY, previousSandboxUid, cacheGid,
                                      seInfo, 0700 | S_ISGID, projectIdApp, projectIdCache);
            if (!status.isOk()) {
                res = status;
                continue;
            }
        }
    }

    return res;
}

binder::Status InstalldNativeService::migrateAppData(const std::optional<std::string>& uuid,
        const std::string& packageName, int32_t userId, int32_t flags) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();

    // This method only exists to upgrade system apps that have requested
    // forceDeviceEncrypted, so their default storage always lives in a
    // consistent location.  This only works on non-FBE devices, since we
    // never want to risk exposing data on a device with real CE/DE storage.

    auto ce_path = create_data_user_ce_package_path(uuid_, userId, pkgname);
    auto de_path = create_data_user_de_package_path(uuid_, userId, pkgname);

    // If neither directory is marked as default, assume CE is default
    if (getxattr(ce_path.c_str(), kXattrDefault, nullptr, 0) == -1
            && getxattr(de_path.c_str(), kXattrDefault, nullptr, 0) == -1) {
        if (setxattr(ce_path.c_str(), kXattrDefault, nullptr, 0, 0) != 0) {
            return error("Failed to mark default storage " + ce_path);
        }
    }

    // Migrate default data location if needed
    auto target = (flags & FLAG_STORAGE_DE) ? de_path : ce_path;
    auto source = (flags & FLAG_STORAGE_DE) ? ce_path : de_path;

    if (getxattr(target.c_str(), kXattrDefault, nullptr, 0) == -1) {
        LOG(WARNING) << "Requested default storage " << target
                << " is not active; migrating from " << source;
        if (delete_dir_contents_and_dir(target) != 0) {
            return error("Failed to delete " + target);
        }
        if (rename(source.c_str(), target.c_str()) != 0) {
            return error("Failed to rename " + source + " to " + target);
        }
    }

    return ok();
}


binder::Status InstalldNativeService::clearAppProfiles(const std::string& packageName,
        const std::string& profileName) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_FILE_NAME(profileName);
    if (!base::EndsWith(profileName, ".prof")) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                         StringPrintf("Profile name %s does not end with .prof",
                                      profileName.c_str()));
    }
    LOCK_PACKAGE();

    binder::Status res = ok();
    if (!clear_primary_reference_profile(packageName, profileName)) {
        res = error("Failed to clear reference profile for " + packageName);
    }
    if (!clear_primary_current_profiles(packageName, profileName)) {
        res = error("Failed to clear current profiles for " + packageName);
    }
    return res;
}

binder::Status InstalldNativeService::clearAppData(const std::optional<std::string>& uuid,
                                                   const std::string& packageName, int32_t userId,
                                                   int32_t flags, int64_t ceDataInode,
                                                   int64_t pccCeDataInode) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();

    binder::Status res = ok();
    if (flags & FLAG_STORAGE_CE) {
        auto path = create_data_user_ce_package_path(uuid_, userId, pkgname, ceDataInode);
        res = clearCeDirectoryLocked(path, flags);

        if (res.isOk()) {
            const std::string pccPackageName = packageName + kPccDataSuffix;
            auto pccPath = create_data_user_ce_package_path(uuid_, userId, pccPackageName.c_str(),
                                                            pccCeDataInode);
            res = clearCeDirectoryLocked(pccPath, flags);
        }
    }
    if (flags & FLAG_STORAGE_DE) {
        std::string suffix;
        if (flags & FLAG_CLEAR_CACHE_ONLY) {
            suffix = CACHE_DIR_POSTFIX;
        } else if (flags & FLAG_CLEAR_CODE_CACHE_ONLY) {
            suffix = CODE_CACHE_DIR_POSTFIX;
        }

        auto path = create_data_user_de_package_path(uuid_, userId, pkgname);
        res = clearDeDirectoryLocked(path, suffix);

        if (res.isOk()) {
            const std::string pccPackageName = packageName + kPccDataSuffix;
            auto pccPath = create_data_user_de_package_path(uuid_, userId, pccPackageName.c_str());
            res = clearDeDirectoryLocked(pccPath, suffix);
        }
    }
    if (flags & FLAG_STORAGE_EXTERNAL) {
        std::lock_guard<std::recursive_mutex> lock(mMountsLock);
        for (const auto& n : mStorageMounts) {
            auto extPath = n.second;

            if (android::base::GetBoolProperty(kFuseProp, false)) {
                std::regex re("^\\/mnt\\/pass_through\\/[0-9]+\\/emulated");
                if (std::regex_match(extPath, re)) {
                    extPath += "/" + std::to_string(userId);
                }
            } else {
                if (n.first.compare(0, 14, "/mnt/media_rw/") != 0) {
                    extPath += StringPrintf("/%d", userId);
                } else if (userId != 0) {
                    // TODO: support devices mounted under secondary users
                    continue;
                }
            }

            if (flags & FLAG_CLEAR_CACHE_ONLY) {
                // Clear only cached data from shared storage
                auto path = StringPrintf("%s/Android/data/%s/cache", extPath.c_str(), pkgname);
                if (delete_dir_contents(path, true) != 0) {
                    res = error("Failed to delete contents of " + path);
                }
            } else if (flags & FLAG_CLEAR_CODE_CACHE_ONLY) {
                // No code cache on shared storage
            } else {
                // Clear everything on shared storage
                auto path = StringPrintf("%s/Android/data/%s", extPath.c_str(), pkgname);
                if (delete_dir_contents(path, true) != 0) {
                    res = error("Failed to delete contents of " + path);
                }
                path = StringPrintf("%s/Android/media/%s", extPath.c_str(), pkgname);
                if (delete_dir_contents(path, true) != 0) {
                    res = error("Failed to delete contents of " + path);
                }
                // Note that we explicitly don't delete OBBs - those are only removed on
                // app uninstall.
            }
        }
    }
    auto status = clearSdkSandboxDataPackageDirectory(uuid, packageName, userId, flags);
    if (!status.isOk()) {
        res = status;
    }
    return res;
}

// Same match as the Magisk emoji modules: apps that bundle or download their own emoji font.
static bool is_emoji_font_name(const char* name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("emoji") != std::string::npos && base::EndsWith(lower, ".ttf");
}

static bool copy_font(int srcFd, int dstFd) {
    char buf[64 * 1024];
    off_t offset = 0;
    while (true) {
        ssize_t n = TEMP_FAILURE_RETRY(pread(srcFd, buf, sizeof(buf), offset));
        if (n < 0) return false;
        if (n == 0) return true;
        if (!base::WriteFully(dstFd, buf, n)) return false;
        offset += n;
    }
}

static bool copy_label(int fromFd, int toFd) {
    char* con = nullptr;
    if (fgetfilecon(fromFd, &con) < 0) return false;
    int res = fsetfilecon(toFd, con);
    freecon(con);
    return res == 0;
}

// Written beside the target and renamed over it: apps that have the old font mapped keep
// their copy instead of faulting on a truncated file.
static bool write_font_file(int dirFd, const std::string& name, int srcFd, uid_t owner, gid_t group,
                            mode_t mode, int labelFromFd) {
    const std::string tmp = "." + name + ".tmp";
    unlinkat(dirFd, tmp.c_str(), 0);
    unique_fd fd(openat(dirFd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0600));
    if (!fd.ok()) return false;
    bool written = copy_font(srcFd, fd.get()) && fchown(fd.get(), owner, group) == 0 &&
            fchmod(fd.get(), mode) == 0 && copy_label(labelFromFd, fd.get()) &&
            renameat(dirFd, tmp.c_str(), dirFd, name.c_str()) == 0;
    if (!written) unlinkat(dirFd, tmp.c_str(), 0);
    return written;
}

static unique_fd make_app_dir(int parentFd, const std::string& name, uid_t uid) {
    if (mkdirat(parentFd, name.c_str(), 0700) < 0 && errno != EEXIST) return {};
    unique_fd fd(openat(parentFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd.ok() || fchown(fd.get(), uid, uid) < 0 || !copy_label(parentFd, fd.get())) {
        return {};
    }
    return fd;
}

static std::vector<std::string> split_app_relative_path(const std::string& path) {
    std::vector<std::string> parts = Split(path, "/");
    for (const auto& part : parts) {
        if (part.empty() || part == "." || part == "..") return {};
    }
    return parts;
}

// The app owns everything below its data dir, so no component is followed through a symlink.
static unique_fd open_app_subdir(int baseFd, const std::vector<std::string>& parts, uid_t uid,
                                 bool create) {
    unique_fd cur(fcntl(baseFd, F_DUPFD_CLOEXEC, 0));
    for (const auto& part : parts) {
        if (!cur.ok()) break;
        unique_fd next(
                openat(cur.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next.ok() && errno == ENOENT && create) next = make_app_dir(cur.get(), part, uid);
        cur = std::move(next);
    }
    return cur;
}

static constexpr int kMaxEmojiFontDepth = 16;

// The app's owner, mode and label are carried over to the new file.
static int replace_emoji_fonts_in_dir(int dirFd, uid_t uid, int srcFd, int depth) {
    if (depth > kMaxEmojiFontDepth) return 0;
    int listFd = fcntl(dirFd, F_DUPFD_CLOEXEC, 0);
    std::unique_ptr<DIR, decltype(&closedir)> dir(listFd < 0 ? nullptr : fdopendir(listFd),
                                                  closedir);
    if (!dir) {
        if (listFd >= 0) close(listFd);
        return 0;
    }
    int replaced = 0;
    for (dirent* de = readdir(dir.get()); de != nullptr; de = readdir(dir.get())) {
        const char* name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (de->d_type == DT_DIR) {
            unique_fd sub(openat(dirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (sub.ok()) replaced += replace_emoji_fonts_in_dir(sub.get(), uid, srcFd, depth + 1);
        } else if (de->d_type == DT_REG && is_emoji_font_name(name)) {
            // O_NONBLOCK: a FIFO swapped in after readdir must not hang installd.
            unique_fd fd(openat(dirFd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            struct stat st;
            if (!fd.ok() || fstat(fd.get(), &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != uid) {
                continue;
            }
            if (write_font_file(dirFd, name, srcFd, st.st_uid, st.st_gid, st.st_mode & 07777,
                                fd.get())) {
                replaced++;
            } else {
                PLOG(WARNING) << "Failed to replace emoji font " << name;
            }
        }
    }
    return replaced;
}

// root-owned and read-only, so the app can't rewrite it with a freshly downloaded font.
static bool lock_emoji_font(int parentFd, const std::string& name, uid_t uid, int srcFd) {
    struct stat st;
    if (fstatat(parentFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(st.st_mode)) {
        return false;
    }
    return write_font_file(parentFd, name, srcFd, AID_ROOT, uid, 0444, parentFd);
}

binder::Status InstalldNativeService::replaceAppEmojiFonts(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t appId, const ParcelFileDescriptor& fontFd,
        const std::vector<std::string>& lockedFiles, int32_t* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();
    const uid_t uid = multiuser_get_uid(userId, appId);
    const int srcFd = fontFd.get();
    *_aidl_return = 0;

    const std::string cePath = create_data_user_ce_package_path(uuid_, userId, pkgname);
    const std::string dePath = create_data_user_de_package_path(uuid_, userId, pkgname);
    unique_fd ceFd(open(cePath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    unique_fd deFd(open(dePath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (ceFd.ok()) *_aidl_return += replace_emoji_fonts_in_dir(ceFd.get(), uid, srcFd, 0);
    if (deFd.ok()) *_aidl_return += replace_emoji_fonts_in_dir(deFd.get(), uid, srcFd, 0);
    if (!ceFd.ok()) return ok();

    for (const auto& lockedFile : lockedFiles) {
        std::vector<std::string> parts = split_app_relative_path(lockedFile);
        if (parts.empty()) {
            return exception(binder::Status::EX_ILLEGAL_ARGUMENT, "Bad path " + lockedFile);
        }
        std::string name = parts.back();
        parts.pop_back();
        unique_fd parentFd = open_app_subdir(ceFd.get(), parts, uid, /*create=*/true);
        if (parentFd.ok() && lock_emoji_font(parentFd.get(), name, uid, srcFd)) {
            (*_aidl_return)++;
        } else {
            PLOG(WARNING) << "Failed to lock emoji font " << lockedFile << " of " << packageName;
        }
    }
    return ok();
}

binder::Status InstalldNativeService::clearAppDataSubdir(const std::optional<std::string>& uuid,
                                                         const std::string& packageName,
                                                         int32_t userId, int32_t appId,
                                                         const std::string& relativePath,
                                                         bool block) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    std::vector<std::string> parts = split_app_relative_path(relativePath);
    if (parts.empty()) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT, "Bad path " + relativePath);
    }
    std::string name = parts.back();
    parts.pop_back();

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const uid_t uid = multiuser_get_uid(userId, appId);
    const std::string cePath = create_data_user_ce_package_path(uuid_, userId, packageName.c_str());
    unique_fd ceFd(open(cePath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!ceFd.ok()) return error("Failed to open " + cePath);

    unique_fd parentFd = open_app_subdir(ceFd.get(), parts, uid, block);
    if (!parentFd.ok()) {
        if (errno == ENOENT && !block) return ok();
        return error("Failed to open parent of " + relativePath);
    }
    unique_fd targetFd(
            openat(parentFd.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (targetFd.ok()) {
        if (delete_dir_contents_fd(parentFd.get(), name.c_str()) != 0) {
            return error("Failed to clear " + relativePath);
        }
    } else if (errno != ENOENT) {
        return error("Failed to open " + relativePath);
    } else if (!block) {
        return ok();
    } else {
        targetFd = make_app_dir(parentFd.get(), name, uid);
        if (!targetFd.ok()) return error("Failed to create " + relativePath);
    }
    if (block && fchmod(targetFd.get(), 0) < 0) {
        return error("Failed to block " + relativePath);
    }
    return ok();
}

binder::Status InstalldNativeService::clearSdkSandboxDataPackageDirectory(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t flags) {
    ENFORCE_VALID_USER(userId);
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();

    binder::Status res = ok();
    constexpr int storageFlags[2] = {FLAG_STORAGE_CE, FLAG_STORAGE_DE};
    for (int i = 0; i < 2; i++) {
        int currentFlag = storageFlags[i];
        if ((flags & currentFlag) == 0) {
            continue;
        }
        bool isCeData = (currentFlag == FLAG_STORAGE_CE);
        std::string suffix;
        if (flags & FLAG_CLEAR_CACHE_ONLY) {
            suffix = CACHE_DIR_POSTFIX;
        } else if (flags & FLAG_CLEAR_CODE_CACHE_ONLY) {
            suffix = CODE_CACHE_DIR_POSTFIX;
        }

        auto appPath = create_data_misc_sdk_sandbox_package_path(uuid_, isCeData, userId, pkgname);
        if (access(appPath.c_str(), F_OK) != 0) continue;
        const auto subDirHandler = [&appPath, &res, &suffix](const std::string& filename) {
            auto filepath = appPath + "/" + filename + suffix;
            if (delete_dir_contents(filepath, true) != 0) {
                res = error("Failed to clear contents of " + filepath);
            }
        };
        const int ec = foreach_subdir(appPath, subDirHandler);
        if (ec != 0) {
            res = error("Failed to process subdirs for " + appPath);
        }
    }
    return res;
}

static int destroy_app_reference_profile(const std::string& pkgname) {
    return delete_dir_contents_and_dir(
        create_primary_reference_profile_package_dir_path(pkgname),
        /*ignore_if_missing*/ true);
}

static int destroy_app_current_profiles(const std::string& pkgname, userid_t userid) {
    return delete_dir_contents_and_dir(
        create_primary_current_profile_package_dir_path(userid, pkgname),
        /*ignore_if_missing*/ true);
}

binder::Status InstalldNativeService::destroyAppProfiles(const std::string& packageName) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE();

    binder::Status res = ok();
    std::vector<userid_t> users = get_known_users(/*volume_uuid*/ nullptr);
    for (auto user : users) {
        if (destroy_app_current_profiles(packageName, user) != 0) {
            res = error("Failed to destroy current profiles for " + packageName);
        }
    }
    if (destroy_app_reference_profile(packageName) != 0) {
        res = error("Failed to destroy reference profile for " + packageName);
    }
    return res;
}

binder::Status InstalldNativeService::deleteReferenceProfile(const std::string& packageName,
                                                             const std::string& profileName) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE();

    // This function only supports primary dex'es.
    std::string path =
            create_reference_profile_path(packageName, profileName, /*is_secondary_dex=*/false);
    if (unlink(path.c_str()) != 0) {
        if (errno == ENOENT) {
            return ok();
        } else {
            return error("Failed to delete profile " + profileName + " for " + packageName);
        }
    }
    return ok();
}

binder::Status InstalldNativeService::destroyAppData(const std::optional<std::string>& uuid,
                                                     const std::string& packageName, int32_t userId,
                                                     int32_t flags, int64_t ceDataInode,
                                                     int64_t pccCeDataInode) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    auto trace_name = StringPrintf("destroyAppData pkg=%s userId=%d flags=%d",
                                   packageName.c_str(), userId, flags);
    ScopedTrace tracer(trace_name.c_str());

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();

    binder::Status res = ok();
    if (flags & FLAG_STORAGE_CE) {
        ScopedTrace tracer_ce("destroy_ce");
        std::string path;
        {
            ScopedTrace tracer_path("create_data_user_ce_package_path");
            path = create_data_user_ce_package_path(uuid_, userId, pkgname, ceDataInode);
        }
        {
            ScopedTrace tracer_del("rename_delete_dir_contents_and_dir");
            if (rename_delete_dir_contents_and_dir(path) != 0) {
                res = error("Failed to delete " + path);
            }
        }

        const std::string pccPackageName = packageName + kPccDataSuffix;
        std::string pccPath;
        {
            ScopedTrace tracer_pcc_path("create_data_user_ce_package_path_pcc");
            pccPath = create_data_user_ce_package_path(uuid_, userId, pccPackageName.c_str(),
                                                       /* ce_data_inode= */ pccCeDataInode);
        }
        if (res.isOk()) {
            ScopedTrace tracer_pcc_del("rename_delete_dir_contents_and_dir_pcc");
            if (rename_delete_dir_contents_and_dir(pccPath) != 0) {
                res = error("Failed to delete " + pccPath);
            }
        }
    }
    if (flags & FLAG_STORAGE_DE) {
        ScopedTrace tracer_de("destroy_de");
        std::string path;
        {
            ScopedTrace tracer_path("create_data_user_de_package_path");
            path = create_data_user_de_package_path(uuid_, userId, pkgname);
        }
        {
            ScopedTrace tracer_del("rename_delete_dir_contents_and_dir");
            if (rename_delete_dir_contents_and_dir(path) != 0) {
                res = error("Failed to delete " + path);
            }
        }

        const std::string pccPackageName = packageName + kPccDataSuffix;
        std::string pccPath;
        {
            ScopedTrace tracer_pcc_path("create_data_user_de_package_path_pcc");
            pccPath = create_data_user_de_package_path(uuid_, userId, pccPackageName.c_str());
        }
        if (res.isOk()) {
            ScopedTrace tracer_pcc_del("rename_delete_dir_contents_and_dir_pcc");
            if (rename_delete_dir_contents_and_dir(pccPath) != 0) {
                res = error("Failed to delete " + pccPath);
            }
        }

        if ((flags & FLAG_CLEAR_APP_DATA_KEEP_ART_PROFILES) == 0) {
            ScopedTrace tracer_profiles("destroy_app_profiles");
            destroy_app_current_profiles(packageName, userId);
            // TODO(calin): If the package is still installed by other users it's probably
            // beneficial to keep the reference profile around.
            // Verify if it's ok to do that.
            destroy_app_reference_profile(packageName);
        }
    }
    if (flags & FLAG_STORAGE_EXTERNAL) {
        ScopedTrace tracer_ext("destroy_external");
        std::lock_guard<std::recursive_mutex> lock(mMountsLock);
        for (const auto& n : mStorageMounts) {
            auto extPath = n.second;

            if (android::base::GetBoolProperty(kFuseProp, false)) {
                std::regex re("^\\/mnt\\/pass_through\\/[0-9]+\\/emulated");
                if (std::regex_match(extPath, re)) {
                    extPath += "/" + std::to_string(userId);
                }
            } else {
                if (n.first.compare(0, 14, "/mnt/media_rw/") != 0) {
                    extPath += StringPrintf("/%d", userId);
                } else if (userId != 0) {
                    // TODO: support devices mounted under secondary users
                    continue;
                }
            }

            auto path = StringPrintf("%s/Android/data/%s", extPath.c_str(), pkgname);
            if (delete_dir_contents_and_dir(path, true) != 0) {
                res = error("Failed to delete contents of " + path);
            }
            path = StringPrintf("%s/Android/media/%s", extPath.c_str(), pkgname);
            if (delete_dir_contents_and_dir(path, true) != 0) {
                res = error("Failed to delete contents of " + path);
            }
            path = StringPrintf("%s/Android/obb/%s", extPath.c_str(), pkgname);
            if (delete_dir_contents_and_dir(path, true) != 0) {
                res = error("Failed to delete contents of " + path);
            }
        }
    }

    binder::Status status;
    {
        ScopedTrace tracer_sdk("destroy_sdk_sandbox");
        status = destroySdkSandboxDataPackageDirectory(uuid, packageName, userId, flags);
    }
    if (!status.isOk()) {
        res = status;
    }
    return res;
}

binder::Status InstalldNativeService::destroySdkSandboxDataPackageDirectory(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t flags) {
    ENFORCE_VALID_USER(userId);

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();

    binder::Status res = ok();
    constexpr int storageFlags[2] = {FLAG_STORAGE_CE, FLAG_STORAGE_DE};
    for (int i = 0; i < 2; i++) {
        int currentFlag = storageFlags[i];
        if ((flags & currentFlag) == 0) {
            continue;
        }
        bool isCeData = (currentFlag == FLAG_STORAGE_CE);
        auto appPath = create_data_misc_sdk_sandbox_package_path(uuid_, isCeData, userId, pkgname);
        if (rename_delete_dir_contents_and_dir(appPath) != 0) {
            res = error("Failed to delete " + appPath);
        }
    }
    return res;
}

static gid_t get_cache_gid(uid_t uid) {
    int32_t gid = multiuser_get_cache_gid(multiuser_get_user_id(uid), multiuser_get_app_id(uid));
    return (gid != -1) ? gid : uid;
}

binder::Status InstalldNativeService::fixupAppData(const std::optional<std::string>& uuid,
        int32_t flags) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    for (auto userId : get_known_users(uuid_)) {
        LOCK_USER();
        atrace_pm_begin("fixup user");
        FTS* fts;
        FTSENT* p;
        auto ce_path = create_data_user_ce_path(uuid_, userId);
        auto de_path = create_data_user_de_path(uuid_, userId);
        char *argv[] = { (char*) ce_path.c_str(), (char*) de_path.c_str(), nullptr };
        if (!(fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr))) {
            return error("Failed to fts_open");
        }
        while ((p = fts_read(fts)) != nullptr) {
            if (p->fts_info == FTS_D && p->fts_level == 1) {
                // Track down inodes of cache directories
                uint64_t raw = 0;
                ino_t inode_cache = 0;
                ino_t inode_code_cache = 0;
                if (getxattr(p->fts_path, kXattrInodeCache, &raw, sizeof(raw)) == sizeof(raw)) {
                    inode_cache = raw;
                }
                if (getxattr(p->fts_path, kXattrInodeCodeCache, &raw, sizeof(raw)) == sizeof(raw)) {
                    inode_code_cache = raw;
                }

                // Figure out expected GID of each child
                FTSENT* child = fts_children(fts, 0);
                while (child != nullptr) {
                    if ((child->fts_statp->st_ino == inode_cache)
                            || (child->fts_statp->st_ino == inode_code_cache)
                            || !strcmp(child->fts_name, "cache")
                            || !strcmp(child->fts_name, "code_cache")) {
                        child->fts_number = get_cache_gid(p->fts_statp->st_uid);
                    } else {
                        child->fts_number = p->fts_statp->st_uid;
                    }
                    child = child->fts_link;
                }
            } else if (p->fts_level >= 2) {
                if (p->fts_level > 2) {
                    // Inherit GID from parent once we're deeper into tree
                    p->fts_number = p->fts_parent->fts_number;
                }

                uid_t uid = p->fts_parent->fts_statp->st_uid;
                gid_t cache_gid = get_cache_gid(uid);
                gid_t expected = p->fts_number;
                gid_t actual = p->fts_statp->st_gid;
                if (actual == expected) {
#if FIXUP_DEBUG
                    LOG(DEBUG) << "Ignoring " << p->fts_path << " with expected GID " << expected;
#endif
                    if (!(flags & FLAG_FORCE)) {
                        fts_set(fts, p, FTS_SKIP);
                    }
                } else if ((actual == uid) || (actual == cache_gid)) {
                    // Only consider fixing up when current GID belongs to app
                    if (p->fts_info != FTS_D) {
                        LOG(INFO) << "Fixing " << p->fts_path << " with unexpected GID " << actual
                                << " instead of " << expected;
                    }
                    switch (p->fts_info) {
                    case FTS_DP:
                        // If we're moving towards cache GID, we need to set S_ISGID
                        if (expected == cache_gid) {
                            if (chmod(p->fts_path, 02771) != 0) {
                                PLOG(WARNING) << "Failed to chmod " << p->fts_path;
                            }
                        }
                        [[fallthrough]]; // also set GID
                    case FTS_F:
                        if (chown(p->fts_path, -1, expected) != 0) {
                            PLOG(WARNING) << "Failed to chown " << p->fts_path;
                        }
                        break;
                    case FTS_SL:
                    case FTS_SLNONE:
                        if (lchown(p->fts_path, -1, expected) != 0) {
                            PLOG(WARNING) << "Failed to chown " << p->fts_path;
                        }
                        break;
                    }
                } else {
                    // Ignore all other GID transitions, since they're kinda shady
                    LOG(WARNING) << "Ignoring " << p->fts_path << " with unexpected GID " << actual
                            << " instead of " << expected;
                    if (!(flags & FLAG_FORCE)) {
                        fts_set(fts, p, FTS_SKIP);
                    }
                }
            }
        }
        fts_close(fts);
        atrace_pm_end();
    }
    return ok();
}

static int32_t copy_directory_recursive(const char* from, const char* to) {
    char* argv[] =
            {(char*)kCpPath,
             (char*)"-F", /* delete any existing destination file first (--remove-destination) */
             (char*)"--preserve=mode,ownership,timestamps,xattr", /* preserve properties */
             (char*)"-R", /* recurse into subdirectories (DEST must be a directory) */
             (char*)"-P", /* Do not follow symlinks [default] */
             (char*)"-d", /* don't dereference symlinks */
             (char*)from,
             (char*)to};

    LOG(DEBUG) << "Copying " << from << " to " << to;
    return logwrap_fork_execvp(ARRAY_SIZE(argv), argv, nullptr, false, LOG_ALOG, false, nullptr);
}

static binder::Status createSnapshotForPackage(const std::string& from, const std::string& to,
                                               const std::string& rollback_package_path,
                                               bool& clear_on_exit) {
    binder::Status res = ok();
    int rc = create_dir_if_needed(to.c_str(), kRollbackFolderMode);
    if (rc != 0) {
        return error(rc, "Failed to create folder " + to);
    }

    rc = delete_dir_contents(rollback_package_path, true /* ignore_if_missing */);
    if (rc != 0) {
        return error(rc, "Failed clearing existing snapshot " + rollback_package_path);
    }

    // Check if we have data to copy.
    if (access(from.c_str(), F_OK) == 0) {
        rc = copy_directory_recursive(from.c_str(), to.c_str());
    }

    if (rc != 0) {
        res = error(rc, "Failed copying " + from + " to " + to);
        clear_on_exit = true;
        return res;
    }
    return res;
}

static binder::Status createDeSnapshotForPackage(const char* volume_uuid, const char* package_name,
                                                 int32_t userId, int32_t snapshotId,
                                                 bool& clear_de_on_exit) {
    auto from = create_data_user_de_package_path(volume_uuid, userId, package_name);
    auto to = create_data_misc_de_rollback_path(volume_uuid, userId, snapshotId);
    auto rollback_package_path =
            create_data_misc_de_rollback_package_path(volume_uuid, userId, snapshotId,
                                                      package_name);

    return createSnapshotForPackage(from, to, rollback_package_path, clear_de_on_exit);
}

static binder::Status createCeSnapshotForPackage(const char* volume_uuid, const char* package_name,
                                                 int32_t userId, int32_t snapshotId,
                                                 bool& clear_ce_on_exit) {
    auto from = create_data_user_ce_package_path(volume_uuid, userId, package_name);
    auto to = create_data_misc_ce_rollback_path(volume_uuid, userId, snapshotId);
    auto rollback_package_path =
            create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                      package_name);

    return createSnapshotForPackage(from, to, rollback_package_path, clear_ce_on_exit);
}

binder::Status InstalldNativeService::snapshotAppData(const std::optional<std::string>& volumeUuid,
                                                      const std::string& packageName,
                                                      int32_t userId, int32_t snapshotId,
                                                      int32_t storageFlags, int64_t* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID_IS_TEST_OR_NULL(volumeUuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    const char* volume_uuid = volumeUuid ? volumeUuid->c_str() : nullptr;
    const char* package_name = packageName.c_str();
    const std::string pccPackageName = std::string(packageName) + kPccDataSuffix;

    binder::Status res = ok();
    // Default result to 0, it will be populated with inode of ce data snapshot
    // if FLAG_STORAGE_CE has been passed.
    if (_aidl_return != nullptr) *_aidl_return = 0;

    bool clear_ce_on_exit = false;
    bool clear_de_on_exit = false;

    auto deleter = [&clear_ce_on_exit, &clear_de_on_exit, &volume_uuid, &userId, &package_name,
                    &pccPackageName, &snapshotId] {
        if (clear_de_on_exit) {
            auto to = create_data_misc_de_rollback_package_path(volume_uuid, userId, snapshotId,
                                                                package_name);
            if (delete_dir_contents(to.c_str(), 1, nullptr) != 0) {
                LOG(WARNING) << "Failed to delete app data snapshot: " << to;
            }

            auto to_pcc = create_data_misc_de_rollback_package_path(volume_uuid, userId, snapshotId,
                                                                    pccPackageName.c_str());
            if (delete_dir_contents(to_pcc.c_str(), 1, nullptr, true /* ignore_if_missing */) !=
                0) {
                LOG(WARNING) << "Failed to delete app data PCC snapshot: " << to_pcc;
            }
        }

        if (clear_ce_on_exit) {
            auto to = create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                                package_name);
            if (delete_dir_contents(to.c_str(), 1, nullptr) != 0) {
                LOG(WARNING) << "Failed to delete app data snapshot: " << to;
            }

            auto to_pcc = create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                                    pccPackageName.c_str());
            if (delete_dir_contents(to_pcc.c_str(), 1, nullptr, true /* ignore_if_missing */) !=
                0) {
                LOG(WARNING) << "Failed to delete app data PCC snapshot: " << to_pcc;
            }
        }
    };

    auto scope_guard = android::base::make_scope_guard(deleter);

    if (storageFlags & FLAG_STORAGE_DE) {
        res = createDeSnapshotForPackage(volume_uuid, package_name, userId, snapshotId,
                                         clear_de_on_exit);
        if (!res.isOk()) {
            return res;
        }

        res = createDeSnapshotForPackage(volume_uuid, pccPackageName.c_str(), userId, snapshotId,
                                         clear_de_on_exit);
        if (!res.isOk()) {
            return res;
        }
    }

    // The app may not have any data at all, in which case it's OK to skip here.
    auto from_ce = create_data_user_ce_package_path(volume_uuid, userId, package_name);
    auto from_pcc_ce =
            create_data_user_ce_package_path(volume_uuid, userId, pccPackageName.c_str());
    if (access(from_ce.c_str(), F_OK) != 0 && access(from_pcc_ce.c_str(), F_OK) != 0) {
        LOG(INFO) << "Missing source " << from_ce;
        return ok();
    }

    // ce_data_inode is not needed when FLAG_CLEAR_CACHE_ONLY is set.
    binder::Status clear_cache_result =
            clearAppData(volumeUuid, packageName, userId, storageFlags | FLAG_CLEAR_CACHE_ONLY,
                         /* ceDataInode=*/0, /*pccCeDataInode=*/0);
    if (!clear_cache_result.isOk()) {
        // It should be fine to continue snapshot if we for some reason failed
        // to clear cache.
        LOG(WARNING) << "Failed to clear cache of app " << packageName;
    }

    // ce_data_inode is not needed when FLAG_CLEAR_CODE_CACHE_ONLY is set.
    binder::Status clear_code_cache_result =
            clearAppData(volumeUuid, packageName, userId, storageFlags | FLAG_CLEAR_CODE_CACHE_ONLY,
                         /* ceDataInode=*/0, /*pccCeDataInode=*/0);
    if (!clear_code_cache_result.isOk()) {
        // It should be fine to continue snapshot if we for some reason failed
        // to clear code_cache.
        LOG(WARNING) << "Failed to clear code_cache of app " << packageName;
    }

    if (storageFlags & FLAG_STORAGE_CE) {
        res = createCeSnapshotForPackage(volume_uuid, package_name, userId, snapshotId,
                                         clear_ce_on_exit);
        if (!res.isOk()) {
            return res;
        }

        res = createCeSnapshotForPackage(volume_uuid, pccPackageName.c_str(), userId, snapshotId,
                                         clear_ce_on_exit);
        if (!res.isOk()) {
            return res;
        }

        if (_aidl_return != nullptr) {
            auto ce_snapshot_path =
                    create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                              package_name);
            int rc = get_path_inode(ce_snapshot_path, reinterpret_cast<ino_t*>(_aidl_return));
            if (rc != 0) {
                res = error(rc, "Failed to get_path_inode for " + ce_snapshot_path);
                clear_ce_on_exit = true;
                return res;
            }
        }
    }

    return res;
}

static void deleteDirsOnFailedRollback(const char* volume_uuid, const char* package_name,
                                       const char* pcc_package_name, const int32_t userId,
                                       bool delete_ce, bool delete_de, bool delete_pcc_ce,
                                       bool delete_pcc_de) {
    if (delete_de) {
        auto de_data = create_data_user_de_package_path(volume_uuid, userId, package_name);
        LOG(WARNING) << "rollback failed. Erasing rolled back de_data " << de_data;
        if (delete_dir_contents(de_data.c_str(), 1, nullptr) != 0) {
            LOG(WARNING) << "Failed to delete rolled back de_data " << de_data;
        }
    }
    if (delete_ce) {
        auto ce_data = create_data_user_ce_package_path(volume_uuid, userId, package_name);
        LOG(WARNING) << "rollback failed. Erasing rolled back ce_data " << ce_data;
        if (delete_dir_contents(ce_data.c_str(), 1, nullptr) != 0) {
            LOG(WARNING) << "Failed to delete rolled back ce_data " << ce_data;
        }
    }
    if (delete_pcc_de) {
        auto pcc_de_data = create_data_user_de_package_path(volume_uuid, userId, pcc_package_name);
        LOG(WARNING) << "rollback failed. Erasing rolled back pcc_de_data " << pcc_de_data;
        if (delete_dir_contents(pcc_de_data.c_str(), 1, nullptr) != 0) {
            LOG(WARNING) << "Failed to delete rolled back pcc_de_data " << pcc_de_data;
        }
    }
    if (delete_pcc_ce) {
        auto pcc_ce_data = create_data_user_ce_package_path(volume_uuid, userId, pcc_package_name);
        LOG(WARNING) << "rollback failed. Erasing rolled back ce_data " << pcc_ce_data;
        if (delete_dir_contents(pcc_ce_data.c_str(), 1, nullptr) != 0) {
            LOG(WARNING) << "Failed to delete rolled back pcc_ce_data " << pcc_ce_data;
        }
    }
}

binder::Status InstalldNativeService::restoreAppDataSnapshot(
        const std::optional<std::string>& volumeUuid, const std::string& packageName,
        const int32_t appId, const int32_t pccId, const std::string& seInfo, const int32_t userId,
        const int32_t snapshotId, int32_t storageFlags) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID_IS_TEST_OR_NULL(volumeUuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    const char* volume_uuid = volumeUuid ? volumeUuid->c_str() : nullptr;
    const char* package_name = packageName.c_str();
    const std::string pccPackageName = std::string(packageName) + kPccDataSuffix;

    auto from_ce = create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                             package_name);
    auto from_de = create_data_misc_de_rollback_package_path(volume_uuid, userId, snapshotId,
                                                             package_name);
    auto from_pcc_ce = create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                                 pccPackageName.c_str());
    auto from_pcc_de = create_data_misc_de_rollback_package_path(volume_uuid, userId, snapshotId,
                                                                 pccPackageName.c_str());

    const bool needs_ce_rollback =
            ((storageFlags & FLAG_STORAGE_CE) != 0) && (access(from_ce.c_str(), F_OK) == 0);
    const bool needs_de_rollback =
            ((storageFlags & FLAG_STORAGE_DE) != 0) && (access(from_de.c_str(), F_OK) == 0);
    const bool needs_pcc_ce_rollback = pccId > 0 && ((storageFlags & FLAG_STORAGE_CE) != 0) &&
            (access(from_pcc_ce.c_str(), F_OK) == 0);
    const bool needs_pcc_de_rollback = pccId > 0 && ((storageFlags & FLAG_STORAGE_DE) != 0) &&
            (access(from_pcc_de.c_str(), F_OK) == 0);

    if (!needs_ce_rollback && !needs_de_rollback && !needs_pcc_ce_rollback &&
        !needs_pcc_de_rollback) {
        return ok();
    }

    // We know we're going to rollback one of the CE or DE data, so we clear
    // application data first. Note that it's possible that we're asked to
    // restore both CE & DE data but that one of the restores fail. Leaving the
    // app with no data in those cases is arguably better than leaving the app
    // with mismatched / stale data.
    LOG(INFO) << "Clearing app data for " << packageName << " to restore snapshot.";
    // It's fine to pass 0 as ceDataInode here, because restoreAppDataSnapshot
    // can only be called when user unlocks the phone, meaning that CE user data
    // is decrypted.
    binder::Status res = clearAppData(volumeUuid, packageName, userId, storageFlags,
                                      0 /* ceDataInode */, 0 /* pccCeDataInode */);
    if (!res.isOk()) {
        return res;
    }

    if (needs_ce_rollback) {
        auto to_ce = create_data_user_ce_path(volume_uuid, userId);
        int rc = copy_directory_recursive(from_ce.c_str(), to_ce.c_str());
        if (rc != 0) {
            res = error(rc, "Failed copying " + from_ce + " to " + to_ce);
            return res;
        }
        delete_dir_contents_and_dir(from_ce, true /* ignore_if_missing */);
    }

    if (needs_de_rollback) {
        auto to_de = create_data_user_de_path(volume_uuid, userId);
        int rc = copy_directory_recursive(from_de.c_str(), to_de.c_str());
        if (rc != 0) {
            deleteDirsOnFailedRollback(volume_uuid, package_name, pccPackageName.c_str(), userId,
                                       /* delete_ce=*/needs_ce_rollback, /* delete_de= */ false,
                                       /* delete_pcc_ce= */ false, /* delete_pcc_de= */ false);
            res = error(rc, "Failed copying " + from_de + " to " + to_de);
            return res;
        }
        delete_dir_contents_and_dir(from_de, true /* ignore_if_missing */);
    }

    if (needs_pcc_ce_rollback) {
        auto to_ce = create_data_user_ce_path(volume_uuid, userId);
        int rc = copy_directory_recursive(from_pcc_ce.c_str(), to_ce.c_str());
        if (rc != 0) {
            deleteDirsOnFailedRollback(volume_uuid, package_name, pccPackageName.c_str(), userId,
                                       /* delete_ce=*/needs_ce_rollback,
                                       /* delete_de= */ needs_de_rollback,
                                       /* delete_pcc_ce= */ false, /* delete_pcc_de= */ false);
            res = error(rc, "Failed copying " + from_pcc_ce + " to " + to_ce);
            return res;
        }
        delete_dir_contents_and_dir(from_pcc_ce, true /* ignore_if_missing */);
    }

    if (needs_pcc_de_rollback) {
        auto to_de = create_data_user_de_path(volume_uuid, userId);
        int rc = copy_directory_recursive(from_pcc_de.c_str(), to_de.c_str());
        if (rc != 0) {
            deleteDirsOnFailedRollback(volume_uuid, package_name, pccPackageName.c_str(), userId,
                                       /* delete_ce=*/needs_ce_rollback,
                                       /* delete_de= */ needs_de_rollback,
                                       /* delete_pcc_ce= */ needs_pcc_ce_rollback,
                                       /* delete_pcc_de= */ false);
            res = error(rc, "Failed copying " + from_pcc_de + " to " + to_de);
            return res;
        }
        delete_dir_contents_and_dir(from_pcc_de, true /* ignore_if_missing */);
    }

    // Finally, restore the SELinux label on the app data.
    res = restoreconAppData(volumeUuid, packageName, userId, storageFlags, appId, seInfo);

    if (!res.isOk()) {
        return res;
    }

    if (pccId > 0) {
        auto pccRes =
                restoreconAppData(volumeUuid, pccPackageName, userId, storageFlags, pccId, seInfo);
        if (!pccRes.isOk()) {
            return pccRes;
        }
    }

    return res;
}

binder::Status InstalldNativeService::destroyAppDataSnapshot(
        const std::optional<std::string>& volumeUuid, const std::string& packageName,
        const int32_t userId, const int64_t ceSnapshotInode, const int32_t snapshotId,
        int32_t storageFlags) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID_IS_TEST_OR_NULL(volumeUuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();

    const char* volume_uuid = volumeUuid ? volumeUuid->c_str() : nullptr;
    const char* package_name = packageName.c_str();
    const std::string pccPackageName = std::string(packageName) + kPccDataSuffix;

    if (storageFlags & FLAG_STORAGE_DE) {
        auto de_snapshot_path = create_data_misc_de_rollback_package_path(volume_uuid, userId,
                                                                          snapshotId, package_name);
        auto pcc_de_snapshot_path =
                create_data_misc_de_rollback_package_path(volume_uuid, userId, snapshotId,
                                                          pccPackageName.c_str());

        int res = delete_dir_contents_and_dir(de_snapshot_path, true /* ignore_if_missing */);
        if (res != 0) {
            return error(res, "Failed clearing snapshot " + de_snapshot_path);
        }
        res = delete_dir_contents_and_dir(pcc_de_snapshot_path, true /* ignore_if_missing */);
        if (res != 0) {
            return error(res, "Failed clearing snapshot " + pcc_de_snapshot_path);
        }
    }

    if (storageFlags & FLAG_STORAGE_CE) {
        auto ce_snapshot_path =
                create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                          package_name, ceSnapshotInode);
        auto pcc_ce_snapshot_path =
                create_data_misc_ce_rollback_package_path(volume_uuid, userId, snapshotId,
                                                          pccPackageName.c_str(), 0);

        int res = delete_dir_contents_and_dir(ce_snapshot_path, true /* ignore_if_missing */);
        if (res != 0) {
            return error(res, "Failed clearing snapshot " + ce_snapshot_path);
        }
        res = delete_dir_contents_and_dir(pcc_ce_snapshot_path, true /* ignore_if_missing */);
        if (res != 0) {
            return error(res, "Failed clearing snapshot " + pcc_ce_snapshot_path);
        }
    }
    return ok();
}

binder::Status InstalldNativeService::destroyCeSnapshotsNotSpecified(
        const std::optional<std::string>& volumeUuid, const int32_t userId,
        const std::vector<int32_t>& retainSnapshotIds) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID_IS_TEST_OR_NULL(volumeUuid);
    LOCK_USER();

    const char* volume_uuid = volumeUuid ? volumeUuid->c_str() : nullptr;

    auto base_path = create_data_misc_ce_rollback_base_path(volume_uuid, userId);

    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(base_path.c_str()), closedir);
    if (!dir) {
        return error(-1, "Failed to open rollback base dir " + base_path);
    }

    struct dirent* ent;
    while ((ent = readdir(dir.get()))) {
        if (ent->d_type != DT_DIR) {
            continue;
        }

        uint snapshot_id;
        bool parse_ok = ParseUint(ent->d_name, &snapshot_id);
        if (parse_ok &&
                std::find(retainSnapshotIds.begin(), retainSnapshotIds.end(),
                          snapshot_id) == retainSnapshotIds.end()) {
            auto rollback_path =
                    create_data_misc_ce_rollback_path(volume_uuid, userId, snapshot_id);
            int res = delete_dir_contents_and_dir(rollback_path, true /* ignore_if_missing */);
            if (res != 0) {
                return error(res, "Failed clearing snapshot " + rollback_path);
            }
        }
    }
    return ok();
}

static binder::Status copyAppDataForPackage(const char* from_uuid, const char* to_uuid,
                                            userid_t userId, const char* package_name) {
    {
        auto from = create_data_user_de_package_path(from_uuid, userId, package_name);
        auto to = create_data_user_de_path(to_uuid, userId);
        int rc = copy_directory_recursive(from.c_str(), to.c_str());
        if (rc != 0) {
            return error(rc, "Failed copying DE data from " + from + " to " + to);
        }
    }

    {
        auto from = create_data_user_ce_package_path(from_uuid, userId, package_name);
        auto to = create_data_user_ce_path(to_uuid, userId);
        int rc = copy_directory_recursive(from.c_str(), to.c_str());
        if (rc != 0) {
            return error(rc, "Failed copying CE data from " + from + " to " + to);
        }
    }

    return ok();
}

static int rollbackAppCeDataForPackage(const char* to_uuid, userid_t userId,
                                       const char* package_name) {
    auto to_ce = create_data_user_ce_package_path(to_uuid, userId, package_name);
    int res = delete_dir_contents(to_ce.c_str(), 1, nullptr);
    if (res != 0) {
        LOG(WARNING) << "Failed to rollback " << to_ce;
    }
    return res;
}

static int rollbackAppDeDataForPackage(const char* to_uuid, userid_t userId,
                                       const char* package_name) {
    auto to_de = create_data_user_de_package_path(to_uuid, userId, package_name);
    int res = delete_dir_contents(to_de.c_str(), 1, nullptr);
    if (res != 0) {
        LOG(WARNING) << "Failed to rollback " << to_de;
    }
    return res;
}

binder::Status InstalldNativeService::moveCompleteApp(const std::optional<std::string>& fromUuid,
                                                      const std::optional<std::string>& toUuid,
                                                      const std::string& packageName, int32_t appId,
                                                      int32_t pccId, const std::string& seInfo,
                                                      int32_t targetSdkVersion,
                                                      const std::string& fromCodePath) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(fromUuid);
    CHECK_ARGUMENT_UUID(toUuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE();

    const char* from_uuid = fromUuid ? fromUuid->c_str() : nullptr;
    const char* to_uuid = toUuid ? toUuid->c_str() : nullptr;
    const char* package_name = packageName.c_str();

    binder::Status res = ok();
    std::vector<userid_t> users = get_known_users(from_uuid);

    auto to_app_package_path_parent = create_data_app_path(to_uuid);
    auto to_app_package_path = StringPrintf("%s/%s", to_app_package_path_parent.c_str(),
                                            android::base::Basename(fromCodePath).c_str());

    // Copy app
    {
        int rc = copy_directory_recursive(fromCodePath.c_str(), to_app_package_path_parent.c_str());
        if (rc != 0) {
            res = error(rc, "Failed copying " + fromCodePath + " to " + to_app_package_path);
            goto fail;
        }

        if (selinux_android_restorecon(to_app_package_path.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE) != 0) {
            res = error("Failed to restorecon " + to_app_package_path);
            goto fail;
        }
    }

    // Copy private data for all known users
    for (auto userId : users) {
        LOCK_USER();

        // Data source may not exist for all users; that's okay
        auto from_ce = create_data_user_ce_package_path(from_uuid, userId, package_name);
        if (access(from_ce.c_str(), F_OK) != 0) {
            LOG(INFO) << "Missing source " << from_ce;
            continue;
        }

        if (!createAppDataLocked(toUuid, packageName, userId, FLAG_STORAGE_CE | FLAG_STORAGE_DE,
                                 appId, /* previousAppId */ -1, seInfo, targetSdkVersion, nullptr,
                                 nullptr, nullptr, nullptr, pccId, /* previousPccId */ 0)
                     .isOk()) {
            res = error("Failed to create package target");
            goto fail;
        }
        res = copyAppDataForPackage(from_uuid, to_uuid, userId, package_name);
        if (!res.isOk()) {
            goto fail;
        }

        if (pccId > 0) {
            const std::string pccPackageName = std::string(package_name) + kPccDataSuffix;
            res = copyAppDataForPackage(from_uuid, to_uuid, userId, pccPackageName.c_str());
            if (!res.isOk()) {
                goto fail;
            }
        }

        if (!restoreconAppDataLocked(toUuid, packageName, userId, FLAG_STORAGE_CE | FLAG_STORAGE_DE,
                                     appId, seInfo)
                     .isOk()) {
            res = error("Failed to restorecon");
            goto fail;
        }
    }

    // Copy sdk data for all known users
    for (auto userId : users) {
        LOCK_USER();

        constexpr int storageFlags[2] = {FLAG_STORAGE_CE, FLAG_STORAGE_DE};
        for (int currentFlag : storageFlags) {
            const bool isCeData = currentFlag == FLAG_STORAGE_CE;

            const auto from = create_data_misc_sdk_sandbox_package_path(from_uuid, isCeData, userId,
                                                                        package_name);
            if (access(from.c_str(), F_OK) != 0) {
                LOG(INFO) << "Missing source " << from;
                continue;
            }
            const auto to = create_data_misc_sdk_sandbox_path(to_uuid, isCeData, userId);

            const int rc = copy_directory_recursive(from.c_str(), to.c_str());
            if (rc != 0) {
                res = error(rc, "Failed copying " + from + " to " + to);
                goto fail;
            }
        }

        if (!restoreconSdkDataLocked(toUuid, packageName, userId, FLAG_STORAGE_CE | FLAG_STORAGE_DE,
                                     appId, seInfo)
                     .isOk()) {
            res = error("Failed to restorecon");
            goto fail;
        }
    }
    // We let the framework scan the new location and persist that before
    // deleting the data in the old location; this ordering ensures that
    // we can recover from things like battery pulls.
    return ok();

fail:
    // Nuke everything we might have already copied
    {
        if (delete_dir_contents(to_app_package_path.c_str(), 1, nullptr) != 0) {
            LOG(WARNING) << "Failed to rollback " << to_app_package_path;
        }
    }
    for (auto userId : users) {
        LOCK_USER();
        rollbackAppCeDataForPackage(to_uuid, userId, package_name);
        rollbackAppDeDataForPackage(to_uuid, userId, package_name);
        if (pccId > 0) {
            const std::string pccPackageName = std::string(package_name) + kPccDataSuffix;
            rollbackAppCeDataForPackage(to_uuid, userId, pccPackageName.c_str());
            rollbackAppDeDataForPackage(to_uuid, userId, pccPackageName.c_str());
        }
    }
    for (auto userId : users) {
        LOCK_USER();
        constexpr int storageFlags[2] = {FLAG_STORAGE_CE, FLAG_STORAGE_DE};
        for (int currentFlag : storageFlags) {
            const bool isCeData = currentFlag == FLAG_STORAGE_CE;
            const auto to = create_data_misc_sdk_sandbox_package_path(to_uuid, isCeData, userId,
                                                                      package_name);
            if (delete_dir_contents(to.c_str(), 1, nullptr) != 0) {
                LOG(WARNING) << "Failed to rollback " << to;
            }
        }
    }
    return res;
}

binder::Status InstalldNativeService::createUserData(const std::optional<std::string>& uuid,
        int32_t userId, int32_t userSerial ATTRIBUTE_UNUSED, int32_t flags) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    LOCK_USER();

    auto trace_name = StringPrintf("createUserData userId=%d flags=%d", userId, flags);
    ScopedTrace tracer(trace_name.c_str());

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    if (flags & FLAG_STORAGE_DE) {
        if (uuid_ == nullptr) {
            if (ensure_config_user_dirs(userId) != 0) {
                return error(StringPrintf("Failed to ensure dirs for %d", userId));
            }
        }
    }

    return ok();
}

binder::Status InstalldNativeService::destroyUserData(const std::optional<std::string>& uuid,
        int32_t userId, int32_t flags) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    LOCK_USER();

    auto trace_name = StringPrintf("destroyUserData userId=%d flags=%d", userId, flags);
    ScopedTrace tracer(trace_name.c_str());

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    binder::Status res = ok();
    if (flags & FLAG_STORAGE_DE) {
        ScopedTrace tracer_de("destroy_de");
        auto path = create_data_user_de_path(uuid_, userId);
        // Contents only, as vold is responsible for the user_de dir itself.
        if (delete_dir_contents(path, true) != 0) {
            res = error("Failed to delete contents of " + path);
        }
        auto sdk_sandbox_de_path =
                create_data_misc_sdk_sandbox_path(uuid_, /*isCeData=*/false, userId);
        if (delete_dir_contents_and_dir(sdk_sandbox_de_path, true) != 0) {
            res = error("Failed to delete " + sdk_sandbox_de_path);
        }
        if (uuid_ == nullptr) {
            path = create_data_misc_legacy_path(userId);
            if (delete_dir_contents_and_dir(path, true) != 0) {
                res = error("Failed to delete " + path);
            }
            path = create_primary_cur_profile_dir_path(userId);
            if (delete_dir_contents_and_dir(path, true) != 0) {
                res = error("Failed to delete " + path);
            }
        }
    }
    if (flags & FLAG_STORAGE_CE) {
        ScopedTrace tracer_ce("destroy_ce");
        auto path = create_data_user_ce_path(uuid_, userId);
        // Contents only, as vold is responsible for the user_ce dir itself.
        if (delete_dir_contents(path, true) != 0) {
            res = error("Failed to delete contents of " + path);
        }
        auto sdk_sandbox_ce_path =
                create_data_misc_sdk_sandbox_path(uuid_, /*isCeData=*/true, userId);
        if (delete_dir_contents_and_dir(sdk_sandbox_ce_path, true) != 0) {
            res = error("Failed to delete " + sdk_sandbox_ce_path);
        }
        path = findDataMediaPath(uuid, userId);
        // Contents only, as vold is responsible for the media dir itself.
        if (delete_dir_contents(path, true) != 0) {
            res = error("Failed to delete contents of " + path);
        }
    }
    return res;
}

binder::Status InstalldNativeService::freeCache(const std::optional<std::string>& uuid,
        int64_t targetFreeBytes, int32_t flags) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
#ifndef GRANULAR_LOCKS
    std::lock_guard lock(mLock);
#endif // !GRANULAR_LOCKS

    auto uuidString = uuid.value_or("");
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    auto data_path = create_data_path(uuid_);
    auto noop = (flags & FLAG_FREE_CACHE_NOOP);
    auto defy_target = (flags & FLAG_FREE_CACHE_DEFY_TARGET_FREE_BYTES);

    int64_t free = data_disk_free(data_path);
    if (free < 0) {
        return error("Failed to determine free space for " + data_path);
    }

    int64_t needed = targetFreeBytes - free;
    if (!defy_target) {
        LOG(DEBUG) << "Device " << data_path << " has " << free << " free; requested "
                << targetFreeBytes << "; needed " << needed;

        if (free >= targetFreeBytes) {
            return ok();
        }
    }

    if (flags & FLAG_FREE_CACHE_V2) {
        // This new cache strategy fairly removes files from UIDs by deleting
        // files from the UIDs which are most over their allocated quota

        // 1. Create trackers for every known UID
        atrace_pm_begin("create");
        const auto users = get_known_users(uuid_);
#ifdef GRANULAR_LOCKS
        std::vector<UserLock> userLocks;
        userLocks.reserve(users.size());
        std::vector<UserWriteLockGuard> lockGuards;
        lockGuards.reserve(users.size());
#endif // GRANULAR_LOCKS
        std::unordered_map<uid_t, std::shared_ptr<CacheTracker>> trackers;
        for (auto userId : users) {
#ifdef GRANULAR_LOCKS
            userLocks.emplace_back(userId, mUserIdLock, mLock);
            lockGuards.emplace_back(userLocks.back());
#endif // GRANULAR_LOCKS
            FTS *fts;
            FTSENT *p;

            // Create a list of data paths whose children have cache directories
            auto ce_path = create_data_user_ce_path(uuid_, userId);
            auto de_path = create_data_user_de_path(uuid_, userId);
            auto media_path = findDataMediaPath(uuid, userId) + "/Android/data/";
            auto ce_sdk_path = create_data_misc_sdk_sandbox_path(uuid_, /*isCeData=*/true, userId);
            auto de_sdk_path = create_data_misc_sdk_sandbox_path(uuid_, /*isCeData=*/false, userId);

            std::vector<std::string> dataPaths = {ce_path, de_path, media_path};
            foreach_subdir(ce_sdk_path, [&ce_sdk_path, &dataPaths](const std::string subDir) {
                const auto fullpath = ce_sdk_path + "/" + subDir;
                dataPaths.push_back(fullpath);
            });
            foreach_subdir(de_sdk_path, [&de_sdk_path, &dataPaths](const std::string subDir) {
                const auto fullpath = de_sdk_path + "/" + subDir;
                dataPaths.push_back((char*)fullpath.c_str());
            });

            char* argv[dataPaths.size() + 1];
            for (unsigned int i = 0; i < dataPaths.size(); i++) {
                argv[i] = (char*)dataPaths[i].c_str();
            }
            argv[dataPaths.size()] = nullptr;

            if (!(fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr))) {
                return error("Failed to fts_open");
            }
            while ((p = fts_read(fts)) != nullptr) {
                if (p->fts_info == FTS_D && p->fts_level == 1) {
                    uid_t uid = p->fts_statp->st_uid;

                    // If uid belongs to sdk sandbox, then the cache should be attributed to the
                    // original client app.
                    const auto client_uid = multiuser_convert_sdk_sandbox_to_app_uid(uid);
                    const bool isSandboxUid = (client_uid != (uid_t)-1);
                    if (isSandboxUid) uid = client_uid;

                    if (multiuser_get_app_id(uid) == AID_MEDIA_RW) {
                        uid = (multiuser_get_app_id(p->fts_statp->st_gid) - AID_EXT_GID_START)
                                + AID_APP_START;
                    }
                    auto search = trackers.find(uid);
                    if (search != trackers.end()) {
                        search->second->addDataPath(p->fts_path);
                    } else {
                        auto tracker = std::shared_ptr<CacheTracker>(new CacheTracker(
                                multiuser_get_user_id(uid), multiuser_get_app_id(uid), uuidString));
                        tracker->addDataPath(p->fts_path);
                        {
                            std::lock_guard<std::recursive_mutex> lock(mQuotasLock);
                            tracker->cacheQuota = mCacheQuotas[uid];
                        }
                        if (tracker->cacheQuota == 0) {
#if MEASURE_DEBUG
                            LOG(WARNING) << "UID " << uid << " has no cache quota; assuming 64MB";
#endif
                            tracker->cacheQuota = 67108864;
                        }
                        trackers[uid] = tracker;
                    }
                    fts_set(fts, p, FTS_SKIP);
                }
            }
            fts_close(fts);
        }
        atrace_pm_end();

        // 2. Populate tracker stats and insert into priority queue
        atrace_pm_begin("populate");
        auto cmp = [](std::shared_ptr<CacheTracker> left, std::shared_ptr<CacheTracker> right) {
            return (left->getCacheRatio() < right->getCacheRatio());
        };
        std::priority_queue<std::shared_ptr<CacheTracker>,
                std::vector<std::shared_ptr<CacheTracker>>, decltype(cmp)> queue(cmp);
        for (const auto& it : trackers) {
            it.second->loadStats();
            queue.push(it.second);
        }
        atrace_pm_end();

        // 3. Bounce across the queue, freeing items from whichever tracker is
        // the most over their assigned quota
        atrace_pm_begin("bounce");
        std::shared_ptr<CacheTracker> active;
        while (active || !queue.empty()) {
            // Only look at apps under quota when explicitly requested
            if (active && (active->getCacheRatio() < 10000)
                    && !(flags & FLAG_FREE_CACHE_V2_DEFY_QUOTA)) {
                LOG(DEBUG) << "Active ratio " << active->getCacheRatio()
                        << " isn't over quota, and defy not requested";
                break;
            }

            // Find the best tracker to work with; this might involve swapping
            // if the active tracker is no longer the most over quota
            bool nextBetter = active && !queue.empty()
                    && active->getCacheRatio() < queue.top()->getCacheRatio();
            if (!active || nextBetter) {
                if (active) {
                    // Current tracker still has items, so we'll consider it
                    // again later once it bubbles up to surface
                    queue.push(active);
                }
                active = queue.top(); queue.pop();
                active->ensureItems();
                continue;
            }

            // If no items remain, go find another tracker
            if (active->items.empty()) {
                active = nullptr;
                continue;
            } else {
                auto item = active->items.back();
                active->items.pop_back();

                LOG(DEBUG) << "Purging " << item->toString() << " from " << active->toString();
                if (!noop) {
                    item->purge();
                }
                active->cacheUsed -= item->size;
                needed -= item->size;
            }

            if (!defy_target) {
                // Verify that we're actually done before bailing, since sneaky
                // apps might be using hardlinks
                if (needed <= 0) {
                    free = data_disk_free(data_path);
                    needed = targetFreeBytes - free;
                    if (needed <= 0) {
                        break;
                    } else {
                        LOG(WARNING) << "Expected to be done but still need " << needed;
                    }
                }
            }
        }
        atrace_pm_end();

    } else {
        return error("Legacy cache logic no longer supported");
    }

    if (!defy_target) {
        free = data_disk_free(data_path);
        if (free >= targetFreeBytes) {
            return ok();
        } else {
            return error(StringPrintf("Failed to free up %" PRId64 " on %s; final free space %" PRId64,
                    targetFreeBytes, data_path.c_str(), free));
        }
    } else {
        return ok();
    }
}

binder::Status InstalldNativeService::rmdex(const std::string& codePath,
        const std::string& instructionSet) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PATH(codePath);

    char dex_path[PKG_PATH_MAX];

    const char* path = codePath.c_str();
    const char* instruction_set = instructionSet.c_str();

    if (validate_apk_path(path) && validate_system_app_path(path)) {
        return error("Invalid path " + codePath);
    }

    if (!create_cache_path(dex_path, path, instruction_set)) {
        return error("Failed to create cache path for " + codePath);
    }

    ALOGV("unlink %s\n", dex_path);
    if (unlink(dex_path) < 0) {
        // It's ok if we don't have a dalvik cache path. Report error only when the path exists
        // but could not be unlinked.
        if (errno != ENOENT) {
            return error(StringPrintf("Failed to unlink %s", dex_path));
        }
    }
    return ok();
}

struct stats {
    int64_t codeSize;
    int64_t dataSize;
    int64_t cacheSize;
};

#if MEASURE_DEBUG
static std::string toString(std::vector<int64_t> values) {
    std::stringstream res;
    res << "[";
    for (size_t i = 0; i < values.size(); i++) {
        res << values[i];
        if (i < values.size() - 1) {
            res << ",";
        }
    }
    res << "]";
    return res.str();
}
#endif

// On devices without sdcardfs, if internal and external are on
// the same volume, a uid such as u0_a123 is used for both
// internal and external storage; therefore, subtract that
// amount from internal to make sure we don't count it double.
// This needs to happen for data, cache and OBB
static void deductDoubleSpaceIfNeeded(stats* stats, int64_t doubleSpaceToBeDeleted, uid_t uid,
                                      const std::string& uuid) {
    if (!supports_sdcardfs()) {
        stats->dataSize -= doubleSpaceToBeDeleted;
        long obbProjectId = get_project_id(uid, PROJECT_ID_EXT_OBB_START);
        int64_t appObbSize = GetOccupiedSpaceForProjectId(uuid, obbProjectId);
        stats->dataSize -= appObbSize;
    }
}

static void collectPccQuotaStats(const std::string& uuid, int32_t userId, int32_t pccId,
                                 struct stats* stats) {
    if (stats == nullptr || pccId <= 0) {
        return;
    }
    int64_t space;
    uid_t uid = multiuser_get_uid(userId, pccId);
    static const bool supportsProjectId = internal_storage_has_project_id();

    if (!supportsProjectId) {
        if ((space = GetOccupiedSpaceForUid(uuid, uid)) != -1) {
            stats->dataSize += space;
        }
        int cacheGid = multiuser_get_cache_gid(userId, pccId);
        if (cacheGid != -1) {
            if ((space = GetOccupiedSpaceForGid(uuid, cacheGid)) != -1) {
                stats->cacheSize += space;
            }
        }
    } else {
        long projectId = get_pcc_project_id(uid, PROJECT_ID_PCC_START);
        if ((space = GetOccupiedSpaceForProjectId(uuid, projectId)) != -1) {
            stats->dataSize += space;
        }
        projectId = get_pcc_project_id(uid, PROJECT_ID_PCC_CACHE_START);
        if ((space = GetOccupiedSpaceForProjectId(uuid, projectId)) != -1) {
            stats->cacheSize += space;
            stats->dataSize += space;
        }
    }
}

static void collectQuotaStatsForApp(const std::string& uuid, int32_t userId, int32_t appId,
                                    struct stats* stats, struct stats* extStats) {
    // early exit if appId <= 0 so that we don't return app storage stats to caller
    if (appId <= 0) {
        return;
    }
    int64_t space, doubleSpaceToBeDeleted = 0;
    uid_t uid = multiuser_get_uid(userId, appId);
    static const bool supportsProjectId = internal_storage_has_project_id();

    if (extStats != nullptr) {
        space = get_occupied_app_space_external(uuid, userId, appId);

        if (space != -1) {
            extStats->dataSize += space;
            doubleSpaceToBeDeleted += space;
        }

        space = get_occupied_app_cache_space_external(uuid, userId, appId);
        if (space != -1) {
            extStats->dataSize += space; // cache counts for "data"
            extStats->cacheSize += space;
            doubleSpaceToBeDeleted += space;
        }
    }

    if (stats != nullptr) {
        if (!supportsProjectId) {
            if ((space = GetOccupiedSpaceForUid(uuid, uid)) != -1) {
                stats->dataSize += space;
            }
            deductDoubleSpaceIfNeeded(stats, doubleSpaceToBeDeleted, uid, uuid);
            int sdkSandboxUid = multiuser_get_sdk_sandbox_uid(userId, appId);
            if (sdkSandboxUid != -1) {
                if ((space = GetOccupiedSpaceForUid(uuid, sdkSandboxUid)) != -1) {
                    stats->dataSize += space;
                }
            }
            int cacheGid = multiuser_get_cache_gid(userId, appId);
            if (cacheGid != -1) {
                if ((space = GetOccupiedSpaceForGid(uuid, cacheGid)) != -1) {
                    stats->cacheSize += space;
                }
            }
        } else {
            long projectId = get_project_id(uid, PROJECT_ID_APP_START);
            if ((space = GetOccupiedSpaceForProjectId(uuid, projectId)) != -1) {
                stats->dataSize += space;
            }
            projectId = get_project_id(uid, PROJECT_ID_APP_CACHE_START);
            if ((space = GetOccupiedSpaceForProjectId(uuid, projectId)) != -1) {
                stats->cacheSize += space;
                stats->dataSize += space;
            }
        }

        int sharedGid = multiuser_get_shared_gid(0, appId);
        if (sharedGid != -1) {
            if ((space = GetOccupiedSpaceForGid(uuid, sharedGid)) != -1) {
                stats->codeSize += space;
            }
        }
    }
}

static void collectQuotaStats(const std::string& uuid, int32_t userId, int32_t appId, int32_t pccId,
                              struct stats* stats, struct stats* extStats) {
    collectQuotaStatsForApp(uuid, userId, appId, stats, extStats);
    collectPccQuotaStats(uuid, userId, pccId, stats);
}

static void collectManualStats(const std::string& path, struct stats* stats) {
    DIR *d;
    int dfd;
    struct dirent *de;
    struct stat s;

    d = opendir(path.c_str());
    if (d == nullptr) {
        if (errno != ENOENT) {
            PLOG(WARNING) << "Failed to open " << path;
        }
        return;
    }
    dfd = dirfd(d);
    while ((de = readdir(d))) {
        const char *name = de->d_name;

        int64_t size = 0;
        if (fstatat(dfd, name, &s, AT_SYMLINK_NOFOLLOW) == 0) {
            size = s.st_blocks * 512;
        }

        if (de->d_type == DT_DIR) {
            if (!strcmp(name, ".")) {
                // Don't recurse, but still count node size
            } else if (!strcmp(name, "..")) {
                // Don't recurse or count node size
                continue;
            } else {
                // Measure all children nodes
                size = 0;
                calculate_tree_size(StringPrintf("%s/%s", path.c_str(), name), &size);
            }

            if (!strcmp(name, "cache") || !strcmp(name, "code_cache")) {
                stats->cacheSize += size;
            }
        }

        // Legacy symlink isn't owned by app
        if (de->d_type == DT_LNK && !strcmp(name, "lib")) {
            continue;
        }

        // Everything found inside is considered data
        stats->dataSize += size;
    }
    closedir(d);
}

void collectManualStatsForSubDirectories(const std::string& path, struct stats* stats) {
    const auto subDirHandler = [&path, &stats](const std::string& subDir) {
        auto fullpath = path + "/" + subDir;
        collectManualStats(fullpath, stats);
    };
    foreach_subdir(path, subDirHandler);
}

static void collectManualStatsForUser(const std::string& path, struct stats* stats,
                                      bool exclude_apps = false,
                                      bool is_sdk_sandbox_storage = false) {
    DIR *d;
    int dfd;
    struct dirent *de;
    struct stat s;

    d = opendir(path.c_str());
    if (d == nullptr) {
        if (errno != ENOENT) {
            PLOG(WARNING) << "Failed to open " << path;
        }
        return;
    }
    dfd = dirfd(d);
    while ((de = readdir(d))) {
        if (de->d_type == DT_DIR) {
            const char *name = de->d_name;
            if (fstatat(dfd, name, &s, AT_SYMLINK_NOFOLLOW) != 0) {
                continue;
            }
            int32_t user_uid = multiuser_get_app_id(s.st_uid);
            if (!strcmp(name, ".") || !strcmp(name, "..")) {
                continue;
            } else if (exclude_apps &&
                       ((user_uid >= AID_APP_START && user_uid <= AID_APP_END) ||
                        (user_uid >= AID_PCC_COMPONENT_PROCESS_START &&
                         user_uid <= AID_PCC_COMPONENT_PROCESS_END))) {
                continue;
            } else if (is_sdk_sandbox_storage) {
                // In case of sdk sandbox storage (e.g. /data/misc_ce/0/sdksandbox/<package-name>),
                // collect individual stats of each subdirectory (shared, storage of each sdk etc.)
                collectManualStatsForSubDirectories(StringPrintf("%s/%s", path.c_str(), name),
                                                    stats);
            } else {
                collectManualStats(StringPrintf("%s/%s", path.c_str(), name), stats);
            }
        }
    }
    closedir(d);
}

static void collectManualExternalStatsForUser(const std::string& path, struct stats* stats) {
    FTS *fts;
    FTSENT *p;
    char *argv[] = { (char*) path.c_str(), nullptr };
    if (!(fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr))) {
        PLOG(ERROR) << "Failed to fts_open " << path;
        return;
    }
    while ((p = fts_read(fts)) != nullptr) {
        p->fts_number = p->fts_parent->fts_number;
        switch (p->fts_info) {
        case FTS_D:
            if (p->fts_level == 3
                    && !strcmp(p->fts_parent->fts_name, "obb")
                    && !strcmp(p->fts_parent->fts_parent->fts_name, "Android")) {
                p->fts_number = 1;
            } else if (p->fts_level == 4
                    && !strcmp(p->fts_name, "cache")
                    && !strcmp(p->fts_parent->fts_parent->fts_name, "data")
                    && !strcmp(p->fts_parent->fts_parent->fts_parent->fts_name, "Android")) {
                p->fts_number = 2;
            }
            [[fallthrough]]; // to count the directory
        case FTS_DEFAULT:
        case FTS_F:
        case FTS_SL:
        case FTS_SLNONE:
            int64_t size = (p->fts_statp->st_blocks * 512);
            if (p->fts_number == 1) {
                stats->codeSize += size;
            } else {
                if (p->fts_number == 2) {
                    stats->cacheSize += size;
                }
                stats->dataSize += size;
            }
            break;
        }
    }
    fts_close(fts);
}
static bool ownsExternalStorage(int32_t appId) {
    // if project id calculation is supported then, there is no need to
    // calculate in a different way and project_id based calculation can work
    if (internal_storage_has_project_id()) {
        return false;
    }

    //  Fetch external storage owner appid  and check if it is the same as the
    //  current appId whose size is calculated
    struct stat s;
    auto _picDir = StringPrintf("%s/Pictures", create_data_media_path(nullptr, 0).c_str());
    // check if the stat are present
    if (stat(_picDir.c_str(), &s) == 0) {
        // fetch the appId from the uid of the media app
        return ((int32_t)multiuser_get_app_id(s.st_uid) == appId);
    }
    return false;
}
binder::Status InstalldNativeService::getAppSize(const std::optional<std::string>& uuid,
                                                 const std::vector<std::string>& packageNames,
                                                 int32_t userId, int32_t flags, int32_t appId,
                                                 int32_t pccId,
                                                 const std::vector<int64_t>& ceDataInodes,
                                                 const std::vector<std::string>& codePaths,
                                                 std::vector<int64_t>* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
    if (packageNames.size() != ceDataInodes.size()) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                         "packageNames/ceDataInodes size mismatch.");
    }
    for (const auto& packageName : packageNames) {
        CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    }
    for (const auto& codePath : codePaths) {
        CHECK_ARGUMENT_PATH(codePath);
    }
    // NOTE: Locking is relaxed on this method, since it's limited to
    // read-only measurements without mutation.

    // When modifying this logic, always verify using tests:
    // runtest -x frameworks/base/services/tests/servicestests/src/com/android/server/pm/InstallerTest.java -m testGetAppSize

#if MEASURE_DEBUG
    LOG(INFO) << "Measuring user " << userId << " app " << appId << " pcc " << pccId;
#endif

    // Here's a summary of the common storage locations across the platform,
    // and how they're each tagged:
    //
    // /data/app/com.example                           UID system
    // /data/app/com.example/oat                       UID system
    // /data/user/0/com.example                        UID u0_a10      GID u0_a10
    // /data/user/0/com.example/cache                  UID u0_a10      GID u0_a10_cache
    // /data/media/0/foo.txt                           UID u0_media_rw
    // /data/media/0/bar.jpg                           UID u0_media_rw GID u0_media_image
    // /data/media/0/Android/data/com.example          UID u0_media_rw GID u0_a10_ext
    // /data/media/0/Android/data/com.example/cache    UID u0_media_rw GID u0_a10_ext_cache
    // /data/media/obb/com.example                     UID system

    struct stats stats;
    struct stats extStats;
    memset(&stats, 0, sizeof(stats));
    memset(&extStats, 0, sizeof(extStats));

    auto uuidString = uuid.value_or("");
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;

    if (!IsQuotaSupported(uuidString)) {
        flags &= ~FLAG_USE_QUOTA;
    }

    atrace_pm_begin("obb");
    for (const auto& packageName : packageNames) {
        auto obbCodePath = create_data_media_package_path(uuid_, userId,
                "obb", packageName.c_str());
        calculate_tree_size(obbCodePath, &extStats.codeSize);
    }
    atrace_pm_end();
    // Calculating the app size of the external storage owning app in a manual way, since
    // calculating it through quota apis also includes external media storage in the app storage
    // numbers
    if (flags & FLAG_USE_QUOTA && (appId <= 0 || appId >= AID_APP_START) &&
        (appId <= 0 || !ownsExternalStorage(appId))) {
        if (appId > 0) {
            atrace_pm_begin("code");
            for (const auto& codePath : codePaths) {
                calculate_tree_size(codePath, &stats.codeSize, -1,
                                    multiuser_get_shared_gid(0, appId));
            }
            atrace_pm_end();
        }

        atrace_pm_begin("quota");
        collectQuotaStats(uuidString, userId, appId, pccId, &stats, &extStats);
        atrace_pm_end();
    } else {
        if (appId > 0) {
            atrace_pm_begin("code");
            for (const auto& codePath : codePaths) {
                calculate_tree_size(codePath, &stats.codeSize);
            }
            atrace_pm_end();
        }

        for (size_t i = 0; i < packageNames.size(); i++) {
            const char* pkgname = packageNames[i].c_str();

            if (appId > 0) {
                atrace_pm_begin("data");
                auto cePath =
                        create_data_user_ce_package_path(uuid_, userId, pkgname, ceDataInodes[i]);
                collectManualStats(cePath, &stats);
                auto dePath = create_data_user_de_package_path(uuid_, userId, pkgname);
                collectManualStats(dePath, &stats);
                atrace_pm_end();
            }

            if (pccId > 0) {
                atrace_pm_begin("pcc");
                const std::string pccPackageName = std::string(pkgname) + kPccDataSuffix;
                auto pccCePath =
                        create_data_user_ce_package_path(uuid_, userId, pccPackageName.c_str());
                collectManualStats(pccCePath, &stats);
                auto pccDePath =
                        create_data_user_de_package_path(uuid_, userId, pccPackageName.c_str());
                collectManualStats(pccDePath, &stats);
                atrace_pm_end();
            }

            // In case of sdk sandbox storage (e.g. /data/misc_ce/0/sdksandbox/<package-name>),
            // collect individual stats of each subdirectory (shared, storage of each sdk etc.)
            if (appId >= AID_APP_START && appId <= AID_APP_END) {
                atrace_pm_begin("sdksandbox");
                auto sdkSandboxCePath =
                        create_data_misc_sdk_sandbox_package_path(uuid_, true, userId, pkgname);
                collectManualStatsForSubDirectories(sdkSandboxCePath, &stats);
                auto sdkSandboxDePath =
                        create_data_misc_sdk_sandbox_package_path(uuid_, false, userId, pkgname);
                collectManualStatsForSubDirectories(sdkSandboxDePath, &stats);
                atrace_pm_end();
            }

            if (appId > 0 && !uuid) {
                atrace_pm_begin("profiles");
                calculate_tree_size(
                        create_primary_current_profile_package_dir_path(userId, pkgname),
                        &stats.dataSize);
                calculate_tree_size(
                        create_primary_reference_profile_package_dir_path(pkgname),
                        &stats.codeSize);
                atrace_pm_end();
            }

            if (appId > 0) {
                atrace_pm_begin("external");
                auto extPath = create_data_media_package_path(uuid_, userId, "data", pkgname);
                collectManualStats(extPath, &extStats);
                auto mediaPath = create_data_media_package_path(uuid_, userId, "media", pkgname);
                calculate_tree_size(mediaPath, &extStats.dataSize);
                atrace_pm_end();
            }
        }

        if (appId > 0 && !uuid) {
            atrace_pm_begin("dalvik");
            int32_t sharedGid = multiuser_get_shared_gid(0, appId);
            if (sharedGid != -1) {
                calculate_tree_size(create_data_dalvik_cache_path(), &stats.codeSize,
                        sharedGid, -1);
            }
            atrace_pm_end();
        }
    }

    std::vector<int64_t> ret;
    ret.push_back(stats.codeSize);
    ret.push_back(stats.dataSize);
    ret.push_back(stats.cacheSize);
    ret.push_back(extStats.codeSize);
    ret.push_back(extStats.dataSize);
    ret.push_back(extStats.cacheSize);
#if MEASURE_DEBUG
    LOG(DEBUG) << "Final result " << toString(ret);
#endif
    *_aidl_return = ret;
    return ok();
}

struct external_sizes {
    int64_t audioSize;
    int64_t videoSize;
    int64_t imageSize;
    int64_t totalSize; // excludes OBBs (Android/obb), but includes app data + cache
    int64_t obbSize;
};

#define PER_USER_RANGE 100000

static long getProjectIdForUser(int userId, long projectId) {
    return userId * PER_USER_RANGE + projectId;
}

static external_sizes getExternalSizesForUserWithQuota(const std::string& uuid, int32_t userId, const std::vector<int32_t>& appIds) {
    struct external_sizes sizes = {};
    int64_t space;

    if (supports_sdcardfs()) {
        uid_t uid = multiuser_get_uid(userId, AID_MEDIA_RW);
        if ((space = GetOccupiedSpaceForUid(uuid, uid)) != -1) {
            sizes.totalSize = space;
        }

        gid_t audioGid = multiuser_get_uid(userId, AID_MEDIA_AUDIO);
        if ((space = GetOccupiedSpaceForGid(uuid, audioGid)) != -1) {
            sizes.audioSize = space;
        }

        gid_t videoGid = multiuser_get_uid(userId, AID_MEDIA_VIDEO);
        if ((space = GetOccupiedSpaceForGid(uuid, videoGid)) != -1) {
            sizes.videoSize = space;
        }

        gid_t imageGid = multiuser_get_uid(userId, AID_MEDIA_IMAGE);
        if ((space = GetOccupiedSpaceForGid(uuid, imageGid)) != -1) {
            sizes.imageSize = space;
        }

        if ((space = GetOccupiedSpaceForGid(uuid, AID_MEDIA_OBB)) != -1) {
            sizes.obbSize = space;
        }
    } else {
        int64_t totalSize = 0;
        long defaultProjectId = getProjectIdForUser(userId, PROJECT_ID_EXT_DEFAULT);
        if ((space = GetOccupiedSpaceForProjectId(uuid, defaultProjectId)) != -1) {
            // This is all files that are not audio/video/images, excluding
            // OBBs and app-private data
            totalSize += space;
        }

        long audioProjectId = getProjectIdForUser(userId, PROJECT_ID_EXT_MEDIA_AUDIO);
        if ((space = GetOccupiedSpaceForProjectId(uuid, audioProjectId)) != -1) {
            sizes.audioSize = space;
            totalSize += space;
        }

        long videoProjectId = getProjectIdForUser(userId, PROJECT_ID_EXT_MEDIA_VIDEO);
        if ((space = GetOccupiedSpaceForProjectId(uuid, videoProjectId)) != -1) {
            sizes.videoSize = space;
            totalSize += space;
        }

        long imageProjectId = getProjectIdForUser(userId, PROJECT_ID_EXT_MEDIA_IMAGE);
        if ((space = GetOccupiedSpaceForProjectId(uuid, imageProjectId)) != -1) {
            sizes.imageSize = space;
            totalSize += space;
        }

        int64_t totalAppDataSize = 0;
        int64_t totalAppCacheSize = 0;
        int64_t totalAppObbSize = 0;
        for (auto appId : appIds) {
            if (appId >= AID_APP_START) {
                // App data
                uid_t uid = multiuser_get_uid(userId, appId);
                long projectId = uid - AID_APP_START + PROJECT_ID_EXT_DATA_START;
                totalAppDataSize += GetOccupiedSpaceForProjectId(uuid, projectId);

                // App cache
                long cacheProjectId = uid - AID_APP_START + PROJECT_ID_EXT_CACHE_START;
                totalAppCacheSize += GetOccupiedSpaceForProjectId(uuid, cacheProjectId);

                // App OBBs
                long obbProjectId = uid - AID_APP_START + PROJECT_ID_EXT_OBB_START;
                totalAppObbSize += GetOccupiedSpaceForProjectId(uuid, obbProjectId);
            }
        }
        // Total size should include app data + cache
        totalSize += totalAppDataSize;
        totalSize += totalAppCacheSize;
        sizes.totalSize = totalSize;

        // Only OBB is separate
        sizes.obbSize = totalAppObbSize;
    }

    return sizes;
}

binder::Status InstalldNativeService::getUserSize(const std::optional<std::string>& uuid,
                                                  int32_t userId, int32_t flags,
                                                  const std::vector<int32_t>& appIds,
                                                  const std::vector<int32_t>& pccIds,
                                                  std::vector<int64_t>* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_APP_IDS_PCC_IDS(appIds, pccIds);
    // NOTE: Locking is relaxed on this method, since it's limited to
    // read-only measurements without mutation.

    // When modifying this logic, always verify using tests:
    // runtest -x frameworks/base/services/tests/servicestests/src/com/android/server/pm/InstallerTest.java -m testGetUserSize

#if MEASURE_DEBUG
    LOG(INFO) << "Measuring user " << userId;
#endif

    struct stats stats;
    struct stats extStats;
    memset(&stats, 0, sizeof(stats));
    memset(&extStats, 0, sizeof(extStats));

    auto uuidString = uuid.value_or("");
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;

    if (!IsQuotaSupported(uuidString)) {
        flags &= ~FLAG_USE_QUOTA;
    }

    if (flags & FLAG_USE_QUOTA) {
        atrace_pm_begin("code");
        calculate_tree_size(create_data_app_path(uuid_), &stats.codeSize, -1, -1, true);
        atrace_pm_end();

        atrace_pm_begin("data");
        auto cePath = create_data_user_ce_path(uuid_, userId);
        collectManualStatsForUser(cePath, &stats, true);
        auto dePath = create_data_user_de_path(uuid_, userId);
        collectManualStatsForUser(dePath, &stats, true);
        atrace_pm_end();

        if (!uuid) {
            atrace_pm_begin("profile");
            auto userProfilePath = create_primary_cur_profile_dir_path(userId);
            calculate_tree_size(userProfilePath, &stats.dataSize, -1, -1, true);
            auto refProfilePath = create_primary_ref_profile_dir_path();
            calculate_tree_size(refProfilePath, &stats.codeSize, -1, -1, true);
            atrace_pm_end();
        }

        atrace_pm_begin("external");
        auto sizes = getExternalSizesForUserWithQuota(uuidString, userId, appIds);
        extStats.dataSize += sizes.totalSize;
        extStats.codeSize += sizes.obbSize;
        atrace_pm_end();

        if (!uuid) {
            atrace_pm_begin("dalvik");
            calculate_tree_size(create_data_dalvik_cache_path(), &stats.codeSize,
                    -1, -1, true);
            calculate_tree_size(create_primary_cur_profile_dir_path(userId), &stats.dataSize,
                    -1, -1, true);
            atrace_pm_end();
        }
        atrace_pm_begin("quota");
        int64_t dataSize = extStats.dataSize;
        for (size_t i = 0; i < appIds.size(); i++) {
            int32_t appId = appIds[i];
            int32_t pccId = pccIds[i];
            if (appId >= AID_APP_START) {
                collectQuotaStats(uuidString, userId, appId, pccId, &stats, &extStats);
#if MEASURE_DEBUG
                // Sleep to make sure we don't lose logs
                usleep(1);
#endif
            }
        }
        extStats.dataSize = dataSize;
        atrace_pm_end();
    } else {
        atrace_pm_begin("code");
        calculate_tree_size(create_data_app_path(uuid_), &stats.codeSize);
        atrace_pm_end();

        atrace_pm_begin("data");
        auto cePath = create_data_user_ce_path(uuid_, userId);
        collectManualStatsForUser(cePath, &stats);
        auto dePath = create_data_user_de_path(uuid_, userId);
        collectManualStatsForUser(dePath, &stats);
        atrace_pm_end();

        atrace_pm_begin("sdksandbox");
        auto sdkSandboxCePath = create_data_misc_sdk_sandbox_path(uuid_, true, userId);
        collectManualStatsForUser(sdkSandboxCePath, &stats, false, true);
        auto sdkSandboxDePath = create_data_misc_sdk_sandbox_path(uuid_, false, userId);
        collectManualStatsForUser(sdkSandboxDePath, &stats, false, true);
        atrace_pm_end();

        if (!uuid) {
            atrace_pm_begin("profile");
            auto userProfilePath = create_primary_cur_profile_dir_path(userId);
            calculate_tree_size(userProfilePath, &stats.dataSize);
            auto refProfilePath = create_primary_ref_profile_dir_path();
            calculate_tree_size(refProfilePath, &stats.codeSize);
            atrace_pm_end();
        }

        atrace_pm_begin("external");
        auto dataMediaPath = create_data_media_path(uuid_, userId);
        collectManualExternalStatsForUser(dataMediaPath, &extStats);

#if MEASURE_DEBUG
        LOG(DEBUG) << "Measured external data " << extStats.dataSize << " cache "
                << extStats.cacheSize << " code " << extStats.codeSize;
#endif
        atrace_pm_end();

        if (!uuid) {
            atrace_pm_begin("dalvik");
            calculate_tree_size(create_data_dalvik_cache_path(), &stats.codeSize);
            calculate_tree_size(create_primary_cur_profile_dir_path(userId), &stats.dataSize);
            atrace_pm_end();
        }
    }

    std::vector<int64_t> ret;
    ret.push_back(stats.codeSize);
    ret.push_back(stats.dataSize);
    ret.push_back(stats.cacheSize);
    ret.push_back(extStats.codeSize);
    ret.push_back(extStats.dataSize);
    ret.push_back(extStats.cacheSize);
#if MEASURE_DEBUG
    LOG(DEBUG) << "Final result " << toString(ret);
#endif
    *_aidl_return = ret;
    return ok();
}

binder::Status InstalldNativeService::getExternalSize(const std::optional<std::string>& uuid,
        int32_t userId, int32_t flags, const std::vector<int32_t>& appIds,
        std::vector<int64_t>* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    // NOTE: Locking is relaxed on this method, since it's limited to
    // read-only measurements without mutation.

    // When modifying this logic, always verify using tests:
    // runtest -x frameworks/base/services/tests/servicestests/src/com/android/server/pm/InstallerTest.java -m testGetExternalSize

#if MEASURE_DEBUG
    LOG(INFO) << "Measuring external " << userId;
#endif

    auto uuidString = uuid.value_or("");
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;

    int64_t totalSize = 0;
    int64_t audioSize = 0;
    int64_t videoSize = 0;
    int64_t imageSize = 0;
    int64_t appSize = 0;
    int64_t obbSize = 0;

    if (!IsQuotaSupported(uuidString)) {
        flags &= ~FLAG_USE_QUOTA;
    }

    if (flags & FLAG_USE_QUOTA) {
        atrace_pm_begin("quota");
        auto sizes = getExternalSizesForUserWithQuota(uuidString, userId, appIds);
        totalSize = sizes.totalSize;
        audioSize = sizes.audioSize;
        videoSize = sizes.videoSize;
        imageSize = sizes.imageSize;
        obbSize = sizes.obbSize;
        atrace_pm_end();

        atrace_pm_begin("apps");
        struct stats extStats;
        memset(&extStats, 0, sizeof(extStats));
        for (auto appId : appIds) {
            if (appId >= AID_APP_START) {
                // PCC does not support external storage, so pccId is ignored here.
                collectQuotaStats(uuidString, userId, appId, /*pccId=*/0, nullptr, &extStats);
            }
        }
        appSize = extStats.dataSize;
        atrace_pm_end();
    } else {
        atrace_pm_begin("manual");
        FTS *fts;
        FTSENT *p;
        auto path = create_data_media_path(uuid_, userId);
        char *argv[] = { (char*) path.c_str(), nullptr };
        if (!(fts = fts_open(argv, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr))) {
            return error("Failed to fts_open " + path);
        }
        while ((p = fts_read(fts)) != nullptr) {
            char* ext;
            int64_t size = (p->fts_statp->st_blocks * 512);
            switch (p->fts_info) {
            case FTS_F:
                // Only categorize files not belonging to apps
                if (p->fts_parent->fts_number == 0) {
                    ext = strrchr(p->fts_name, '.');
                    if (ext != nullptr) {
                        switch (MatchExtension(++ext)) {
                        case AID_MEDIA_AUDIO: audioSize += size; break;
                        case AID_MEDIA_VIDEO: videoSize += size; break;
                        case AID_MEDIA_IMAGE: imageSize += size; break;
                        }
                    }
                }
                [[fallthrough]]; // always count against total
            case FTS_D:
                // Ignore data belonging to specific apps
                p->fts_number = p->fts_parent->fts_number;
                if (p->fts_level == 1 && !strcmp(p->fts_name, "Android")) {
                    p->fts_number = 1;
                }
                [[fallthrough]]; // always count against total
            case FTS_DEFAULT:
            case FTS_SL:
            case FTS_SLNONE:
                if (p->fts_parent->fts_number == 1) {
                    appSize += size;
                }
                totalSize += size;
                break;
            }
        }
        fts_close(fts);
        atrace_pm_end();

        atrace_pm_begin("obb");
        auto obbPath = StringPrintf("%s/Android/obb",
                create_data_media_path(uuid_, userId).c_str());
        calculate_tree_size(obbPath, &obbSize);
        if (!(flags & FLAG_USE_QUOTA)) {
            totalSize -= obbSize;
        }
        atrace_pm_end();
    }

    std::vector<int64_t> ret;
    ret.push_back(totalSize);
    ret.push_back(audioSize);
    ret.push_back(videoSize);
    ret.push_back(imageSize);
    ret.push_back(appSize);
    ret.push_back(obbSize);
#if MEASURE_DEBUG
    LOG(DEBUG) << "Final result " << toString(ret);
#endif
    *_aidl_return = ret;
    return ok();
}

binder::Status InstalldNativeService::getAppCrates(
        const std::optional<std::string>& uuid,
        const std::vector<std::string>& packageNames, int32_t userId,
        std::optional<std::vector<std::optional<CrateMetadata>>>* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    for (const auto& packageName : packageNames) {
        CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    }
#ifdef ENABLE_STORAGE_CRATES
    LOCK_PACKAGE_USER();

    auto retVector = std::vector<std::optional<CrateMetadata>>();
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;

    std::function<void(CratedFolder, CrateMetadata&&)> onCreateCrate =
            [&](CratedFolder cratedFolder, CrateMetadata&& crateMetadata) -> void {
        if (cratedFolder == nullptr) {
            return;
        }
        retVector.push_back(std::move(crateMetadata));
    };

    for (const auto& packageName : packageNames) {
#if CRATE_DEBUG
        LOG(DEBUG) << "packageName = " << packageName;
#endif
        auto crateManager = std::make_unique<CrateManager>(uuid_, userId, packageName);
        crateManager->traverseAllCrates(onCreateCrate);
    }

#if CRATE_DEBUG
    LOG(WARNING) << "retVector.size() =" << retVector.size();
    for (auto& item : retVector) {
        CrateManager::dump(*item);
    }
#endif

    *_aidl_return = std::move(retVector);
#else // ENABLE_STORAGE_CRATES
    _aidl_return->reset();

    /* prevent compile warning fail */
    if (userId < 0) {
        return error();
    }
#endif // ENABLE_STORAGE_CRATES
    return ok();
}

binder::Status InstalldNativeService::getUserCrates(
        const std::optional<std::string>& uuid, int32_t userId,
        std::optional<std::vector<std::optional<CrateMetadata>>>* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
#ifdef ENABLE_STORAGE_CRATES
    LOCK_USER();

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    auto retVector = std::vector<std::optional<CrateMetadata>>();

    std::function<void(CratedFolder, CrateMetadata&&)> onCreateCrate =
            [&](CratedFolder cratedFolder, CrateMetadata&& crateMetadata) -> void {
        if (cratedFolder == nullptr) {
            return;
        }
        retVector.push_back(std::move(crateMetadata));
    };

    std::function<void(FTSENT*)> onHandingPackage = [&](FTSENT* packageDir) -> void {
        auto crateManager = std::make_unique<CrateManager>(uuid_, userId, packageDir->fts_name);
        crateManager->traverseAllCrates(onCreateCrate);
    };
    CrateManager::traverseAllPackagesForUser(uuid, userId, onHandingPackage);

#if CRATE_DEBUG
    LOG(DEBUG) << "retVector.size() =" << retVector.size();
    for (auto& item : retVector) {
        CrateManager::dump(*item);
    }
#endif

    *_aidl_return = std::move(retVector);
#else // ENABLE_STORAGE_CRATES
    _aidl_return->reset();

    /* prevent compile warning fail */
    if (userId < 0) {
        return error();
    }
#endif // ENABLE_STORAGE_CRATES
    return ok();
}

binder::Status InstalldNativeService::setAppQuota(const std::optional<std::string>& uuid,
        int32_t userId, int32_t appId, int64_t cacheQuota) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    std::lock_guard<std::recursive_mutex> lock(mQuotasLock);

    int32_t uid = multiuser_get_uid(userId, appId);
    mCacheQuotas[uid] = cacheQuota;

    return ok();
}

// Dumps the contents of a profile file, using pkgname's dex files for pretty
// printing the result.
binder::Status InstalldNativeService::dumpProfiles(int32_t uid, const std::string& packageName,
                                                   const std::string& profileName,
                                                   const std::string& codePath,
                                                   bool dumpClassesAndMethods, bool* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(codePath);
    LOCK_PACKAGE();

    *_aidl_return = dump_profiles(uid, packageName, profileName, codePath, dumpClassesAndMethods);
    return ok();
}

// Copy the contents of a system profile over the data profile.
binder::Status InstalldNativeService::copySystemProfile(const std::string& systemProfile,
        int32_t packageUid, const std::string& packageName, const std::string& profileName,
        bool* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PATH(systemProfile);
    if (!base::EndsWith(systemProfile, ".prof")) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                         StringPrintf("System profile path %s does not end with .prof",
                                      systemProfile.c_str()));
    }
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_FILE_NAME(profileName);
    if (!base::EndsWith(profileName, ".prof")) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT,
                         StringPrintf("Profile name %s does not end with .prof",
                                      profileName.c_str()));
    }
    LOCK_PACKAGE();
    *_aidl_return = copy_system_profile(systemProfile, packageUid, packageName, profileName);
    return ok();
}

// TODO: Consider returning error codes.
binder::Status InstalldNativeService::mergeProfiles(int32_t uid, const std::string& packageName,
        const std::string& profileName, int* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE();

    *_aidl_return = analyze_primary_profiles(uid, packageName, profileName);
    return ok();
}

binder::Status InstalldNativeService::createProfileSnapshot(int32_t appId,
        const std::string& packageName, const std::string& profileName,
        const std::string& classpath, bool* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE();

    *_aidl_return = create_profile_snapshot(appId, packageName, profileName, classpath);
    return ok();
}

binder::Status InstalldNativeService::destroyProfileSnapshot(const std::string& packageName,
        const std::string& profileName) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE();

    std::string snapshot = create_snapshot_profile_path(packageName, profileName);
    if ((unlink(snapshot.c_str()) != 0) && (errno != ENOENT)) {
        return error("Failed to destroy profile snapshot for " + packageName + ":" + profileName);
    }
    return ok();
}

static const char* getCStr(const std::optional<std::string>& data,
        const char* default_value = nullptr) {
    return data ? data->c_str() : default_value;
}
binder::Status InstalldNativeService::dexopt(
        const std::string& apkPath, int32_t uid, const std::string& packageName,
        const std::string& instructionSet, int32_t dexoptNeeded,
        const std::optional<std::string>& outputPath, int32_t dexFlags,
        const std::string& compilerFilter, const std::optional<std::string>& uuid,
        const std::optional<std::string>& classLoaderContext,
        const std::optional<std::string>& seInfo, bool downgrade, int32_t targetSdkVersion,
        const std::optional<std::string>& profileName,
        const std::optional<std::string>& dexMetadataPath,
        const std::optional<std::string>& compilationReason, bool* aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PATH(apkPath);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(outputPath);
    CHECK_ARGUMENT_PATH(dexMetadataPath);
    const auto userId = multiuser_get_user_id(uid);
    LOCK_PACKAGE_USER();

    const char* oat_dir = getCStr(outputPath);
    const char* instruction_set = instructionSet.c_str();
    if (oat_dir != nullptr && !createOatDirs(packageName, oat_dir, {instruction_set}).isOk()) {
        // Can't create oat dir - let dexopt use cache dir.
        oat_dir = nullptr;
    }

    const char* apk_path = apkPath.c_str();
    const char* pkgname = packageName.c_str();
    const char* compiler_filter = compilerFilter.c_str();
    const char* volume_uuid = getCStr(uuid);
    const char* class_loader_context = getCStr(classLoaderContext);
    const char* se_info = getCStr(seInfo);
    const char* profile_name = getCStr(profileName);
    const char* dm_path = getCStr(dexMetadataPath);
    const char* compilation_reason = getCStr(compilationReason);
    std::string error_msg;
    bool completed = false; // not necessary but for compiler
    int res = android::installd::dexopt(apk_path, uid, pkgname, instruction_set, dexoptNeeded,
            oat_dir, dexFlags, compiler_filter, volume_uuid, class_loader_context, se_info,
            downgrade, targetSdkVersion, profile_name, dm_path, compilation_reason, &error_msg,
            &completed);
    *aidl_return = completed;
    return res ? error(res, error_msg) : ok();
}

binder::Status InstalldNativeService::controlDexOptBlocking(bool block) {
    android::installd::control_dexopt_blocking(block);
    return ok();
}

binder::Status InstalldNativeService::linkNativeLibraryDirectory(
        const std::optional<std::string>& uuid, const std::string& packageName,
        const std::string& nativeLibPath32, int32_t userId) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(nativeLibPath32);
    LOCK_PACKAGE_USER();

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgname = packageName.c_str();
    const char* asecLibDir = nativeLibPath32.c_str();
    struct stat s, libStat;
    binder::Status res = ok();

    auto _pkgdir = create_data_user_ce_package_path(uuid_, userId, pkgname);
    auto _libsymlink = _pkgdir + PKG_LIB_POSTFIX;

    const char* pkgdir = _pkgdir.c_str();
    const char* libsymlink = _libsymlink.c_str();

    if (stat(pkgdir, &s) < 0) {
        return error("Failed to stat " + _pkgdir);
    }

    char *con = nullptr;
    if (::lgetfilecon(pkgdir, &con) < 0) {
        return error("Failed to lgetfilecon " + _pkgdir);
    }

    if (chown(pkgdir, AID_INSTALL, AID_INSTALL) < 0) {
        res = error("Failed to chown " + _pkgdir);
        goto out;
    }

    if (chmod(pkgdir, 0700) < 0) {
        res = error("Failed to chmod " + _pkgdir);
        goto out;
    }

    if (lstat(libsymlink, &libStat) < 0) {
        if (errno != ENOENT) {
            res = error("Failed to stat " + _libsymlink);
            goto out;
        }
    } else {
        if (S_ISDIR(libStat.st_mode)) {
            if (delete_dir_contents(libsymlink, 1, nullptr) < 0) {
                res = error("Failed to delete " + _libsymlink);
                goto out;
            }
        } else if (S_ISLNK(libStat.st_mode)) {
            if (unlink(libsymlink) < 0) {
                res = error("Failed to unlink " + _libsymlink);
                goto out;
            }
        }
    }

    if (symlink(asecLibDir, libsymlink) < 0) {
        res = error("Failed to symlink " + _libsymlink + " to " + nativeLibPath32);
        goto out;
    }

    if (lsetfilecon(libsymlink, con) < 0) {
        res = error("Failed to lsetfilecon " + _libsymlink);
        goto out;
    }

out:
    free(con);
    if (chmod(pkgdir, s.st_mode) < 0) {
        auto msg = "Failed to cleanup chmod " + _pkgdir;
        if (res.isOk()) {
            res = error(msg);
        } else {
            PLOG(ERROR) << msg;
        }
    }

    if (chown(pkgdir, s.st_uid, s.st_gid) < 0) {
        auto msg = "Failed to cleanup chown " + _pkgdir;
        if (res.isOk()) {
            res = error(msg);
        } else {
            PLOG(ERROR) << msg;
        }
    }

    return res;
}

binder::Status InstalldNativeService::restoreconAppData(const std::optional<std::string>& uuid,
        const std::string& packageName, int32_t userId, int32_t flags, int32_t appId,
        const std::string& seInfo) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE_USER();
    return restoreconAppDataLocked(uuid, packageName, userId, flags, appId, seInfo);
}

binder::Status InstalldNativeService::restoreconAppDataLocked(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t flags, int32_t appId, const std::string& seInfo) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);

    binder::Status res = ok();

    // SELINUX_ANDROID_RESTORECON_DATADATA flag is set by libselinux. Not needed here.
    unsigned int seflags = SELINUX_ANDROID_RESTORECON_RECURSE;
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgName = packageName.c_str();
    const char* seinfo = seInfo.c_str();

    uid_t uid = multiuser_get_uid(userId, appId);
    if (flags & FLAG_STORAGE_CE) {
        auto path = create_data_user_ce_package_path(uuid_, userId, pkgName);
        if (selinux_android_restorecon_pkgdir(path.c_str(), seinfo, uid, seflags) < 0) {
            res = error("restorecon failed for " + path);
        }
    }
    if (flags & FLAG_STORAGE_DE) {
        auto path = create_data_user_de_package_path(uuid_, userId, pkgName);
        if (selinux_android_restorecon_pkgdir(path.c_str(), seinfo, uid, seflags) < 0) {
            res = error("restorecon failed for " + path);
        }
    }
    return res;
}

binder::Status InstalldNativeService::restoreconSdkDataLocked(
        const std::optional<std::string>& uuid, const std::string& packageName, int32_t userId,
        int32_t flags, int32_t appId, const std::string& seInfo) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);

    binder::Status res = ok();

    // SELINUX_ANDROID_RESTORECON_DATADATA flag is set by libselinux. Not needed here.
    unsigned int seflags = SELINUX_ANDROID_RESTORECON_RECURSE;
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const char* pkgName = packageName.c_str();
    const char* seinfo = seInfo.c_str();

    uid_t uid = multiuser_get_sdk_sandbox_uid(userId, appId);
    constexpr int storageFlags[2] = {FLAG_STORAGE_CE, FLAG_STORAGE_DE};
    for (int currentFlag : storageFlags) {
        if ((flags & currentFlag) == 0) {
            continue;
        }
        const bool isCeData = (currentFlag == FLAG_STORAGE_CE);
        const auto packagePath =
                create_data_misc_sdk_sandbox_package_path(uuid_, isCeData, userId, pkgName);
        if (access(packagePath.c_str(), F_OK) != 0) {
            LOG(INFO) << "Missing source " << packagePath;
            continue;
        }
        const auto subDirHandler = [&packagePath, &seinfo, &uid, &seflags,
                                    &res](const std::string& subDir) {
            const auto& fullpath = packagePath + "/" + subDir;
            if (selinux_android_restorecon_pkgdir(fullpath.c_str(), seinfo, uid, seflags) < 0) {
                res = error("restorecon failed for " + fullpath);
            }
        };
        const auto ec = foreach_subdir(packagePath, subDirHandler);
        if (ec != 0) {
            res = error("Failed to restorecon for subdirs of " + packagePath);
        }
    }
    return res;
}

binder::Status InstalldNativeService::createOrDeletePccDirectoryLocked(
        const char* volumeUuid, userid_t userId, const char* packageName, int32_t pccId,
        int32_t previousPccId, const std::string& seInfo, mode_t targetMode, bool isCeStorage,
        int64_t* pccDataInode) {
    binder::Status res = ok();

    const std::string pccPackageName = std::string(packageName) + kPccDataSuffix;
    auto pccPath = isCeStorage
            ? create_data_user_ce_package_path(volumeUuid, userId, pccPackageName.c_str())
            : create_data_user_de_package_path(volumeUuid, userId, pccPackageName.c_str());

    if (pccId > 0) {
        int32_t pccUid = multiuser_get_uid(userId, pccId);
        int32_t previousPccUid =
                previousPccId > 0 ? (int32_t)multiuser_get_uid(userId, previousPccId) : -1;
        long pccProjectIdApp = get_pcc_project_id(pccUid, PROJECT_ID_PCC_START);
        long pccProjectIdCache = get_pcc_project_id(pccUid, PROJECT_ID_PCC_CACHE_START);
        int32_t pccCacheGid = multiuser_get_cache_gid(userId, pccId);
        if (pccCacheGid == -1) {
            return exception(binder::Status::EX_ILLEGAL_STATE,
                             StringPrintf("cacheGid cannot be -1 for pcc data"));
        }

        // Create the PCC directory and its subdirectories (cache, code_cache).
        res = createAppDataDirs(pccPath, pccUid, pccUid, previousPccUid, pccCacheGid, seInfo,
                                targetMode, pccProjectIdApp, pccProjectIdCache);
        if (!res.isOk()) {
            return res;
        }

        // The write_path_inode calls are only necessary for CE storage.
        if (isCeStorage) {
            if (write_path_inode(pccPath, "cache", kXattrInodeCache) ||
                write_path_inode(pccPath, "code_cache", kXattrInodeCodeCache)) {
                res = error("Failed to write_path_inode for " + pccPath);
                return res;
            }
        }
        if (pccDataInode != nullptr) {
            ino_t result;
            if (get_path_inode(pccPath, &result) != 0) {
                return error("Failed to get_path_inode for " + pccPath);
            }
            *pccDataInode = static_cast<uint64_t>(result);
        }
    } else {
        // If no pccId is provided, ensure the directory is cleaned up.
        if (rename_delete_dir_contents_and_dir(pccPath) != 0) {
            res = error("Failed to delete " + pccPath);
            return res;
        }
    }

    return res;
}

binder::Status InstalldNativeService::destroyPccData(const std::optional<std::string>& uuid,
                                                     const std::string& packageName, int32_t userId,
                                                     int32_t flags, int64_t ceDataInode) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER(userId);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);

    LOCK_PACKAGE_USER();

    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    const std::string pccPackageName = packageName + kPccDataSuffix;

    binder::Status res = ok();

    if (flags & FLAG_STORAGE_CE) {
        auto pccPath = create_data_user_ce_package_path(uuid_, userId, pccPackageName.c_str(),
                                                        ceDataInode);

        if (rename_delete_dir_contents_and_dir(pccPath, /* ignore_if_missing= */ true) != 0) {
            PLOG(ERROR) << "Failed to delete PCC CE directory: " << pccPath;
            res = error("Failed to delete PCC CE directory: " + pccPath);
        }
    }

    if (flags & FLAG_STORAGE_DE) {
        auto pccPath = create_data_user_de_package_path(uuid_, userId, pccPackageName.c_str());

        if (rename_delete_dir_contents_and_dir(pccPath, /* ignore_if_missing= */ true) != 0) {
            PLOG(ERROR) << "Failed to delete PCC DE directory: " << pccPath;
            res = error("Failed to delete PCC DE directory: " + pccPath);
        }
    }

    return res;
}

binder::Status InstalldNativeService::clearCeDirectoryLocked(const std::string& path,
                                                             int32_t flags) {
    binder::Status res = ok();

    auto pathToClear = path;
    if (flags & FLAG_CLEAR_CACHE_ONLY) {
        pathToClear = read_path_inode(path, "cache", kXattrInodeCache);
    } else if (flags & FLAG_CLEAR_CODE_CACHE_ONLY) {
        pathToClear = read_path_inode(path, "code_cache", kXattrInodeCodeCache);
    }

    if (access(pathToClear.c_str(), F_OK) == 0) {
        if (delete_dir_contents(pathToClear) != 0) {
            res = error("Failed to delete contents of " + pathToClear);
        } else if ((flags & (FLAG_CLEAR_CACHE_ONLY | FLAG_CLEAR_CODE_CACHE_ONLY)) == 0) {
            // For a full clear, remove the remembered cache inodes.
            remove_path_xattr(path, kXattrInodeCache);
            remove_path_xattr(path, kXattrInodeCodeCache);
        }
    }

    return res;
}

// Helper to clear Device Encrypted (DE) storage for a given base path and suffix.
binder::Status InstalldNativeService::clearDeDirectoryLocked(const std::string& path,
                                                             const std::string& suffix) {
    binder::Status res = ok();

    auto pathToClear = path + suffix;
    if (access(pathToClear.c_str(), F_OK) == 0) {
        if (delete_dir_contents(pathToClear) != 0) {
            res = error("Failed to delete contents of " + pathToClear);
        }
    }

    return res;
}

binder::Status InstalldNativeService::createOatDirs(const std::string& packageName,
                                                    const std::string& oatDir,
                                                    const std::vector<std::string>& oatSubDirs) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(oatDir);
    LOCK_PACKAGE();

    const char* oat_dir = oatDir.c_str();

    if (validate_apk_path(oat_dir)) {
        return error("Invalid path " + oatDir);
    }
    if (fs_prepare_dir(oat_dir, S_IRWXU | S_IRWXG | S_IXOTH, AID_SYSTEM, AID_INSTALL)) {
        return error("Failed to prepare " + oatDir);
    }
    if (selinux_android_restorecon(oat_dir, 0)) {
        return error("Failed to restorecon " + oatDir);
    }
    for (const std::string& sub_dir : oatSubDirs) {
        // Create the given sub-directory as well as any nonexistent parent directories.
        std::filesystem::path current_path(oat_dir);
        std::filesystem::path sub_path(sub_dir);
        if (sub_path.is_absolute()) {
            return error("Invalid oat sub directory " + sub_dir);
        }
        for (const std::filesystem::path& component : sub_path) {
            current_path /= component;
            std::string path_str = current_path.string();
            CHECK_ARGUMENT_PATH(path_str);
            if (fs_prepare_dir(path_str.c_str(), S_IRWXU | S_IRWXG | S_IXOTH, AID_SYSTEM,
                               AID_INSTALL)) {
                return error(StringPrintf("Failed to prepare %s", path_str.c_str()));
            }
        }
    }
    return ok();
}

binder::Status InstalldNativeService::rmPackageDir(const std::string& packageName,
                                                   const std::string& packageDir) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PATH(packageDir);
    LOCK_PACKAGE();

    if (validate_apk_path(packageDir.c_str())) {
        return error("Invalid path " + packageDir);
    }
    if (rm_package_dir(packageDir) != 0) {
        return error("Failed to delete " + packageDir);
    }
    return ok();
}

binder::Status InstalldNativeService::linkFile(const std::string& packageName,
                                               const std::string& relativePath,
                                               const std::string& fromBase,
                                               const std::string& toBase) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(fromBase);
    CHECK_ARGUMENT_PATH(toBase);
    LOCK_PACKAGE();

    const char* relative_path = relativePath.c_str();
    const char* from_base = fromBase.c_str();
    const char* to_base = toBase.c_str();
    char from_path[PKG_PATH_MAX];
    char to_path[PKG_PATH_MAX];
    snprintf(from_path, PKG_PATH_MAX, "%s/%s", from_base, relative_path);
    snprintf(to_path, PKG_PATH_MAX, "%s/%s", to_base, relative_path);

    if (validate_apk_path_subdirs(from_path)) {
        return error(StringPrintf("Invalid from path %s", from_path));
    }

    if (validate_apk_path_subdirs(to_path)) {
        return error(StringPrintf("Invalid to path %s", to_path));
    }

    if (link(from_path, to_path) < 0) {
        return error(StringPrintf("Failed to link from %s to %s", from_path, to_path));
    }

    return ok();
}

binder::Status InstalldNativeService::moveAb(const std::string& packageName,
                                             const std::string& apkPath,
                                             const std::string& instructionSet,
                                             const std::string& outputPath) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(apkPath);
    CHECK_ARGUMENT_PATH(outputPath);
    LOCK_PACKAGE();

    const char* apk_path = apkPath.c_str();
    const char* instruction_set = instructionSet.c_str();
    const char* oat_dir = outputPath.c_str();

    bool success = move_ab(apk_path, instruction_set, oat_dir);
    return success ? ok() : error();
}

binder::Status InstalldNativeService::deleteOdex(const std::string& packageName,
                                                 const std::string& apkPath,
                                                 const std::string& instructionSet,
                                                 const std::optional<std::string>& outputPath,
                                                 int64_t* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(apkPath);
    CHECK_ARGUMENT_PATH(outputPath);
    LOCK_PACKAGE();

    const char* apk_path = apkPath.c_str();
    const char* instruction_set = instructionSet.c_str();
    const char* oat_dir = outputPath ? outputPath->c_str() : nullptr;

    *_aidl_return = delete_odex(apk_path, instruction_set, oat_dir);
    return *_aidl_return == -1 ? error() : ok();
}

binder::Status InstalldNativeService::reconcileSecondaryDexFile(
        const std::string& dexPath, const std::string& packageName, int32_t uid,
        const std::vector<std::string>& isas, const std::optional<std::string>& volumeUuid,
        int32_t storage_flag, bool* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(volumeUuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(dexPath);
    const auto userId = multiuser_get_user_id(uid);
    LOCK_PACKAGE_USER();

    bool result = android::installd::reconcile_secondary_dex_file(
            dexPath, packageName, uid, isas, volumeUuid, storage_flag, _aidl_return);
    return result ? ok() : error();
}

binder::Status InstalldNativeService::hashSecondaryDexFile(
        const std::string& dexPath, const std::string& packageName, int32_t uid,
        const std::optional<std::string>& volumeUuid, int32_t storageFlag,
        std::vector<uint8_t>* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(volumeUuid);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(dexPath);

    // mLock is not taken here since we will never modify the file system.
    // If a file is modified just as we are reading it this may result in an
    // anomalous hash, but that's ok.
    bool result = android::installd::hash_secondary_dex_file(
        dexPath, packageName, uid, volumeUuid, storageFlag, _aidl_return);
    return result ? ok() : error();
}
/**
 * Returns true if ioctl feature (F2FS_IOC_FS{GET,SET}XATTR) is supported as
 * these were introduced in Linux 4.14, so kernel versions before that will fail
 * while setting project id attributes. Only when these features are enabled,
 * storage calculation using project_id is enabled
 */
bool check_if_ioctl_feature_is_supported() {
    bool result = false;
    auto temp_path = StringPrintf("%smisc/installd/ioctl_check", android_data_dir.c_str());
    if (access(temp_path.c_str(), F_OK) != 0) {
        int fd = open(temp_path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
        result = set_quota_project_id(temp_path, 0, false) == 0;
        close(fd);
        // delete the temp file
        remove(temp_path.c_str());
    }
    return result;
}

binder::Status InstalldNativeService::setFirstBoot() {
    ENFORCE_UID(AID_SYSTEM);
    std::lock_guard<std::recursive_mutex> lock(mMountsLock);
    std::string uuid;
    if (GetOccupiedSpaceForProjectId(uuid, 0) != -1 && check_if_ioctl_feature_is_supported()) {
        auto first_boot_path =
                StringPrintf("%smisc/installd/using_project_ids", android_data_dir.c_str());
        if (access(first_boot_path.c_str(), F_OK) != 0) {
            close(open(first_boot_path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644));
        }
    }
    return ok();
}

binder::Status InstalldNativeService::invalidateMounts() {
    ENFORCE_UID(AID_SYSTEM);
    std::lock_guard<std::recursive_mutex> lock(mMountsLock);

    mStorageMounts.clear();

#if !BYPASS_QUOTA
    if (!InvalidateQuotaMounts()) {
        return error("Failed to read mounts");
    }
#endif

    std::ifstream in("/proc/mounts");
    if (!in.is_open()) {
        return error("Failed to read mounts");
    }

    std::string source;
    std::string target;
    std::string ignored;
    while (!in.eof()) {
        std::getline(in, source, ' ');
        std::getline(in, target, ' ');
        std::getline(in, ignored);

        if (android::base::GetBoolProperty(kFuseProp, false)) {
            const std::regex kMntFuseRe =
                    std::regex(R"(^/mnt/pass_through/(0|[0-9]+/[A-Z0-9]{4}-[A-Z0-9]{4}).*)");
            if (std::regex_match(target, kMntFuseRe)) {
                LOG(DEBUG) << "Found storage mount " << source << " at " << target;
                mStorageMounts[source] = target;
            }
        } else {
#if !BYPASS_SDCARDFS
            if (target.find(kMntSdcardfs) == 0) {
                LOG(DEBUG) << "Found storage mount " << source << " at " << target;
                mStorageMounts[source] = target;
            }
#endif
        }
    }
    return ok();
}

// Mount volume's CE and DE storage to mirror
binder::Status InstalldNativeService::tryMountDataMirror(
        const std::optional<std::string>& uuid) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
    if (!sAppDataIsolationEnabled) {
        return ok();
    }
    if (!uuid) {
        return error("Should not happen, mounting uuid == null");
    }

    const char* uuid_ = uuid->c_str();

    std::lock_guard<std::recursive_mutex> lock(mMountsLock);

    std::string mirrorVolCePath(StringPrintf("%s/%s", kDataMirrorCePath, uuid_));
    if (fs_prepare_dir(mirrorVolCePath.c_str(), 0511, AID_SYSTEM, AID_SYSTEM) != 0) {
        return error("Failed to create CE data mirror");
    }

    std::string mirrorVolDePath(StringPrintf("%s/%s", kDataMirrorDePath, uuid_));
    if (fs_prepare_dir(mirrorVolDePath.c_str(), 0511, AID_SYSTEM, AID_SYSTEM) != 0) {
        return error("Failed to create DE data mirror");
    }

    std::string mirrorVolMiscCePath(StringPrintf("%s/%s", kMiscMirrorCePath, uuid_));
    if (fs_prepare_dir(mirrorVolMiscCePath.c_str(), 0511, AID_SYSTEM, AID_SYSTEM) != 0) {
        return error("Failed to create CE misc mirror");
    }

    std::string mirrorVolMiscDePath(StringPrintf("%s/%s", kMiscMirrorDePath, uuid_));
    if (fs_prepare_dir(mirrorVolMiscDePath.c_str(), 0511, AID_SYSTEM, AID_SYSTEM) != 0) {
        return error("Failed to create DE misc mirror");
    }

    auto cePath = StringPrintf("%s/user", create_data_path(uuid_).c_str());
    auto dePath = StringPrintf("%s/user_de", create_data_path(uuid_).c_str());
    auto miscCePath = StringPrintf("%s/misc_ce", create_data_path(uuid_).c_str());
    auto miscDePath = StringPrintf("%s/misc_de", create_data_path(uuid_).c_str());

    if (access(cePath.c_str(), F_OK) != 0) {
        return error("Cannot access CE path: " + cePath);
    }
    if (access(dePath.c_str(), F_OK) != 0) {
        return error("Cannot access DE path: " + dePath);
    }
    if (access(miscCePath.c_str(), F_OK) != 0) {
        return error("Cannot access misc CE path: " + cePath);
    }
    if (access(miscDePath.c_str(), F_OK) != 0) {
        return error("Cannot access misc DE path: " + dePath);
    }

    struct stat ceStat, mirrorCeStat;
    if (stat(cePath.c_str(), &ceStat) != 0) {
        return error("Failed to stat " + cePath);
    }
    if (stat(mirrorVolCePath.c_str(), &mirrorCeStat) != 0) {
        return error("Failed to stat " + mirrorVolCePath);
    }

    if (mirrorCeStat.st_ino == ceStat.st_ino && mirrorCeStat.st_dev == ceStat.st_dev) {
        // As it's being called by prepareUserStorage, it can be called multiple times.
        // Hence, we if we mount it already, we should skip it.
        LOG(INFO) << "CE dir is mounted already: " + cePath;
        return ok();
    }

    // Mount CE mirror
    if (TEMP_FAILURE_RETRY(mount(cePath.c_str(), mirrorVolCePath.c_str(), NULL,
            MS_NOSUID | MS_NODEV | MS_NOATIME | MS_BIND | MS_NOEXEC, nullptr)) == -1) {
        return error("Failed to mount " + mirrorVolCePath);
    }

    // Mount DE mirror
    if (TEMP_FAILURE_RETRY(mount(dePath.c_str(), mirrorVolDePath.c_str(), NULL,
            MS_NOSUID | MS_NODEV | MS_NOATIME | MS_BIND | MS_NOEXEC, nullptr)) == -1) {
        return error("Failed to mount " + mirrorVolDePath);
    }

    // Mount misc CE mirror
    if (TEMP_FAILURE_RETRY(mount(miscCePath.c_str(), mirrorVolMiscCePath.c_str(), NULL,
                                 MS_NOSUID | MS_NODEV | MS_NOATIME | MS_BIND | MS_NOEXEC,
                                 nullptr)) == -1) {
        return error("Failed to mount " + mirrorVolMiscCePath);
    }

    // Mount misc DE mirror
    if (TEMP_FAILURE_RETRY(mount(miscDePath.c_str(), mirrorVolMiscDePath.c_str(), NULL,
                                 MS_NOSUID | MS_NODEV | MS_NOATIME | MS_BIND | MS_NOEXEC,
                                 nullptr)) == -1) {
        return error("Failed to mount " + mirrorVolMiscDePath);
    }

    return ok();
}

// Unmount volume's CE and DE storage from mirror
binder::Status InstalldNativeService::onPrivateVolumeRemoved(
        const std::optional<std::string>& uuid) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
    if (!sAppDataIsolationEnabled) {
        return ok();
    }
    if (!uuid) {
        // It happens when private volume failed to mount.
        LOG(INFO) << "Ignore unmount uuid=null";
        return ok();
    }
    const char* uuid_ = uuid->c_str();

    binder::Status res = ok();

    std::string mirrorCeVolPath(StringPrintf("%s/%s", kDataMirrorCePath, uuid_));
    std::string mirrorDeVolPath(StringPrintf("%s/%s", kDataMirrorDePath, uuid_));
    std::string mirrorMiscCeVolPath(StringPrintf("%s/%s", kMiscMirrorCePath, uuid_));
    std::string mirrorMiscDeVolPath(StringPrintf("%s/%s", kMiscMirrorDePath, uuid_));

    std::lock_guard<std::recursive_mutex> lock(mMountsLock);

    // Unmount CE storage
    if (TEMP_FAILURE_RETRY(umount(mirrorCeVolPath.c_str())) != 0) {
        if (errno != ENOENT) {
            res = error(StringPrintf("Failed to umount %s %s", mirrorCeVolPath.c_str(),
                                strerror(errno)));
        }
    }
    if (delete_dir_contents_and_dir(mirrorCeVolPath, true) != 0) {
        res = error("Failed to delete " + mirrorCeVolPath);
    }

    // Unmount DE storage
    if (TEMP_FAILURE_RETRY(umount(mirrorDeVolPath.c_str())) != 0) {
        if (errno != ENOENT) {
            res = error(StringPrintf("Failed to umount %s %s", mirrorDeVolPath.c_str(),
                                strerror(errno)));
        }
    }
    if (delete_dir_contents_and_dir(mirrorDeVolPath, true) != 0) {
        res = error("Failed to delete " + mirrorDeVolPath);
    }

    // Unmount misc CE storage
    if (TEMP_FAILURE_RETRY(umount(mirrorMiscCeVolPath.c_str())) != 0) {
        if (errno != ENOENT) {
            res = error(StringPrintf("Failed to umount %s %s", mirrorMiscCeVolPath.c_str(),
                                     strerror(errno)));
        }
    }
    if (delete_dir_contents_and_dir(mirrorMiscCeVolPath, true) != 0) {
        res = error("Failed to delete " + mirrorMiscCeVolPath);
    }

    // Unmount misc DE storage
    if (TEMP_FAILURE_RETRY(umount(mirrorMiscDeVolPath.c_str())) != 0) {
        if (errno != ENOENT) {
            res = error(StringPrintf("Failed to umount %s %s", mirrorMiscDeVolPath.c_str(),
                                     strerror(errno)));
        }
    }
    if (delete_dir_contents_and_dir(mirrorMiscDeVolPath, true) != 0) {
        res = error("Failed to delete " + mirrorMiscDeVolPath);
    }

    return res;
}

std::string InstalldNativeService::findDataMediaPath(
        const std::optional<std::string>& uuid, userid_t userid) {
    std::lock_guard<std::recursive_mutex> lock(mMountsLock);
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    auto path = StringPrintf("%s/media", create_data_path(uuid_).c_str());
    auto resolved = mStorageMounts[path];
    if (resolved.empty()) {
        LOG(WARNING) << "Failed to find storage mount for " << path;
        resolved = path;
    }
    return StringPrintf("%s/%u", resolved.c_str(), userid);
}

binder::Status InstalldNativeService::isQuotaSupported(
        const std::optional<std::string>& uuid, bool* _aidl_return) {
    *_aidl_return = IsQuotaSupported(uuid.value_or(""));
    return ok();
}

binder::Status InstalldNativeService::prepareAppProfile(const std::string& packageName,
        int32_t userId, int32_t appId, const std::string& profileName, const std::string& codePath,
        const std::optional<std::string>& dexMetadata, bool* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    ENFORCE_VALID_USER_OR_NULL(userId);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(codePath);
    LOCK_PACKAGE_USER();

    *_aidl_return = prepare_app_profile(packageName, userId, appId, profileName, codePath,
        dexMetadata);
    return ok();
}

binder::Status InstalldNativeService::migrateLegacyObbData() {
    ENFORCE_UID(AID_SYSTEM);
    // NOTE: The lint warning doesn't apply to the use of system(3) with
    // absolute parse and no command line arguments.
    if (system("/system/bin/migrate_legacy_obb_data") != 0) { // NOLINT(cert-env33-c)
        LOG(ERROR) << "Unable to migrate legacy obb data";
    }

    return ok();
}

binder::Status InstalldNativeService::cleanupInvalidPackageDirs(
        const std::optional<std::string>& uuid, int32_t userId, int32_t flags) {
    ENFORCE_VALID_USER(userId);
    const char* uuid_cstr = uuid ? uuid->c_str() : nullptr;

    if (flags & FLAG_STORAGE_CE) {
        auto ce_path = create_data_user_ce_path(uuid_cstr, userId);
        cleanup_invalid_package_dirs_under_path(ce_path);
        auto sdksandbox_ce_path =
                create_data_misc_sdk_sandbox_path(uuid_cstr, /*isCeData=*/true, userId);
        cleanup_invalid_package_dirs_under_path(sdksandbox_ce_path);
    }

    if (flags & FLAG_STORAGE_DE) {
        auto de_path = create_data_user_de_path(uuid_cstr, userId);
        cleanup_invalid_package_dirs_under_path(de_path);
        auto sdksandbox_de_path =
                create_data_misc_sdk_sandbox_path(uuid_cstr, /*isCeData=*/false, userId);
        cleanup_invalid_package_dirs_under_path(sdksandbox_de_path);
    }

    return ok();
}

binder::Status InstalldNativeService::getOdexVisibility(
        const std::string& packageName, const std::string& apkPath,
        const std::string& instructionSet, const std::optional<std::string>& outputPath,
        int32_t* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    CHECK_ARGUMENT_PATH(apkPath);
    CHECK_ARGUMENT_PATH(outputPath);
    LOCK_PACKAGE();

    const char* apk_path = apkPath.c_str();
    const char* instruction_set = instructionSet.c_str();
    const char* oat_dir = outputPath ? outputPath->c_str() : nullptr;

    *_aidl_return = get_odex_visibility(apk_path, instruction_set, oat_dir);
    return *_aidl_return == -1 ? error() : ok();
}

// Creates an auth token to be used in enableFsverity. This token is really to store a proof that
// the caller can write to a file, represented by the authFd. Effectively, system_server as the
// attacker-in-the-middle cannot enable fs-verity on arbitrary app files. If the FD is not writable,
// return null.
//
// app process uid is passed for additional ownership check, such that one app can not be
// authenticated for another app's file. These parameters are assumed trusted for this purpose of
// consistency check.
//
// Notably, creating the token allows us to manage the writable FD easily during enableFsverity.
// Since enabling fs-verity to a file requires no outstanding writable FD, passing the authFd to the
// server allows the server to hold the only reference (as long as the client app doesn't).
binder::Status InstalldNativeService::createFsveritySetupAuthToken(
        const ParcelFileDescriptor& authFd, int32_t uid,
        sp<IFsveritySetupAuthToken>* _aidl_return) {
    CHECK_ARGUMENT_APP_ID(multiuser_get_app_id(uid));
    ENFORCE_VALID_USER(multiuser_get_user_id(uid));

    auto token = sp<FsveritySetupAuthToken>::make();
    binder::Status status = token->authenticate(authFd, uid);
    if (!status.isOk()) {
        return status;
    }
    *_aidl_return = token;
    return ok();
}

// Enables fs-verity for filePath, which must be an absolute path and the same inode as in the auth
// token previously returned from createFsveritySetupAuthToken, and owned by the app uid. As
// installd is more privileged than its client / system server, we attempt to limit what a
// (compromised) client can do.
//
// The reason for this app request to go through installd is to avoid exposing a risky area (PKCS#7
// signature verification) in the kernel to the app as an attack surface (it can't be system server
// because it can't override DAC and manipulate app files). Note that we should be able to drop
// these hops and simply the app calls the ioctl, once all upgrading devices run with a kernel
// without fs-verity built-in signature (https://r.android.com/2650402).
binder::Status InstalldNativeService::enableFsverity(const sp<IFsveritySetupAuthToken>& authToken,
                                                     const std::string& filePath,
                                                     const std::string& packageName,
                                                     int32_t* _aidl_return) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_PATH(filePath);
    CHECK_ARGUMENT_PACKAGE_NAME(packageName);
    LOCK_PACKAGE();
    if (authToken == nullptr) {
        return exception(binder::Status::EX_ILLEGAL_ARGUMENT, "Received a null auth token");
    }

    // Authenticate to check the targeting file is the same inode as the authFd. With O_PATH, we
    // prevent a malicious client from blocking installd by providing a path to FIFO. After the
    // authentication, the actual open is safe.
    sp<IBinder> authTokenBinder = IInterface::asBinder(authToken)->localBinder();
    if (authTokenBinder == nullptr) {
        return exception(binder::Status::EX_SECURITY, "Received a non-local auth token");
    }
    unique_fd pathFd(open(filePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_PATH));
    // Returns a constant errno to avoid one app probing file existence of the others, before the
    // authentication is done.
    const int kFixedErrno = EPERM;
    if (pathFd.get() < 0) {
        PLOG(DEBUG) << "Failed to open the path";
        *_aidl_return = kFixedErrno;
        return ok();
    }
    std::string procFdPath(StringPrintf("/proc/self/fd/%d", pathFd.get()));
    struct stat stFromPath;
    if (stat(procFdPath.c_str(), &stFromPath) < 0) {
        PLOG(DEBUG) << "Failed to stat proc fd " << pathFd.get() << " -> " << filePath;
        *_aidl_return = kFixedErrno;
        return ok();
    }
    auto authTokenInstance = sp<FsveritySetupAuthToken>::cast(authTokenBinder);
    if (!authTokenInstance->isSameStat(stFromPath)) {
        LOG(DEBUG) << "FD authentication failed";
        *_aidl_return = kFixedErrno;
        return ok();
    }

    unique_fd rfd(open(procFdPath.c_str(), O_RDONLY | O_CLOEXEC));
    fsverity_enable_arg arg = {};
    arg.version = 1;
    arg.hash_algorithm = FS_VERITY_HASH_ALG_SHA256;
    arg.block_size = 4096;
    if (ioctl(rfd.get(), FS_IOC_ENABLE_VERITY, &arg) < 0) {
        *_aidl_return = errno;
    } else {
        *_aidl_return = 0;
    }
    return ok();
}

// PinnedPath holds an opened file descriptor to the parent directory of a path
// that has been verified to not contain any symlinks and has had its entire
// tree recursively verified for ownership. This allows users of this
// structure to safely perform operations on the full path, by only having to verify
// that the basename itself is not a symlink.
//
// fd: file descriptor to the parent directory.
// name: basename of the file/directory within that parent.
// path: the full absolute path as a string (primarily for logging).
// appDataPath: the full absolute path of the app's data directory
struct PinnedPath {
    android::base::unique_fd fd;
    std::string name;
    std::string path;
    std::string appDataPath;
};

/**
 * Recursively verifies that all files and directories under a given path are owned by the
 * specified callerUid. This is a critical security check to prevent applications from
 * manipulating or adopting data they do not own through privileged installd operations.
 *
 * @param dfd File descriptor to the parent directory.
 * @param name Basename of the item to verify.
 * @param callerUid The UID that must own all items in the tree.
 * @param absolute_path Full path for error reporting.
 * @param notifyError Callback to report failure status and messages.
 * @return true if the entire tree is owned by callerUid, false otherwise.
 */
static bool verify_app_data_recursive(int dfd, const char* name, uid_t callerUid,
                                      const std::string& absolute_path,
                                      std::function<void(int, const std::string&)> notifyError) {
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        notifyError(IAppDataOperationCallback::STATUS_FAILURE, "Failed to stat " + absolute_path);
        return false;
    }

    if (st.st_uid != callerUid) {
        notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                    "PERMISSION_DENIED: Source " + absolute_path + " does not belong to caller");
        return false;
    }

    if (S_ISLNK(st.st_mode)) return true;

    if (S_ISDIR(st.st_mode)) {
        android::base::unique_fd sub_dfd(
                openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (sub_dfd.get() < 0) {
            notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                        "Failed to open " + absolute_path);
            return false;
        }

        DIR* d = Fdopendir(std::move(sub_dfd));
        if (d == nullptr) {
            notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                        "Failed to open " + absolute_path);
            return false;
        }

        struct dirent* de;
        for (;;) {
            // Reset errno before reading the next file
            errno = 0;
            de = readdir(d);

            // if readdir returns nullptr, either there is no next file, or we hit an error.
            // in the event there is an error, treat this verification as a failure and return.
            if (de == nullptr) {
                if (errno != 0) {
                    notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                                "Failed to read directory " + absolute_path);
                    closedir(d);
                    return false;
                }
                break;
            }

            const char* child_name = de->d_name;

            // Don't follow the self or parent links, just move to the next file.
            if (strcmp(child_name, ".") == 0 || strcmp(child_name, "..") == 0) continue;

            std::string child_absolute_path = absolute_path + "/" + child_name;
            if (!verify_app_data_recursive(dirfd(d), child_name, callerUid, child_absolute_path,
                                           notifyError)) {
                closedir(d);
                return false;
            }
        }
        closedir(d);
    }
    return true;
}

/**
 * Validates the source path for an app data operation and performs a deep ownership check.
 * It ensures the path is valid for the given user, pins the parent directory to prevent
 * TOCTOU attacks, and recursively verifies that the entire source tree is owned by the caller.
 *
 * @param uuid Volume UUID.
 * @param from The absolute source path.
 * @param userId The user ID the operation is for.
 * @param callerUid The UID of the application that initiated the request.
 * @param notifyError Callback for error reporting.
 * @return A PinnedPath object if verification succeeds, std::nullopt otherwise.
 */
static std::optional<PinnedPath> verify_app_data_source(
        const std::optional<std::string>& uuid, const std::string& from, int userId,
        int32_t callerUid, std::function<void(int, const std::string&)> notifyError) {
    const char* uuid_ptr = uuid ? uuid->c_str() : nullptr;
    auto [root, suffix] = split_app_data_path(uuid_ptr, userId, from);
    if (root.empty() || suffix.empty()) {
        notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                    "Failed to split app data path: " + from);
        return std::nullopt;
    }

    std::string appDataPath = root;
    if (suffix.size() > 1 && suffix[0] == '/') {
        size_t pos = suffix.find('/', 1);
        if (pos != std::string::npos) {
            appDataPath += suffix.substr(0, pos);
        } else {
            appDataPath += suffix;
        }
    }

    struct stat st;
    if (stat(appDataPath.c_str(), &st) != 0) {
        notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                    "App data root does not exist: " + appDataPath);
        return std::nullopt;
    }

    std::string baseName;
    android::base::unique_fd dfd = open_parent_recursive(from, root, &baseName);
    if (dfd.get() < 0) {
        if (errno == ENOENT) {
            notifyError(IAppDataOperationCallback::STATUS_FAILURE, "Source does not exist");
        } else {
            notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                        "Failed to open source parent dir: " + from + ": " + strerror(errno));
        }
        return std::nullopt;
    }

    // Final check for the last component
    if (!verify_app_data_recursive(dfd.get(), baseName.c_str(), static_cast<uid_t>(callerUid), from,
                                   notifyError)) {
        return std::nullopt;
    }

    return PinnedPath{std::move(dfd), baseName, from, appDataPath};
}

static std::optional<PinnedPath> setup_app_data_target(
        const std::optional<std::string>& uuid, const std::string& from, const std::string& to,
        int32_t userId, int32_t uid, std::function<void(int, const std::string&)> notifyError) {
    const char* uuid_ = uuid ? uuid->c_str() : nullptr;
    auto [root, suffix] = split_app_data_path(uuid_, userId, to);
    if (root.empty() || suffix.empty()) {
        notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                    "Failed to split app data path: " + to);
        return std::nullopt;
    }

    std::string appDataPath = root;
    if (suffix.size() > 1 && suffix[0] == '/') {
        size_t pos = suffix.find('/', 1);
        if (pos != std::string::npos) {
            appDataPath += suffix.substr(0, pos);
        } else {
            appDataPath += suffix;
        }
    }

    struct stat st;
    if (stat(appDataPath.c_str(), &st) != 0) {
        notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                    "App data root does not exist: " + appDataPath);
        return std::nullopt;
    }
    mode_t mode = 0771;

    android::base::unique_fd dfd = mkdirs_recursive(to, root, uid, uid, mode);
    if (dfd.get() < 0) {
        notifyError(IAppDataOperationCallback::STATUS_FAILURE,
                    "Failed to create dest dir: " + to + ": " + strerror(errno));
        return std::nullopt;
    }

    std::string baseName = android::base::Basename(from);
    return PinnedPath{std::move(dfd), baseName, to + "/" + baseName, appDataPath};
}

static void copy_app_data_recursive(int src_dfd, int dst_dfd, const char* name,
                                    const std::string& absolute_path, uid_t uid,
                                    std::vector<std::string>* failed_files) {
    auto fail = [&](const char* msg) {
        PLOG(WARNING) << msg << " " << name;
        failed_files->push_back(absolute_path);
    };

    struct stat st;
    if (fstatat(src_dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        fail("Failed to stat");
        return;
    }

    if (S_ISLNK(st.st_mode)) return;

    if (S_ISDIR(st.st_mode)) {
        if (mkdirat(dst_dfd, name, st.st_mode & 07777) != 0 && errno != EEXIST) {
            fail("Failed to mkdirat");
            return;
        }
        unique_fd sub_src_fd(
                openat(src_dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (sub_src_fd.get() < 0) {
            fail("Failed to openat src");
            return;
        }
        unique_fd sub_dst_fd(
                openat(dst_dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (sub_dst_fd.get() < 0) {
            fail("Failed to openat dst");
            return;
        }
        bool failed = false;
        if (fchown(sub_dst_fd.get(), uid, uid) != 0) {
            PLOG(WARNING) << "Failed to fchown " << absolute_path;
            failed = true;
        }
        if (fchmod(sub_dst_fd.get(), st.st_mode & 07777) != 0) {
            PLOG(WARNING) << "Failed to fchmod " << absolute_path;
            failed = true;
        }
        if (failed) {
            failed_files->push_back(absolute_path);
        }

        DIR* d = Fdopendir(std::move(sub_src_fd));
        if (d == nullptr) {
            PLOG(WARNING) << "Failed to fdopendir";
            failed_files->push_back(absolute_path);
            return;
        }
        struct dirent* de;
        while ((de = readdir(d))) {
            const char* child_name = de->d_name;
            if (strcmp(child_name, ".") == 0 || strcmp(child_name, "..") == 0) continue;
            std::string child_absolute_path = absolute_path + "/" + child_name;
            copy_app_data_recursive(dirfd(d), sub_dst_fd.get(), child_name, child_absolute_path,
                                    uid, failed_files);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        unique_fd file_src_fd(openat(src_dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (file_src_fd.get() < 0) {
            fail("Failed to openat src file");
            return;
        }
        unique_fd file_dst_fd(
                openat(dst_dfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (file_dst_fd.get() < 0) {
            fail("Failed to openat dst file");
            return;
        }
        if (!copy_simple_file(file_src_fd.get(), file_dst_fd.get(), uid, uid, st.st_mode,
                              st.st_size, [&](const char* msg) { fail(msg); })) {
            return;
        }
    }
}

static void copy_app_data(const std::optional<std::string>& uuid, const std::string& from,
                          const std::string& to, int32_t userId, int32_t appId,
                          const std::string& seInfo, int32_t flags, int32_t callerUid,
                          android::sp<IAppDataOperationCallback> callback) {
    (void)flags;

    auto notifyStatus = [&](int status, const std::string& msg,
                            const std::vector<std::string>& failedFiles) {
        if (callback) {
            std::vector<std::optional<std::string>> aidlFailedFiles;
            for (const auto& f : failedFiles) {
                aidlFailedFiles.push_back(std::make_optional(f));
            }
            callback->onStatusChanged(status, msg, std::make_optional(aidlFailedFiles));
        }
    };
    auto notifyError = [&](int s, const std::string& m) { notifyStatus(s, m, {from}); };

    uid_t uid = multiuser_get_uid(userId, appId);

    notifyStatus(IAppDataOperationCallback::STATUS_RUNNING, "", {});

    // Verify the source (recursively)
    auto source = verify_app_data_source(uuid, from, userId, callerUid, notifyError);
    if (!source) return;

    auto target = setup_app_data_target(uuid, from, to, userId, uid, notifyError);
    if (!target) return;

    std::vector<std::string> failed_files;
    copy_app_data_recursive(source->fd.get(), target->fd.get(), source->name.c_str(), from, uid,
                            &failed_files);

    if (selinux_android_restorecon_pkgdir(target->appDataPath.c_str(), seInfo.c_str(), uid,
                                          SELINUX_ANDROID_RESTORECON_RECURSE) < 0) {
        notifyStatus(IAppDataOperationCallback::STATUS_FAILURE, "Restorecon failed", failed_files);
    } else if (!failed_files.empty()) {
        notifyStatus(IAppDataOperationCallback::STATUS_FAILURE, "Some files failed to copy",
                     failed_files);
    } else {
        notifyStatus(IAppDataOperationCallback::STATUS_SUCCESS, "", failed_files);
    }
}

static void move_app_data(const std::optional<std::string>& uuid, const std::string& from,
                          const std::string& to, int32_t userId, int32_t appId,
                          const std::string& seInfo, int32_t flags, int32_t callerUid,
                          android::sp<IAppDataOperationCallback> callback) {
    (void)flags;

    auto notifyStatus = [&](int status, const std::string& msg,
                            const std::vector<std::string>& failedFiles) {
        if (callback) {
            std::vector<std::optional<std::string>> aidlFailedFiles;
            for (const auto& f : failedFiles) {
                aidlFailedFiles.push_back(std::make_optional(f));
            }
            callback->onStatusChanged(status, msg, std::make_optional(aidlFailedFiles));
        }
    };
    auto notifyError = [&](int s, const std::string& m) { notifyStatus(s, m, {from}); };

    uid_t uid = multiuser_get_uid(userId, appId);

    notifyStatus(IAppDataOperationCallback::STATUS_RUNNING, "", {});

    // Moving app data by a highly privileged component like installd is a delicate operation.
    // The main risk stems from the fact that unprivileged apps are allowed to use symlinks.
    // This can create attack vectors such as the app having a file in its own data directory
    // point to a file belonging to another app, and then having installd 'migrate' it,
    // thereby granting the package access to the file.
    //
    // The code here prevents that by the following:
    //
    // 1. It verifies the source path is normalized and does not contain any symlinks;
    //    it does that using file-descriptor pinning, which prevents TOCTOU attacks - eg
    //    a file being changed to a symlink *after* we'd concluced that it wasn't a symlink
    //
    // 2. It sets up the target path in much the same way - creating directories when needed,
    //    but never following any symlinks.
    //
    // 3. We must recursively chown the target path to the correct uid; again,
    //    we carefully traverse the path to not follow any symlinks.
    //
    // 4. Finally, selinux_android_restorecon is called; since restorecon itself resolves
    //    paths fully, it will just apply the correct context according to the path and the
    //    passed in label.
    //
    // Note that we do fully trust the passed in paths and appId parameters, as this is only
    // callable from system UIDs. The thing we cannot trust is the files under those paths.

    // Verify the source (recursively)
    auto source = verify_app_data_source(uuid, from, userId, callerUid, notifyError);
    if (!source) return;

    // Setup the target
    auto target = setup_app_data_target(uuid, from, to, userId, uid, notifyError);
    if (!target) return;

    // Do an atomic rename
    if (renameat(source->fd.get(), source->name.c_str(), target->fd.get(), source->name.c_str()) !=
        0) {
        notifyStatus(IAppDataOperationCallback::STATUS_FAILURE,
                     "Rename failed: " + std::string(strerror(errno)), {from});
        return;
    }

    // Fix up the permissions recursively
    std::vector<std::string> failed_files;
    {
        android::base::unique_fd fd(
                openat(target->fd.get(), source->name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (fd.get() >= 0) {
            chown_recursive(fd.get(), uid, uid, target->path, &failed_files);
        } else {
            PLOG(WARNING) << "Failed to open moved data for chown";
            failed_files.push_back(target->path);
        }
    }

    if (selinux_android_restorecon_pkgdir(target->appDataPath.c_str(), seInfo.c_str(), uid,
                                          SELINUX_ANDROID_RESTORECON_RECURSE) < 0) {
        notifyStatus(IAppDataOperationCallback::STATUS_FAILURE, "Restorecon failed", failed_files);
    } else if (!failed_files.empty()) {
        notifyStatus(IAppDataOperationCallback::STATUS_FAILURE, "Some files failed to chown",
                     failed_files);
    } else {
        notifyStatus(IAppDataOperationCallback::STATUS_SUCCESS, "", failed_files);
    }
}

binder::Status InstalldNativeService::copyAppDataPath(
        const std::optional<std::string>& uuid, const std::string& fromPath,
        const std::string& toPath, int32_t userId, int32_t appId, const std::string& seInfo,
        int32_t flags, int32_t callerUid, const android::sp<IAppDataOperationCallback>& callback) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PATH(fromPath);
    CHECK_ARGUMENT_PATH(toPath);
    std::thread t(copy_app_data, uuid, fromPath, toPath, userId, appId, seInfo, flags, callerUid,
                  callback);
    t.detach();
    return ok();
}

binder::Status InstalldNativeService::moveAppDataPath(
        const std::optional<std::string>& uuid, const std::string& fromPath,
        const std::string& toPath, int32_t userId, int32_t appId, const std::string& seInfo,
        int32_t flags, int32_t callerUid, const android::sp<IAppDataOperationCallback>& callback) {
    ENFORCE_UID(AID_SYSTEM);
    CHECK_ARGUMENT_UUID(uuid);
    CHECK_ARGUMENT_PATH(fromPath);
    CHECK_ARGUMENT_PATH(toPath);
    std::thread t(move_app_data, uuid, fromPath, toPath, userId, appId, seInfo, flags, callerUid,
                  callback);
    t.detach();
    return ok();
}

}  // namespace installd
}  // namespace android
