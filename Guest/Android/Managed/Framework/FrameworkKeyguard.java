// Keyguard state for a device with no lock-screen credential that is never
// locked while the app runs: TrustManagerService ("trust") and the
// WindowManagerService keyguard calls KeyguardManager makes. KeyguardManager's
// constructor uses ServiceManager.getServiceOrThrow("trust"), so without it
// getSystemService(KEYGUARD_SERVICE) is null. The host desktop's own lock
// screen is not observed. Rules follow Android 15's services.
import android.os.IBinder;
import java.util.Objects;

final class FrameworkKeyguard {
    private static final int USER_CURRENT_OR_SELF = -3, USER_CURRENT = -2, DEVICE_ID_DEFAULT = 0;

    private enum Trust { isDeviceLocked, isDeviceSecure, registerTrustListener, unregisterTrustListener, isTrustUsuallyManaged }

    private final FrameworkPackages packages;

    FrameworkKeyguard(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("trust", "android.app.trust.ITrustManager", new FrameworkServices.Call() {
            @Override public String[] methods() {
                Trust[] values = Trust.values();
                String[] names = new String[values.length];
                for (int i = 0; i < values.length; i++) names[i] = values[i].name();
                return names;
            }
            @Override public Object invoke(String method, Object[] a) { return trust(method, a); }
        });
    }

    private void enforce(String permission) {
        if (!packages.granted(permission)) throw new SecurityException(permission);
    }

    // ActivityManager.handleIncomingUser for the single user 0.
    private void user(int userId, String permission) {
        if (userId != 0 && userId != USER_CURRENT && userId != USER_CURRENT_OR_SELF) enforce(permission);
    }

    private Object trust(String method, Object[] a) {
        Trust contract;
        try { contract = Trust.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("trust." + method); }
        switch (contract) {
            case isDeviceLocked:
            case isDeviceSecure:
                // Virtual devices are insecure; the default device has no credential.
                if ((Integer) a[1] != DEVICE_ID_DEFAULT) return false;
                user((Integer) a[0], "android.permission.INTERACT_ACROSS_USERS_FULL");
                return false;
            case registerTrustListener:
            case unregisterTrustListener:
            case isTrustUsuallyManaged:
                enforce("android.permission.TRUST_LISTENER");
                throw new IllegalStateException("TRUST_LISTENER granted to an app");
            default: throw new UnsupportedOperationException("trust." + method);
        }
    }

    // Whether `method` is an IWindowManager keyguard call handled by window().
    boolean handlesWindow(String method) {
        switch (method) {
            case "isKeyguardLocked": case "isKeyguardSecure": case "disableKeyguard": case "reenableKeyguard":
            case "exitKeyguardSecurely": case "addKeyguardLockedStateListener":
            case "removeKeyguardLockedStateListener": return true;
            default: return false;
        }
    }

    Object window(String method, Object[] a) throws Exception {
        switch (method) {
            case "isKeyguardLocked": return false;
            case "isKeyguardSecure":
                user((Integer) a[0], "android.permission.INTERACT_ACROSS_USERS");
                return false;
            // No keyguard is showing, so disabling it changes nothing visible;
            // KeyguardDisableHandler's per-token bookkeeping has no effect here.
            case "disableKeyguard":
                user((Integer) a[2], "android.permission.INTERACT_ACROSS_USERS_FULL");
                enforce("android.permission.DISABLE_KEYGUARD");
                return null;
            case "reenableKeyguard":
                user((Integer) a[1], "android.permission.INTERACT_ACROSS_USERS_FULL");
                enforce("android.permission.DISABLE_KEYGUARD");
                Objects.requireNonNull((IBinder) a[0], "token is null");
                return null;
            case "exitKeyguardSecurely": {
                enforce("android.permission.DISABLE_KEYGUARD");
                if (a[0] == null) throw new IllegalArgumentException("callback == null");
                // Not locked: PhoneWindowManager reports success immediately.
                a[0].getClass().getMethod("onKeyguardExitResult", boolean.class).invoke(a[0], true);
                return null;
            }
            case "addKeyguardLockedStateListener":
            case "removeKeyguardLockedStateListener":
                enforce("android.permission.SUBSCRIBE_TO_KEYGUARD_LOCKED_STATE");
                throw new IllegalStateException("SUBSCRIBE_TO_KEYGUARD_LOCKED_STATE granted to an app");
            default: throw new UnsupportedOperationException("window." + method);
        }
    }
}
