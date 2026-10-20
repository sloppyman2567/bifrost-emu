// AppWidgetService ("appwidget"), which SystemServer starts on devices with
// FEATURE_APP_WIDGETS, as phones have; AppWidgetManager.getInstance is null
// without it. There is no home screen here, so no widget is ever bound to a
// provider (Android 15 AppWidgetServiceImpl).
import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.os.Process;
import java.util.Objects;

final class FrameworkAppWidgets implements FrameworkServices.Call {
    private enum Contract { getAppWidgetIds }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkAppWidgets(FrameworkPackages packages) { this.packages = packages; }

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
                // enforceCallFromPackage: the provider must be the caller's own.
                ComponentName provider = Objects.requireNonNull((ComponentName) a[0], "provider");
                if (packages.packageUid(provider.getPackageName()) != Process.myUid())
                    throw new SecurityException("Package " + provider.getPackageName()
                            + " does not belong to " + Process.myUid());
                return new int[0];
            }
            default: throw new UnsupportedOperationException("appwidget." + method);
        }
    }
}
