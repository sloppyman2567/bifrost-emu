// UsageStatsService ("usagestats", android.app.usage.IUsageStatsManager) for one user and one foreground app, as
// UsageStatsService.BinderService in Android 15 answers an app: its permission gates and argument checks are kept.
// This device records no usage history, so queries an app may make return empty results and every package it can see
// is in the ACTIVE standby bucket; calls that would change what the system tracks need a system permission an app
// never holds and end as SecurityException (UnsupportedOperationException for a holder, since nothing is tracked).
import android.content.pm.PackageInfo;
import android.os.IBinder;
import android.os.Process;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

final class FrameworkUsageStats implements FrameworkServices.Call {
    private enum Contract {
        queryUsageStats, queryConfigurationStats, queryEventStats, queryEvents, queryEventsForPackage,
        queryEventsForUser, queryEventsForPackageForUser, queryEventsWithFilter, setAppInactive, isAppStandbyEnabled,
        isAppInactive, onCarrierPrivilegedAppsChanged, reportChooserSelection, getAppStandbyBucket,
        setAppStandbyBucket, getAppStandbyBuckets, setAppStandbyBuckets, getAppMinStandbyBucket,
        setEstimatedLaunchTime, setEstimatedLaunchTimes, registerAppUsageObserver, unregisterAppUsageObserver,
        registerUsageSessionObserver, unregisterUsageSessionObserver, registerAppUsageLimitObserver,
        unregisterAppUsageLimitObserver, reportUsageStart, reportPastUsageStart, reportUsageStop,
        reportUserInteraction, reportUserInteractionWithBundle, getUsageSource, forceUsageSourceSettingRead,
        getLastTimeAnyComponentUsed, queryBroadcastResponseStats, clearBroadcastResponseStats,
        clearBroadcastEvents, isPackageExemptedFromBroadcastResponseStats, getAppStandbyConstant
    }

    // UsageStatsManager.STANDBY_BUCKET_ACTIVE and USAGE_SOURCE_TASK_ROOT_ACTIVITY.
    private static final int BUCKET_ACTIVE = 10, USAGE_SOURCE_TASK_ROOT_ACTIVITY = 1;

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final FrameworkAppOps appOps;
    // Activities that reported usage start, by IBinder hash code as the service keys them, and their tokens.
    private final Map<Integer, Set<String>> usageReporters = new HashMap<>();

    FrameworkUsageStats(FrameworkPackages packages, FrameworkAppOps appOps) {
        this.packages = packages;
        this.appOps = appOps;
    }

    void install(FrameworkServices services) throws Exception {
        services.register("usagestats", "android.app.usage.IUsageStatsManager", this);
    }

    // ActivityManager.handleIncomingUser with allowAll=false: another user needs INTERACT_ACROSS_USERS(_FULL).
    private void user(int user) {
        if (user != Process.myUid() / 100000)
            throw new SecurityException("Usage stats access to another user");
    }

    private void enforce(String permission, String message) {
        if (!packages.granted(permission)) throw new SecurityException(message);
    }

    // BinderService.hasQueryPermission: the GET_USAGE_STATS op (read, not noted), falling back to PACKAGE_USAGE_STATS.
    private boolean hasQueryPermission(String callingPackage) throws Exception {
        int op = (Integer) android.app.AppOpsManager.class.getMethod("strOpToOp", String.class).invoke(null, "android:get_usage_stats");
        int mode = appOps.mode(op, Process.myUid(), callingPackage);
        if (mode == FrameworkAppOps.MODE_DEFAULT) return packages.granted("android.permission.PACKAGE_USAGE_STATS");
        return mode == FrameworkAppOps.MODE_ALLOWED;
    }

    // checkCallerIsSameApp: the package must be the caller's own.
    private void sameApp(String pkg) {
        if (packages.packageUid(pkg) != Process.myUid())
            throw new SecurityException("Calling uid " + Process.myUid() + " cannot query events for package " + pkg);
    }

    private static Object slice(List<?> list) throws Exception {
        return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class).newInstance(list);
    }

    // No component has run on a device that keeps no history.
    private static Object noEvents() throws Exception {
        return Class.forName("android.app.usage.UsageEvents").getConstructor(List.class, String[].class)
                .newInstance(new ArrayList<Object>(), new String[0]);
    }

    private static UnsupportedOperationException untracked(String what) {
        return new UnsupportedOperationException("usagestats: " + what + " is not tracked");
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("usagestats." + method); }
        switch (contract) {
            case queryUsageStats:
                if (!hasQueryPermission((String) a[3])) return null;
                user((Integer) a[4]);
                return slice(new ArrayList<Object>());
            case queryConfigurationStats:
            case queryEventStats:
                return hasQueryPermission((String) a[3]) ? slice(new ArrayList<Object>()) : null;
            case queryEvents:
                return hasQueryPermission((String) a[2]) ? noEvents() : null;
            case queryEventsWithFilter: {
                Objects.requireNonNull(a[0], "query"); Objects.requireNonNull(a[1], "callingPackage");
                if (!hasQueryPermission((String) a[1])) return null;
                int queried = (Integer) a[0].getClass().getMethod("getUserId").invoke(a[0]);
                if (queried != -10000 /* UserHandle.USER_NULL */) user(queried);
                return noEvents();
            }
            case queryEventsForPackage:
                sameApp((String) a[2]);
                return noEvents();
            case queryEventsForUser:
                if (!hasQueryPermission((String) a[3])) return null;
                user((Integer) a[2]);
                return noEvents();
            case queryEventsForPackageForUser:
                if (!hasQueryPermission((String) a[4])) return null;
                user((Integer) a[2]);
                sameApp((String) a[3]);
                return noEvents();
            case isAppStandbyEnabled: return true;
            case isAppInactive: {
                user((Integer) a[1]);
                String packageName = (String) a[0], callingPackage = (String) a[2];
                if (packageName.equals(callingPackage)) {
                    if (packages.packageUid(callingPackage) != Process.myUid()) return false;
                } else if (!hasQueryPermission(callingPackage)) return false;
                return false;  // every package is ACTIVE
            }
            case setAppInactive:
                user((Integer) a[2]);
                enforce("android.permission.CHANGE_APP_IDLE_STATE", "No permission to change app idle state");
                throw untracked("app idle state");
            case getAppStandbyBucket: {
                user((Integer) a[2]);
                String packageName = (String) a[0];
                int uid = packages.packageUid(packageName);
                if (uid != Process.myUid() && !hasQueryPermission((String) a[1]))
                    throw new SecurityException("Don't have permission to query app standby bucket");
                if (uid < 0) throw new IllegalArgumentException("Cannot get standby bucket for non existent package (" + packageName + ")");
                return BUCKET_ACTIVE;
            }
            case getAppMinStandbyBucket: {
                user((Integer) a[2]);
                String packageName = (String) a[0];
                int uid = packages.packageUid(packageName);
                if (uid != Process.myUid() && !hasQueryPermission((String) a[1]))
                    throw new SecurityException("Don't have permission to query min app standby bucket");
                if (uid < 0) throw new IllegalArgumentException("Cannot get min standby bucket for non existent package (" + packageName + ")");
                return 0;  // no minimum: no exemption applies
            }
            case getAppStandbyBuckets: {
                user((Integer) a[1]);
                if (!hasQueryPermission((String) a[0])) throw new SecurityException("Don't have permission to query app standby bucket");
                List<Object> infos = new ArrayList<>();
                Class<?> info = Class.forName("android.app.usage.AppStandbyInfo");
                for (String name : packages.visiblePackageNames())
                    infos.add(info.getConstructor(String.class, int.class).newInstance(name, BUCKET_ACTIVE));
                return slice(infos);
            }
            case setAppStandbyBucket:
            case setAppStandbyBuckets:
                enforce("android.permission.CHANGE_APP_IDLE_STATE", "Permission Denial: requires android.permission.CHANGE_APP_IDLE_STATE");
                throw untracked("app standby buckets");
            case setEstimatedLaunchTime:
            case setEstimatedLaunchTimes:
                enforce("android.permission.CHANGE_APP_LAUNCH_TIME_ESTIMATE",
                        "Permission Denial: requires android.permission.CHANGE_APP_LAUNCH_TIME_ESTIMATE");
                throw untracked("launch time estimates");
            case onCarrierPrivilegedAppsChanged:
                enforce("android.permission.BIND_CARRIER_SERVICES", "onCarrierPrivilegedAppsChanged can only be called by privileged apps.");
                throw untracked("carrier privileged apps");
            case reportChooserSelection: {
                if (a[0] == null) throw new IllegalArgumentException("Package selection must not be null.");
                String contentType = (String) a[2], action = (String) a[4];
                if (contentType == null || contentType.trim().isEmpty() || action == null || action.trim().isEmpty()) return null;
                enforce("android.permission.REPORT_USAGE_STATS",
                        "Only the system or holders of the REPORT_USAGE_STATS permission are allowed to call reportChooserSelection");
                throw untracked("chooser selections");
            }
            case reportUserInteraction:
            case reportUserInteractionWithBundle: {
                if (contract == Contract.reportUserInteractionWithBundle) {
                    Objects.requireNonNull(a[0]);
                    android.os.PersistableBundle extras = (android.os.PersistableBundle) a[2];
                    if (extras == null || extras.size() == 0) throw new IllegalArgumentException("Emtry extras!");
                    if (isEmpty(extras.getString("android.app.usage.extra.EVENT_CATEGORY")))
                        throw new IllegalArgumentException("Empty android.app.usage.extra.EVENT_CATEGORY");
                    if (isEmpty(extras.getString("android.app.usage.extra.EVENT_ACTION")))
                        throw new IllegalArgumentException("Empty android.app.usage.extra.EVENT_ACTION");
                }
                enforce("android.permission.REPORT_USAGE_STATS",
                        "Only the system or holders of the REPORT_USAGE_STATS permission are allowed to call reportUserInteraction");
                user((Integer) a[1]);
                if (packages.packageUid((String) a[0]) < 0) throw new IllegalArgumentException("Package " + a[0] + " does not exist!");
                throw untracked("user interactions");
            }
            case registerAppUsageObserver:
                enforce("android.permission.OBSERVE_APP_USAGE", "Caller doesn't have OBSERVE_APP_USAGE permission");
                if (a[1] == null || ((String[]) a[1]).length == 0) throw new IllegalArgumentException("Must specify at least one package");
                if (a[3] == null) throw new NullPointerException("callbackIntent can't be null");
                throw untracked("app usage observers");
            case unregisterAppUsageObserver:
            case unregisterUsageSessionObserver:
                enforce("android.permission.OBSERVE_APP_USAGE", "Caller doesn't have OBSERVE_APP_USAGE permission");
                throw untracked("usage observers");
            case registerUsageSessionObserver:
                enforce("android.permission.OBSERVE_APP_USAGE", "Caller doesn't have OBSERVE_APP_USAGE permission");
                if (a[1] == null || ((String[]) a[1]).length == 0) throw new IllegalArgumentException("Must specify at least one observed entity");
                if (a[4] == null) throw new NullPointerException("limitReachedCallbackIntent can't be null");
                throw untracked("usage session observers");
            case registerAppUsageLimitObserver:
            case unregisterAppUsageLimitObserver:
                if (!packages.granted("android.permission.SUSPEND_APPS") || !packages.granted("android.permission.OBSERVE_APP_USAGE"))
                    throw new SecurityException("Caller must be the active supervision app or it must have both SUSPEND_APPS and OBSERVE_APP_USAGE permissions");
                throw untracked("app usage limit observers");
            case reportUsageStart:
            case reportPastUsageStart: {
                IBinder activity = (IBinder) a[0];
                String token = (String) a[1], callingPackage = (String) a[contract == Contract.reportUsageStart ? 2 : 3];
                Set<String> tokens = usageReporters.computeIfAbsent(activity.hashCode(), unused -> new HashSet<>());
                if (!tokens.add(token))
                    throw new IllegalArgumentException(token + " for " + callingPackage + " is already reported as started for this activity");
                return null;
            }
            case reportUsageStop: {
                Set<String> tokens = usageReporters.get(((IBinder) a[0]).hashCode());
                if (tokens == null) throw new IllegalArgumentException("Unknown reporter trying to stop token " + a[1] + " for " + a[2]);
                if (!tokens.remove(a[1]))
                    throw new IllegalArgumentException(a[1] + " for " + a[2] + " is already reported as stopped for this activity");
                return null;
            }
            case getUsageSource:
                enforce("android.permission.OBSERVE_APP_USAGE", "Caller doesn't have OBSERVE_APP_USAGE permission");
                return USAGE_SOURCE_TASK_ROOT_ACTIVITY;
            case forceUsageSourceSettingRead: return null;  // the default source never changes
            case getLastTimeAnyComponentUsed:
                enforce("android.permission.INTERACT_ACROSS_USERS", "Caller doesn't have INTERACT_ACROSS_USERS permission");
                if (!hasQueryPermission((String) a[1])) throw new SecurityException("Don't have permission to query usage stats");
                return 0L;
            case queryBroadcastResponseStats: {
                Objects.requireNonNull(a[2]);
                if ((Long) a[1] < 0) throw new IllegalArgumentException("id needs to be >=0");
                enforce("android.permission.ACCESS_BROADCAST_RESPONSE_STATS", "queryBroadcastResponseStats");
                user((Integer) a[3]);
                Object empty = Class.forName("android.app.usage.BroadcastResponseStatsList")
                        .getConstructor(java.util.Collection.class).newInstance(new ArrayList<Object>());
                return empty;
            }
            case clearBroadcastResponseStats:
                Objects.requireNonNull(a[2]);
                if ((Long) a[1] < 0) throw new IllegalArgumentException("id needs to be >=0");
                enforce("android.permission.ACCESS_BROADCAST_RESPONSE_STATS", "clearBroadcastResponseStats");
                user((Integer) a[3]);
                return null;  // nothing recorded
            case clearBroadcastEvents:
                Objects.requireNonNull(a[0]);
                enforce("android.permission.ACCESS_BROADCAST_RESPONSE_STATS", "clearBroadcastEvents");
                user((Integer) a[1]);
                return null;
            case isPackageExemptedFromBroadcastResponseStats:
                Objects.requireNonNull(a[0]);
                enforce("android.permission.DUMP", "isPackageExemptedFromBroadcastResponseStats");
                return false;
            case getAppStandbyConstant:
                Objects.requireNonNull(a[0]);
                enforce("android.permission.READ_DEVICE_CONFIG", "Caller doesn't have READ_DEVICE_CONFIG permission");
                return null;  // no constant is overridden
            default: throw new UnsupportedOperationException("usagestats." + method);
        }
    }

    private static boolean isEmpty(String value) { return value == null || value.isEmpty(); }
}
