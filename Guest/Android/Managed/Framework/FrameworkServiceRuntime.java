// Session counterpart of ActiveServices. ActivityThread owns all Service
// instances and lifecycle callbacks; this class owns tokens, starts and bindings.
import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class FrameworkServiceRuntime {
    // Android 15 ActivityThread.SERVICE_DONE_EXECUTING_*.
    private static final int DONE_STOP = 2, DONE_UNBIND = 4;
    // Binds made by the framework's own services (AccountManagerService binding
    // an authenticator) run with the system's identity: no export or permission
    // check applies to the target service. Set only around the bind call.
    static final ThreadLocal<Boolean> SYSTEM_BIND = ThreadLocal.withInitial(() -> false);
    private final FrameworkPackages packages;
    private final FrameworkProcessRuntime processes;
    private FrameworkProcessRuntime.Remote remote;
    private final Map<ComponentName, Record> names = new LinkedHashMap<>();
    private final Map<IBinder, Record> tokens = new LinkedHashMap<>();
    private final Map<IBinder, List<Connection>> connections = new LinkedHashMap<>();
    private final Map<String, Record> instances = new LinkedHashMap<>();
    private Object applicationThread;
    private ApplicationInfo application;
    private Handler main;
    private Runnable idle;
    private boolean shuttingDown;
    private long bindSequence;
    private int processState;

    // Root and authenticated child calls share service records, but never caller identity.
    private static final class Caller {
        final Object thread;
        final int uid;
        final String packageName;
        final FrameworkProcessRuntime.Child child;
        Caller(Object thread, int uid, String packageName, FrameworkProcessRuntime.Child child) {
            this.thread = thread; this.uid = uid; this.packageName = packageName; this.child = child;
        }
    }
    private Caller caller;

    private static final class Record {
        final Binder token = new Binder();
        final ServiceInfo info;
        final ComponentName name;
        final Map<Intent.FilterComparison, Binding> bindings = new LinkedHashMap<>();
        boolean created, started, stopping;
        long activeSince, lastActivity;  // SystemClock.elapsedRealtime(): first made active, last started or bound
        int lastStartId;
        String instance;
        FrameworkProcessRuntime.Child child;
        Record(ServiceInfo info) {
            this.info = info;
            name = new ComponentName(info.packageName, info.name);
        }
    }
    private static final class Binding {
        final Record record;
        final Intent intent;
        final List<Connection> clients = new ArrayList<>();
        boolean requested, received, hasBound, unbinding, doRebind;
        IBinder binder;
        Binding(Record record, Intent intent) { this.record = record; this.intent = intent; }
    }
    private static final class Connection {
        final Binding binding;
        final IInterface client;
        final long flags;
        final FrameworkProcessRuntime.Child owner;
        Connection(Binding binding, Object client, long flags, Caller caller) {
            this.binding = binding; this.client = (IInterface) client; this.flags = flags; this.owner = caller.child;
        }
    }

    FrameworkServiceRuntime(FrameworkPackages packages) {
        this.packages = packages;
        processes = new FrameworkProcessRuntime(packages, this);
    }

    void remote(FrameworkProcessRuntime.Remote remote) { this.remote = remote; }
    FrameworkProcessRuntime processes() { return processes; }

    synchronized void attach(Object thread, ApplicationInfo application) throws Exception {
        this.application = new ApplicationInfo(application);
        java.lang.reflect.Method method = thread.getClass().getDeclaredMethod("getApplicationThread");
        method.setAccessible(true);
        applicationThread = method.invoke(thread);
        caller = new Caller(applicationThread, application.uid, application.packageName, null);
        main = new Handler(Looper.getMainLooper());
        processState = ActivityManager.class.getField("PROCESS_STATE_IMPORTANT_FOREGROUND").getInt(null);
    }

    private static Intent copy(Intent intent) {
        Parcel parcel = Parcel.obtain();
        try {
            intent.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return Intent.CREATOR.createFromParcel(parcel);
        } finally { parcel.recycle(); }
    }

    private void schedule(String method, Class<?>[] types, Object... args) throws Exception {
        long trace = FrameworkTrace.begin("applicationThread", method, args);
        try {
            Class.forName("android.app.IApplicationThread").getMethod(method, types).invoke(applicationThread, args);
            FrameworkTrace.end(trace, null, null);
        } catch (Exception error) {
            FrameworkTrace.end(trace, null, error);
            throw error;
        }
    }

    private void schedule(Record record, String method, Class<?>[] types, Object... args) throws Exception {
        if (record.child == null) schedule(method, types, args);
        else processes.schedule(record.child, method, types, args);
    }

    private void checkCaller(Caller identity, Object thread, String callingPackage, int user) {
        if (application == null || shuttingDown)
            throw new IllegalStateException("Service runtime is not accepting requests");
        if (identity == null || identity.uid >= 99000) throw new SecurityException("Isolated processes cannot start or bind services");
        if (thread != identity.thread || !identity.packageName.equals(callingPackage)
                || user != identity.uid / 100000)
            throw new SecurityException("Foreign service caller or user");
    }

    private ServiceInfo resolve(Caller identity, Object thread, Intent intent, String type, String callingPackage, int user)
            throws Exception {
        checkCaller(identity, thread, callingPackage, user);
        if (intent == null) throw new IllegalArgumentException("null service Intent");
        // ActiveServices.retrieveServiceLocked resolves an implicit Intent
        // (Context requires its package) through PackageManager's filters.
        ComponentName component = intent.getComponent();
        if (component == null) {
            List<android.content.pm.ResolveInfo> matches = packages.queryServices(intent, type,
                    PackageManager.GET_META_DATA, user);
            if (matches.isEmpty()) return null;
            component = new ComponentName(matches.get(0).serviceInfo.packageName, matches.get(0).serviceInfo.name);
        }
        // ActiveServices checks its existing records before consulting PackageManager.
        // A component disabled with DONT_KILL_APP must remain stoppable while alive.
        Record existing = names.get(component);
        ServiceInfo info = existing != null ? existing.info : packages.serviceInfo(component, PackageManager.GET_META_DATA, user);
        if (info == null) return null;
        if (!(identity.child == null && SYSTEM_BIND.get()) && info.applicationInfo.uid != identity.uid && (!info.exported
                || (info.permission != null && packages.checkPermission(info.permission,
                        identity.uid, Context.DEVICE_ID_DEFAULT) != PackageManager.PERMISSION_GRANTED)))
            throw new SecurityException("Service is not accessible: " + info.name);
        return info;
    }

    private boolean separateProcess(ServiceInfo info) {
        String process = application.processName == null ? application.packageName : application.processName;
        String serviceProcess = info.processName == null ? info.packageName : info.processName;
        if (process.equals(serviceProcess) && info.applicationInfo.uid == application.uid
                && (info.flags & ServiceInfo.FLAG_ISOLATED_PROCESS) == 0) return false;
        return true;
    }

    private Record record(ServiceInfo info) {
        ComponentName name = new ComponentName(info.packageName, info.name);
        Record record = names.get(name);
        if (record == null) {
            record = new Record(info);
            names.put(name, record);
            tokens.put(record.token, record);
        }
        return record;
    }

    private void create(Record record) throws Exception {
        if (record.created) return;
        if (separateProcess(record.info)) record.child = processes.start(record.info, record.instance);
        Class<?> compat = Class.forName("android.content.res.CompatibilityInfo");
        schedule(record, "scheduleCreateService", new Class<?>[] {IBinder.class, ServiceInfo.class, compat, int.class},
                record.token, record.info, compat.getField("DEFAULT_COMPATIBILITY_INFO").get(null),
                processState);
        record.created = true;
        record.activeSince = record.lastActivity = SystemClock.elapsedRealtime();
        for (Binding binding : record.bindings.values()) requestBinding(binding, false);
    }

    synchronized ComponentName startPending(Intent intent, String resolvedType, String owner, boolean foreground) throws Exception {
        return start(new Object[] {applicationThread, intent, resolvedType, foreground, owner, null, 0});
    }

    synchronized ComponentName start(Object[] a) throws Exception {
        if (remote != null) {
            checkCaller(caller, a[0], (String) a[4], (Integer) a[6]);
            return remote.startService(a);
        }
        return start(a, caller);
    }

    private ComponentName start(Object[] a, Caller identity) throws Exception {
        Intent intent = copy((Intent) a[1]);
        ServiceInfo info = resolve(identity, a[0], intent, (String) a[2], (String) a[4], (Integer) a[6]);
        if (info == null) return null;
        if ((Boolean) a[3])
            throw new UnsupportedOperationException("Foreground-service notification/deadline support is not implemented");
        Record record = record(info);
        record.started = true;
        record.lastActivity = SystemClock.elapsedRealtime();
        if (++record.lastStartId <= 0) record.lastStartId = 1;
        create(record);
        Class<?> argsType = Class.forName("android.app.ServiceStartArgs");
        Object args = argsType.getConstructor(boolean.class, int.class, int.class, Intent.class)
                .newInstance(false, record.lastStartId, 0, intent);
        Class<?> slice = Class.forName("android.content.pm.ParceledListSlice");
        schedule(record, "scheduleServiceArgs", new Class<?>[] {IBinder.class, slice}, record.token,
                slice.getConstructor(List.class).newInstance(Arrays.asList(args)));
        return record.name;
    }

    synchronized int stop(Object[] a) throws Exception {
        if (remote != null) {
            checkCaller(caller, a[0], application.packageName, (Integer) a[3]);
            return remote.stopService(a);
        }
        return stop(a, caller);
    }

    private int stop(Object[] a, Caller identity) throws Exception {
        ServiceInfo info = resolve(identity, a[0], (Intent) a[1], (String) a[2], identity.packageName, (Integer) a[3]);
        if (info == null) return 0;
        Record record = names.get(new ComponentName(info.packageName, info.name));
        if (record == null) return 0;
        record.started = false;
        stopIfUnused(record);
        return 1;
    }

    synchronized boolean stopSelf(ComponentName name, IBinder token, int startId) throws Exception {
        if (remote != null) return remote.stopSelf(name, token, startId);
        return stopSelf(name, token, startId, null);
    }

    private boolean stopSelf(ComponentName name, IBinder token, int startId, FrameworkProcessRuntime.Child owner) throws Exception {
        Record record = tokens.get(token);
        if (record != null && record.child != owner) throw new SecurityException("Foreign service token");
        if (record == null || record.stopping || !record.name.equals(name)
                || (startId >= 0 && startId != record.lastStartId)) return false;
        record.started = false;
        stopIfUnused(record);
        return true;
    }

    // microG's AdRequestService answers every ad request "Not supported".
    // Google Mobile Ads builds its requests itself only when this bind fails
    // (RemoteAdRequestClientTask.onConnectionFailed), as without Play
    // services, so the stub is never bound.
    private static final ComponentName MICROG_AD_REQUESTS =
            new ComponentName("com.google.android.gms", "org.microg.gms.ads.AdRequestService");
    // In-app purchases are not offered. Binding the Play Store companion's
    // billing service started a whole Play Store runtime (~370 MiB) for every
    // game that links the Play Billing Library; a failed bind is what a device
    // without the Play Store reports, and the library answers BILLING_UNAVAILABLE.
    private static final ComponentName MICROG_BILLING =
            new ComponentName("com.android.vending", "com.android.vending.billing.InAppBillingService");

    synchronized int bind(Object[] a, boolean instance) throws Exception {
        if (remote != null) {
            checkCaller(caller, a[0], (String) a[instance ? 7 : 6], (Integer) a[instance ? 8 : 7]);
            return remote.bindService(a, instance);
        }
        return bind(a, instance, caller);
    }

    private int bind(Object[] a, boolean instance, Caller identity) throws Exception {
        Intent intent = copy((Intent) a[2]);
        ServiceInfo info = resolve(identity, a[0], intent, (String) a[3], (String) a[instance ? 7 : 6], (Integer) a[instance ? 8 : 7]);
        if (info == null) return 0;
        if (MICROG_AD_REQUESTS.equals(new ComponentName(info.packageName, info.name))) return 0;
        if (MICROG_BILLING.equals(new ComponentName(info.packageName, info.name))) return 0;
        // ActiveServices returns 0 when bringUpServiceLocked cannot start
        // the hosting process. Context.bindService then returns false.
        // An unavailable process must not become an uncaught SDK exception.
        long flags = ((Number) a[5]).longValue();
        if ((flags & Integer.toUnsignedLong(Context.BIND_EXTERNAL_SERVICE)) != 0) {
            // ActiveServices runs an exported external isolated service under
            // its client's package identity, retaining the provider's APK/code.
            if (!info.exported || (info.flags & ServiceInfo.FLAG_ISOLATED_PROCESS) == 0
                    || (info.flags & ServiceInfo.FLAG_EXTERNAL_SERVICE) == 0)
                throw new SecurityException("Service is not an external isolated service");
            info = new ServiceInfo(info);
            info.applicationInfo = new ApplicationInfo(info.applicationInfo);
            info.packageName = info.applicationInfo.packageName = identity.packageName;
            info.applicationInfo.uid = identity.uid;
        }
        String instanceName = instance ? (String) a[6] : null;
        Record record;
        if (instanceName == null) record = record(info);
        else {
            if ((info.flags & ServiceInfo.FLAG_ISOLATED_PROCESS) == 0)
                throw new IllegalArgumentException("Named instances require an isolated service");
            String key = info.packageName + "/" + info.name + ":" + instanceName;
            record = instances.get(key);
            if (record == null) {
                record = new Record(info); record.instance = instanceName;
                instances.put(key, record); tokens.put(record.token, record);
            }
        }
        Intent.FilterComparison key = new Intent.FilterComparison(intent);
        Binding binding = record.bindings.get(key);
        if (binding == null) { binding = new Binding(record, intent); record.bindings.put(key, binding); }
        List<Connection> existing = connections.get(((IInterface) a[4]).asBinder());
        if (existing != null)
            for (Connection connection : existing)
                if (connection.owner != identity.child) throw new SecurityException("Foreign service connection");
        Connection connection = new Connection(binding, a[4], flags, identity);
        binding.clients.add(connection);
        record.lastActivity = SystemClock.elapsedRealtime();
        connections.computeIfAbsent(connection.client.asBinder(), unused -> new ArrayList<>()).add(connection);
        if ((flags & Context.BIND_AUTO_CREATE) != 0) create(record);
        if (binding.received) {
            connected(connection, binding.binder, false);
            if (binding.doRebind && !binding.unbinding) requestBinding(binding, true);
        } else if (record.created && !binding.requested) requestBinding(binding, false);
        return 1;
    }

    private void requestBinding(Binding binding, boolean rebind) throws Exception {
        if (binding.clients.isEmpty() || binding.record.stopping || (binding.requested && !rebind)) return;
        schedule(binding.record, "scheduleBindService", new Class<?>[] {IBinder.class, Intent.class, boolean.class, int.class, long.class},
                binding.record.token, copy(binding.intent), rebind,
                processState, ++bindSequence);
        binding.requested = true;
        binding.hasBound = true;
        binding.doRebind = false;
    }

    private static void connected(Connection connection, IBinder binder, boolean dead) throws Exception {
        long trace = FrameworkTrace.begin("serviceConnection", "connected",
                new Object[] {connection.binding.record.name, binder, dead});
        try {
            Class.forName("android.app.IServiceConnection").getMethod("connected", ComponentName.class,
                    IBinder.class, boolean.class).invoke(connection.client, connection.binding.record.name, binder, dead);
            FrameworkTrace.end(trace, null, null);
        } catch (Exception error) {
            FrameworkTrace.end(trace, null, error);
            throw error;
        }
    }

    synchronized IBinder peek(Intent intent, String callingPackage) throws Exception {
        if (remote != null) {
            checkCaller(caller, applicationThread, callingPackage, application.uid / 100000);
            return remote.peekService(intent, callingPackage);
        }
        return peek(intent, callingPackage, caller);
    }

    private IBinder peek(Intent intent, String callingPackage, Caller identity) throws Exception {
        if (resolve(identity, identity.thread, intent, null, callingPackage, identity.uid / 100000) == null) return null;
        if (intent.getComponent() == null)
            throw new UnsupportedOperationException("Service intent-filter resolution is not implemented");
        Record record = names.get(intent.getComponent());
        if (record == null || !record.created || record.stopping) return null;
        Binding binding = record.bindings.get(new Intent.FilterComparison(intent));
        return binding != null && binding.received ? binding.binder : null;
    }

    synchronized void publish(IBinder token, Intent intent, IBinder binder) throws Exception {
        if (remote != null && !tokens.containsKey(token)) { remote.publish(token, intent, binder); return; }
        Record record = tokens.get(token);
        long trace = FrameworkTrace.begin("serviceRuntime", "publishLookup",
                new Object[] {token, intent, record != null});
        FrameworkTrace.end(trace, record == null ? null : record.name, null);
        if (record == null || record.stopping) return;
        Binding binding = record.bindings.get(new Intent.FilterComparison(intent));
        if (binding == null || binding.received) return;
        binding.binder = binder;
        binding.received = true;
        for (Connection connection : new ArrayList<>(binding.clients)) connected(connection, binder, false);
    }

    synchronized boolean unbind(Object client) throws Exception {
        if (remote != null) return remote.unbindService(client);
        return unbind(client, null);
    }

    private boolean unbind(Object client, FrameworkProcessRuntime.Child owner) throws Exception {
        return unbind(client, owner, false);
    }

    private boolean unbind(Object client, FrameworkProcessRuntime.Child owner, boolean callerDied) throws Exception {
        IBinder binder = ((IInterface) client).asBinder();
        List<Connection> removed = connections.get(binder);
        if (removed == null) return false;
        for (Connection connection : removed)
            if (connection.owner != owner) throw new SecurityException("Foreign service connection");
        connections.remove(binder);
        for (Connection connection : removed) {
            Binding binding = connection.binding;
            binding.clients.remove(connection);
            if (binding.clients.isEmpty() && !(callerDied && binding.record.child == owner)) requestUnbind(binding);
        }
        for (Connection connection : removed)
            if (!(callerDied && connection.binding.record.child == owner)) stopIfUnused(connection.binding.record);
        return true;
    }

    private void requestUnbind(Binding binding) throws Exception {
        if (!binding.hasBound) return;
        schedule(binding.record, "scheduleUnbindService", new Class<?>[] {IBinder.class, Intent.class},
                binding.record.token, copy(binding.intent));
        binding.hasBound = false;
        binding.unbinding = true;
        binding.doRebind = false;
    }

    synchronized void unbindFinished(IBinder token, Intent intent, boolean rebind) throws Exception {
        if (remote != null && !tokens.containsKey(token)) { remote.unbind(token, intent, rebind); return; }
        Record record = tokens.get(token);
        if (record == null) return;
        Binding binding = record.bindings.get(new Intent.FilterComparison(intent));
        if (binding == null) return;
        binding.unbinding = false;
        binding.doRebind = rebind;
        if (rebind && !record.stopping && !binding.clients.isEmpty()) requestBinding(binding, true);
    }

    synchronized void done(IBinder token, int type, Intent intent) throws Exception {
        if (remote != null && !tokens.containsKey(token)) { remote.done(token, type, intent); return; }
        Record record = tokens.get(token);
        if (record == null) return;
        if (type == DONE_UNBIND && intent != null) unbindFinished(token, intent, false);
        if (type == DONE_STOP) {
            tokens.remove(token);
            FrameworkProcessRuntime.Child child = record.child;
            if (child != null) main.post(() -> {
                synchronized (FrameworkServiceRuntime.this) {
                    for (Record live : tokens.values()) if (live.child == child) return;
                    processes.stop(child);
                }
            });
            notifyIdle();
        }
    }

    // No service record (created, starting or stopping) lives in `child`.
    synchronized boolean idle(FrameworkProcessRuntime.Child child) {
        for (Record record : tokens.values()) if (record.child == child) return false;
        return true;
    }

    private void stopIfUnused(Record record) throws Exception {
        if (record.stopping || record.started) return;
        for (Binding binding : record.bindings.values())
            for (Connection connection : binding.clients)
                if ((connection.flags & Context.BIND_AUTO_CREATE) != 0) return;
        destroy(record);
    }

    private void destroy(Record record) throws Exception {
        if (record.stopping) return;
        record.stopping = true;
        if (record.instance == null) names.remove(record.name);
        else instances.values().remove(record);
        for (Binding binding : record.bindings.values()) {
            requestUnbind(binding);
            for (Connection connection : new ArrayList<>(binding.clients)) {
                connected(connection, null, true);
                List<Connection> list = connections.get(connection.client.asBinder());
                list.remove(connection);
                if (list.isEmpty()) connections.remove(connection.client.asBinder());
            }
            binding.clients.clear();
        }
        if (record.created) schedule(record, "scheduleStopService", new Class<?>[] {IBinder.class}, record.token);
        else { tokens.remove(record.token); notifyIdle(); }
    }

    synchronized void processDied(FrameworkProcessRuntime.Child child) throws Exception {
        for (List<Connection> list : new ArrayList<>(connections.values()))
            if (!list.isEmpty() && list.get(0).owner == child) unbind(list.get(0).client, child, true);
        for (Record record : new ArrayList<>(tokens.values())) {
            if (record.child != child) continue;
            record.created = false; record.child = null;
            for (Binding binding : record.bindings.values()) {
                binding.requested = binding.received = binding.hasBound = false;
                binding.binder = null;
                for (Connection connection : new ArrayList<>(binding.clients)) connected(connection, null, true);
            }
            if (record.stopping) {
                tokens.remove(record.token);
                notifyIdle();
            } else stopIfUnused(record);
        }
    }

    // Called only after the parent controller authenticates the child's PID, UID and key.
    synchronized void fromChild(FrameworkProcessRuntime.Child child, int operation, Parcel data, Parcel reply) throws Exception {
        if (child.applicationThread == null) throw new SecurityException("Unavailable service caller");
        Caller identity = new Caller(child.applicationThread, child.uid, child.info.packageName, child);
        Intent intent;
        switch (operation) {
            case FrameworkProcessRuntime.SERVICE_START: {
                intent = data.readTypedObject(Intent.CREATOR); String type = data.readString();
                boolean foreground = data.readBoolean(); String name = data.readString(); int user = data.readInt();
                ComponentName result = start(new Object[] {identity.thread, intent, type, foreground, name, null, user}, identity);
                reply.writeNoException(); reply.writeTypedObject(result, 0); return;
            }
            case FrameworkProcessRuntime.SERVICE_STOP: {
                intent = data.readTypedObject(Intent.CREATOR); String type = data.readString(); int user = data.readInt();
                int result = stop(new Object[] {identity.thread, intent, type, user}, identity);
                reply.writeNoException(); reply.writeInt(result); return;
            }
            case FrameworkProcessRuntime.SERVICE_BIND: {
                boolean instance = data.readBoolean(); intent = data.readTypedObject(Intent.CREATOR);
                String type = data.readString();
                Object client = Class.forName("android.app.IServiceConnection$Stub").getMethod("asInterface", IBinder.class)
                        .invoke(null, data.readStrongBinder());
                long flags = data.readLong(); String instanceName = data.readString(), name = data.readString(); int user = data.readInt();
                Object[] args = instance ? new Object[] {identity.thread, null, intent, type, client, flags, instanceName, name, user}
                        : new Object[] {identity.thread, null, intent, type, client, flags, name, user};
                int result = bind(args, instance, identity);
                reply.writeNoException(); reply.writeInt(result); return;
            }
            case FrameworkProcessRuntime.SERVICE_UNBIND: {
                Object client = Class.forName("android.app.IServiceConnection$Stub").getMethod("asInterface", IBinder.class)
                        .invoke(null, data.readStrongBinder());
                boolean result = unbind(client, child); reply.writeNoException(); reply.writeBoolean(result); return;
            }
            case FrameworkProcessRuntime.SERVICE_PEEK: {
                intent = data.readTypedObject(Intent.CREATOR); String name = data.readString();
                IBinder result = peek(intent, name, identity); reply.writeNoException(); reply.writeStrongBinder(result); return;
            }
            case FrameworkProcessRuntime.SERVICE_STOP_SELF: {
                ComponentName name = data.readTypedObject(ComponentName.CREATOR); IBinder token = data.readStrongBinder();
                boolean result = stopSelf(name, token, data.readInt(), child);
                reply.writeNoException(); reply.writeBoolean(result); return;
            }
            default: throw new IllegalArgumentException("Unknown service operation");
        }
    }

    synchronized void whenIdle(Runnable callback) {
        idle = callback;
        notifyIdle();
    }

    // ActivityManager.getRunningServices, which for an app without GET_TASKS reports only the services of
    // its own uid (ActiveServices.getRunningServiceInfoLocked): every created service of this app, started or
    // only bound, up to maxNum. A service in a child process carries that process's pid.
    synchronized List<ActivityManager.RunningServiceInfo> running(int maxNum) {
        List<ActivityManager.RunningServiceInfo> result = new ArrayList<>();
        for (Record record : tokens.values()) {
            if (result.size() >= maxNum) break;
            if (!record.created || record.stopping || record.info.applicationInfo.uid != application.uid) continue;
            ActivityManager.RunningServiceInfo info = new ActivityManager.RunningServiceInfo();
            info.service = record.name;
            info.uid = record.info.applicationInfo.uid;
            info.pid = record.child != null ? record.child.pid : Process.myPid();
            info.process = record.info.processName;
            info.started = record.started;
            info.flags = record.started ? ActivityManager.RunningServiceInfo.FLAG_STARTED : 0;
            for (Binding binding : record.bindings.values()) info.clientCount += binding.clients.size();
            info.activeSince = record.activeSince;
            info.lastActivityTime = record.lastActivity;
            result.add(info);
        }
        return result;
    }

    private boolean active() {
        for (Record record : tokens.values())
            if (record.created && (record.child == null || record.started
                    || record.info.applicationInfo.uid == application.uid)) return true;
        return false;
    }

    private void notifyIdle() {
        if (idle == null || active()) return;
        main.post(() -> {
            Runnable callback;
            synchronized (FrameworkServiceRuntime.this) {
                if (active()) return;
                callback = idle;
                idle = null;
            }
            if (callback != null) callback.run();
        });
    }

    synchronized void shutdown(Runnable callback) throws Exception {
        shuttingDown = true;
        for (Record record : new ArrayList<>(tokens.values())) destroy(record);
        whenIdle(callback);
    }
}
