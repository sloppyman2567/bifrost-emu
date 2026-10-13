// VibratorManagerService ("vibrator_manager") for a device without vibrators.
// SystemVibratorManager and SystemVibrator then report hasVibrator() == false;
// accepted requests end as unsupported, as AOSP ignores a vibration no
// vibrator can play. Permission checks follow Android 15's service.
import android.os.Process;
import java.util.Objects;

final class FrameworkVibrator implements FrameworkServices.Call {
    private enum Contract {
        getVibratorIds,
        getVibratorInfo,
        isVibrating,
        registerVibratorStateListener,
        unregisterVibratorStateListener,
        setAlwaysOnEffect,
        vibrate,
        cancelVibrate,
        performHapticFeedback
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkVibrator(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("vibrator_manager", "android.os.IVibratorManagerService", this);
    }

    private void enforce(String permission) {
        if (!packages.granted(permission)) throw new SecurityException(permission);
    }

    @Override public Object invoke(String method, Object[] a) {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("vibrator_manager." + method); }
        switch (contract) {
            case getVibratorIds: return new int[0];
            case getVibratorInfo: return null;
            case isVibrating:
            case registerVibratorStateListener:
            case unregisterVibratorStateListener:
                enforce("android.permission.ACCESS_VIBRATOR_STATE");
                return false;
            case setAlwaysOnEffect:
                enforce("android.permission.VIBRATE_ALWAYS_ON");
                return false;
            case vibrate: {
                // vibrateWithPermissionCheck, then vibrateInternal's checks.
                enforce("android.permission.VIBRATE");
                Objects.requireNonNull(a[3], "vibration");
                if (a[6] == null) return null;  // "token must not be null": logged and dropped
                if ((Integer) a[0] != Process.myUid()) enforce("android.permission.UPDATE_APP_OPS_STATS");
                return null;
            }
            // Nothing is vibrating, so no vibration matches the token.
            case cancelVibrate: enforce("android.permission.VIBRATE"); return null;
            // Haptic constants map to vibrations that no vibrator can play.
            case performHapticFeedback: return null;
            default: throw new UnsupportedOperationException("vibrator_manager." + method);
        }
    }
}
