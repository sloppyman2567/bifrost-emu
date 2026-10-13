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
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class FrameworkServiceRuntime {
    // Android 15 ActivityThread.SERVICE_DONE_EXECUTING_*.
    private static final int DONE_STOP = 2, DONE_UNBIND = 4;
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

    private static final class Record {
        final Binder token = new Binder();
        final ServiceInfo info;
        final ComponentName name;
        final Map<Intent.FilterComparison, Binding> bindings = new LinkedHashMap<>();
        boolean created, started, stopping;
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
        Connection(Binding binding, Object client, long flags) {
            this.binding = binding; this.client = (IInterface) client; this.flags = flags;
        }
    }

    FrameworkServiceRuntime(FrameworkPackages packages) {
        this.packages = packages;
        processes = new FrameworkProcessRuntime(packages, this);
    }

    void remote(FrameworkProcessRuntime.Remote remote) { this.remote = remote; }

    synchronized void attach(Object thread, ApplicationInfo application) throws Exception {
        this.application = new ApplicationInfo(application);
        java.lang.reflect.Method method = thread.getClass().getDeclaredMethod("getApplicationThread");
        method.setAccessible(true);
        applicationThread = method.invoke(thread);
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

    private ServiceInfo resolve(Object caller, Intent intent, String callingPackage, int user) {
        if (application == null || shuttingDown)
            throw new IllegalStateException("Service runtime is not accepting requests");
        if (application.uid >= 99000) throw new SecurityException("Isolated processes cannot start or bind services");
        if (caller != applicationThread || !application.packageName.equals(callingPackage)
                || user != application.uid / 100000)
            throw new SecurityException("Foreign service caller or user");
        if (intent == null) throw new IllegalArgumentException("null service Intent");
        if (intent.getComponent() == null && intent.getPackage() != null
                && !application.packageName.equals(intent.getPackage())) return null;
        if (intent.getComponent() == null)
            throw new UnsupportedOperationException("Service intent-filter resolution is not implemented");
        // ActiveServices checks its existing records before consulting PackageManager.
        // A component disabled with DONT_KILL_APP must remain stoppable while alive.
        Record existing = names.get(intent.getComponent());
        if (existing != null) return existing.info;
        ServiceInfo info = packages.serviceInfo(intent.getComponent(), PackageManager.GET_META_DATA, user);
        if (info == null) return null;
        if (info.applicationInfo.uid != application.uid && (!info.exported
                || (info.permission != null && packages.checkPermission(info.permission,
                        application.uid, Context.DEVICE_ID_DEFAULT) != PackageManager.PERMISSION_GRANTED)))
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
        for (Binding binding : record.bindings.values()) requestBinding(binding, false);
    }

    synchronized ComponentName startPending(Intent intent, String resolvedType, String owner, boolean foreground) throws Exception {
        return start(new Object[] {applicationThread, intent, resolvedType, foreground, owner, null, 0});
    }

    synchronized ComponentName start(Object[] a) throws Exception {
        Intent intent = copy((Intent) a[1]);
        ServiceInfo info = resolve(a[0], intent, (String) a[4], (Integer) a[6]);
        if (info == null) return null;
        if ((Boolean) a[3])
            throw new UnsupportedOperationException("Foreground-service notification/deadline support is not implemented");
        Record record = record(info);
        record.started = true;
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
        ServiceInfo info = resolve(a[0], (Intent) a[1],
                application == null ? null : application.packageName, (Integer) a[3]);
        if (info == null) return 0;
        Record record = names.get(new ComponentName(info.packageName, info.name));
        if (record == null) return 0;
        record.started = false;
        stopIfUnused(record);
        return 1;
    }

    synchronized boolean stopSelf(ComponentName name, IBinder token, int startId) throws Exception {
        Record record = tokens.get(token);
        if (record == null || record.stopping || !record.name.equals(name)
                || (startId >= 0 && startId != record.lastStartId)) return false;
        record.started = false;
        stopIfUnused(record);
        return true;
    }

    synchronized int bind(Object[] a, boolean instance) throws Exception {
        Intent intent = copy((Intent) a[2]);
        ServiceInfo info = resolve(a[0], intent, (String) a[instance ? 7 : 6], (Integer) a[instance ? 8 : 7]);
        if (info == null) return 0;
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
            info.packageName = info.applicationInfo.packageName = application.packageName;
            info.applicationInfo.uid = application.uid;
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
        Connection connection = new Connection(binding, a[4], flags);
        binding.clients.add(connection);
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

    synchronized IBinder peek(Intent intent, String callingPackage) {
        if (application == null || !application.packageName.equals(callingPackage))
            throw new SecurityException("Foreign service caller");
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
        List<Connection> removed = connections.remove(((IInterface) client).asBinder());
        if (removed == null) return false;
        for (Connection connection : removed) {
            Binding binding = connection.binding;
            binding.clients.remove(connection);
            if (binding.clients.isEmpty()) requestUnbind(binding);
        }
        for (Connection connection : removed) stopIfUnused(connection.binding.record);
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
            }
        }
    }

    synchronized void whenIdle(Runnable callback) {
        idle = callback;
        notifyIdle();
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
