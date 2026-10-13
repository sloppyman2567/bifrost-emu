// UiModeManagerService for a phone-type device with AOSP's default resources:
// normal UI mode, night mode off and locked (config_defaultNightMode = 1,
// config_lockDayNightMode = true, config_lockUiMode = false) and standard
// contrast. The process and Activity configurations carry the same uiMode.
import android.content.res.Configuration;
import android.os.IBinder;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;

final class FrameworkUiMode implements FrameworkServices.Call {
    // UiModeManager.MODE_NIGHT_* values.
    private static final int MODE_NIGHT_AUTO = 0, MODE_NIGHT_NO = 1, MODE_NIGHT_YES = 2, MODE_NIGHT_CUSTOM = 3;
    // The global configuration's uiMode (UiModeManagerService.updateConfigurationLocked).
    static final int CONFIGURATION_UI_MODE = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;

    private enum Contract {
        addCallback,
        getCurrentModeType,
        getNightMode,
        setNightMode,
        setApplicationNightMode,
        isUiModeLocked,
        isNightModeLocked,
        getContrast,
        getActiveProjectionTypes,
        getProjectingPackages,
        addOnProjectionStateChangedListener,
        removeOnProjectionStateChangedListener
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    // Registered for the life of the process; the mode and contrast never
    // change here, so no callback is ever notified.
    private final Set<IBinder> callbacks = new HashSet<>();

    FrameworkUiMode(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("uimode", "android.app.IUiModeManager", this);
    }

    private static IBinder binder(Object callback) throws Exception {
        if (callback == null) throw new NullPointerException("callback");
        return (IBinder) callback.getClass().getMethod("asBinder").invoke(callback);
    }

    private void enforce(String permission) {
        if (!packages.granted(permission)) throw new SecurityException(permission);
    }

    private static void nightMode(int mode) {
        if (mode < MODE_NIGHT_AUTO || mode > MODE_NIGHT_CUSTOM)
            throw new IllegalArgumentException("Unknown mode: " + mode);
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("uimode." + method); }
        switch (contract) {
            case addCallback: {
                IBinder binder = binder(a[0]);
                if (callbacks.size() >= 256 && !callbacks.contains(binder))
                    throw new IllegalStateException("UI mode callback limit");
                callbacks.add(binder);
                return null;
            }
            case getCurrentModeType: return CONFIGURATION_UI_MODE & Configuration.UI_MODE_TYPE_MASK;
            case getNightMode: return MODE_NIGHT_NO;
            // Night mode is locked and apps lack MODIFY_DAY_NIGHT_MODE, so
            // UiModeManagerService logs and ignores the request.
            case setNightMode: nightMode((Integer) a[0]); return null;
            case setApplicationNightMode: {
                int mode = (Integer) a[0];
                nightMode(mode);
                // MODE_NIGHT_NO/AUTO/CUSTOM resolve to the current light
                // configuration; dark mode needs a per-app configuration change.
                if (mode == MODE_NIGHT_YES)
                    throw new UnsupportedOperationException("uimode.setApplicationNightMode(YES): per-app configuration changes");
                return null;
            }
            case isUiModeLocked: return false;
            case isNightModeLocked: return true;
            case getContrast: return 0f;
            // No car or automotive projection exists on this device.
            case getActiveProjectionTypes:
                enforce("android.permission.READ_PROJECTION_STATE");
                return 0;
            case getProjectingPackages:
                enforce("android.permission.READ_PROJECTION_STATE");
                return new ArrayList<String>();
            case addOnProjectionStateChangedListener:
            case removeOnProjectionStateChangedListener:
                enforce("android.permission.READ_PROJECTION_STATE");
                return null;
            default: throw new UnsupportedOperationException("uimode." + method);
        }
    }
}
