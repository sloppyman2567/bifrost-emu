// AppWidgetService ("appwidget"), which SystemServer starts on devices with
// FEATURE_APP_WIDGETS, as phones have; AppWidgetManager.getInstance is null
// without it. There is no home screen here, so no widget is ever bound to a
// provider (Android 15 AppWidgetServiceImpl). Provider enumeration is not
// modelled: AppWidgetProviderInfo parsing from receivers' meta-data is absent,
// so no package is listed as a provider.
import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.os.Process;
import java.util.Objects;

final class FrameworkAppWidgets implements FrameworkServices.Call {
    private enum Contract { getAppWidgetIds, getInstalledProvidersForProfile }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkAppWidgets(FrameworkPackages packages) { this.packages = packages; }

    // AppWidgetServiceImpl.SecurityPolicy.enforceCallFromPackage: the package must be the caller's own.
    private void enforceCallFromPackage(String packageName) {
        if (packages.packageUid(packageName) != Process.myUid())
            throw new SecurityException("Package " + packageName + " does not belong to " + Process.myUid());
    }

    void install(FrameworkServices services) throws Exception {
        services.register("appwidget", "com.android.internal.appwidget.IAppWidgetService", this);
        packages.provideFeature(PackageManager.FEATURE_APP_WIDGETS);
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("appwidget." + method); }
        switch (contract) {
            case getAppWidgetIds: {
                ComponentName provider = Objects.requireNonNull((ComponentName) a[0], "provider");
                enforceCallFromPackage(provider.getPackageName());
                return new int[0];
            }
            // (categoryFilter, profileId, packageName): only the user's own profile is an enabled group profile
            // (null otherwise); this profile lists no providers.
            case getInstalledProvidersForProfile: {
                if ((Integer) a[1] != Process.myUid() / 100000) return null;
                enforceCallFromPackage((String) a[2]);
                return Class.forName("android.content.pm.ParceledListSlice")
                        .getConstructor(java.util.List.class).newInstance(new java.util.ArrayList<Object>());
            }
            default: throw new UnsupportedOperationException("appwidget." + method);
        }
    }
}
