// App-ops service ("appops"). SystemServiceRegistry builds AppOpsManager from
// ServiceManager.getServiceOrThrow("appops"), so without it
// getSystemService(APP_OPS_SERVICE) is null. Methods follow AOSP 15
// AppOpsService and are added as apps are observed calling them.
final class FrameworkAppOps {
    private static final int MODE_ALLOWED = 0, MODE_ERRORED = 2;  // AppOpsManager
    private static final int INVALID_UID = -1, ROOT_UID = 0, SHELL_UID = 2000,
            MEDIA_UID = 1013, AUDIOSERVER_UID = 1041, CAMERASERVER_UID = 1047;  // Process
    private final FrameworkPackages packages;

    FrameworkAppOps(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("appops", "com.android.internal.app.IAppOpsService", this::call);
    }

    private Object call(String method, Object[] a) throws Exception {
        switch (method) {
            // Firebase measurement (GMS UidVerifier) via AppOpsManager.checkPackage.
            case "checkPackage": return checkPackage((Integer) a[0], (String) a[1]);
            default:
                System.out.println("APPOPS_UNSUPPORTED " + method);
                throw new UnsupportedOperationException("appops." + method);
        }
    }

    // AppOpsService.checkPackage: verifyAndGetBypass, then the special name
    // must resolve to uid or the package must exist. Failures are
    // MODE_ERRORED, which AppOpsManager turns into SecurityException.
    private int checkPackage(int uid, String packageName) {
        if (packageName == null) throw new NullPointerException("packageName");
        int existing = packages.packageUid(packageName);
        if (!belongs(uid, packageName, existing)) return MODE_ERRORED;
        return specialUid(packageName) == uid || existing != -1 ? MODE_ALLOWED : MODE_ERRORED;
    }

    // verifyAndGetBypass: root may name anything; otherwise a fixed system
    // name or the installed package must belong to the uid's app id. This
    // profile has one user, so the package uid and caller uid share it.
    private static boolean belongs(int uid, String packageName, int existing) {
        if (uid == ROOT_UID) return true;
        int appId = uid % 100000;  // UserHandle.getAppId
        int fixed = packageName.equals("com.android.shell") ? SHELL_UID : specialUid(packageName);
        if (fixed != INVALID_UID) return fixed == appId;
        return existing != -1 && existing == uid;
    }

    // AppOpsService.resolveUid.
    private static int specialUid(String packageName) {
        switch (packageName) {
            case "root": return ROOT_UID;
            case "shell": case "dumpstate": return SHELL_UID;
            case "media": return MEDIA_UID;
            case "audioserver": return AUDIOSERVER_UID;
            case "cameraserver": return CAMERASERVER_UID;
            default: return INVALID_UID;
        }
    }
}
