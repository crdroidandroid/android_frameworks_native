/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.os;

/** @hide */
interface IInstalld {
    void createUserData(@nullable @utf8InCpp String uuid, int userId, int userSerial, int flags);
    void destroyUserData(@nullable @utf8InCpp String uuid, int userId, int flags);
    void setFirstBoot();
    android.os.CreateAppDataResult createAppData(in android.os.CreateAppDataArgs args);
    android.os.CreateAppDataResult[] createAppDataBatched(in android.os.CreateAppDataArgs[] args);

    void reconcileSdkData(in android.os.ReconcileSdkDataArgs args);

    void restoreconAppData(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
            int userId, int flags, int appId, @utf8InCpp String seInfo);
    void migrateAppData(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
            int userId, int flags);
    void clearAppData(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
            int userId, int flags, long ceDataInode, long pccCeDataInode);
    void destroyAppData(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
            int userId, int flags, long ceDataInode, long pccCeDataInode);
    void destroyPccData(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
         int userId, int flags, long ceDataInode);

    void fixupAppData(@nullable @utf8InCpp String uuid, int flags);

    long[] getAppSize(@nullable @utf8InCpp String uuid, in @utf8InCpp String[] packageNames,
            int userId, int flags, int appId, int pccId, in long[] ceDataInodes,
            in @utf8InCpp String[] codePaths);
    long[] getUserSize(@nullable @utf8InCpp String uuid, int userId, int flags, in int[] appIds,
            in int[] pccIds);
    long[] getExternalSize(@nullable @utf8InCpp String uuid, int userId, int flags, in int[] appIds);

    @nullable
    android.os.storage.CrateMetadata[] getAppCrates(
            @nullable @utf8InCpp String uuid, in @utf8InCpp String[] packageNames,
             int userId);
    @nullable
    android.os.storage.CrateMetadata[] getUserCrates(
            @nullable @utf8InCpp String uuid, int userId);

    void setAppQuota(@nullable @utf8InCpp String uuid, int userId, int appId, long cacheQuota);

    void moveCompleteApp(@nullable @utf8InCpp String fromUuid, @nullable @utf8InCpp String toUuid,
            @utf8InCpp String packageName, int appId, int pccId,
            @utf8InCpp String seInfo, int targetSdkVersion, @utf8InCpp String fromCodePath);

    // Returns false if it is cancelled. Returns true if it is completed or have other errors.
    boolean dexopt(@utf8InCpp String apkPath, int uid, @utf8InCpp String packageName,
            @utf8InCpp String instructionSet, int dexoptNeeded,
            @nullable @utf8InCpp String outputPath, int dexFlags,
            @utf8InCpp String compilerFilter, @nullable @utf8InCpp String uuid,
            @nullable @utf8InCpp String sharedLibraries,
            @nullable @utf8InCpp String seInfo, boolean downgrade, int targetSdkVersion,
            @nullable @utf8InCpp String profileName,
            @nullable @utf8InCpp String dexMetadataPath,
            @nullable @utf8InCpp String compilationReason);
    // Blocks (when block is true) or unblock (when block is false) dexopt.
    // Blocking also invloves cancelling the currently running dexopt.
    void controlDexOptBlocking(boolean block);

    void rmdex(@utf8InCpp String codePath, @utf8InCpp String instructionSet);

    int mergeProfiles(int uid, @utf8InCpp String packageName, @utf8InCpp String profileName);
    boolean dumpProfiles(int uid, @utf8InCpp String packageName, @utf8InCpp String  profileName,
            @utf8InCpp String codePath, boolean dumpClassesAndMethods);
    boolean copySystemProfile(@utf8InCpp String systemProfile, int uid,
            @utf8InCpp String packageName, @utf8InCpp String profileName);
    void clearAppProfiles(@utf8InCpp String packageName, @utf8InCpp String profileName);
    void destroyAppProfiles(@utf8InCpp String packageName);
    void deleteReferenceProfile(@utf8InCpp String packageName, @utf8InCpp String profileName);

    boolean createProfileSnapshot(int appId, @utf8InCpp String packageName,
            @utf8InCpp String profileName, @utf8InCpp String classpath);
    void destroyProfileSnapshot(@utf8InCpp String packageName, @utf8InCpp String profileName);

    void rmPackageDir(@utf8InCpp String packageName, @utf8InCpp String packageDir);
    void freeCache(@nullable @utf8InCpp String uuid, long targetFreeBytes, int flags);
    void linkNativeLibraryDirectory(@nullable @utf8InCpp String uuid,
            @utf8InCpp String packageName, @utf8InCpp String nativeLibPath32, int userId);
    void createOatDirs(@utf8InCpp String packageName, @utf8InCpp String oatDir,
            in @utf8InCpp List<String> oatSubDirs);
    void linkFile(@utf8InCpp String packageName, @utf8InCpp String relativePath,
            @utf8InCpp String fromBase, @utf8InCpp String toBase);
    void moveAb(@utf8InCpp String packageName, @utf8InCpp String apkPath,
            @utf8InCpp String instructionSet, @utf8InCpp String outputPath);
    long deleteOdex(@utf8InCpp String packageName, @utf8InCpp String apkPath,
            @utf8InCpp String instructionSet, @nullable @utf8InCpp String outputPath);

    boolean reconcileSecondaryDexFile(@utf8InCpp String dexPath, @utf8InCpp String pkgName,
        int uid, in @utf8InCpp String[] isas, @nullable @utf8InCpp String volume_uuid,
        int storage_flag);

    byte[] hashSecondaryDexFile(@utf8InCpp String dexPath, @utf8InCpp String pkgName,
        int uid, @nullable @utf8InCpp String volumeUuid, int storageFlag);

    void invalidateMounts();
    boolean isQuotaSupported(@nullable @utf8InCpp String uuid);

    boolean prepareAppProfile(@utf8InCpp String packageName,
        int userId, int appId, @utf8InCpp String profileName, @utf8InCpp String codePath,
        @nullable @utf8InCpp String dexMetadata);

    long snapshotAppData(@nullable @utf8InCpp String uuid, in @utf8InCpp String packageName,
            int userId, int snapshotId, int storageFlags);
    void restoreAppDataSnapshot(@nullable @utf8InCpp String uuid, in @utf8InCpp String packageName,
            int appId, int pccId, @utf8InCpp String seInfo, int user, int snapshotId, int storageflags);
    void destroyAppDataSnapshot(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
            int userId, long ceSnapshotInode, int snapshotId, int storageFlags);
    void destroyCeSnapshotsNotSpecified(@nullable @utf8InCpp String uuid, int userId,
            in int[] retainSnapshotIds);

    void tryMountDataMirror(@nullable @utf8InCpp String volumeUuid);
    void onPrivateVolumeRemoved(@nullable @utf8InCpp String volumeUuid);

    void migrateLegacyObbData();

    void cleanupInvalidPackageDirs(@nullable @utf8InCpp String uuid, int userId, int flags);

    int getOdexVisibility(@utf8InCpp String packageName, @utf8InCpp String apkPath,
            @utf8InCpp String instructionSet, @nullable @utf8InCpp String outputPath);

    interface IFsveritySetupAuthToken {
        // Using an interface here is an easy way to create and maintain an IBinder object across
        // the processes. When installd creates this binder object, it stores the file stat
        // privately for later authentication, and only returns the reference to the caller process.
        // Once the binder object has no reference count, it gets destructed automatically
        // (alternatively, installd can maintain an internal mapping, but it is more error prone
        // because the app may crash and not finish the fs-verity setup, keeping the memory unused
        // forever).
        //
        // We don't necessarily need a method here, so it's left blank intentionally.
    }
    IFsveritySetupAuthToken createFsveritySetupAuthToken(in ParcelFileDescriptor authFd, int uid);
    int enableFsverity(in IFsveritySetupAuthToken authToken, @utf8InCpp String filePath,
            @utf8InCpp String packageName);

    /**
     * Callback interface for long-running app data operations.
     */
    oneway interface IAppDataOperationCallback {
        const int STATUS_UNKNOWN = 0;
        const int STATUS_RUNNING = 1;
        const int STATUS_SUCCESS = 2;
        const int STATUS_FAILURE = 3;

        /**
         * Called when the status of the operation changes.
         *
         * @param status One of the STATUS_* constants.
         * @param message Optional descriptive message (e.g., error details).
         * @param failedFiles Optional list of files that failed to copy/move.
         */
        void onStatusChanged(int status, @nullable @utf8InCpp String message,
                in @nullable @utf8InCpp String[] failedFiles);
    }

    /**
     * Asynchronously copies a file or directory from one app data location to another.
     *
     * @param uuid The volume UUID (or null for internal storage).
     * @param fromPath The absolute source path.
     * @param toPath The absolute destination path.
     * @param userId The user ID the destination path belongs to.
     * @param appId The app ID the destination path belongs to (used for ownership).
     * @param seInfo The SELinux label to apply to the destination.
     * @param flags Operation flags.
     * @param callerUid The UID of the caller who requested the operation.
     * @param callback Callback for status updates.
     */
    oneway void copyAppDataPath(@nullable @utf8InCpp String uuid, @utf8InCpp String fromPath,
            @utf8InCpp String toPath, int userId, int appId, @utf8InCpp String seInfo, int flags,
            int callerUid, IAppDataOperationCallback callback);

    /**
     * Asynchronously moves a file or directory from one app data location to another.
     *
     * @param uuid The volume UUID (or null for internal storage).
     * @param fromPath The absolute source path.
     * @param toPath The absolute destination path.
     * @param userId The user ID the destination path belongs to.
     * @param appId The app ID the destination path belongs to (used for ownership).
     * @param seInfo The SELinux label to apply to the destination.
     * @param flags Operation flags.
     * @param callerUid The UID of the caller who requested the operation.
     * @param callback Callback for status updates.
     */
    oneway void moveAppDataPath(@nullable @utf8InCpp String uuid, @utf8InCpp String fromPath,
            @utf8InCpp String toPath, int userId, int appId, @utf8InCpp String seInfo, int flags,
            int callerUid, IAppDataOperationCallback callback);

    /**
     * Overwrites every *emoji*.ttf the app owns in its CE and DE data with the font in
     * fontFd, then writes it to each of lockedFiles (paths relative to the CE data dir) as a
     * root-owned read-only file. Returns the number of files written.
     */
    int replaceAppEmojiFonts(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
            int userId, int appId, in ParcelFileDescriptor fontFd,
            in @utf8InCpp String[] lockedFiles);

    /**
     * Empties a directory relative to the app's CE data dir. With block, the directory is
     * created if missing and left root-owned mode 000 so the app can't write into it. Without
     * block, a directory blocked this way is handed back to the app.
     */
    void clearAppDataSubdir(@nullable @utf8InCpp String uuid, @utf8InCpp String packageName,
            int userId, int appId, @utf8InCpp String relativePath, boolean block);

    const int FLAG_STORAGE_DE = 0x1;
    const int FLAG_STORAGE_CE = 0x2;
    const int FLAG_STORAGE_EXTERNAL = 0x4;
    const int FLAG_STORAGE_SDK = 0x8;

    const int FLAG_CLEAR_CACHE_ONLY = 0x10;
    const int FLAG_CLEAR_CODE_CACHE_ONLY = 0x20;

    const int FLAG_FREE_CACHE_V2 = 0x100;
    const int FLAG_FREE_CACHE_V2_DEFY_QUOTA = 0x200;
    const int FLAG_FREE_CACHE_NOOP = 0x400;
    // Set below flag to clear cache irrespective of target free bytes required
    const int FLAG_FREE_CACHE_DEFY_TARGET_FREE_BYTES = 0x800;

    const int FLAG_USE_QUOTA = 0x1000;
    const int FLAG_FORCE = 0x2000;

    const int FLAG_CLEAR_APP_DATA_KEEP_ART_PROFILES = 0x20000;
}
