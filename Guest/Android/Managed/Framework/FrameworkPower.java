// Virtual-device wake locks. The emulator continues to execute while its host
// process runs; these locks do not change the host's global suspend policy.
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.WorkSource;
import java.lang.reflect.InvocationTargetException;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.function.BooleanSupplier;

final class FrameworkPower implements FrameworkServices.Call {
    private enum Contract {
        acquireWakeLock,
        releaseWakeLock,
        updateWakeLockWorkSource,
        updateWakeLockCallback,
        isWakeLockLevelSupported,
        isInteractive,
        isDisplayInteractive,
        isPowerSaveMode,
        areAutoPowerSaveModesEnabled,
        isDeviceIdleMode,
        isLightDeviceIdleMode,
        isLowPowerStandbySupported,
        isLowPowerStandbyEnabled,
        isBatterySaverSupported,
        getPowerSaveState,
        isExemptFromLowPowerStandby,
        isReasonAllowedInLowPowerStandby,
        isFeatureAllowedInLowPowerStandby
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final BooleanSupplier interactive;
    private final Map<IBinder, Lock> locks = new HashMap<>();

    private final class Lock implements IBinder.DeathRecipient {
        final IBinder token;
        final String pkg;
        int flags;
        String tag;
        Object callback;
        Lock(IBinder token, String pkg) { this.token = token; this.pkg = pkg; }
        @Override public void binderDied() { release(token); }
    }

    FrameworkPower(FrameworkPackages packages, BooleanSupplier interactive) {
        this.packages = packages; this.interactive = interactive;
    }

    void install(FrameworkServices services) throws Exception {
        services.register("power", "android.os.IPowerManager", this);
    }

    private void permission(String permission) {
        if (packages.checkPermission(permission, Process.myUid(), Context.DEVICE_ID_DEFAULT)
                != PackageManager.PERMISSION_GRANTED) throw new SecurityException(permission);
    }

    private static boolean supported(int level) {
        return level == 1 || level == 6 || level == 10 || level == 26;
    }

    private static void workSource(Object value) throws Exception {
        if (value != null && !(Boolean) WorkSource.class.getMethod("isEmpty").invoke(value))
            throw new SecurityException("Wake lock attribution requires UPDATE_DEVICE_STATS");
    }

    private static void callback(Object callback, boolean held) {
        if (callback == null) return;
        try {
            Class.forName("android.os.IWakeLockCallback").getMethod("onStateChanged", boolean.class)
                    .invoke(callback, held);
        } catch (ReflectiveOperationException error) {
            Throwable cause = error instanceof InvocationTargetException
                    ? ((InvocationTargetException) error).getCause() : error;
            if (!(cause instanceof RemoteException)) throw new IllegalStateException("Wake lock callback", cause);
        }
    }

    private synchronized void release(IBinder token) {
        Lock lock = locks.remove(token);
        if (lock != null) { lock.token.unlinkToDeath(lock, 0); callback(lock.callback, false); }
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("power." + method); }
        switch (contract) {
            case getPowerSaveState: {
                // AOSP returns the default builder state when battery saver
                // is unsupported, including its 0.5 brightness factor.
                Class<?> builder = Class.forName("android.os.PowerSaveState$Builder");
                return builder.getMethod("build").invoke(builder.getConstructor().newInstance());
            }
            case isExemptFromLowPowerStandby:
            case isReasonAllowedInLowPowerStandby:
            case isFeatureAllowedInLowPowerStandby:
                return true; // AOSP permits these while standby is disabled/unsupported.
            case acquireWakeLock: {
                permission("android.permission.WAKE_LOCK");
                IBinder token = (IBinder) Objects.requireNonNull(a[0], "wake lock token");
                int flags = (Integer) a[1];
                String tag = (String) Objects.requireNonNull(a[2], "wake lock tag"), pkg = (String) a[3];
                if (packages.packageUid(pkg) != Process.myUid()) throw new SecurityException("Foreign wake lock package");
                if (!supported(flags & 0xffff)) throw new IllegalArgumentException("Unsupported wake lock level");
                workSource(a[4]);
                int display = (Integer) a[6];
                if (display != -1 && display != 0) throw new IllegalArgumentException("Unknown wake lock display");
                Lock lock = locks.get(token);
                if (lock == null) {
                    if (locks.size() >= 1024) throw new IllegalStateException("Wake lock limit");
                    lock = new Lock(token, pkg);
                    token.linkToDeath(lock, 0); locks.put(token, lock);
                } else if (!lock.pkg.equals(pkg)) throw new SecurityException("Foreign wake lock token");
                Object old = lock.callback;
                lock.flags = flags; lock.tag = tag; lock.callback = a[7];
                if (old != lock.callback) { callback(old, false); callback(lock.callback, true); }
                return null;
            }
            case releaseWakeLock: permission("android.permission.WAKE_LOCK"); release((IBinder) a[0]); return null;
            case updateWakeLockWorkSource: {
                permission("android.permission.WAKE_LOCK"); workSource(a[1]);
                if (!locks.containsKey(a[0])) throw new IllegalArgumentException("Wake lock not active");
                return null;
            }
            case updateWakeLockCallback: {
                permission("android.permission.WAKE_LOCK");
                Lock lock = locks.get(a[0]);
                if (lock == null) throw new IllegalArgumentException("Wake lock not active");
                if (lock.callback != a[1]) {
                    callback(lock.callback, false); lock.callback = a[1]; callback(lock.callback, true);
                }
                return null;
            }
            case isWakeLockLevelSupported: return supported((Integer) a[0]);
            case isInteractive: return interactive.getAsBoolean();
            case isDisplayInteractive: return (Integer) a[0] == 0 && interactive.getAsBoolean();
            case isPowerSaveMode:
            case areAutoPowerSaveModesEnabled:
            case isDeviceIdleMode:
            case isLightDeviceIdleMode:
            case isLowPowerStandbySupported:
            case isLowPowerStandbyEnabled:
            case isBatterySaverSupported: return false;
            default: throw new UnsupportedOperationException("power." + method);
        }
    }
}
