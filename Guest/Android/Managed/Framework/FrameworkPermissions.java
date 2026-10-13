// PermissionManagerService ("permissionmgr") and PermissionCheckerService
// ("permission_checker") over FrameworkPackages' install-time grants: normal
// permissions requested in the manifest are granted, runtime permissions stay
// denied and no user has ever answered a permission dialog. PermissionManager
// and PermissionCheckerManager use ServiceManager.getServiceOrThrow, so without
// these services getSystemService returns null and PackageManager.checkPermission
// dereferences it. Semantics follow Android 15's PermissionManagerService.
import android.content.AttributionSource;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.os.IBinder;
import android.os.Process;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

final class FrameworkPermissions {
    // VirtualDeviceManager.PERSISTENT_DEVICE_ID_DEFAULT and Context.DEVICE_ID_DEFAULT.
    private static final String DEFAULT_DEVICE = "default:0";
    private static final int DEVICE_ID_DEFAULT = 0;
    // PermissionChecker results and AppOpsManager modes.
    private static final int GRANTED = 0, SOFT_DENIED = 1, HARD_DENIED = 2;
    private static final int MODE_ALLOWED = 0, MODE_IGNORED = 1, MODE_ERRORED = 2, MODE_DEFAULT = 3,
            MODE_FOREGROUND = 4, OP_NONE = -1;
    // SystemConfig's default for <split-permission> without targetSdk
    // (Build.VERSION_CODES.CUR_DEVELOPMENT + 1).
    private static final int ALL_TARGETS = 10001;
    // Android 15 frameworks/base/data/etc/platform.xml <split-permission>
    // entries, in file order. The provisioned runtime is pinned to that
    // release; its image files do not include platform.xml.
    private static final Object[][] SPLITS = {
        {"android.permission.ACCESS_FINE_LOCATION", ALL_TARGETS, "android.permission.ACCESS_COARSE_LOCATION"},
        {"android.permission.WRITE_EXTERNAL_STORAGE", ALL_TARGETS, "android.permission.READ_EXTERNAL_STORAGE"},
        {"android.permission.READ_CONTACTS", 16, "android.permission.READ_CALL_LOG"},
        {"android.permission.WRITE_CONTACTS", 16, "android.permission.WRITE_CALL_LOG"},
        {"android.permission.ACCESS_FINE_LOCATION", 29, "android.permission.ACCESS_BACKGROUND_LOCATION"},
        {"android.permission.ACCESS_COARSE_LOCATION", 29, "android.permission.ACCESS_BACKGROUND_LOCATION"},
        {"android.permission.READ_EXTERNAL_STORAGE", 29, "android.permission.ACCESS_MEDIA_LOCATION"},
        {"android.permission.WRITE_EXTERNAL_STORAGE", 29, "android.permission.ACCESS_MEDIA_LOCATION"},
        {"android.permission.BLUETOOTH", 31, "android.permission.BLUETOOTH_SCAN"},
        {"android.permission.BLUETOOTH", 31, "android.permission.BLUETOOTH_CONNECT"},
        {"android.permission.BLUETOOTH", 31, "android.permission.BLUETOOTH_ADVERTISE"},
        {"android.permission.BLUETOOTH_ADMIN", 31, "android.permission.BLUETOOTH_SCAN"},
        {"android.permission.BLUETOOTH_ADMIN", 31, "android.permission.BLUETOOTH_CONNECT"},
        {"android.permission.BLUETOOTH_ADMIN", 31, "android.permission.BLUETOOTH_ADVERTISE"},
        {"android.permission.BODY_SENSORS", 33, "android.permission.BODY_SENSORS_BACKGROUND"},
        {"android.permission.READ_EXTERNAL_STORAGE", 33, "android.permission.READ_MEDIA_AUDIO"},
        {"android.permission.READ_EXTERNAL_STORAGE", 33, "android.permission.READ_MEDIA_VIDEO"},
        {"android.permission.READ_EXTERNAL_STORAGE", 33, "android.permission.READ_MEDIA_IMAGES"},
        {"android.permission.WRITE_EXTERNAL_STORAGE", 33, "android.permission.READ_MEDIA_AUDIO"},
        {"android.permission.WRITE_EXTERNAL_STORAGE", 33, "android.permission.READ_MEDIA_VIDEO"},
        {"android.permission.WRITE_EXTERNAL_STORAGE", 33, "android.permission.READ_MEDIA_IMAGES"},
        {"android.permission.READ_MEDIA_IMAGES", ALL_TARGETS, "android.permission.READ_MEDIA_VISUAL_USER_SELECTED"},
        {"android.permission.READ_MEDIA_VIDEO", ALL_TARGETS, "android.permission.READ_MEDIA_VISUAL_USER_SELECTED"},
        {"android.permission.ACCESS_MEDIA_LOCATION", ALL_TARGETS, "android.permission.READ_MEDIA_VISUAL_USER_SELECTED"},
    };

    private enum Manager {
        checkPermission,
        checkUidPermission,
        getPermissionInfo,
        queryPermissionsByGroup,
        shouldShowRequestPermissionRationale,
        isPermissionRevokedByPolicy,
        getSplitPermissions,
        registerAttributionSource,
        isRegisteredAttributionSource,
        getRegisteredAttributionSourceCount,
        addOnPermissionsChangeListener,
        removeOnPermissionsChangeListener
    }

    private enum Checker { checkPermission, checkOp, finishDataDelivery }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    // Registered attribution sources by token (AttributionSourceRegistry).
    private final Map<IBinder, AttributionSource> attributions = new HashMap<>();

    FrameworkPermissions(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("permissionmgr", "android.permission.IPermissionManager", new FrameworkServices.Call() {
            @Override public String[] methods() { return names(Manager.values()); }
            @Override public Object invoke(String method, Object[] a) throws Exception { return manager(method, a); }
        });
        services.register("permission_checker", "android.permission.IPermissionChecker", new FrameworkServices.Call() {
            @Override public String[] methods() { return names(Checker.values()); }
            @Override public Object invoke(String method, Object[] a) throws Exception { return checker(method, a); }
        });
    }

    // The single installed package holds the grants; other packages and users
    // are not visible, as package visibility filtering would report.
    private int packagePermission(String packageName, String permission, int userId) {
        Objects.requireNonNull(packageName, "packageName");
        Objects.requireNonNull(permission, "permissionName");
        int uid = packages.packageUid(packageName);
        if (userId != 0 || uid == -1) return PackageManager.PERMISSION_DENIED;
        return packages.checkPermission(permission, uid, DEVICE_ID_DEFAULT);
    }

    private void enforce(String permission) {
        if (!packages.granted(permission)) throw new SecurityException(permission);
    }

    private static AttributionSource source(Object state) throws Exception {
        Objects.requireNonNull(state, "attributionSource");
        return AttributionSource.class.getConstructor(Class.forName("android.content.AttributionSourceState"))
                .newInstance(state);
    }

    private static Object hidden(AttributionSource source, String name) throws Exception {
        Method method = AttributionSource.class.getMethod(name);
        return method.invoke(source);
    }

    private synchronized Object manager(String method, Object[] a) throws Exception {
        Manager contract;
        try { contract = Manager.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("permissionmgr." + method); }
        switch (contract) {
            case checkPermission:
                if (!DEFAULT_DEVICE.equals(a[2])) return PackageManager.PERMISSION_DENIED;
                return packagePermission((String) a[0], (String) a[1], (Integer) a[3]);
            case checkUidPermission:
                return packages.checkPermission((String) Objects.requireNonNull(a[1], "permissionName"),
                        (Integer) a[0], (Integer) a[2]);
            case getPermissionInfo: return packages.permissionInfo((String) a[0]);
            case queryPermissionsByGroup: {
                List<PermissionInfo> result = packages.permissionsInGroup((String) a[0]);
                // PermissionManagerServiceImpl returns null for an unknown non-null group.
                if (a[0] != null && result.isEmpty()) return null;
                return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                        .newInstance(result);
            }
            // Rationale is shown only after the user denied a request without
            // "don't ask again" (FLAG_PERMISSION_USER_SET); no user ever did.
            case shouldShowRequestPermissionRationale:
            // No device policy fixes or revokes permissions here.
            case isPermissionRevokedByPolicy:
                Objects.requireNonNull(a[0], "packageName");
                Objects.requireNonNull(a[1], "permissionName");
                return false;
            case getSplitPermissions: {
                Class<?> type = Class.forName("android.content.pm.permission.SplitPermissionInfoParcelable");
                java.lang.reflect.Constructor<?> create = type.getConstructor(String.class, List.class, int.class);
                List<Object> result = new ArrayList<>(SPLITS.length);
                for (Object[] split : SPLITS)
                    result.add(create.newInstance(split[0], Arrays.asList((String) split[2]), split[1]));
                return result;
            }
            case registerAttributionSource: {
                AttributionSource source = source(a[0]);
                // registerAttributionSource: the source must be the caller's
                // own package, and a proxy chain longer than one must be registered.
                if (source.getUid() != Process.myUid() && !packages.granted("android.permission.UPDATE_APP_OPS_STATS"))
                    throw new SecurityException("Cannot register attribution source for uid:" + source.getUid());
                if (packages.packageUid(source.getPackageName()) != source.getUid())
                    throw new SecurityException("Cannot register attribution source for package:" + source.getPackageName());
                AttributionSource next = source.getNext();
                if (next != null && next.getNext() != null && !registered(next))
                    throw new SecurityException("Cannot register forged attribution source:" + source);
                IBinder token = (IBinder) hidden(source, "getToken");
                if (attributions.size() >= 4096 && !attributions.containsKey(token))
                    throw new IllegalStateException("Attribution source limit");
                attributions.put(token, (AttributionSource) hidden(source, "withDefaultToken"));
                return token;
            }
            case isRegisteredAttributionSource: return registered(source(a[0]));
            case getRegisteredAttributionSourceCount: {
                enforce("android.permission.UPDATE_APP_OPS_STATS");
                int count = 0;
                for (AttributionSource source : attributions.values()) if (source.getUid() == (Integer) a[0]) count++;
                return count;
            }
            case addOnPermissionsChangeListener:
            case removeOnPermissionsChangeListener:
                enforce("android.permission.OBSERVE_GRANT_REVOKE_PERMISSIONS");
                return null;
            default: throw new UnsupportedOperationException("permissionmgr." + method);
        }
    }

    private boolean registered(AttributionSource source) throws Exception {
        AttributionSource cached = attributions.get((IBinder) hidden(source, "getToken"));
        return cached != null && (Boolean) AttributionSource.class.getMethod("equalsExceptToken", AttributionSource.class)
                .invoke(cached, source);
    }

    // AppOpsManager.opToDefaultMode: no service changed an op's mode here.
    private static int defaultMode(int op) throws Exception {
        return (Integer) Class.forName("android.app.AppOpsManager").getMethod("opToDefaultMode", int.class)
                .invoke(null, op);
    }

    private static int permissionOp(String permission) throws Exception {
        return (Integer) Class.forName("android.app.AppOpsManager").getMethod("permissionToOpCode", String.class)
                .invoke(null, permission);
    }

    private static int result(int mode) {
        switch (mode) {
            case MODE_ALLOWED: case MODE_FOREGROUND: case MODE_DEFAULT: return GRANTED;
            case MODE_IGNORED: return SOFT_DENIED;
            default: return HARD_DENIED;
        }
    }

    // PermissionCheckerService for one app: every link of the attribution
    // chain must be this app and hold the permission; other apps are not
    // trusted proxies. A held permission's app op then decides the result.
    private Object checker(String method, Object[] a) throws Exception {
        Checker contract;
        try { contract = Checker.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("permission_checker." + method); }
        switch (contract) {
            case checkPermission: {
                String permission = (String) Objects.requireNonNull(a[0], "permission");
                for (AttributionSource link = source(a[1]); link != null; link = link.getNext()) {
                    if (link.getUid() != Process.myUid()
                            || packages.packageUid(link.getPackageName()) != link.getUid()
                            || packages.checkPermission(permission, link.getUid(), DEVICE_ID_DEFAULT)
                                    != PackageManager.PERMISSION_GRANTED)
                        return HARD_DENIED;
                }
                int op = (Integer) a[6] != OP_NONE ? (Integer) a[6] : permissionOp(permission);
                return op < 0 ? GRANTED : result(defaultMode(op));
            }
            case checkOp: {
                AttributionSource source = source(a[1]);
                if (source.getUid() != Process.myUid() || packages.packageUid(source.getPackageName()) != source.getUid())
                    return HARD_DENIED;
                return result(defaultMode((Integer) a[0]));
            }
            // No op is started, so there is nothing to finish.
            case finishDataDelivery: source(a[1]); return null;
            default: throw new UnsupportedOperationException("permission_checker." + method);
        }
    }
}
