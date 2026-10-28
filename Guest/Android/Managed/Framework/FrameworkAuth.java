// AuthService ("auth", android.hardware.biometrics.IAuthService) for a device with no biometric sensors and no screen
// lock, as Android 15's AuthService and BiometricService answer an app (PreAuthInfo.getInternalStatus): a request for
// biometrics alone is BIOMETRIC_ERROR_NO_HARDWARE, one that allows the device credential is NONE_ENROLLED (the screen
// lock is not set), and authenticate reports the same through the receiver instead of showing a prompt. Calls only the
// system makes need USE_BIOMETRIC_INTERNAL (SecurityException); with it, they end as unsupported (no sensors).
import android.os.IBinder;
import android.os.Process;
import java.util.ArrayList;

final class FrameworkAuth implements FrameworkServices.Call {
    private enum Contract {
        createTestSession, getSensorProperties, getUiPackage, authenticate, cancelAuthentication, canAuthenticate,
        getLastAuthenticationTime, hasEnrolledBiometrics, registerEnabledOnKeyguardCallback,
        registerAuthenticationStateListener, unregisterAuthenticationStateListener, invalidateAuthenticatorIds,
        getAuthenticatorIds, resetLockoutTimeBound, resetLockout, getButtonLabel, getPromptMessage, getSettingName
    }
    // BiometricManager.Authenticators, BiometricManager.BIOMETRIC_* results, BiometricConstants errors, modalities.
    private static final int WEAK = 0x00FF, STRONG = 0x000F, CREDENTIAL = 0x8000;
    private static final int ERROR_NONE_ENROLLED = 11, ERROR_NO_HARDWARE = 12;
    private static final int ERROR_CANCELED = 5, ERROR_HW_NOT_PRESENT = 12, ERROR_NO_DEVICE_CREDENTIAL = 14;
    private static final int TYPE_NONE = 0, TYPE_CREDENTIAL = 1;

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final FrameworkAppOps appOps;
    private long requestId;

    FrameworkAuth(FrameworkPackages packages, FrameworkAppOps appOps) { this.packages = packages; this.appOps = appOps; }

    void install(FrameworkServices services) throws Exception {
        services.register("auth", "android.hardware.biometrics.IAuthService", this);
    }

    private void user(int user) {
        if (user != Process.myUid() / 100000) requireInternal();
        else requireUse();
    }

    // AuthService.checkPermission / checkInternalPermission.
    private void requireUse() {
        if (!packages.granted("android.permission.USE_FINGERPRINT") && !packages.granted("android.permission.USE_BIOMETRIC"))
            throw new SecurityException("Must have USE_BIOMETRIC permission");
    }
    private void requireInternal() {
        if (!packages.granted("android.permission.USE_BIOMETRIC_INTERNAL")) throw new SecurityException("Must have USE_BIOMETRIC_INTERNAL permission");
    }
    private void requireTest() {
        if (!packages.granted("android.permission.TEST_BIOMETRIC")) throw new SecurityException("Permission Denial: requires android.permission.TEST_BIOMETRIC");
    }

    // Utils.isValidAuthenticatorConfig: unset, or strong/weak biometrics with or without the device credential.
    private static boolean valid(int authenticators) {
        switch (authenticators) {
            case 0: case STRONG: case WEAK: case CREDENTIAL: case CREDENTIAL | STRONG: case CREDENTIAL | WEAK: return true;
            default: return false;
        }
    }

    // PreAuthInfo.getInternalStatus with no sensors and no credential: {modality, BiometricConstants error}.
    private static int[] status(int authenticators) {
        boolean credential = (authenticators & CREDENTIAL) != 0, biometric = authenticators == 0 || (authenticators & WEAK) != 0;
        if (credential) return new int[] {TYPE_CREDENTIAL, ERROR_NO_DEVICE_CREDENTIAL};
        return new int[] {TYPE_NONE, ERROR_HW_NOT_PRESENT};
    }

    // Utils.biometricConstantsToBiometricManager for those two errors.
    private static int publicResult(int constantsError) {
        return constantsError == ERROR_NO_DEVICE_CREDENTIAL ? ERROR_NONE_ENROLLED : ERROR_NO_HARDWARE;
    }

    private String string(String name) {
        android.content.res.Resources resources = android.content.res.Resources.getSystem();
        int id = resources.getIdentifier(name, "string", "android");
        return id == 0 ? null : resources.getString(id);
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("auth." + method); }
        switch (contract) {
            case createTestSession:
                requireTest();
                throw new IllegalArgumentException("No biometric sensor " + a[0]);
            case getSensorProperties:
                requireTest();
                return new ArrayList<Object>();
            case getUiPackage:
                requireTest();
                return "com.android.systemui";
            case authenticate: {
                int userId = (Integer) a[2];
                user(userId);
                Object receiver = a[3];
                String opPackageName = (String) a[4];
                // checkAppOps(USE_BIOMETRIC), null arguments, and the caller being foreground fail fast as canceled.
                boolean allowed = opPackageName != null && appOps.mode(op("android:use_biometric"), Process.myUid(), opPackageName)
                        == FrameworkAppOps.MODE_ALLOWED;
                if (!allowed || a[0] == null || receiver == null || a[5] == null) {
                    if (receiver != null) onError(receiver, TYPE_NONE, ERROR_CANCELED);
                    return -1L;
                }
                int authenticators = (Integer) a[5].getClass().getMethod("getAuthenticators").invoke(a[5]);
                int[] status = status(authenticators);
                onError(receiver, status[0], status[1]);
                return ++requestId;
            }
            case cancelAuthentication:
                requireUse();
                return null;
            case canAuthenticate: {
                user((Integer) a[1]);
                int authenticators = (Integer) a[2];
                if (!valid(authenticators)) throw new SecurityException("Invalid authenticator configuration");
                return publicResult(status(authenticators)[1]);
            }
            case getLastAuthenticationTime:
                user((Integer) a[0]);
                return -1L;  // BiometricManager.BIOMETRIC_NO_AUTHENTICATION: nothing was authenticated
            case getAuthenticatorIds:
                if ((Integer) a[0] != Process.myUid() / 100000) requireInternal();
                return new long[0];
            case getButtonLabel: case getPromptMessage: case getSettingName: {
                user((Integer) a[0]);
                int[] status = status((Integer) a[2]);
                if (status[0] == TYPE_NONE) return null;
                if (contract == Contract.getPromptMessage) return string("screen_lock_dialog_default_subtitle");
                return string("screen_lock_app_setting_name");
            }
            default:
                requireInternal();
                if (contract == Contract.hasEnrolledBiometrics) return false;
                throw new UnsupportedOperationException("auth." + method + " has no biometric sensor behind it");
        }
    }

    private int op(String name) throws Exception {
        return (Integer) android.app.AppOpsManager.class.getMethod("strOpToOp", String.class).invoke(null, name);
    }

    // IBiometricServiceReceiver.onError(modality, error, vendorCode)
    private static void onError(Object receiver, int modality, int error) throws Exception {
        receiver.getClass().getMethod("onError", int.class, int.class, int.class).invoke(receiver, modality, error, 0);
    }
}
