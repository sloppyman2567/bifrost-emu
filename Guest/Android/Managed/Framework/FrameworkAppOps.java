// App-ops service ("appops"): every IAppOpsService method of Android 15
// (android-15.0.0_r1), following AppOpsService, AttributedOp,
// AppOpsRestrictionsImpl and AudioRestrictionManager. SystemServiceRegistry
// builds AppOpsManager from ServiceManager.getServiceOrThrow("appops").
//
// The service runs inside the app's process, so Binder.getCallingUid() is the
// app's uid and AOSP's callingPid == myPid bypass is deliberately absent:
// privileged methods fail for the app exactly as they do for a third-party
// app on a device. Modes the app may not set are changed through the
// package-private setUidModeUnchecked/setModeUnchecked (the permission-policy
// path of AppOpsService.setUidMode/setMode), never through the Binder entry.
//
// Not modelled: persistence across launches (the app is a fresh install every
// run, AppOpsService would otherwise read appops.xml), package visibility
// filtering, uid-state tracking (the process is UID_STATE_TOP, so
// MODE_FOREGROUND evaluates to MODE_ALLOWED), role grants on reset, DPM
// deferral and virtual devices (none exist, so every device id but the
// default one is invalid, as with an empty VirtualDeviceManager).
// Hidden APIs (the build links the public android.jar) are reflection-only.
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Process;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class FrameworkAppOps {
    // AppOpsManager.MODE_*.
    static final int MODE_ALLOWED = 0, MODE_IGNORED = 1, MODE_ERRORED = 2, MODE_DEFAULT = 3, MODE_FOREGROUND = 4;
    private static final int INVALID_UID = -1, ROOT_UID = 0, SYSTEM_UID = 1000, SHELL_UID = 2000,
            MEDIA_UID = 1013, AUDIOSERVER_UID = 1041, CAMERASERVER_UID = 1047;  // Process
    private static final int OP_NONE = -1, ALL_OPS = -2, UID_ANY = -2;  // AppOpsManager, AppOpsService
    private static final int OP_FLAG_SELF = 1, OP_FLAG_TRUSTED_PROXY = 2, OP_FLAG_UNTRUSTED_PROXY = 4,
            OP_FLAG_TRUSTED_PROXIED = 8, OP_FLAG_UNTRUSTED_PROXIED = 16, OP_FLAGS_ALL = 31;
    private static final int UID_STATE_TOP = 200, USER_ALL = -1, USER_CURRENT = -2, USER_CURRENT_OR_SELF = -3;
    private static final int CALL_BACK_ON_SWITCHED_OP = 2;  // AppOpsManager.CALL_BACK_ON_SWITCHED_OP
    private static final int START_TYPE_FAILED = 0, START_TYPE_STARTED = 1;  // OnOpStartedListener
    private static final int ATTRIBUTION_FLAG_TRUSTED = 8, ATTRIBUTION_CHAIN_ID_NONE = -1;
    private static final int FILTER_BY_UID = 1, FILTER_BY_PACKAGE_NAME = 2, FILTER_BY_ATTRIBUTION_TAG = 4,
            FILTER_BY_OP_NAMES = 8;  // AppOpsManager.HistoricalOpsRequest filters
    private static final int MAX_UNFORWARDED_OPS = 10, MAX_HISTORY_EVENTS = 8192;
    private static final String PERSISTENT_DEVICE_ID_DEFAULT = "default:0";  // VirtualDeviceManager

    private static final String MANAGE_APPOPS = "android.permission.MANAGE_APPOPS",
            GET_APP_OPS_STATS = "android.permission.GET_APP_OPS_STATS",
            MANAGE_APP_OPS_MODES = "android.permission.MANAGE_APP_OPS_MODES",
            UPDATE_APP_OPS_STATS = "android.permission.UPDATE_APP_OPS_STATS",
            WATCH_APPOPS = "android.permission.WATCH_APPOPS",
            MANAGE_APP_OPS_RESTRICTIONS = "android.permission.MANAGE_APP_OPS_RESTRICTIONS",
            GET_HISTORICAL_APP_OPS_STATS = "android.permission.GET_HISTORICAL_APP_OPS_STATS",
            INTERACT_ACROSS_USERS = "android.permission.INTERACT_ACROSS_USERS",
            INTERACT_ACROSS_USERS_FULL = "android.permission.INTERACT_ACROSS_USERS_FULL";

    // Every IAppOpsService method, in AIDL order; the enum drives dispatch and
    // the service inventory, and the test checks it against the interface.
    private enum Contract {
        checkOperation, noteOperation, startOperation, finishOperation, startWatchingMode, stopWatchingMode,
        permissionToOpCode, checkAudioOperation, shouldCollectNotes, setCameraAudioRestriction,
        startWatchingModeWithFlags, noteProxyOperation, startProxyOperation, finishProxyOperation, checkPackage,
        collectRuntimeAppOpAccessMessage, reportRuntimeAppOpAccessMessageAndGetConfig, getPackagesForOps,
        getOpsForPackage, getHistoricalOps, getHistoricalOpsFromDiskRaw, offsetHistory, setHistoryParameters,
        addHistoricalOps, resetHistoryParameters, resetPackageOpsNoHistory, clearHistory, rebootHistory, getUidOps,
        setUidMode, setMode, resetAllModes, setAudioRestriction, setUserRestrictions, setUserRestriction,
        removeUser, startWatchingActive, stopWatchingActive, isOperationActive, isProxying, startWatchingStarted,
        stopWatchingStarted, startWatchingNoted, stopWatchingNoted, startWatchingAsyncNoted, stopWatchingAsyncNoted,
        extractAsyncOps, checkOperationRaw, reloadNonHistoricalState, collectNoteOpCallsForValidation,
        noteProxyOperationWithState, startProxyOperationWithState, finishProxyOperationWithState,
        checkOperationRawForDevice, checkOperationForDevice, noteOperationForDevice, startOperationForDevice,
        finishOperationForDevice, getPackagesForOpsForDevice
    }

    // ---- hidden framework API, resolved once ----
    private static final Class<?> MANAGER = android.app.AppOpsManager.class;
    private static final int NUM_OP = numOps();  // AppOpsManager._NUM_OP
    private static final Method OP_TO_DEFAULT_MODE = method(MANAGER, "opToDefaultMode", int.class),
            OP_TO_SWITCH = method(MANAGER, "opToSwitch", int.class),
            OP_RESTRICTS_READ = method(MANAGER, "opRestrictsRead", int.class),
            OP_ALLOWS_RESET = method(MANAGER, "opAllowsReset", int.class),
            OP_TO_RESTRICTION = method(MANAGER, "opToRestriction", int.class),
            OP_TO_PERMISSION = method(MANAGER, "opToPermission", int.class),
            PERMISSION_TO_OP_CODE = method(MANAGER, "permissionToOpCode", String.class),
            SHOULD_FORCE_COLLECT_NOTE = method(MANAGER, "shouldForceCollectNoteForOp", int.class),
            RESOLVE_PACKAGE_NAME = method(MANAGER, "resolvePackageName", int.class, String.class),
            STR_OP_TO_OP = method(MANAGER, "strOpToOp", String.class),
            OP_TO_PUBLIC_NAME = method(MANAGER, "opToPublicName", int.class),
            MAKE_KEY = method(MANAGER, "makeKey", int.class, int.class);
    private static final Constructor<?> SYNC_NOTED = constructor("android.app.SyncNotedAppOp",
                    int.class, int.class, String.class, String.class),
            ASYNC_NOTED = constructor("android.app.AsyncNotedAppOp", int.class, int.class, String.class, String.class, long.class),
            PACKAGE_OPS = constructor("android.app.AppOpsManager$PackageOps", String.class, int.class, List.class),
            OP_ENTRY = constructor("android.app.AppOpsManager$OpEntry", int.class, int.class, Map.class),
            ATTRIBUTED_ENTRY = constructor("android.app.AppOpsManager$AttributedOpEntry", int.class, boolean.class,
                    android.util.LongSparseArray.class, android.util.LongSparseArray.class),
            NOTE_EVENT = constructor("android.app.AppOpsManager$NoteOpEvent", long.class, long.class, type("android.app.AppOpsManager$OpEventProxyInfo")),
            PROXY_INFO = constructor("android.app.AppOpsManager$OpEventProxyInfo", int.class, String.class, String.class),
            HISTORICAL_OPS = constructor("android.app.AppOpsManager$HistoricalOps", long.class, long.class),
            SAMPLING_CONFIG = constructor("com.android.internal.app.MessageSamplingConfig", int.class, int.class, long.class),
            ATTRIBUTION_FROM_STATE = constructor("android.content.AttributionSource", type("android.content.AttributionSourceState"));

    private static int numOps() {
        try { return MANAGER.getField("_NUM_OP").getInt(null); }
        catch (ReflectiveOperationException error) { throw new ExceptionInInitializerError(error); }
    }

    private static Class<?> type(String name) {
        try { return Class.forName(name); }
        catch (ClassNotFoundException error) { throw new IllegalStateException(error); }
    }

    private static Method method(Class<?> owner, String name, Class<?>... types) {
        try { return owner.getMethod(name, types); }
        catch (NoSuchMethodException error) { throw new IllegalStateException(error); }
    }

    private static Constructor<?> constructor(String name, Class<?>... types) {
        try { return type(name).getConstructor(types); }
        catch (NoSuchMethodException error) { throw new IllegalStateException(error); }
    }

    private static Object invoke(Method method, Object target, Object... args) {
        try { return method.invoke(target, args); }
        catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException) throw (RuntimeException) cause;
            if (cause instanceof Error) throw (Error) cause;
            throw new IllegalStateException(cause);
        } catch (IllegalAccessException error) { throw new IllegalStateException(error); }
    }

    private static Object create(Constructor<?> constructor, Object... args) {
        try { return constructor.newInstance(args); }
        catch (InvocationTargetException error) {
            if (error.getCause() instanceof RuntimeException) throw (RuntimeException) error.getCause();
            throw new IllegalStateException(error.getCause());
        } catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }

    private static int intOf(Method method, Object... args) { return (Integer) invoke(method, null, args); }
    private static boolean boolOf(Method method, Object... args) { return (Boolean) invoke(method, null, args); }
    static int defaultMode(int op) { return intOf(OP_TO_DEFAULT_MODE, op); }
    // AppOpsManager.opToSwitch; reserved op numbers switch to OP_NONE, which AOSP then indexes with (an
    // ArrayIndexOutOfBoundsException), so such an op is its own switch here.
    static int switchOp(int op) {
        int switchCode = intOf(OP_TO_SWITCH, op);
        return switchCode < 0 ? op : switchCode;
    }
    static int numOp() { return NUM_OP; }
    static String[] contractMethods() { return names(Contract.values()); }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    // ---- state ----
    private final FrameworkPackages packages;
    private final Object lock = new Object();
    private final Map<Long, Integer> uidModes = new HashMap<>();       // (uid, switch op) -> mode
    private final Map<String, Integer> packageModes = new HashMap<>(); // user/package/switch op -> mode
    private final Map<String, Op> ops = new LinkedHashMap<>();         // uid/package/op
    private final Map<String, Set<String>> declaredTags = new HashMap<>();
    private final List<HistoryEvent> history = new ArrayList<>();
    private final Map<IBinder, ModeWatcher> modeWatchers = new HashMap<>();
    private final Map<IBinder, Map<Integer, Watcher>> activeWatchers = new HashMap<>(),
            startedWatchers = new HashMap<>(), notedWatchers = new HashMap<>();
    private final Map<String, List<Object>> asyncWatchers = new HashMap<>();
    private final Map<String, List<Object>> unforwardedAsync = new HashMap<>();
    private final Map<IBinder, Map<Integer, Map<Integer, Boolean>>> userRestrictions = new HashMap<>();   // token -> user -> op
    private final Map<IBinder, Map<Integer, Object>> userRestrictionExclusions = new HashMap<>();         // token -> user -> PackageTagsList
    private final Map<Integer, Map<Integer, AudioRestriction>> audioRestrictions = new HashMap<>();       // op -> usage
    private int cameraAudioRestriction;  // CameraDevice.AUDIO_RESTRICTION_*
    private Set<Integer> cameraMutedUsages;
    private ExecutorService notifier;

    private static final class Watcher {
        final Object callback;  // the IApps*Callback interface
        final int watchingUid, callingUid;
        Watcher(Object callback, int watchingUid, int callingUid) {
            this.callback = callback; this.watchingUid = watchingUid; this.callingUid = callingUid;
        }
    }

    private static final class ModeWatcher {
        final Object callback;
        final int flags, notifiedOp, callingUid;
        final Set<Integer> switchOps = new HashSet<>();
        final Set<String> watchedPackages = new HashSet<>();
        ModeWatcher(Object callback, int flags, int notifiedOp, int callingUid) {
            this.callback = callback; this.flags = flags; this.notifiedOp = notifiedOp; this.callingUid = callingUid;
        }
    }

    private static final class AudioRestriction {
        final int mode;
        final Set<String> exceptionPackages = new HashSet<>();
        AudioRestriction(int mode) { this.mode = mode; }
    }

    // One start(clientId) of an attributed op; startCount nests like AttributedOp.InProgressStartOpEvent.
    private static final class InProgress {
        final long startTime = System.currentTimeMillis(), startElapsed = android.os.SystemClock.elapsedRealtime();
        final int flags, uidState, attributionFlags, chainId, deviceId;
        int startCount;
        IBinder.DeathRecipient recipient;
        InProgress(int flags, int uidState, int attributionFlags, int chainId, int deviceId) {
            this.flags = flags; this.uidState = uidState; this.attributionFlags = attributionFlags;
            this.chainId = chainId; this.deviceId = deviceId;
        }
    }

    // AttributedOp: access/reject events keyed by makeKey(uidState, flags) (time, duration, proxy uid/package/tag).
    private static final class Attribution {
        final String tag;
        final Map<IBinder, InProgress> running = new HashMap<>(), paused = new HashMap<>();
        final Map<Long, long[]> accessEvents = new HashMap<>(), rejectEvents = new HashMap<>();
        final Map<Long, Object[]> accessProxies = new HashMap<>();
        Attribution(String tag) { this.tag = tag; }
    }

    private static final class Op {
        final int code, uid;
        final String packageName;
        final Map<String, Attribution> attributions = new LinkedHashMap<>();
        Op(int code, int uid, String packageName) { this.code = code; this.uid = uid; this.packageName = packageName; }
        boolean running() {
            for (Attribution attribution : attributions.values()) if (!attribution.running.isEmpty()) return true;
            return false;
        }
    }

    private static final class HistoryEvent {
        final long time, duration;
        final int code, uid, uidState, flags;
        final String packageName, tag;
        final boolean rejected;
        HistoryEvent(long time, long duration, int code, int uid, String packageName, String tag, int uidState, int flags,
                boolean rejected) {
            this.time = time; this.duration = duration; this.code = code; this.uid = uid;
            this.packageName = packageName; this.tag = tag; this.uidState = uidState; this.flags = flags;
            this.rejected = rejected;
        }
    }

    // Package/attribution verification result (AppOpsService.PackageVerificationResult).
    private static final class Verified {
        final boolean validTag;
        Verified(boolean validTag) { this.validTag = validTag; }
    }

    FrameworkAppOps(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("appops", "com.android.internal.app.IAppOpsService", new FrameworkServices.Call() {
            @Override public String[] methods() { return names(Contract.values()); }
            @Override public Object invoke(String method, Object[] a) throws Exception { return call(method, a); }
        });
    }

    private Object call(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) {
            throw new IllegalStateException("appops." + method + " is not part of the Android 15 IAppOpsService contract");
        }
        switch (contract) {
            case checkOperation: return checkOperation((Integer) a[0], (Integer) a[1], (String) a[2], null, 0, false);
            case checkOperationRaw: return checkOperation((Integer) a[0], (Integer) a[1], (String) a[2], (String) a[3], 0, true);
            case checkOperationForDevice:
                return checkOperation((Integer) a[0], (Integer) a[1], (String) a[2], null, (Integer) a[3], false);
            case checkOperationRawForDevice:
                return checkOperation((Integer) a[0], (Integer) a[1], (String) a[2], (String) a[3], (Integer) a[4], true);
            case checkAudioOperation: return checkAudioOperation((Integer) a[0], (Integer) a[1], (Integer) a[2], (String) a[3]);
            case setAudioRestriction: setAudioRestriction((Integer) a[0], (Integer) a[1], (Integer) a[2], (Integer) a[3], (String[]) a[4]); return null;
            case setCameraAudioRestriction: setCameraAudioRestriction((Integer) a[0]); return null;
            case checkPackage: return checkPackage((Integer) a[0], (String) a[1]);
            case noteOperation:
                return noteOperation((Integer) a[0], (Integer) a[1], (String) a[2], (String) a[3], 0, (Boolean) a[4], (String) a[5], (Boolean) a[6]);
            case noteOperationForDevice:
                return noteOperation((Integer) a[0], (Integer) a[1], (String) a[2], (String) a[3], (Integer) a[4], (Boolean) a[5], (String) a[6], (Boolean) a[7]);
            case noteProxyOperation:
                return noteProxyOperation((Integer) a[0], a[1], (Boolean) a[2], (String) a[3], (Boolean) a[4], (Boolean) a[5]);
            case noteProxyOperationWithState:
                return noteProxyOperation((Integer) a[0], attributionSource(a[1]), (Boolean) a[2], (String) a[3], (Boolean) a[4], (Boolean) a[5]);
            case startOperation:
                return startOperation((IBinder) a[0], (Integer) a[1], (Integer) a[2], (String) a[3], (String) a[4], 0,
                        (Boolean) a[5], (Boolean) a[6], (String) a[7], (Boolean) a[8], (Integer) a[9], (Integer) a[10]);
            case startOperationForDevice:
                return startOperation((IBinder) a[0], (Integer) a[1], (Integer) a[2], (String) a[3], (String) a[4], (Integer) a[5],
                        (Boolean) a[6], (Boolean) a[7], (String) a[8], (Boolean) a[9], (Integer) a[10], (Integer) a[11]);
            case startProxyOperation:
                return startProxyOperation((IBinder) a[0], (Integer) a[1], a[2], (Boolean) a[3], (Boolean) a[4], (String) a[5],
                        (Boolean) a[6], (Boolean) a[7], (Integer) a[8], (Integer) a[9], (Integer) a[10]);
            case startProxyOperationWithState:
                return startProxyOperation((IBinder) a[0], (Integer) a[1], attributionSource(a[2]), (Boolean) a[3], (Boolean) a[4],
                        (String) a[5], (Boolean) a[6], (Boolean) a[7], (Integer) a[8], (Integer) a[9], (Integer) a[10]);
            case finishOperation: finishOperation((IBinder) a[0], (Integer) a[1], (Integer) a[2], (String) a[3], (String) a[4], 0); return null;
            case finishOperationForDevice:
                finishOperation((IBinder) a[0], (Integer) a[1], (Integer) a[2], (String) a[3], (String) a[4], (Integer) a[5]); return null;
            case finishProxyOperation: finishProxyOperation((IBinder) a[0], (Integer) a[1], a[2], (Boolean) a[3]); return null;
            case finishProxyOperationWithState:
                finishProxyOperation((IBinder) a[0], (Integer) a[1], attributionSource(a[2]), (Boolean) a[3]); return null;
            case startWatchingMode: startWatchingMode((Integer) a[0], (String) a[1], 0, a[2]); return null;
            case startWatchingModeWithFlags: startWatchingMode((Integer) a[0], (String) a[1], (Integer) a[2], a[3]); return null;
            case stopWatchingMode: stopWatchingMode(a[0]); return null;
            case startWatchingActive: startWatching(activeWatchers, (int[]) a[0], a[1], false); return null;
            case stopWatchingActive: stopWatching(activeWatchers, a[0], false); return null;
            case startWatchingStarted: startWatching(startedWatchers, (int[]) a[0], a[1], true); return null;
            case stopWatchingStarted: stopWatching(startedWatchers, a[0], true); return null;
            case startWatchingNoted: startWatching(notedWatchers, (int[]) a[0], a[1], true); return null;
            case stopWatchingNoted: stopWatching(notedWatchers, a[0], true); return null;
            case startWatchingAsyncNoted: startWatchingAsyncNoted((String) a[0], a[1]); return null;
            case stopWatchingAsyncNoted: stopWatchingAsyncNoted((String) a[0], a[1]); return null;
            case extractAsyncOps: return extractAsyncOps((String) a[0]);
            case permissionToOpCode: return a[0] == null ? OP_NONE : intOf(PERMISSION_TO_OP_CODE, a[0]);
            case shouldCollectNotes: return shouldCollectNotes((Integer) a[0]);
            case isOperationActive: return isOperationActive((Integer) a[0], (Integer) a[1], (String) a[2]);
            case isProxying: return isProxying((Integer) a[0], (String) a[1], (String) a[2], (Integer) a[3], (String) a[4]);
            case getPackagesForOps: return getPackagesForOps((int[]) a[0]);
            case getPackagesForOpsForDevice: return getPackagesForOps((int[]) a[0]);
            case getOpsForPackage: return getOpsForPackage((Integer) a[0], (String) a[1], (int[]) a[2]);
            case getUidOps: return getUidOps((Integer) a[0], (int[]) a[1]);
            case getHistoricalOps: getHistoricalOps(false, a); return null;
            case getHistoricalOpsFromDiskRaw: getHistoricalOps(true, a); return null;
            case setUidMode: setUidMode((Integer) a[0], (Integer) a[1], (Integer) a[2]); return null;
            case setMode: setMode((Integer) a[0], (Integer) a[1], (String) a[2], (Integer) a[3]); return null;
            case resetAllModes: resetAllModes((Integer) a[0], (String) a[1]); return null;
            case setUserRestrictions: setUserRestrictions((Bundle) a[0], (IBinder) a[1], (Integer) a[2]); return null;
            case setUserRestriction:
                setUserRestriction((Integer) a[0], (Boolean) a[1], (IBinder) a[2], (Integer) a[3], a[4]); return null;
            case removeUser: removeUser((Integer) a[0]); return null;
            case offsetHistory: enforce(MANAGE_APPOPS); return null;
            case setHistoryParameters: enforce(MANAGE_APPOPS); return null;
            case addHistoricalOps: enforce(MANAGE_APPOPS); return null;
            case resetHistoryParameters: enforce(MANAGE_APPOPS); return null;
            case resetPackageOpsNoHistory: resetPackageOpsNoHistory((String) a[0]); return null;
            case clearHistory: enforce(MANAGE_APPOPS); synchronized (lock) { history.clear(); } return null;
            case rebootHistory:
                enforce(MANAGE_APPOPS);
                if ((Long) a[0] < 0) throw new IllegalArgumentException();
                return null;
            case reloadNonHistoricalState: enforce(MANAGE_APPOPS); return null;  // no persisted state to reload
            case collectRuntimeAppOpAccessMessage: return null;  // only system or instrumented callers; neither exists
            case reportRuntimeAppOpAccessMessageAndGetConfig:
                // No package is ever sampled (mSampledPackage stays null), so AOSP's answer is "watch nothing for an hour".
                Objects.requireNonNull(a[0], "packageName");
                return create(SAMPLING_CONFIG, OP_NONE, 0, System.currentTimeMillis() + 3600_000L);
            case collectNoteOpCallsForValidation: collectNoteOpCallsForValidation((String) a[0], (Integer) a[1]); return null;
            default: throw new AssertionError(contract);
        }
    }

    // ---- verification ----

    private int callingUid() { return Binder.getCallingUid(); }

    private boolean holds(String permission) {
        return packages.checkPermission(permission, callingUid(), Context.DEVICE_ID_DEFAULT) == PackageManager.PERMISSION_GRANTED;
    }

    private void enforce(String permission) {
        if (!holds(permission))
            throw new SecurityException("Neither user " + callingUid() + " nor current process has " + permission + ".");
    }

    // AppOpsService.verifyIncomingOp: a valid code, and a restricted-read op needs one of three privileged permissions.
    private void verifyIncomingOp(int op) {
        if (op < 0 || op >= NUM_OP) throw new IllegalArgumentException("Bad operation #" + op);
        if (boolOf(OP_RESTRICTS_READ, op) && !holds(MANAGE_APPOPS) && !holds(GET_APP_OPS_STATS) && !holds(MANAGE_APP_OPS_MODES))
            throw new SecurityException("verifyIncomingOp: uid " + callingUid()
                    + " does not have any of {MANAGE_APPOPS, GET_APP_OPS_STATS, MANAGE_APP_OPS_MODES}");
    }

    private void verifyIncomingUid(int uid) {
        if (uid == callingUid()) return;
        enforce(UPDATE_APP_OPS_STATS);
    }

    private void verifyIncomingProxyUid(Object attributionSource) {
        if (sourceUid(attributionSource) == callingUid() || trustedSource(attributionSource)) return;
        enforce(UPDATE_APP_OPS_STATS);
    }

    private void enforceManageAppOpsModes() { enforce(MANAGE_APP_OPS_MODES); }

    private void checkSystemUid(String function) {
        if (callingUid() != SYSTEM_UID) throw new SecurityException(function + " must by called by the system");
    }

    private boolean validDevice(int deviceId) { return deviceId == Context.DEVICE_ID_DEFAULT; }

    private static int resolveUid(String packageName) {
        if (packageName == null) return INVALID_UID;
        switch (packageName) {
            case "root": return ROOT_UID;
            case "shell": case "dumpstate": return SHELL_UID;
            case "media": return MEDIA_UID;
            case "audioserver": return AUDIOSERVER_UID;
            case "cameraserver": return CAMERASERVER_UID;
            default: return INVALID_UID;
        }
    }

    private static String resolvePackageName(int uid, String packageName) {
        return (String) invoke(RESOLVE_PACKAGE_NAME, null, uid, packageName);
    }

    // AppOpsService.isIncomingPackageValid: no package, a special package or an existing one.
    private boolean incomingPackageValid(String packageName) {
        if (packageName == null) return true;
        if (callingUid() == SYSTEM_UID || resolveUid(resolvePackageName(callingUid(), packageName)) != INVALID_UID) return true;
        return packages.packageUid(packageName) != -1;
    }

    // verifyAndGetBypass: root may name anything; otherwise a fixed system
    // name or the installed package must belong to the uid's app id. This
    // profile has one user, so the package uid and caller uid share it.
    private static boolean belongs(int uid, String packageName, int existing) {
        if (uid == ROOT_UID) return true;
        int appId = uid % 100000;  // UserHandle.getAppId
        int fixed = packageName.equals("com.android.shell") ? SHELL_UID : resolveUid(packageName);
        if (fixed != INVALID_UID) return fixed == appId;
        return existing != -1 && existing == uid;
    }

    private Verified verify(int uid, String packageName, String tag) {
        if (uid == ROOT_UID) return new Verified(true);
        int existing = packages.packageUid(packageName);
        if (!belongs(uid, packageName, existing))
            throw new SecurityException("Specified package \"" + packageName + "\" under uid " + uid + " but it is not");
        if (packageName.equals("com.android.shell") || resolveUid(packageName) != INVALID_UID) return new Verified(true);
        return new Verified(tag == null || validTags(packageName).contains(tag));
    }

    private Set<String> validTags(String packageName) {
        synchronized (declaredTags) {
            Set<String> tags = declaredTags.get(packageName);
            if (tags == null) {
                tags = new HashSet<>();
                android.content.pm.PackageInfo info = packages.processPackage(packageName);
                if (info != null && info.attributions != null)
                    for (android.content.pm.Attribution attribution : info.attributions) tags.add(attribution.getTag());
                declaredTags.put(packageName, tags);
            }
            return tags;
        }
    }

    // AppOpsService.checkPackage: verification, then the special name must
    // resolve to the uid or the package must exist. Failures are MODE_ERRORED,
    // which AppOpsManager turns into SecurityException.
    private int checkPackage(int uid, String packageName) {
        Objects.requireNonNull(packageName);
        try { verify(uid, packageName, null); }
        catch (SecurityException failed) { return MODE_ERRORED; }
        return resolveUid(packageName) == uid || packages.packageUid(packageName) != -1 ? MODE_ALLOWED : MODE_ERRORED;
    }

    // ---- modes ----

    private static long uidKey(int uid, int code) { return ((long) uid << 32) | (code & 0xffffffffL); }
    private static String packageKey(int uid, String packageName, int code) { return uid / 100000 + "/" + packageName + "/" + code; }
    private static String opKey(int uid, String packageName, int code) { return uid + "/" + packageName + "/" + code; }

    // UidState.evalMode with the process always in the foreground.
    private static int eval(int mode) { return mode == MODE_FOREGROUND ? MODE_ALLOWED : mode; }

    private int uidMode(int uid, int switchCode) {
        Integer stored = uidModes.get(uidKey(uid, switchCode));
        return stored == null ? defaultMode(switchCode) : stored;
    }

    private int packageMode(int uid, String packageName, int switchCode) {
        Integer stored = packageModes.get(packageKey(uid, packageName, switchCode));
        return stored == null ? defaultMode(switchCode) : stored;
    }

    // checkOperationUnchecked's mode: a uid override that differs from the default wins, then the package
    // mode of the switch op; with no Op record yet (nothing noted, started or set) the default is returned
    // as stored, unevaluated, so a MODE_FOREGROUND default stays MODE_FOREGROUND until the op exists.
    private int effectiveMode(int uid, String packageName, int code, boolean raw) {
        int switchCode = switchOp(code);
        int rawUid = uidMode(uid, switchCode);
        if (rawUid != defaultMode(switchCode)) return raw ? rawUid : eval(rawUid);
        if (!ops.containsKey(opKey(uid, packageName, switchCode))) return defaultMode(switchCode);
        int mode = packageMode(uid, packageName, switchCode);
        return raw ? mode : eval(mode);
    }

    private int checkOperation(int code, int uid, String packageName, String tag, int deviceId, boolean raw) {
        verifyIncomingOp(code);
        if (!validDevice(deviceId)) return MODE_IGNORED;
        if (!incomingPackageValid(packageName)) return defaultMode(code);
        String resolved = resolvePackageName(uid, packageName);
        if (resolved == null) return MODE_IGNORED;
        try { verify(uid, resolved, null); }
        catch (SecurityException failed) { return defaultMode(code); }
        synchronized (lock) {
            if (restricted(uid, code, resolved, tag, true)) return MODE_IGNORED;
            return effectiveMode(uid, resolved, code, raw);
        }
    }

    private int checkAudioOperation(int code, int usage, int uid, String packageName) {
        synchronized (lock) {
            if (cameraAudioRestriction != 0 && (code == opVibrate() || code == opPlayAudio() && cameraAudioRestriction == 3)
                    && cameraMutedUsages().contains(usage)) return MODE_IGNORED;
            Map<Integer, AudioRestriction> usages = audioRestrictions.get(code);
            AudioRestriction restriction = usages == null ? null : usages.get(usage);
            if (restriction != null && !restriction.exceptionPackages.contains(packageName)) return restriction.mode;
        }
        return checkOperation(code, uid, packageName, null, 0, false);
    }

    private static int opConstant(String name) {
        try { return MANAGER.getField(name).getInt(null); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }
    private static int opVibrate() { return opConstant("OP_VIBRATE"); }
    private static int opPlayAudio() { return opConstant("OP_PLAY_AUDIO"); }

    // AudioRestrictionManager.CAMERA_AUDIO_RESTRICTIONS: usages whose suppression
    // behavior is notification, call or alarm.
    private Set<Integer> cameraMutedUsages() {
        if (cameraMutedUsages == null) {
            try {
                Class<?> attributes = android.media.AudioAttributes.class;
                android.util.SparseIntArray suppressible = (android.util.SparseIntArray) attributes.getField("SUPPRESSIBLE_USAGES").get(null);
                int notification = attributes.getField("SUPPRESSIBLE_NOTIFICATION").getInt(null),
                        call = attributes.getField("SUPPRESSIBLE_CALL").getInt(null),
                        alarm = attributes.getField("SUPPRESSIBLE_ALARM").getInt(null);
                Set<Integer> muted = new HashSet<>();
                for (int i = 0; i < suppressible.size(); i++) {
                    int behavior = suppressible.valueAt(i);
                    if (behavior == notification || behavior == call || behavior == alarm) muted.add(suppressible.keyAt(i));
                }
                cameraMutedUsages = muted;
            } catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
        }
        return cameraMutedUsages;
    }

    private void setAudioRestriction(int code, int usage, int uid, int mode, String[] exceptionPackages) {
        enforceManageAppOpsModes();
        verifyIncomingUid(uid);
        verifyIncomingOp(code);
        synchronized (lock) {
            Map<Integer, AudioRestriction> usages = audioRestrictions.get(code);
            if (usages == null) audioRestrictions.put(code, usages = new HashMap<>());
            usages.remove(usage);
            if (mode != MODE_ALLOWED) {
                AudioRestriction restriction = new AudioRestriction(mode);
                if (exceptionPackages != null)
                    for (String name : exceptionPackages) if (name != null) restriction.exceptionPackages.add(name.trim());
                usages.put(usage, restriction);
            }
        }
        notifyModeChanged(opPlayAudio(), UID_ANY, null);
    }

    private void setCameraAudioRestriction(int mode) {
        enforceManageAppOpsModes();
        synchronized (lock) { cameraAudioRestriction = mode; }
        notifyModeChanged(opPlayAudio(), UID_ANY, null);
        notifyModeChanged(opVibrate(), UID_ANY, null);
    }

    // AppOpsRestrictionsImpl.getUserRestriction, over every client token.
    private boolean restricted(int uid, int code, String packageName, String tag, boolean isCheckOp) {
        int user = uid / 100000;
        for (Map.Entry<IBinder, Map<Integer, Map<Integer, Boolean>>> client : userRestrictions.entrySet()) {
            Map<Integer, Boolean> codes = client.getValue().get(user);
            if (codes == null || !Boolean.TRUE.equals(codes.get(code))) continue;
            Map<Integer, Object> exclusions = userRestrictionExclusions.get(client.getKey());
            Object excluded = exclusions == null ? null : exclusions.get(user);
            if (excluded == null) return true;
            if (!excludes(excluded, packageName, tag, isCheckOp)) return true;
        }
        return false;
    }

    private static boolean excludes(Object list, String packageName, String tag, boolean isCheckOp) {
        try {
            Class<?> type = Class.forName("android.os.PackageTagsList");
            return isCheckOp ? (Boolean) type.getMethod("includes", String.class).invoke(list, packageName)
                    : (Boolean) type.getMethod("contains", String.class, String.class).invoke(list, packageName, tag);
        } catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }

    private void setUserRestrictions(Bundle restrictions, IBinder token, int user) {
        checkSystemUid("setUserRestrictions");
        Objects.requireNonNull(restrictions);
        Objects.requireNonNull(token);
        for (int op = 0; op < NUM_OP; op++) {
            String restriction = (String) invoke(OP_TO_RESTRICTION, null, op);
            if (restriction != null) setUserRestrictionUnchecked(op, restrictions.getBoolean(restriction, false), token, user, null);
        }
    }

    private void setUserRestriction(int code, boolean restricted, IBinder token, int user, Object excluded) {
        enforce(MANAGE_APP_OPS_RESTRICTIONS);
        if (user != callingUid() / 100000 && !holds(INTERACT_ACROSS_USERS_FULL) && !holds(INTERACT_ACROSS_USERS))
            throw new SecurityException("Need INTERACT_ACROSS_USERS_FULL or INTERACT_ACROSS_USERS to interact cross user ");
        verifyIncomingOp(code);
        Objects.requireNonNull(token);
        setUserRestrictionUnchecked(code, restricted, token, user, excluded);
    }

    // AppOpsRestrictionsImpl.setUserRestriction plus the watcher notification when the state changed.
    void setUserRestrictionUnchecked(int code, boolean restricted, IBinder token, int user, Object excluded) {
        boolean changed = false;
        synchronized (lock) {
            Map<Integer, Map<Integer, Boolean>> perUser = userRestrictions.get(token);
            Map<Integer, Boolean> codes = perUser == null ? null : perUser.get(user);
            boolean wasRestricted = codes != null && Boolean.TRUE.equals(codes.get(code));
            if (restricted) {
                if (perUser == null) userRestrictions.put(token, perUser = new HashMap<>());
                if (codes == null) perUser.put(user, codes = new HashMap<>());
                codes.put(code, true);
            } else if (codes != null) {
                codes.remove(code);
                if (codes.isEmpty()) perUser.remove(user);
                if (perUser.isEmpty()) userRestrictions.remove(token);
            }
            changed = wasRestricted != restricted;
            boolean addingExclusions = excluded != null && !(Boolean) invokeVirtual(excluded, "isEmpty");
            Map<Integer, Object> exclusions = userRestrictionExclusions.get(token);
            if (addingExclusions) {
                if (exclusions == null) userRestrictionExclusions.put(token, exclusions = new HashMap<>());
                exclusions.put(user, excluded);
                changed = true;
            } else if (exclusions != null) {
                changed |= exclusions.remove(user) != null;
                if (exclusions.isEmpty()) userRestrictionExclusions.remove(token);
            }
        }
        if (changed) notifyModeChanged(code, UID_ANY, null);
    }

    private static Object invokeVirtual(Object target, String name) {
        try { return target.getClass().getMethod(name).invoke(target); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }

    private void removeUser(int user) {
        checkSystemUid("removeUser");
        synchronized (lock) {
            for (Iterator<Map.Entry<IBinder, Map<Integer, Map<Integer, Boolean>>>> it = userRestrictions.entrySet().iterator(); it.hasNext();) {
                Map.Entry<IBinder, Map<Integer, Map<Integer, Boolean>>> entry = it.next();
                entry.getValue().remove(user);
                if (entry.getValue().isEmpty()) it.remove();
            }
            for (Map<Integer, Object> exclusions : userRestrictionExclusions.values()) exclusions.remove(user);
            ops.values().removeIf(op -> op.uid / 100000 == user);
            uidModes.keySet().removeIf(key -> (int) (key >> 32) / 100000 == user);
            packageModes.keySet().removeIf(key -> key.startsWith(user + "/"));
        }
    }

    // AppOpsService.setUidMode: privileged; the Binder entry checks MANAGE_APP_OPS_MODES.
    private void setUidMode(int code, int uid, int mode) {
        enforceManageAppOpsModes();
        verifyIncomingOp(code);
        setUidModeUnchecked(code, uid, mode);
    }

    // The permission-policy path: AppOpsService.setUidMode after its checks.
    void setUidModeUnchecked(int code, int uid, int mode) {
        int switchCode = switchOp(code);
        synchronized (lock) {
            int defaultMode = defaultMode(switchCode);
            Integer previous = uidModes.get(uidKey(uid, switchCode));
            if (previous == null && mode == defaultMode) return;
            if (previous != null && previous == mode) return;
            if (mode == defaultMode) uidModes.remove(uidKey(uid, switchCode));
            else uidModes.put(uidKey(uid, switchCode), mode);
        }
        notifyModeChanged(switchCode, uid, packages.nameForUid(uid));
    }

    private void setMode(int code, int uid, String packageName, int mode) {
        enforceManageAppOpsModes();
        verifyIncomingOp(code);
        if (!incomingPackageValid(packageName)) return;
        setModeUnchecked(code, uid, packageName, mode);
    }

    // AppOpsService.setMode after its checks: a failed verification leaves the state unchanged.
    void setModeUnchecked(int code, int uid, String packageName, int mode) {
        int switchCode = switchOp(code);
        try { verify(uid, packageName, null); }
        catch (SecurityException failed) { return; }
        synchronized (lock) {
            ops.computeIfAbsent(opKey(uid, packageName, switchCode), key -> new Op(switchCode, uid, packageName));
            if (packageMode(uid, packageName, switchCode) == mode) return;
            if (mode == defaultMode(switchCode)) packageModes.remove(packageKey(uid, packageName, switchCode));
            else packageModes.put(packageKey(uid, packageName, switchCode), mode);
        }
        notifyModeChanged(switchCode, uid, packageName);
    }

    private void resetAllModes(int requestedUser, String requestedPackage) {
        int user = requestedUser == USER_CURRENT || requestedUser == USER_CURRENT_OR_SELF ? callingUid() / 100000 : requestedUser;
        if (user != callingUid() / 100000 && !holds(INTERACT_ACROSS_USERS_FULL))
            throw new SecurityException("Neither user " + callingUid() + " nor current process has "
                    + INTERACT_ACROSS_USERS_FULL + ".");
        int requestedUid = requestedPackage == null ? -1 : packages.packageUid(requestedPackage);
        enforceManageAppOpsModes();
        List<int[]> changes = new ArrayList<>();
        List<String> changedPackages = new ArrayList<>();
        synchronized (lock) {
            for (Iterator<Map.Entry<Long, Integer>> it = uidModes.entrySet().iterator(); it.hasNext();) {
                Map.Entry<Long, Integer> entry = it.next();
                int uid = (int) (entry.getKey() >> 32), code = (int) (long) entry.getKey();
                if ((requestedUid != -1 && uid != requestedUid) || !boolOf(OP_ALLOWS_RESET, code)) continue;
                it.remove();
                changes.add(new int[] {code, uid});
                changedPackages.add(packages.nameForUid(uid));
            }
            for (Iterator<Map.Entry<String, Integer>> it = packageModes.entrySet().iterator(); it.hasNext();) {
                Map.Entry<String, Integer> entry = it.next();
                String[] parts = entry.getKey().split("/");
                int code = Integer.parseInt(parts[2]);
                String packageName = parts[1];
                if (user != USER_ALL && user != Integer.parseInt(parts[0])) continue;
                if (requestedPackage != null && !requestedPackage.equals(packageName)) continue;
                if (!boolOf(OP_ALLOWS_RESET, code)) continue;
                it.remove();
                int uid = packages.packageUid(packageName);
                changes.add(new int[] {code, uid});
                changedPackages.add(packageName);
            }
            ops.values().removeIf(op -> !op.running() && op.attributions.isEmpty()
                    && (requestedPackage == null || requestedPackage.equals(op.packageName)));
        }
        for (int i = 0; i < changes.size(); i++) notifyModeChanged(changes.get(i)[0], changes.get(i)[1], changedPackages.get(i));
    }

    private void resetPackageOpsNoHistory(String packageName) {
        enforce(MANAGE_APPOPS);
        Objects.requireNonNull(packageName);
        int uid = packages.packageUid(packageName);
        if (uid == -1) return;
        synchronized (lock) {
            ops.values().removeIf(op -> op.uid == uid && op.packageName.equals(packageName));
            packageModes.keySet().removeIf(key -> key.startsWith(uid / 100000 + "/" + packageName + "/"));
        }
    }

    // ---- mode watchers ----

    private void startWatchingMode(int op, String packageName, int flags, Object callback) {
        if (op < OP_NONE || op > NUM_OP - 1) throw new IllegalArgumentException("Invalid op code: " + op);
        if (callback == null) return;
        IBinder binder = ((IInterface) callback).asBinder();
        synchronized (lock) {
            int switchCode = op != OP_NONE ? switchOp(op) : op;
            int notified = (flags & CALL_BACK_ON_SWITCHED_OP) == 0 ? (op == OP_NONE ? ALL_OPS : op) : switchCode;
            ModeWatcher watcher = modeWatchers.get(binder);
            if (watcher == null) modeWatchers.put(binder, watcher = new ModeWatcher(callback, flags, notified, callingUid()));
            if (switchCode != OP_NONE) watcher.switchOps.add(switchCode);
            if (packageName != null) watcher.watchedPackages.add(packageName);
        }
    }

    private void stopWatchingMode(Object callback) {
        if (callback == null) return;
        synchronized (lock) { modeWatchers.remove(((IInterface) callback).asBinder()); }
    }

    // AppOpsService.notifyOpChanged: every watcher of the switch op or the package, once per reported op.
    private void notifyModeChanged(int switchCode, int uid, String packageName) {
        List<Object[]> deliveries = new ArrayList<>();
        synchronized (lock) {
            for (ModeWatcher watcher : modeWatchers.values()) {
                boolean byOp = watcher.switchOps.contains(switchCode);
                boolean byPackage = packageName != null && watcher.watchedPackages.contains(packageName);
                if (!byOp && !byPackage) continue;
                int[] reported;
                if (watcher.notifiedOp == ALL_OPS) reported = opsSwitchingTo(switchCode);
                else if (watcher.notifiedOp == OP_NONE) reported = new int[] {switchCode};
                else reported = new int[] {watcher.notifiedOp};
                for (int op : reported)
                    if (!ignoreCallback(op, watcher.callingUid)) deliveries.add(new Object[] {watcher.callback, op});
            }
        }
        for (Object[] delivery : deliveries)
            dispatch(delivery[0], "com.android.internal.app.IAppOpsCallback", "opChanged",
                    delivery[1], uid, packageName, PERSISTENT_DEVICE_ID_DEFAULT);
    }

    private static int[] opsSwitchingTo(int switchCode) {
        List<Integer> codes = new ArrayList<>();
        for (int op = 0; op < NUM_OP; op++) if (switchOp(op) == switchCode) codes.add(op);
        int[] result = new int[codes.size()];
        for (int i = 0; i < result.length; i++) result[i] = codes.get(i);
        return result;
    }

    // shouldIgnoreCallback: a restricted-read op's change is not revealed to a watcher without MANAGE_APPOPS.
    private boolean ignoreCallback(int op, int watcherUid) {
        return boolOf(OP_RESTRICTS_READ, op) && packages.checkPermission(MANAGE_APPOPS, watcherUid, Context.DEVICE_ID_DEFAULT)
                != PackageManager.PERMISSION_GRANTED;
    }

    // ---- active / started / noted watchers ----

    private void startWatching(Map<IBinder, Map<Integer, Watcher>> registry, int[] opCodes, Object callback, boolean strict) {
        int watchedUid = holds(WATCH_APPOPS) ? INVALID_UID : callingUid();
        if (strict) {
            if (opCodes == null || opCodes.length == 0) throw new IllegalArgumentException("Ops cannot be null or empty");
            checkRange(opCodes);
            Objects.requireNonNull(callback, "Callback cannot be null");
        } else {
            if (opCodes != null) checkRange(opCodes);
            if (callback == null) return;
            if (opCodes == null) throw new NullPointerException("ops");
        }
        IBinder binder = ((IInterface) callback).asBinder();
        synchronized (lock) {
            Map<Integer, Watcher> byOp = registry.get(binder);
            if (byOp == null) registry.put(binder, byOp = new HashMap<>());
            Watcher watcher = new Watcher(callback, watchedUid, callingUid());
            for (int op : opCodes) byOp.put(op, watcher);
        }
    }

    private static void checkRange(int[] opCodes) {
        for (int op : opCodes)
            if (op < 0 || op >= NUM_OP) throw new IllegalArgumentException("Invalid op code in: " + Arrays.toString(opCodes));
    }

    private void stopWatching(Map<IBinder, Map<Integer, Watcher>> registry, Object callback, boolean strict) {
        if (callback == null) {
            if (strict) throw new NullPointerException("Callback cannot be null");
            return;
        }
        synchronized (lock) { registry.remove(((IInterface) callback).asBinder()); }
    }

    private List<Object> watchersOf(Map<IBinder, Map<Integer, Watcher>> registry, int code, int uid) {
        List<Object> callbacks = new ArrayList<>();
        synchronized (lock) {
            for (Map<Integer, Watcher> byOp : registry.values()) {
                Watcher watcher = byOp.get(code);
                if (watcher == null || watcher.watchingUid >= 0 && watcher.watchingUid != uid) continue;
                if (!ignoreCallback(code, watcher.callingUid)) callbacks.add(watcher.callback);
            }
        }
        return callbacks;
    }

    private void notifyActive(int code, int uid, String packageName, String tag, int deviceId, boolean active,
            int attributionFlags, int chainId) {
        for (Object callback : watchersOf(activeWatchers, code, uid))
            dispatch(callback, "com.android.internal.app.IAppOpsActiveCallback", "opActiveChanged",
                    code, uid, packageName, tag, deviceId, active, attributionFlags, chainId);
    }

    private void notifyStarted(int code, int uid, String packageName, String tag, int deviceId, int flags, int mode,
            int startType, int attributionFlags, int chainId) {
        for (Object callback : watchersOf(startedWatchers, code, uid))
            dispatch(callback, "com.android.internal.app.IAppOpsStartedCallback", "opStarted",
                    code, uid, packageName, tag, deviceId, flags, mode, startType, attributionFlags, chainId);
    }

    private void notifyNoted(int code, int uid, String packageName, String tag, int deviceId, int flags, int mode) {
        for (Object callback : watchersOf(notedWatchers, code, uid))
            dispatch(callback, "com.android.internal.app.IAppOpsNotedCallback", "opNoted",
                    code, uid, packageName, tag, deviceId, flags, mode);
    }

    // Callbacks are oneway in AOSP: delivered in order from a service thread, never under the lock.
    private void dispatch(Object callback, String iface, String method, Object... args) {
        synchronized (this) {
            if (notifier == null) notifier = Executors.newSingleThreadExecutor(task -> {
                Thread thread = new Thread(task, "appops-callbacks");
                thread.setDaemon(true);
                return thread;
            });
        }
        notifier.execute(() -> {
            try {
                for (Method candidate : Class.forName(iface).getMethods())
                    if (candidate.getName().equals(method) && candidate.getParameterTypes().length == args.length) {
                        candidate.invoke(callback, args);
                        return;
                    }
                throw new IllegalStateException(iface + "." + method + "/" + args.length + " is absent");
            } catch (ReflectiveOperationException error) {
                System.out.println("APPOPS_CALLBACK_FAILED " + method + " " + error);  // a dead or throwing client is dropped, as for RemoteException
            }
        });
    }

    // ---- async noted watchers ----

    private static String asyncKey(String packageName, int uid) { return packageName + "/" + uid; }

    private void startWatchingAsyncNoted(String packageName, Object callback) {
        Objects.requireNonNull(packageName);
        Objects.requireNonNull(callback);
        int uid = callingUid();
        verify(uid, packageName, null);
        synchronized (lock) {
            List<Object> callbacks = asyncWatchers.get(asyncKey(packageName, uid));
            if (callbacks == null) asyncWatchers.put(asyncKey(packageName, uid), callbacks = new ArrayList<>());
            IBinder binder = ((IInterface) callback).asBinder();
            for (Object existing : callbacks) if (((IInterface) existing).asBinder() == binder) return;
            callbacks.add(callback);
        }
    }

    private void stopWatchingAsyncNoted(String packageName, Object callback) {
        Objects.requireNonNull(packageName);
        Objects.requireNonNull(callback);
        int uid = callingUid();
        verify(uid, packageName, null);
        synchronized (lock) {
            List<Object> callbacks = asyncWatchers.get(asyncKey(packageName, uid));
            if (callbacks == null) return;
            IBinder binder = ((IInterface) callback).asBinder();
            callbacks.removeIf(existing -> ((IInterface) existing).asBinder() == binder);
            if (callbacks.isEmpty()) asyncWatchers.remove(asyncKey(packageName, uid));
        }
    }

    private Object extractAsyncOps(String packageName) {
        Objects.requireNonNull(packageName);
        int uid = callingUid();
        verify(uid, packageName, null);
        synchronized (lock) { return unforwardedAsync.remove(asyncKey(packageName, uid)); }
    }

    private void collectAsyncNotedOp(int uid, String packageName, int code, String tag, String message) {
        Objects.requireNonNull(message);
        Object noted = create(ASYNC_NOTED, code, callingUid(), tag, message, System.currentTimeMillis());
        List<Object> callbacks;
        synchronized (lock) {
            callbacks = new ArrayList<>(asyncWatchers.getOrDefault(asyncKey(packageName, uid), new ArrayList<>()));
            if (callbacks.isEmpty()) {
                List<Object> pending = unforwardedAsync.get(asyncKey(packageName, uid));
                if (pending == null) unforwardedAsync.put(asyncKey(packageName, uid), pending = new ArrayList<>(1));
                pending.add(noted);
                if (pending.size() > MAX_UNFORWARDED_OPS) pending.remove(0);
            }
        }
        for (Object callback : callbacks)
            dispatch(callback, "com.android.internal.app.IAppOpsAsyncNotedCallback", "opNoted", noted);
    }

    // ---- note / start / finish ----

    private Object synced(int mode, int code, String tag, String packageName) {
        return create(SYNC_NOTED, mode, code, tag, packageName);
    }

    private Op opLocked(int uid, String packageName, int code) {
        return ops.computeIfAbsent(opKey(uid, packageName, code), key -> new Op(code, uid, packageName));
    }

    // Mode a note or start sees: a non-default uid override wins, else the package mode of the switch op.
    private int operationMode(int uid, String packageName, int code) {
        int switchCode = switchOp(code);
        int rawUid = uidMode(uid, switchCode);
        if (rawUid != defaultMode(switchCode)) return eval(rawUid);
        return eval(packageMode(uid, packageName, switchCode));
    }

    private void record(int code, int uid, String packageName, String tag, int flags, boolean rejected, long duration) {
        history.add(new HistoryEvent(System.currentTimeMillis(), duration, code, uid, packageName, tag, UID_STATE_TOP, flags, rejected));
        if (history.size() > MAX_HISTORY_EVENTS) history.remove(0);
    }

    private static long key(int flags) { return (Long) invoke(MAKE_KEY, null, UID_STATE_TOP, flags); }

    private Object noteOperation(int code, int uid, String packageName, String tag, int deviceId, boolean collectAsync,
            String message, boolean collectMessage) {
        verifyIncomingUid(uid);
        verifyIncomingOp(code);
        if (!validDevice(deviceId)) return synced(MODE_IGNORED, code, tag, packageName);
        if (!incomingPackageValid(packageName)) return synced(MODE_ERRORED, code, tag, packageName);
        String resolved = resolvePackageName(uid, packageName);
        if (resolved == null) return synced(MODE_IGNORED, code, tag, packageName);
        return noteUnchecked(code, uid, resolved, tag, OP_FLAG_SELF, INVALID_UID, null, null, collectAsync, message);
    }

    private Object noteUnchecked(int code, int uid, String packageName, String tag, int flags, int proxyUid,
            String proxyPackage, String proxyTag, boolean collectAsync, String message) {
        Verified verified;
        try { verified = verify(uid, packageName, tag); }
        catch (SecurityException failed) { return synced(MODE_ERRORED, code, tag, packageName); }
        if (!verified.validTag) tag = null;
        int mode;
        synchronized (lock) {
            Op op = opLocked(uid, packageName, code);
            opLocked(uid, packageName, switchOp(code));
            Attribution attribution = op.attributions.computeIfAbsent(tag, Attribution::new);
            if (restricted(uid, code, packageName, tag, false)) mode = MODE_IGNORED;
            else mode = operationMode(uid, packageName, code);
            if (mode != MODE_ALLOWED) {
                attribution.rejectEvents.put(key(flags), new long[] {System.currentTimeMillis(), -1});
                record(code, uid, packageName, tag, flags, true, -1);
            } else {
                long now = System.currentTimeMillis();
                attribution.accessEvents.put(key(flags), new long[] {now, -1});
                attribution.accessProxies.put(key(flags), proxyUid == INVALID_UID ? null : new Object[] {proxyUid, proxyPackage, proxyTag});
                record(code, uid, packageName, tag, flags, false, -1);
            }
        }
        notifyNoted(code, uid, packageName, tag, 0, flags, mode);
        if (mode == MODE_ALLOWED && collectAsync) collectAsyncNotedOp(uid, packageName, code, tag, message);
        return synced(mode, code, tag, packageName);
    }

    private Object startOperation(IBinder clientId, int code, int uid, String packageName, String tag, int deviceId,
            boolean startIfModeDefault, boolean collectAsync, String message, boolean collectMessage,
            int attributionFlags, int chainId) {
        verifyIncomingUid(uid);
        verifyIncomingOp(code);
        if (!validDevice(deviceId)) return synced(MODE_IGNORED, code, tag, packageName);
        if (!incomingPackageValid(packageName)) return synced(MODE_ERRORED, code, tag, packageName);
        String resolved = resolvePackageName(uid, packageName);
        if (resolved == null) return synced(MODE_IGNORED, code, tag, packageName);
        // The sandboxed variants only attribute: their data is also gated by the parent op.
        int parent = parentOp(code);
        if (parent != OP_NONE) {
            int result = checkOperation(parent, uid, packageName, null, 0, false);
            if (result != MODE_ALLOWED) return synced(result, code, tag, packageName);
        }
        return startUnchecked(clientId, code, uid, resolved, tag, deviceId, INVALID_UID, null, null, OP_FLAG_SELF,
                startIfModeDefault, collectAsync, message, attributionFlags, chainId);
    }

    private static int parentOp(int code) {
        for (String sandboxed : new String[] {"OP_RECORD_AUDIO_HOTWORD", "OP_RECEIVE_AMBIENT_TRIGGER_AUDIO", "OP_RECORD_AUDIO_SANDBOXED"})
            if (code == opConstant(sandboxed)) return opConstant("OP_RECORD_AUDIO");
        return code == opConstant("OP_CAMERA_SANDBOXED") ? opConstant("OP_CAMERA") : OP_NONE;
    }

    private static boolean shouldStartForMode(int mode, boolean startIfModeDefault) {
        return mode == MODE_ALLOWED || mode == MODE_DEFAULT && startIfModeDefault;
    }

    private Object startUnchecked(IBinder clientId, int code, int uid, String packageName, String tag, int deviceId,
            int proxyUid, String proxyPackage, String proxyTag, int flags, boolean startIfModeDefault,
            boolean collectAsync, String message, int attributionFlags, int chainId) {
        Verified verified;
        try { verified = verify(uid, packageName, tag); }
        catch (SecurityException failed) { return synced(MODE_ERRORED, code, tag, packageName); }
        if (!verified.validTag) tag = null;
        Objects.requireNonNull(clientId, "clientId");
        int mode;
        boolean restricted, becameActive = false;
        synchronized (lock) {
            Op op = opLocked(uid, packageName, code);
            opLocked(uid, packageName, switchOp(code));
            Attribution attribution = op.attributions.computeIfAbsent(tag, Attribution::new);
            restricted = restricted(uid, code, packageName, tag, false);
            mode = operationMode(uid, packageName, code);
            if (!shouldStartForMode(mode, startIfModeDefault)) {
                attribution.rejectEvents.put(key(flags), new long[] {System.currentTimeMillis(), -1});
                record(code, uid, packageName, tag, flags, true, -1);
            } else {
                // A restricted start is paused (AttributedOp.createPaused): tracked, but not active.
                Map<IBinder, InProgress> table = restricted ? attribution.paused : attribution.running;
                becameActive = !restricted && !op.running();
                InProgress progress = table.get(clientId);
                if (progress == null) {
                    progress = new InProgress(flags, UID_STATE_TOP, attributionFlags, chainId, deviceId);
                    table.put(clientId, progress);
                    linkClient(clientId, progress, op, attribution);
                }
                progress.startCount++;
                attribution.accessProxies.put(key(flags), proxyUid == INVALID_UID ? null : new Object[] {proxyUid, proxyPackage, proxyTag});
                if (!restricted) record(code, uid, packageName, tag, flags, false, -1);
            }
        }
        boolean started = shouldStartForMode(mode, startIfModeDefault);
        int startType = started && !restricted ? START_TYPE_STARTED : START_TYPE_FAILED;
        int reported = !started ? mode : restricted ? MODE_IGNORED : MODE_ALLOWED;
        notifyStarted(code, uid, packageName, tag, deviceId, flags, reported, startType, attributionFlags, chainId);
        if (!started) return synced(mode, code, tag, packageName);
        if (becameActive) notifyActive(code, uid, packageName, tag, deviceId, true, attributionFlags, chainId);
        if (collectAsync && !restricted) collectAsyncNotedOp(uid, packageName, code, tag, message);
        return synced(restricted ? MODE_IGNORED : MODE_ALLOWED, code, tag, packageName);
    }

    // A client that dies without finishing has its start released (AttributedOp.onClientDeath).
    private void linkClient(IBinder clientId, InProgress progress, Op op, Attribution attribution) {
        IBinder.DeathRecipient recipient = () -> {
            synchronized (lock) {
                InProgress dead = attribution.running.containsKey(clientId) ? attribution.running.get(clientId)
                        : attribution.paused.get(clientId);
                if (dead == null) return;
                dead.startCount = 1;
            }
            finishAttribution(clientId, op, attribution);
        };
        progress.recipient = recipient;
        try { clientId.linkToDeath(recipient, 0); }
        catch (android.os.RemoteException error) { throw new RuntimeException(error); }
    }

    private void finishOperation(IBinder clientId, int code, int uid, String packageName, String tag, int deviceId) {
        verifyIncomingUid(uid);
        verifyIncomingOp(code);
        if (!validDevice(deviceId) || !incomingPackageValid(packageName)) return;
        String resolved = resolvePackageName(uid, packageName);
        if (resolved == null) return;
        finishUnchecked(clientId, code, uid, resolved, tag);
    }

    private void finishUnchecked(IBinder clientId, int code, int uid, String packageName, String tag) {
        Verified verified;
        try { verified = verify(uid, packageName, tag); }
        catch (SecurityException failed) { return; }
        if (!verified.validTag) tag = null;
        Op op;
        Attribution attribution;
        synchronized (lock) {
            op = ops.get(opKey(uid, packageName, code));
            attribution = op == null ? null : op.attributions.get(tag);
            if (attribution == null || !attribution.running.containsKey(clientId) && !attribution.paused.containsKey(clientId)) {
                System.out.println("APPOPS_NOT_STARTED uid=" + uid + " pkg=" + packageName + " op=" + code);  // AOSP logs "Operation not started"
                return;
            }
        }
        finishAttribution(clientId, op, attribution);
    }

    // AttributedOp.finishOrPause: the last finish of a client records the duration; the last client turns the op inactive.
    private void finishAttribution(IBinder clientId, Op op, Attribution attribution) {
        boolean inactive = false;
        InProgress progress;
        synchronized (lock) {
            boolean wasPaused = !attribution.running.containsKey(clientId);
            progress = wasPaused ? attribution.paused.get(clientId) : attribution.running.get(clientId);
            if (progress == null) return;
            if (--progress.startCount > 0) return;
            (wasPaused ? attribution.paused : attribution.running).remove(clientId);
            if (wasPaused) {
                if (progress.recipient != null) clientId.unlinkToDeath(progress.recipient, 0);
                return;
            }
            long duration = android.os.SystemClock.elapsedRealtime() - progress.startElapsed;
            attribution.accessEvents.put(key(progress.flags), new long[] {progress.startTime, duration});
            record(op.code, op.uid, op.packageName, attribution.tag, progress.flags, false, duration);
            inactive = !op.running();
        }
        if (progress.recipient != null) clientId.unlinkToDeath(progress.recipient, 0);
        if (inactive) notifyActive(op.code, op.uid, op.packageName, attribution.tag, progress.deviceId, false,
                progress.attributionFlags, progress.chainId);
    }

    // ---- proxy operations ----

    private Object attributionSource(Object state) {
        return create(ATTRIBUTION_FROM_STATE, Objects.requireNonNull(state, "attributionSourceState"));
    }

    private static Object source(Object attributionSource, String getter) {
        try { return attributionSource.getClass().getMethod(getter).invoke(attributionSource); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }
    private static int sourceUid(Object s) { return (Integer) source(s, "getUid"); }
    private static String sourceString(Object s, String getter) { return (String) source(s, getter); }

    // AttributionSource.isTrusted(Context), which needs the system's token registry; anything else is untrusted.
    private boolean trustedSource(Object attributionSource) {
        try {
            Object application = Class.forName("android.app.ActivityThread").getMethod("currentApplication").invoke(null);
            return application != null && (Boolean) attributionSource.getClass().getMethod("isTrusted", Context.class)
                    .invoke(attributionSource, application);
        } catch (ReflectiveOperationException | RuntimeException error) { return false; }
    }

    private boolean callerAndAttributionTrusted(Object attributionSource) {
        if (sourceUid(attributionSource) != callingUid() && trustedSource(attributionSource)) return true;
        return holds(UPDATE_APP_OPS_STATS);
    }

    private boolean proxyTrusted(int proxyUid, int proxiedUid) {
        return packages.checkPermission(UPDATE_APP_OPS_STATS, proxyUid, Context.DEVICE_ID_DEFAULT) == PackageManager.PERMISSION_GRANTED
                || callingUid() == proxiedUid;
    }

    private Object noteProxyOperation(int code, Object as, boolean collectAsync, String message, boolean collectMessage,
            boolean skipProxy) {
        Objects.requireNonNull(as, "attributionSource");
        int proxyUid = sourceUid(as), proxiedUid = (Integer) source(as, "getNextUid");
        String proxyPackage = sourceString(as, "getPackageName"), proxyTag = sourceString(as, "getAttributionTag"),
                proxiedPackage = sourceString(as, "getNextPackageName"), proxiedTag = sourceString(as, "getNextAttributionTag");
        verifyIncomingProxyUid(as);
        verifyIncomingOp(code);
        if (!validDevice((Integer) source(as, "getDeviceId")) || !validDevice((Integer) source(as, "getNextDeviceId")))
            return synced(MODE_IGNORED, code, proxiedTag, proxiedPackage);
        if (!incomingPackageValid(proxiedPackage) || !incomingPackageValid(proxyPackage))
            return synced(MODE_ERRORED, code, proxiedTag, proxiedPackage);
        skipProxy = skipProxy && callerAndAttributionTrusted(as);
        String resolvedProxy = resolvePackageName(proxyUid, proxyPackage);
        if (resolvedProxy == null) return synced(MODE_IGNORED, code, proxiedTag, proxiedPackage);
        boolean trusted = proxyTrusted(proxyUid, proxiedUid);
        if (!skipProxy) {
            Object proxied = noteUnchecked(code, proxyUid, resolvedProxy, proxyTag,
                    trusted ? OP_FLAG_TRUSTED_PROXY : OP_FLAG_UNTRUSTED_PROXY, INVALID_UID, null, null, !trusted,
                    "proxy " + message);
            int proxyMode = syncedMode(proxied);
            if (proxyMode != MODE_ALLOWED) return synced(proxyMode, code, proxiedTag, proxiedPackage);
        }
        String resolvedProxied = resolvePackageName(proxiedUid, proxiedPackage);
        if (resolvedProxied == null) return synced(MODE_IGNORED, code, proxiedTag, proxiedPackage);
        return noteUnchecked(code, proxiedUid, resolvedProxied, proxiedTag,
                trusted ? OP_FLAG_TRUSTED_PROXIED : OP_FLAG_UNTRUSTED_PROXIED, proxyUid, resolvedProxy, proxyTag,
                collectAsync, message);
    }

    private static int syncedMode(Object syncedOp) { return (Integer) source(syncedOp, "getOpMode"); }

    private Object startProxyOperation(IBinder clientId, int code, Object as, boolean startIfModeDefault, boolean collectAsync,
            String message, boolean collectMessage, boolean skipProxy, int proxyAttributionFlags,
            int proxiedAttributionFlags, int chainId) {
        Objects.requireNonNull(as, "attributionSource");
        int proxyUid = sourceUid(as), proxiedUid = (Integer) source(as, "getNextUid");
        String proxyPackage = sourceString(as, "getPackageName"), proxyTag = sourceString(as, "getAttributionTag"),
                proxiedPackage = sourceString(as, "getNextPackageName"), proxiedTag = sourceString(as, "getNextAttributionTag");
        verifyIncomingProxyUid(as);
        verifyIncomingOp(code);
        if (!validDevice((Integer) source(as, "getDeviceId")) || !validDevice((Integer) source(as, "getNextDeviceId")))
            return synced(MODE_IGNORED, code, proxiedTag, proxiedPackage);
        if (!incomingPackageValid(proxyPackage) || !incomingPackageValid(proxiedPackage))
            return synced(MODE_ERRORED, code, proxiedTag, proxiedPackage);
        boolean callerTrusted = callerAndAttributionTrusted(as);
        skipProxy = callerTrusted && skipProxy;
        String resolvedProxy = resolvePackageName(proxyUid, proxyPackage);
        if (resolvedProxy == null) return synced(MODE_IGNORED, code, proxiedTag, proxiedPackage);
        boolean chainTrusted = callerTrusted && chainId != ATTRIBUTION_CHAIN_ID_NONE
                && ((proxyAttributionFlags & ATTRIBUTION_FLAG_TRUSTED) != 0 || (proxiedAttributionFlags & ATTRIBUTION_FLAG_TRUSTED) != 0);
        boolean trusted = proxyTrusted(proxyUid, proxiedUid) || chainTrusted;
        String resolvedProxied = resolvePackageName(proxiedUid, proxiedPackage);
        if (resolvedProxied == null) return synced(MODE_IGNORED, code, proxiedTag, proxiedPackage);
        int proxiedFlags = trusted ? OP_FLAG_TRUSTED_PROXIED : OP_FLAG_UNTRUSTED_PROXIED;
        if (!skipProxy) {
            // The proxied start must be able to succeed before the proxy is started.
            Object dryRun = startDryRun(code, proxiedUid, resolvedProxied, proxiedTag, startIfModeDefault);
            if (!shouldStartForMode(syncedMode(dryRun), startIfModeDefault)) return dryRun;
            Object started = startUnchecked(clientId, code, proxyUid, resolvedProxy, proxyTag, 0, INVALID_UID, null, null,
                    trusted ? OP_FLAG_TRUSTED_PROXY : OP_FLAG_UNTRUSTED_PROXY, startIfModeDefault, !trusted,
                    "proxy " + message, proxyAttributionFlags, chainId);
            if (!shouldStartForMode(syncedMode(started), startIfModeDefault)) return started;
        }
        return startUnchecked(clientId, code, proxiedUid, resolvedProxied, proxiedTag, 0, proxyUid, resolvedProxy, proxyTag,
                proxiedFlags, startIfModeDefault, collectAsync, message, proxiedAttributionFlags, chainId);
    }

    // AppOpsService.startOperationDryRun: the result of a start, without changing any op state.
    private Object startDryRun(int code, int uid, String packageName, String tag, boolean startIfModeDefault) {
        try { verify(uid, packageName, tag); }
        catch (SecurityException failed) { return synced(MODE_ERRORED, code, tag, packageName); }
        synchronized (lock) {
            boolean restricted = restricted(uid, code, packageName, tag, false);
            int mode = operationMode(uid, packageName, code);
            if (!shouldStartForMode(mode, startIfModeDefault)) return synced(mode, code, tag, packageName);
            return synced(restricted ? MODE_IGNORED : MODE_ALLOWED, code, tag, packageName);
        }
    }

    private void finishProxyOperation(IBinder clientId, int code, Object as, boolean skipProxy) {
        Objects.requireNonNull(as, "attributionSource");
        int proxyUid = sourceUid(as), proxiedUid = (Integer) source(as, "getNextUid");
        String proxyPackage = sourceString(as, "getPackageName"), proxyTag = sourceString(as, "getAttributionTag"),
                proxiedPackage = sourceString(as, "getNextPackageName"), proxiedTag = sourceString(as, "getNextAttributionTag");
        skipProxy = skipProxy && callerAndAttributionTrusted(as);
        verifyIncomingProxyUid(as);
        verifyIncomingOp(code);
        if (!validDevice((Integer) source(as, "getDeviceId"))) return;
        if (!incomingPackageValid(proxyPackage) || !incomingPackageValid(proxiedPackage)) return;
        String resolvedProxy = resolvePackageName(proxyUid, proxyPackage);
        if (resolvedProxy == null) return;
        if (!skipProxy) finishUnchecked(clientId, code, proxyUid, resolvedProxy, proxyTag);
        String resolvedProxied = resolvePackageName(proxiedUid, proxiedPackage);
        if (resolvedProxied == null) return;
        finishUnchecked(clientId, code, proxiedUid, resolvedProxied, proxiedTag);
    }

    // ---- queries ----

    private boolean isOperationActive(int code, int uid, String packageName) {
        if (callingUid() != uid && !holds(WATCH_APPOPS)) return false;
        verifyIncomingOp(code);
        if (!incomingPackageValid(packageName)) return false;
        String resolved = resolvePackageName(uid, packageName);
        if (resolved == null) return false;
        synchronized (lock) {
            Op op = ops.get(opKey(uid, resolved, code));
            return op != null && op.running();
        }
    }

    private boolean isProxying(int op, String proxyPackage, String proxyTag, int proxiedUid, String proxiedPackage) {
        Objects.requireNonNull(proxyPackage);
        Objects.requireNonNull(proxiedPackage);
        int caller = callingUid();
        synchronized (lock) {
            Op entry = ops.get(opKey(proxiedUid, proxiedPackage, op));
            if (entry == null) return false;
            for (Attribution attribution : entry.attributions.values()) {
                if (attribution.running.isEmpty()) continue;
                for (InProgress progress : attribution.running.values()) {
                    if ((progress.flags & (OP_FLAG_TRUSTED_PROXIED | OP_FLAG_UNTRUSTED_PROXIED)) == 0) continue;
                    Object[] proxy = attribution.accessProxies.get(key(progress.flags));
                    if (proxy != null && (Integer) proxy[0] == caller && proxyPackage.equals(proxy[1])
                            && Objects.equals(proxyTag, proxy[2])) return true;
                }
            }
            return false;
        }
    }

    private boolean shouldCollectNotes(int code) {
        if (code < 0 || code > NUM_OP - 1) throw new IllegalArgumentException("opCode is out of range of [0, " + (NUM_OP - 1) + "]");
        if (boolOf(SHOULD_FORCE_COLLECT_NOTE, code)) return true;
        String permission = (String) invoke(OP_TO_PERMISSION, null, code);
        if (permission == null) return false;
        PermissionInfo info = packages.permissionInfo(permission);
        return info != null && (info.getProtection() == PermissionInfo.PROTECTION_DANGEROUS
                || (info.getProtectionFlags() & PermissionInfo.PROTECTION_FLAG_APPOP) != 0);
    }

    private void collectNoteOpCallsForValidation(String stackTrace, int op) {
        boolean enabled;
        try { enabled = MANAGER.getField("NOTE_OP_COLLECTION_ENABLED").getBoolean(null); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
        if (!enabled) return;
        Objects.requireNonNull(stackTrace);
        if (op < 0 || op >= NUM_OP) throw new IllegalArgumentException();
    }

    private boolean wanted(int[] filter, int code) {
        if (filter == null) return true;
        for (int op : filter) if (op == code) return true;
        return false;
    }

    // AppOpsService.collectOps: only ops that exist, restricted-read ops only for a privileged caller.
    private List<Object> collect(int uid, String packageName, int[] filter, boolean includeRestricted) {
        List<Object> entries = new ArrayList<>();
        Map<Integer, Map<String, Attribution>> byCode = new LinkedHashMap<>();
        for (Op op : ops.values()) {
            if (op.uid != uid || !op.packageName.equals(packageName) || !wanted(filter, op.code)) continue;
            if (boolOf(OP_RESTRICTS_READ, op.code) && !includeRestricted) continue;
            byCode.put(op.code, op.attributions);
        }
        for (Map.Entry<Integer, Map<String, Attribution>> entry : byCode.entrySet()) {
            Map<String, Object> attributed = new LinkedHashMap<>();
            for (Attribution attribution : entry.getValue().values()) attributed.put(attribution.tag, attributedEntry(entry.getKey(), attribution));
            entries.add(create(OP_ENTRY, entry.getKey(), packageMode(uid, packageName, switchOp(entry.getKey())), attributed));
        }
        return entries;
    }

    private Object attributedEntry(int code, Attribution attribution) {
        android.util.LongSparseArray<Object> access = events(attribution.accessEvents, attribution.accessProxies),
                reject = events(attribution.rejectEvents, new HashMap<Long, Object[]>());
        long now = android.os.SystemClock.elapsedRealtime();
        for (InProgress progress : attribution.running.values())
            access.put(key(progress.flags), create(NOTE_EVENT, progress.startTime, now - progress.startElapsed, null));
        return create(ATTRIBUTED_ENTRY, code, !attribution.running.isEmpty(), access.size() == 0 ? null : access,
                reject.size() == 0 ? null : reject);
    }

    private static android.util.LongSparseArray<Object> events(Map<Long, long[]> source, Map<Long, Object[]> proxies) {
        android.util.LongSparseArray<Object> events = new android.util.LongSparseArray<>();
        for (Map.Entry<Long, long[]> event : source.entrySet())
            events.put(event.getKey(), create(NOTE_EVENT, event.getValue()[0], event.getValue()[1], proxyInfo(proxies.get(event.getKey()))));
        return events;
    }

    private static Object proxyInfo(Object[] proxy) {
        return proxy == null ? null : create(PROXY_INFO, proxy[0], proxy[1], proxy[2]);
    }

    private Object packageOps(int uid, String packageName, int[] filter, boolean includeRestricted) {
        List<Object> entries = collect(uid, packageName, filter, includeRestricted);
        return entries.isEmpty() ? null : create(PACKAGE_OPS, packageName, uid, entries);
    }

    private void enforceStatsIfNeeded(int uid, String packageName) {
        if (uid == callingUid() && packageName != null && checkPackage(uid, packageName) == MODE_ALLOWED) return;
        enforce(GET_APP_OPS_STATS);
    }

    private Object getPackagesForOps(int[] filter) {
        boolean all = holds(GET_APP_OPS_STATS);
        boolean restrictedToo = holds(MANAGE_APPOPS);
        List<Object> result = new ArrayList<>();
        synchronized (lock) {
            Set<String> seen = new HashSet<>();
            for (Op op : ops.values()) {
                if (!all && op.uid != callingUid()) continue;
                if (!seen.add(op.uid + "/" + op.packageName)) continue;
                Object entry = packageOps(op.uid, op.packageName, filter, restrictedToo);
                if (entry != null) result.add(entry);
            }
        }
        return result.isEmpty() ? null : result;
    }

    private Object getOpsForPackage(int uid, String packageName, int[] filter) {
        enforceStatsIfNeeded(uid, packageName);
        String resolved = resolvePackageName(uid, packageName);
        if (resolved == null) return new ArrayList<>();
        synchronized (lock) {
            Object entry = packageOps(uid, resolved, filter, holds(MANAGE_APPOPS));
            return entry == null ? null : new ArrayList<>(java.util.Collections.singletonList(entry));
        }
    }

    private Object getUidOps(int uid, int[] filter) {
        enforce(GET_APP_OPS_STATS);
        List<Object> entries = new ArrayList<>();
        synchronized (lock) {
            for (Map.Entry<Long, Integer> entry : uidModes.entrySet()) {
                int owner = (int) (entry.getKey() >> 32), code = (int) (long) entry.getKey();
                if (owner != uid || !wanted(filter, code)) continue;
                if (boolOf(OP_RESTRICTS_READ, code) && !holds(MANAGE_APPOPS)) continue;
                entries.add(create(OP_ENTRY, code, entry.getValue(), new LinkedHashMap<String, Object>()));
            }
        }
        return entries.isEmpty() ? null : new ArrayList<>(java.util.Collections.singletonList(create(PACKAGE_OPS, null, uid, entries)));
    }

    // AppOpsService.getHistoricalOps: validated arguments, the caller's own history unprivileged, others need
    // GET_APP_OPS_STATS (and GET_HISTORICAL_APP_OPS_STATS to even receive an empty result). The disk variant
    // needs MANAGE_APPOPS.
    private void getHistoricalOps(boolean disk, Object[] a) {
        int uid = (Integer) a[0];
        String packageName = (String) a[1], tag = (String) a[2];
        @SuppressWarnings("unchecked") List<String> opNames = (List<String>) a[3];
        int filter = (Integer) a[5], flags = (Integer) a[8];
        long begin = (Long) a[6], end = (Long) a[7];
        Object callback = a[9];
        if ((filter & FILTER_BY_UID) != 0 ? uid == INVALID_UID : uid != INVALID_UID) throw new IllegalArgumentException();
        if ((filter & FILTER_BY_PACKAGE_NAME) != 0) Objects.requireNonNull(packageName);
        else if (packageName != null) throw new IllegalArgumentException();
        if ((filter & FILTER_BY_ATTRIBUTION_TAG) == 0 && tag != null) throw new IllegalArgumentException();
        if ((filter & FILTER_BY_OP_NAMES) != 0) Objects.requireNonNull(opNames);
        else if (opNames != null) throw new IllegalArgumentException();
        if ((filter & ~(FILTER_BY_UID | FILTER_BY_PACKAGE_NAME | FILTER_BY_ATTRIBUTION_TAG | FILTER_BY_OP_NAMES)) != 0
                || begin < 0 || end <= begin || (flags & ~OP_FLAGS_ALL) != 0) throw new IllegalArgumentException();
        Objects.requireNonNull(callback, "callback cannot be null");
        boolean self = !disk && (filter & FILTER_BY_UID) != 0 && uid == callingUid();
        if (disk) enforce(MANAGE_APPOPS);
        else if (!self) {
            if (!holds(GET_HISTORICAL_APP_OPS_STATS)) { sendHistory(callback, null); return; }
            enforce(GET_APP_OPS_STATS);
        }
        Object historical = create(HISTORICAL_OPS, begin, end);
        Set<Integer> codes = null;
        if (opNames != null) {
            codes = new HashSet<>();
            for (String name : opNames) codes.add(intOf(STR_OP_TO_OP, name));
        }
        synchronized (lock) {
            for (HistoryEvent event : history) {
                if (event.time < begin || event.time >= end || (event.flags & flags) == 0) continue;
                if ((filter & FILTER_BY_UID) != 0 && event.uid != uid) continue;
                if ((filter & FILTER_BY_PACKAGE_NAME) != 0 && !packageName.equals(event.packageName)) continue;
                if ((filter & FILTER_BY_ATTRIBUTION_TAG) != 0 && !Objects.equals(tag, event.tag)) continue;
                if (codes != null && !codes.contains(event.code)) continue;
                String method = event.rejected ? "increaseRejectCount" : event.duration >= 0 ? "increaseAccessDuration" : "increaseAccessCount";
                invoke(method(historical.getClass(), method, int.class, int.class, String.class, String.class, int.class, int.class, long.class),
                        historical, event.code, event.uid, event.packageName, event.tag, event.uidState, event.flags,
                        event.duration >= 0 && !event.rejected ? event.duration : 1L);
            }
        }
        sendHistory(callback, historical);
    }

    private void sendHistory(Object callback, Object historical) {
        Bundle result = new Bundle();
        if (historical != null) result.putParcelable("historical_ops", (android.os.Parcelable) historical);
        invoke(method(callback.getClass(), "sendResult", Bundle.class), callback, result);
    }
}
