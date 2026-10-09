// Single-process counterpart of ActiveServices. ActivityThread owns all Service
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
    private final Map<ComponentName, Record> names = new LinkedHashMap<>();
    private final Map<IBinder, Record> tokens = new LinkedHashMap<>();
    private final Map<IBinder, List<Connection>> connections = new LinkedHashMap<>();
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

    FrameworkServiceRuntime(FrameworkPackages packages) { this.packages = packages; }

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
        Class.forName("android.app.IApplicationThread").getMethod(method, types).invoke(applicationThread, args);
    }

    private ServiceInfo resolve(Object caller, Intent intent, String callingPackage, int user) {
        if (application == null || shuttingDown)
            throw new IllegalStateException("Service runtime is not accepting requests");
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
        String process = application.processName == null ? application.packageName : application.processName;
        String serviceProcess = info.processName == null ? info.packageName : info.processName;
        if (!process.equals(serviceProcess) || (info.flags & ServiceInfo.FLAG_ISOLATED_PROCESS) != 0)
            throw new UnsupportedOperationException("Service requires a separate process: " + info.name);
        return info;
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
        Class<?> compat = Class.forName("android.content.res.CompatibilityInfo");
        schedule("scheduleCreateService", new Class<?>[] {IBinder.class, ServiceInfo.class, compat, int.class},
                record.token, record.info, compat.getField("DEFAULT_COMPATIBILITY_INFO").get(null),
                processState);
        record.created = true;
        for (Binding binding : record.bindings.values()) requestBinding(binding, false);
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
        schedule("scheduleServiceArgs", new Class<?>[] {IBinder.class, slice}, record.token,
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
        if (instance && a[6] != null)
            throw new UnsupportedOperationException("Named/isolated service instances are not implemented");
        long flags = ((Number) a[5]).longValue();
        if ((flags & Integer.toUnsignedLong(Context.BIND_EXTERNAL_SERVICE)) != 0)
            throw new UnsupportedOperationException("External service processes are not implemented");
        Record record = record(info);
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
        schedule("scheduleBindService", new Class<?>[] {IBinder.class, Intent.class, boolean.class, int.class, long.class},
                binding.record.token, copy(binding.intent), rebind,
                processState, ++bindSequence);
        binding.requested = true;
        binding.hasBound = true;
        binding.doRebind = false;
    }

    private static void connected(Connection connection, IBinder binder, boolean dead) throws Exception {
        Class.forName("android.app.IServiceConnection").getMethod("connected", ComponentName.class,
                IBinder.class, boolean.class).invoke(connection.client, connection.binding.record.name, binder, dead);
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
        Record record = tokens.get(token);
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
        schedule("scheduleUnbindService", new Class<?>[] {IBinder.class, Intent.class},
                binding.record.token, copy(binding.intent));
        binding.hasBound = false;
        binding.unbinding = true;
        binding.doRebind = false;
    }

    synchronized void unbindFinished(IBinder token, Intent intent, boolean rebind) throws Exception {
        Record record = tokens.get(token);
        if (record == null) return;
        Binding binding = record.bindings.get(new Intent.FilterComparison(intent));
        if (binding == null) return;
        binding.unbinding = false;
        binding.doRebind = rebind;
        if (rebind && !record.stopping && !binding.clients.isEmpty()) requestBinding(binding, true);
    }

    synchronized void done(IBinder token, int type, Intent intent) throws Exception {
        Record record = tokens.get(token);
        if (record == null) return;
        if (type == DONE_UNBIND && intent != null) unbindFinished(token, intent, false);
        if (type == DONE_STOP) {
            tokens.remove(token);
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
        names.remove(record.name);
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
        if (record.created) schedule("scheduleStopService", new Class<?>[] {IBinder.class}, record.token);
        else { tokens.remove(record.token); notifyIdle(); }
    }

    synchronized void whenIdle(Runnable callback) {
        idle = callback;
        notifyIdle();
    }

    private boolean active() {
        for (Record record : tokens.values()) if (record.created) return true;
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
        for (Record record : new ArrayList<>(names.values())) destroy(record);
        whenIdle(callback);
    }
}
