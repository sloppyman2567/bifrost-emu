// NetworkStatsService ("netstats", android.net.INetworkStatsService) and its sessions (INetworkStatsSession), as the
// connectivity module's NetworkStatsService in Android 15 answers an app. There is no per-UID traffic accounting
// here (no netd or eBPF counters): collections are empty, as on a device that has recorded nothing, so summaries and
// histories hold no buckets and a usage callback's threshold is never reached. Counters the kernel keeps
// (getUidStats, getIfaceStats, getTotalStats) answer TrafficStats.UNSUPPORTED. Argument checks and permission gates
// follow the service; operations reserved for the network stack end as SecurityException.
import android.os.Process;
import android.os.SystemClock;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

final class FrameworkNetworkStats implements FrameworkServices.Call {
    private enum Contract {
        openSession, openSessionForUsageStats, getDataLayerSnapshotForUid, getUidStatsForTransport, getMobileIfaces,
        incrementOperationCount, notifyNetworkStatus, forceUpdate, registerUsageCallback, unregisterUsageRequest,
        getUidStats, getIfaceStats, getTotalStats, registerNetworkStatsProvider, noteUidForeground,
        advisePersistThreshold, setStatsProviderWarningAndLimitAsync, clearTrafficStatsRateLimitCaches
    }

    private enum SessionContract {
        getRelevantUids, getDeviceSummaryForNetwork, getSummaryForNetwork, getHistoryForNetwork,
        getHistoryIntervalForNetwork, getSummaryForAllUid, getTaggedSummaryForAllUid, getHistoryForUid,
        getHistoryIntervalForUid, close
    }

    // TrafficStats.UNSUPPORTED, TYPE_* (rx/tx bytes/packets: 0..3), TRANSPORT_* and the recorders' bucket sizes.
    private static final long UNSUPPORTED = -1, MIN_THRESHOLD_BYTES = 2 * 1_048_576L;
    private static final long DEV_BUCKET_MS = 60 * 60 * 1000L, UID_BUCKET_MS = 2 * 60 * 60 * 1000L;
    private static final int TRANSPORT_CELLULAR = 0, TRANSPORT_WIFI = 1, TAG_NONE = 0;

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private FrameworkServices services;
    // Registered usage requests (DataUsageRequest.requestId) and the uid that owns each.
    private final Map<Integer, Integer> requests = new HashMap<>();
    private int nextRequestId;

    FrameworkNetworkStats(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        this.services = services;
        services.register("netstats", "android.net.INetworkStatsService", this);
    }

    private void enforcePackageMatchesUid(String callingPackage) {
        if (packages.packageUid(callingPackage) != Process.myUid())
            throw new SecurityException("Package " + callingPackage + " does not belong to uid " + Process.myUid());
    }

    // PermissionUtils.enforceNetworkStackPermission
    private void enforceNetworkStack() {
        if (!packages.granted("android.permission.NETWORK_STACK") && !packages.granted("android.permission.MAINLINE_NETWORK_STACK"))
            throw new SecurityException("Permission Denial: requires android.permission.NETWORK_STACK or android.permission.MAINLINE_NETWORK_STACK");
    }

    private static Object emptyStats() throws Exception {
        return Class.forName("android.net.NetworkStats").getConstructor(long.class, int.class)
                .newInstance(SystemClock.elapsedRealtime(), 0);
    }

    private static Object emptyHistory(long bucketMs, int fields) throws Exception {
        return Class.forName("android.net.NetworkStatsHistory").getConstructor(long.class, int.class, int.class)
                .newInstance(bucketMs, 0, fields);
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("netstats." + method); }
        switch (contract) {
            case openSession: return session(null);
            case openSessionForUsageStats:
                Objects.requireNonNull(a[1]);
                enforcePackageMatchesUid((String) a[1]);
                return session((String) a[1]);
            case getDataLayerSnapshotForUid:
                // "Snapshots only available for calling UID"; with no counters the caller's own is empty too.
                return emptyStats();
            case getUidStatsForTransport: {
                enforceNetworkStack();
                int transport = (Integer) a[0];
                if (transport != TRANSPORT_WIFI && transport != TRANSPORT_CELLULAR)
                    throw new IllegalArgumentException("Invalid transport " + transport);
                return emptyStats();
            }
            case getMobileIfaces: return new String[0];
            case incrementOperationCount:
                if ((Integer) a[0] != Process.myUid())
                    if (!packages.granted("android.permission.UPDATE_DEVICE_STATS"))
                        throw new SecurityException("Permission Denial: requires android.permission.UPDATE_DEVICE_STATS");
                if ((Integer) a[2] < 0) throw new IllegalArgumentException("operation count can only be incremented");
                if ((Integer) a[1] == TAG_NONE) throw new IllegalArgumentException("operation count must have specific tag");
                return null;  // operation counts only feed snapshots, which hold no traffic here
            case notifyNetworkStatus:
            case forceUpdate:
            case noteUidForeground:
            case advisePersistThreshold:
                enforceNetworkStack();
                throw new UnsupportedOperationException("netstats." + method + " has no traffic accounting behind it");
            case registerUsageCallback: {
                Objects.requireNonNull(a[0], "calling package is null");
                Object request = Objects.requireNonNull(a[1], "DataUsageRequest is null");
                Object template = Objects.requireNonNull(request.getClass().getField("template").get(request), "NetworkTemplate is null");
                Objects.requireNonNull(a[2], "callback is null");
                enforcePackageMatchesUid((String) a[0]);
                long threshold = request.getClass().getField("thresholdInBytes").getLong(request);
                // A caller without the mainline network stack permission gets at least 2 MiB.
                if (!packages.granted("android.permission.MAINLINE_NETWORK_STACK")) threshold = Math.max(MIN_THRESHOLD_BYTES, threshold);
                int id = ++nextRequestId;
                requests.put(id, Process.myUid());
                return Class.forName("android.net.DataUsageRequest")
                        .getConstructor(int.class, template.getClass(), long.class).newInstance(id, template, threshold);
            }
            case unregisterUsageRequest: {
                Object request = Objects.requireNonNull(a[0], "DataUsageRequest is null");
                int id = request.getClass().getField("requestId").getInt(request);
                Integer owner = requests.get(id);
                if (owner != null && owner == Process.myUid()) requests.remove(id);
                return null;
            }
            case getUidStats:
            case getIfaceStats:
            case getTotalStats:
                return UNSUPPORTED;
            case registerNetworkStatsProvider:
                if (!packages.granted("android.permission.NETWORK_STATS_PROVIDER") && !packages.granted("android.permission.NETWORK_STACK")
                        && !packages.granted("android.permission.MAINLINE_NETWORK_STACK"))
                    throw new SecurityException("Permission Denial: requires android.permission.NETWORK_STATS_PROVIDER");
                throw new UnsupportedOperationException("netstats.registerNetworkStatsProvider has no traffic accounting behind it");
            case setStatsProviderWarningAndLimitAsync:
                enforceNetworkStack();
                throw new UnsupportedOperationException("netstats.setStatsProviderWarningAndLimitAsync has no stats providers");
            case clearTrafficStatsRateLimitCaches:
                if (!packages.granted("android.permission.NETWORK_STACK") && !packages.granted("android.permission.MAINLINE_NETWORK_STACK")
                        && !packages.granted("android.permission.NETWORK_SETTINGS"))
                    throw new SecurityException("Permission Denial: requires android.permission.NETWORK_STACK or NETWORK_SETTINGS");
                return null;  // the caches this clears are not kept
            default: throw new UnsupportedOperationException("netstats." + method);
        }
    }

    private Object session(String callingPackage) throws Exception {
        return services.local("netstats.session", "android.net.INetworkStatsSession", new FrameworkServices.Call() {
            @Override public String[] methods() {
                SessionContract[] values = SessionContract.values();
                String[] names = new String[values.length];
                for (int i = 0; i < values.length; i++) names[i] = values[i].name();
                return names;
            }

            // enforceTemplatePermissions: a wifi network key tracks the user's location, so it needs fine location.
            private void enforceTemplate(Object template) throws Exception {
                Objects.requireNonNull(template);
                java.util.Collection<?> keys = (java.util.Collection<?>) template.getClass().getMethod("getWifiNetworkKeys").invoke(template);
                int matchTest = Class.forName("android.net.NetworkTemplate").getField("MATCH_TEST").getInt(null);
                if (!keys.isEmpty() && (Integer) template.getClass().getMethod("getMatchRule").invoke(template) != matchTest
                        && !packages.granted("android.permission.ACCESS_FINE_LOCATION"))
                    throw new SecurityException("Access fine location is required when querying with wifi network keys, make sure "
                            + "the app has the necessary permissions and the location toggle is on.");
            }

            @Override public Object invoke(String method, Object[] a) throws Exception {
                SessionContract contract;
                try { contract = SessionContract.valueOf(method); }
                catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("netstats.session." + method); }
                switch (contract) {
                    case getRelevantUids: return new int[0];
                    case getDeviceSummaryForNetwork:
                    case getSummaryForNetwork:
                    case getSummaryForAllUid:
                    case getTaggedSummaryForAllUid:
                        enforceTemplate(a[0]);
                        return emptyStats();
                    case getHistoryForNetwork:
                    case getHistoryIntervalForNetwork:
                        enforceTemplate(a[0]);
                        return emptyHistory(DEV_BUCKET_MS, (Integer) a[1]);
                    case getHistoryForUid:
                        enforceTemplate(a[0]);
                        return emptyHistory(UID_BUCKET_MS, (Integer) a[4]);
                    case getHistoryIntervalForUid:
                        enforceTemplate(a[0]);
                        // Another uid's tagged history is refused.
                        if ((Integer) a[3] != TAG_NONE && (Integer) a[1] != Process.myUid())
                            throw new SecurityException("Calling package " + callingPackage + " cannot access tag information from a different uid");
                        return emptyHistory(UID_BUCKET_MS, (Integer) a[4]);
                    case close: return null;
                    default: throw new UnsupportedOperationException("netstats.session." + method);
                }
            }
        });
    }
}
