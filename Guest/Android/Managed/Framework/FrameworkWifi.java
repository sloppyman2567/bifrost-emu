// Read-only Wi-Fi view of the same virtual host route as ConnectivityService.
// There is no host radio control, configured AP inventory, or scan backend.
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.DhcpInfo;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Process;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.function.BooleanSupplier;

final class FrameworkWifi implements FrameworkServices.Call {
    private enum Contract {
        getSupportedFeatures, getWifiEnabledState, getConnectionInfo, getDhcpInfo, getVerboseLoggingLevel,
        getScanResults, startScan, setWifiEnabled, registerScanResultsCallback,
        unregisterScanResultsCallback
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final BooleanSupplier online;
    private final Map<IBinder, IBinder.DeathRecipient> scans = new HashMap<>();
    private java.lang.reflect.Constructor<?> scanList;

    FrameworkWifi(FrameworkPackages packages, BooleanSupplier online) {
        this.packages = packages; this.online = online;
    }

    void install(FrameworkServices services) throws Exception {
        // Mainline module builds can relocate this Parcelable's package.
        // Construct the actual interface return type from the provisioned image.
        scanList = Class.forName("android.net.wifi.IWifiManager")
                .getMethod("getScanResults", String.class, String.class).getReturnType()
                .getConstructor(java.util.List.class);
        services.register("wifi", "android.net.wifi.IWifiManager", this);
        // WifiFrameworkInitializer gates construction of the public manager
        // on this feature as well as the Binder entry.
        packages.provideFeature(PackageManager.FEATURE_WIFI);
    }

    private void permission(String name) {
        if (packages.checkPermission(name, Process.myUid(), Context.DEVICE_ID_DEFAULT)
                != PackageManager.PERMISSION_GRANTED) throw new SecurityException(name);
    }

    private void owner(String name) {
        if (packages.packageUid(name) != Process.myUid()) throw new SecurityException("Foreign Wi-Fi package");
    }

    private WifiInfo connection() throws Exception {
        // Android's defaults redact SSID/BSSID/MAC, network id, RSSI and link
        // speed. Do not manufacture access-point details for an Ethernet route.
        WifiInfo info = WifiInfo.class.getDeclaredConstructor().newInstance();
        WifiInfo.class.getMethod("setSupplicantState", SupplicantState.class)
                .invoke(info, online.getAsBoolean() ? SupplicantState.COMPLETED : SupplicantState.DISCONNECTED);
        WifiInfo.class.getMethod("setMacAddress", String.class).invoke(info, "02:00:00:00:00:00");
        return info;
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("wifi." + method); }
        // WifiManager queries this while constructing its public manager;
        // Android explicitly makes the logging-level query permission-free.
        if (contract == Contract.getVerboseLoggingLevel) return 0;
        permission("android.permission.ACCESS_WIFI_STATE");
        switch (contract) {
            case getSupportedFeatures: return 1L; // WIFI_FEATURE_INFRA, no optional radio features.
            case getWifiEnabledState: return WifiManager.WIFI_STATE_ENABLED;
            case getConnectionInfo: owner((String) a[0]); return connection();
            case getDhcpInfo: owner((String) a[0]); return new DhcpInfo();
            case getScanResults:
                owner((String) a[0]);
                return scanList.newInstance(new ArrayList<>());
            case startScan:
                owner((String) a[0]); permission("android.permission.CHANGE_WIFI_STATE"); return false;
            case setWifiEnabled:
                owner((String) a[0]); permission("android.permission.CHANGE_WIFI_STATE"); return false;
            case registerScanResultsCallback: {
                IBinder token = ((IInterface) java.util.Objects.requireNonNull(a[0], "scan callback")).asBinder();
                if (scans.containsKey(token)) return null;
                if (scans.size() >= 128) throw new IllegalStateException("Wi-Fi callback limit");
                IBinder.DeathRecipient death = () -> {
                    synchronized (FrameworkWifi.this) { scans.remove(token); }
                };
                token.linkToDeath(death, 0); scans.put(token, death);
                return null;
            }
            case unregisterScanResultsCallback: {
                IBinder token = ((IInterface) java.util.Objects.requireNonNull(a[0], "scan callback")).asBinder();
                IBinder.DeathRecipient death = scans.remove(token);
                if (death != null) token.unlinkToDeath(death, 0);
                return null;
            }
            default: throw new AssertionError(contract);
        }
    }
}
