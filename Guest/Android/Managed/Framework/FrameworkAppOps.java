// App-ops service ("appops"). SystemServiceRegistry builds AppOpsManager from
// ServiceManager.getServiceOrThrow("appops"), so without it
// getSystemService(APP_OPS_SERVICE) is null. Methods follow AOSP 15
// AppOpsService and are added as apps are observed calling them.
final class FrameworkAppOps {
    private static final int MODE_ALLOWED = 0, MODE_ERRORED = 2;  // AppOpsManager
    private static final int INVALID_UID = -1, ROOT_UID = 0, SHELL_UID = 2000,
            MEDIA_UID = 1013, AUDIOSERVER_UID = 1041, CAMERASERVER_UID = 1047;  // Process
    private final FrameworkPackages packages;
    private final java.util.Set<android.os.IBinder> activeWatchers = new java.util.HashSet<>();
    private static final int NUM_OP = numOps();  // AppOpsManager._NUM_OP (hidden)

    private static int numOps() {
        try { return android.app.AppOpsManager.class.getField("_NUM_OP").getInt(null); }
        catch (ReflectiveOperationException error) { throw new ExceptionInInitializerError(error); }
    }

    FrameworkAppOps(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("appops", "com.android.internal.app.IAppOpsService", this::call);
    }

    private Object call(String method, Object[] a) throws Exception {
        switch (method) {
            // Firebase measurement (GMS UidVerifier) via AppOpsManager.checkPackage.
            case "checkPackage": return checkPackage((Integer) a[0], (String) a[1]);
            case "checkOperation":
                // OP_WRITE_SETTINGS: no user override, so Settings.canWrite
                // falls back to the install-time (pre-M) permission grant.
                if ((Integer) a[0] != 23) throw new UnsupportedOperationException("appops.checkOperation " + a[0]);
                if (checkPackage((Integer) a[1], (String) a[2]) != MODE_ALLOWED) return MODE_ERRORED;
                return 3; // MODE_DEFAULT
            // AppOpsService.startWatchingActive/stopWatchingActive (Google ads
            // SDK). Watchers hear when an op becomes active; this service
            // starts no ops, so registered callbacks are never called.
            case "startWatchingActive": {
                int[] ops = (int[]) a[0];
                if (ops != null) for (int op : ops)
                    if (op < 0 || op >= NUM_OP)
                        throw new IllegalArgumentException("Invalid op code in: " + java.util.Arrays.toString(ops));
                if (a[1] == null) return null;
                if (ops == null) throw new NullPointerException("ops");
                synchronized (activeWatchers) { activeWatchers.add(((android.os.IInterface) a[1]).asBinder()); }
                return null;
            }
            case "stopWatchingActive":
                if (a[0] != null)
                    synchronized (activeWatchers) { activeWatchers.remove(((android.os.IInterface) a[0]).asBinder()); }
                return null;
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
