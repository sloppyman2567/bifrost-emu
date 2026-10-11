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
    public static final class LifecycleRemote extends Service {
        @Override public IBinder onBind(Intent intent) {
            return new Binder() {
                @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags) {
                    if (code != 42) return false;
                    reply.writeInt(android.os.Process.myPid()); reply.writeInt(android.os.Process.myUid());
                    reply.writeInt(data.readInt() + 1);
                    return true;
                }
            };
        }
    }
    public static final class Jobs extends android.app.job.JobService {
        static int starts, stops, work, triggers;
        @Override public boolean onStartJob(android.app.job.JobParameters params) {
            event("job-start " + params.getJobNamespace() + ":" + params.getJobId()); starts++;
            if (params.getJobId() == 12) return true;
            if (params.getJobId() == 14) {
                require(params.getTriggeredContentUris() != null && params.getTriggeredContentUris().length == 1
                        && params.getTriggeredContentUris()[0].toString().equals("content://org.bifrost.fixture/jobs/1"),
                        "content trigger URI was not delivered");
                require(params.getTriggeredContentAuthorities() != null
                        && params.getTriggeredContentAuthorities()[0].equals("org.bifrost.fixture"),
                        "content trigger authority was not delivered");
                triggers++;
            }
            if (params.getJobId() == 11) {
                android.app.job.JobWorkItem item;
                while ((item = params.dequeueWork()) != null) {
                    require(item.getDeliveryCount() == 1, "work delivery count incorrect");
                    require(item.getIntent().getIntExtra("value", -1) == ++work, "work order incorrect");
                    params.completeWork(item);
                }
                android.app.job.JobParameters current = params;
                new Handler().postDelayed(() -> jobFinished(current, false), 50);
                return true;
            }
            return false;
        }
        @Override public boolean onStopJob(android.app.job.JobParameters params) {
            event("job-stop " + params.getJobId()); stops++;
            require(params.getStopReason() == android.app.job.JobParameters.STOP_REASON_CANCELLED_BY_APP,
                    "wrong cancellation reason");
            return false;
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        activity = this;
        setContentView(new SurfaceView(this));
    }
    @Override public void onResume() {
        super.onResume();
        main.post(this::jobChecks);
    }
    private void jobChecks() {
        android.app.job.JobScheduler scheduler = getSystemService(android.app.job.JobScheduler.class);
        android.app.job.JobScheduler named = scheduler.forNamespace("fixture");
        ComponentName service = new ComponentName(this, Jobs.class);
        boolean second = getPreferences(0).getBoolean("jobsSaved", false);
        check((scheduler.getPendingJob(900) != null) == second, "persisted job did not survive restart");
        android.app.job.JobInfo saved = new android.app.job.JobInfo.Builder(900, service)
                .setMinimumLatency(3600000).setPersisted(true).build();
        check(scheduler.schedule(saved) == android.app.job.JobScheduler.RESULT_SUCCESS, "persisted schedule failed");
        getPreferences(0).edit().putBoolean("jobsSaved", true).commit();
        android.app.job.JobInfo sync = new android.app.job.JobInfo.Builder(10, service).setMinimumLatency(1).build();
        check(scheduler.schedule(sync) == 1 && named.schedule(sync) == 1, "namespaced schedule failed");
        check(scheduler.getPendingJob(10) != null && named.getPendingJob(10) != null, "namespaces collided");
        check(scheduler.getPendingJobsInAllNamespaces().size() == 2, "namespace inventory incorrect");
        check(scheduler.getPendingJobReason(12345) == android.app.job.JobScheduler.PENDING_JOB_REASON_INVALID_JOB_ID,
                "missing job reason incorrect");
        try {
            scheduler.schedule(new android.app.job.JobInfo.Builder(9, new ComponentName(this, Lifecycle.class))
                    .setMinimumLatency(1).build());
            throw new AssertionError("service without BIND_JOB_SERVICE was accepted");
        } catch (IllegalArgumentException expected) { checks++; }
        android.app.job.JobInfo work = new android.app.job.JobInfo.Builder(11, service).setMinimumLatency(1).build();
        check(scheduler.enqueue(work, new android.app.job.JobWorkItem(new Intent().putExtra("value", 1))) == 1,
                "first work enqueue failed");
        check(scheduler.enqueue(work, new android.app.job.JobWorkItem(new Intent().putExtra("value", 2))) == 1,
                "second work enqueue failed");
        scheduler.schedule(new android.app.job.JobInfo.Builder(12, service).setMinimumLatency(1).build());
        await(() -> Jobs.starts == 4 && Jobs.work == 2 && scheduler.getPendingJob(10) == null
                && named.getPendingJob(10) == null && scheduler.getPendingJob(11) == null, () -> {
            check(scheduler.getPendingJobReason(12) == android.app.job.JobScheduler.PENDING_JOB_REASON_EXECUTING,
                    "running job reason incorrect");
            scheduler.cancel(12);
            await(() -> Jobs.stops == 1, () -> {
                check(scheduler.getPendingJob(12) == null && scheduler.getPendingJob(900) != null,
                        "cancel removed wrong job");
                android.net.Uri uri = android.net.Uri.parse("content://org.bifrost.fixture/jobs");
                scheduler.schedule(new android.app.job.JobInfo.Builder(14, service)
                        .addTriggerContentUri(new android.app.job.JobInfo.TriggerContentUri(uri,
                                android.app.job.JobInfo.TriggerContentUri.FLAG_NOTIFY_FOR_DESCENDANTS))
                        .setTriggerContentUpdateDelay(500).setTriggerContentMaxDelay(1000).build());
                check(scheduler.getPendingJobReason(14) == android.app.job.JobScheduler.PENDING_JOB_REASON_CONSTRAINT_CONTENT_TRIGGER,
                        "content job started without a notification");
                getContentResolver().notifyChange(android.net.Uri.parse("content://org.bifrost.fixture/other"), null, 0);
                getContentResolver().notifyChange(uri, null, android.content.ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS);
                main.postDelayed(() -> {
                    check(Jobs.triggers == 0, "unmatched or skipped notification triggered a job");
                    getContentResolver().notifyChange(android.net.Uri.withAppendedPath(uri, "1"), null, 0);
                    await(() -> Jobs.triggers == 1 && scheduler.getPendingJob(14) == null, () -> {
                        System.out.println("JOB_SCHEDULER_OK restart=" + second + " checks=" + checks);
                        remoteCheck();
                    });
                }, 1100);
            });
        });
    }
    private void remoteCheck() {
        Client remote = new Client();
        check(bindService(intent(LifecycleRemote.class), remote, BIND_AUTO_CREATE), "remote service bind failed");
        await(() -> remote.connected == 1, () -> {
            android.os.Parcel data = android.os.Parcel.obtain(), reply = android.os.Parcel.obtain();
            try {
                data.writeInt(41);
                check(remote.binder.transact(42, data, reply, 0), "remote Binder transaction failed");
                check(reply.readInt() != android.os.Process.myPid(), "remote service ran in the app process");
                check(reply.readInt() == android.os.Process.myUid() && reply.readInt() == 42,
                        "remote UID or Binder result incorrect");
                System.out.println("SERVICE_PROCESS_OK");
            } catch (android.os.RemoteException error) { throw new AssertionError(error); }
            finally { data.recycle(); reply.recycle(); }
            unbindService(remote);
            begin();
        });
    }
    private void begin() {
        require(android.os.Process.myUid() == 10000 && getApplicationInfo().uid == 10000,
                "installed app has privileged/mismatched UID");
        // Both provisioned and provider-free profiles must respect app identity.
        if (!getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_WEBVIEW)) {
            try {
                new android.webkit.WebView(this).destroy();
                throw new AssertionError("WebView constructed without a provisioned provider");
            } catch (UnsupportedOperationException expected) {
                require(expected.getMessage() == null || !expected.getMessage().contains("privileged"),
                        "WebView rejected app UID");
            }
        }
        System.out.println("MANAGED_APP_IDENTITY_OK uid=" + android.os.Process.myUid());
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
