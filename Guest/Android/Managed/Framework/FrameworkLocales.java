// LocaleManagerService ("locale", android.app.ILocaleManager) for one app and user 0, as Android 15's service
// answers it: an app reads and sets its own locales, another package needs READ_APP_SPECIFIC_LOCALES /
// CHANGE_CONFIGURATION, an unknown package is an IllegalArgumentException. App-specific locales are stored per
// package and read back; applying a different list would change the app's configuration (ATMS's package configuration
// updater), which is not modelled, so that is UnsupportedOperationException rather than a silent success. An override
// LocaleConfig is stored per package (the service keeps it in a file).
import android.os.LocaleList;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

final class FrameworkLocales implements FrameworkServices.Call {
    private enum Contract { setApplicationLocales, getApplicationLocales, getSystemLocales, setOverrideLocaleConfig, getOverrideLocaleConfig }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final Map<String, LocaleList> locales = new HashMap<>();
    private final Map<String, Object> overrides = new HashMap<>();

    FrameworkLocales(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("locale", "android.app.ILocaleManager", this);
    }

    private void user(int user) {
        if (user != android.os.Process.myUid() / 100000) throw new SecurityException("Locale access to another user");
    }

    // isPackageOwnedByCaller: an unknown package is invalid; true for the caller's own.
    private boolean owned(String packageName, int user) {
        int uid = packages.packageUid(packageName);
        if (uid < 0) throw new IllegalArgumentException("Unknown package: " + packageName + " for user " + user);
        return uid == android.os.Process.myUid();
    }

    @Override public synchronized Object invoke(String method, Object[] a) {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("locale." + method); }
        switch (contract) {
            case getApplicationLocales: {
                String packageName = Objects.requireNonNull((String) a[0]);
                user((Integer) a[1]);
                if (!owned(packageName, (Integer) a[1]) && !packages.granted("android.permission.READ_APP_SPECIFIC_LOCALES"))
                    throw new SecurityException("Caller does not have permission to query locales of " + packageName);
                return locales.getOrDefault(packageName, LocaleList.getEmptyLocaleList());
            }
            case getSystemLocales: return android.content.res.Resources.getSystem().getConfiguration().getLocales();
            case setApplicationLocales: {
                String packageName = Objects.requireNonNull((String) a[0]);
                LocaleList list = Objects.requireNonNull((LocaleList) a[2]);
                user((Integer) a[1]);
                if (!owned(packageName, (Integer) a[1]) && !packages.granted("android.permission.CHANGE_CONFIGURATION"))
                    throw new SecurityException("android.permission.CHANGE_CONFIGURATION");
                if (list.equals(locales.getOrDefault(packageName, LocaleList.getEmptyLocaleList()))) return null;  // config unchanged
                throw new UnsupportedOperationException("locale.setApplicationLocales cannot change the app's configuration here");
            }
            case setOverrideLocaleConfig: {
                String packageName = Objects.requireNonNull((String) a[0]);
                user((Integer) a[1]);
                if (!owned(packageName, (Integer) a[1]) && !packages.granted("android.permission.SET_APP_SPECIFIC_LOCALECONFIG"))
                    throw new SecurityException("android.permission.SET_APP_SPECIFIC_LOCALECONFIG");
                if (a[2] == null) overrides.remove(packageName); else overrides.put(packageName, a[2]);
                return null;
            }
            case getOverrideLocaleConfig: {
                String packageName = Objects.requireNonNull((String) a[0]);
                user((Integer) a[1]);
                return overrides.get(packageName);
            }
            default: throw new UnsupportedOperationException("locale." + method);
        }
    }
}
