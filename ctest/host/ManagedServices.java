package org.bifrost.fixture;

import android.app.Activity;
import android.app.Service;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.SurfaceView;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;

/** Real APK exercising ActivityThread's service lifecycle and client dispatcher. */
public final class ManagedServices extends Activity {
    private static ManagedServices activity;
    private static boolean activityDestroyed;
    private final Handler main = new Handler(Looper.getMainLooper());
    private int checks;

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private void check(boolean condition, String message) { require(condition, message); checks++; }
    private static void event(String message) {
        require(Looper.myLooper() == Looper.getMainLooper(), "service callback off main Looper");
        System.out.println("SERVICE_FIXTURE " + message);
    }
    private void await(BooleanSupplier condition, Runnable next) {
        long deadline = android.os.SystemClock.uptimeMillis() + 15000;
        main.post(new Runnable() {
            public void run() {
                if (condition.getAsBoolean()) { next.run(); return; }
                require(android.os.SystemClock.uptimeMillis() < deadline, "service callback timed out");
                main.postDelayed(this, 20);
            }
        });
    }
    private Intent intent(Class<?> type) { return new Intent(this, type); }
    private Intent binding(String action) {
        return intent(Lifecycle.class).setAction(action).putExtra("origin", "original");
    }
    private static final class Client implements ServiceConnection {
        IBinder binder;
        int connected, nullBindings, disconnected, died;
        public void onServiceConnected(ComponentName name, IBinder service) {
            event("connected " + name.getClassName()); binder = service; connected++;
        }
        public void onServiceDisconnected(ComponentName name) { event("disconnected"); disconnected++; }
        public void onNullBinding(ComponentName name) { event("null-binding"); nullBindings++; }
        public void onBindingDied(ComponentName name) { event("binding-died"); died++; }
    }
    private static final class Peeker extends android.content.BroadcastReceiver {
        public void onReceive(android.content.Context context, Intent intent) { }
        IBinder get(android.content.Context context, Intent intent) { return peekService(context, intent); }
    }
    public static final class LocalBinder extends Binder {
        final Lifecycle service;
        final String action;
        LocalBinder(Lifecycle service, String action) { this.service = service; this.action = action; }
        int ping(int value) { return value + 1; }
        @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
                throws android.os.RemoteException {
            if (code != 42) return super.onTransact(code, data, reply, flags);
            reply.writeInt(data.readInt() + 1);
            return true;
        }
    }
    public static final class Lifecycle extends Service {
        static final List<Lifecycle> instances = new ArrayList<>();
        int starts, binds, unbinds, rebinds;
        boolean destroyed;
        public void onCreate() {
            event("create lifecycle");
            require(getApplication() == activity.getApplication(), "service Application identity changed");
            require(getPackageName().equals(activity.getPackageName()) && getFilesDir().isDirectory(),
                    "service Context not attached");
            require(getClassLoader() == activity.getClassLoader(), "service class loader changed");
            instances.add(this);
        }
        public int onStartCommand(Intent intent, int flags, int id) {
            event("start lifecycle " + id);
            require(id == ++starts && flags == 0, "start ID or flags incorrect");
            require(intent.getIntExtra("value", -1) == id, "start Intent changed after request");
            return START_NOT_STICKY;
        }
        public IBinder onBind(Intent intent) {
            event("bind " + intent.getAction()); binds++;
            require("original".equals(intent.getStringExtra("origin")), "bind Intent aliases caller");
            return new LocalBinder(this, intent.getAction());
        }
        public boolean onUnbind(Intent intent) {
            event("unbind " + intent.getAction()); unbinds++;
            return "rebind".equals(intent.getAction());
        }
        public void onRebind(Intent intent) { event("rebind " + intent.getAction()); rebinds++; }
        public void onDestroy() { event("destroy lifecycle"); destroyed = true; }
    }
    public static final class NullBinding extends Service {
        static boolean destroyed;
        public IBinder onBind(Intent intent) { event("bind null"); return null; }
        public void onDestroy() { event("destroy null"); destroyed = true; }
    }
    public static final class SelfStopping extends Service {
        static int starts;
        static boolean destroyed;
        public IBinder onBind(Intent intent) { return null; }
        public int onStartCommand(Intent intent, int flags, int id) {
            event("start self " + id); starts++;
            require(stopSelfResult(id) == (id == 2), "stopSelfResult ignored newest start ID");
            return START_NOT_STICKY;
        }
        public void onDestroy() { event("destroy self"); destroyed = true; }
    }
    public static final class AfterActivity extends Service {
        public IBinder onBind(Intent intent) { return null; }
        public int onStartCommand(Intent intent, int flags, int id) {
            event("start after-activity");
            new Handler().postDelayed(() -> {
                require(activityDestroyed, "Activity did not finish while started service remained alive");
                event("alive after Activity destruction");
                stopSelf();
            }, 700);
            return START_NOT_STICKY;
        }
        public void onDestroy() { event("destroy after-activity"); }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        activity = this;
        setContentView(new SurfaceView(this));
    }
    @Override public void onResume() {
        super.onResume();
        main.post(this::begin);
    }
    private void begin() {
        check(getPackageManager().resolveService(intent(Lifecycle.class), 0).serviceInfo.name
                .equals(Lifecycle.class.getName()), "explicit service resolution failed");
        check(getPackageManager().queryIntentServices(intent(Lifecycle.class), 0).size() == 1,
                "explicit service query failed");
        check(new Peeker().get(this, binding("rebind")) == null, "peekService started a service");
        Intent foreign = new Intent("missing.action").setPackage("org.bifrost.missing");
        Client absent = new Client();
        check(getPackageManager().queryIntentServices(foreign, 0).isEmpty()
                && getPackageManager().resolveService(foreign, 0) == null,
                "absent package resolved a service");
        check(startService(foreign) == null && !bindService(foreign, absent, BIND_AUTO_CREATE),
                "absent package started/bound a service");
        unbindService(absent);
        check(startService(intent(Lifecycle.class).setClassName(getPackageName(), getPackageName() + ".Missing")) == null,
                "missing service started");
        check(startService(intent(Lifecycle.class).setClassName(getPackageName(), Lifecycle.class.getName() + "Disabled")) == null,
                "disabled service started");
        try {
            startService(intent(Lifecycle.class).setClassName(getPackageName(), Lifecycle.class.getName() + "Remote"));
            throw new AssertionError("remote-process service silently ran locally");
        } catch (UnsupportedOperationException expected) { checks++; }
        try {
            startForegroundService(intent(Lifecycle.class));
            throw new AssertionError("unsupported foreground service reported success");
        } catch (UnsupportedOperationException expected) { checks++; }
        Intent start = intent(Lifecycle.class).putExtra("value", 1);
        check(startService(start).getClassName().equals(Lifecycle.class.getName()), "start result incorrect");
        start.putExtra("value", 99);
        startService(intent(Lifecycle.class).putExtra("value", 2));
        await(() -> !Lifecycle.instances.isEmpty() && Lifecycle.instances.get(0).starts == 2, this::bindClients);
    }
    private void bindClients() {
        Lifecycle service = Lifecycle.instances.get(0);
        check(Lifecycle.instances.size() == 1, "repeated start created multiple instances");
        Client first = new Client(), second = new Client();
        Intent original = binding("rebind");
        check(bindService(original, first, BIND_AUTO_CREATE), "first bind failed");
        original.putExtra("origin", "changed");
        check(bindService(binding("rebind").putExtra("ignored", 2), second, BIND_AUTO_CREATE), "second bind failed");
        await(() -> first.connected == 1 && second.connected == 1, () -> {
            check(service.binds == 1 && first.binder == second.binder, "shared binding called onBind twice");
            check(((LocalBinder) first.binder).ping(41) == 42, "local Binder interaction failed");
            check(new Peeker().get(this, binding("rebind")) == first.binder, "peekService lost published Binder");
            android.os.Parcel data = android.os.Parcel.obtain(), reply = android.os.Parcel.obtain();
            try {
                data.writeInt(41);
                check(first.binder.transact(42, data, reply, 0) && reply.readInt() == 42,
                        "service Binder transaction failed");
            } catch (android.os.RemoteException error) { throw new AssertionError(error); }
            finally { data.recycle(); reply.recycle(); }
            unbindService(first);
            check(service.unbinds == 0, "onUnbind before last client");
            unbindService(second);
            Client rebound = new Client();
            check(bindService(binding("rebind"), rebound, BIND_AUTO_CREATE), "bind during pending unbind failed");
            await(() -> rebound.connected == 1 && service.rebinds == 1, () -> distinctBinding(service, rebound));
        });
    }
    private void distinctBinding(Lifecycle service, Client rebound) {
        check(service.binds == 1 && service.unbinds == 1, "rebind re-ran onBind or lost onUnbind");
        Client other = new Client();
        check(bindService(binding("cached"), other, BIND_AUTO_CREATE), "distinct binding failed");
        await(() -> other.connected == 1, () -> {
            check(service.binds == 2 && other.binder != rebound.binder, "distinct intent filters shared binding");
            IBinder cached = other.binder;
            unbindService(other);
            await(() -> service.unbinds == 2, () -> {
                Client again = new Client();
                bindService(binding("cached"), again, BIND_AUTO_CREATE);
                await(() -> again.connected == 1, () -> {
                    check(again.binder == cached && service.binds == 2 && service.rebinds == 1,
                            "onUnbind(false) lost cached Binder");
                    ComponentName component = new ComponentName(this, Lifecycle.class);
                    getPackageManager().setComponentEnabledSetting(component,
                            android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                            android.content.pm.PackageManager.DONT_KILL_APP);
                    check(stopService(intent(Lifecycle.class)), "stopService failed");
                    getPackageManager().setComponentEnabledSetting(component,
                            android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                            android.content.pm.PackageManager.DONT_KILL_APP);
                    main.postDelayed(() -> {
                        check(!service.destroyed, "stopService destroyed auto-bound service");
                        unbindService(again);
                        unbindService(rebound);
                        await(() -> service.destroyed, this::boundOnly);
                    }, 100);
                });
            });
        });
    }
    private void boundOnly() {
        Client client = new Client();
        check(bindService(binding("rebind"), client, BIND_AUTO_CREATE), "bound-only create failed");
        await(() -> client.connected == 1, () -> {
            check(Lifecycle.instances.size() == 2, "destroyed service instance reused");
            Lifecycle service = ((LocalBinder) client.binder).service;
            check(service.starts == 0, "bound-only service received onStartCommand");
            unbindService(client);
            await(() -> service.destroyed, this::dormantBinding);
        });
    }
    private void dormantBinding() {
        Client client = new Client();
        int instances = Lifecycle.instances.size();
        check(bindService(binding("dormant"), client, 0), "non-auto bind registration failed");
        main.postDelayed(() -> {
            check(Lifecycle.instances.size() == instances && client.connected == 0,
                    "non-auto bind created a service");
            startService(intent(Lifecycle.class).putExtra("value", 1));
            await(() -> client.connected == 1 && ((LocalBinder) client.binder).service.starts == 1, () -> {
                Lifecycle service = ((LocalBinder) client.binder).service;
                check(stopService(intent(Lifecycle.class)), "non-auto bound service stop failed");
                await(() -> service.destroyed && client.disconnected == 1 && client.died == 1, () -> {
                    checks++;
                    unbindService(client);
                    immediateUnbind();
                });
            });
        }, 100);
    }
    private void immediateUnbind() {
        Client client = new Client();
        int instances = Lifecycle.instances.size();
        check(bindService(binding("pending"), client, BIND_AUTO_CREATE), "pending bind failed");
        unbindService(client);
        await(() -> Lifecycle.instances.size() == instances + 1
                && Lifecycle.instances.get(instances).destroyed, () -> {
            check(client.connected == 0 && client.nullBindings == 0, "callback delivered after immediate unbind");
            nullBinding();
        });
    }
    private void nullBinding() {
        Client client = new Client();
        check(bindService(intent(NullBinding.class), client, BIND_AUTO_CREATE), "null-return binding failed");
        await(() -> client.nullBindings == 1, () -> {
            check(client.connected == 0, "null binder reported onServiceConnected");
            unbindService(client);
            await(() -> NullBinding.destroyed, this::selfStop);
        });
    }
    private void selfStop() {
        startService(intent(SelfStopping.class));
        startService(intent(SelfStopping.class));
        await(() -> SelfStopping.destroyed, () -> {
            check(SelfStopping.starts == 2, "stopSelfResult dropped queued starts");
            check(!stopService(intent(SelfStopping.class)), "destroyed service still running");
            startService(intent(AfterActivity.class));
            System.out.println("SERVICE_EXECUTION_OK checks=" + checks);
            finish();
        });
    }
    @Override public void onDestroy() {
        super.onDestroy();
        activityDestroyed = true;
        event("Activity destroyed");
    }
}
