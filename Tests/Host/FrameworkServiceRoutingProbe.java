// Real ART/Binder regression: a service child binds another supervised process.
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class FrameworkServiceRoutingProbe {
    private static final String NAME = "org.bifrost.routing";
    private static volatile boolean rootDestroyed;
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static Intent intent(Context context, Class<?> type) { return new Intent(context, type); }
    private static File marker(Context context) { return new File(context.getFilesDir(), "helper-destroyed"); }
    public static final class Root extends Service {
        public IBinder onBind(Intent i) { return new Binder(); }
        public int onStartCommand(Intent i, int flags, int id) { return START_NOT_STICKY; }
        public void onDestroy() { rootDestroyed = true; }
    }
    public static final class Helper extends Service {
        private boolean stopped;
        public int onStartCommand(Intent i, int flags, int id) {
            stopped = stopSelfResult(id);
            return START_NOT_STICKY;
        }
        public IBinder onBind(Intent i) {
            return new Binder() {
                protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    if (code != 42) return false;
                    reply.writeInt(Process.myPid()); reply.writeBoolean(stopped); return true;
                }
            };
        }
        public void onDestroy() {
            try { check(marker(this).createNewFile(), "helper destruction marker exists"); }
            catch (Exception e) { throw new IllegalStateException(e); }
        }
    }
    public static final class Child extends Service {
        private volatile boolean rootDone, helperDone, selfDone;
        private int helperPid;
        public void onCreate() {
            check(System.getenv("ASGARD_PROCESS_TOKEN") == null, "child received supervisor capability");
            try {
                ClassLoader framework = ClassLoader.getSystemClassLoader();
                Field f = framework.loadClass("ManagedLauncher").getDeclaredField("serviceRuntime"); f.setAccessible(true);
                Object runtime = f.get(null);
                Field r = runtime.getClass().getDeclaredField("remote"); r.setAccessible(true);
                Object remote = r.get(runtime);
                Method peek = remote.getClass().getDeclaredMethod("peekService", Intent.class, String.class); peek.setAccessible(true);
                try { peek.invoke(remote, intent(this, Root.class), "org.bifrost.forged");
                    throw new AssertionError("parent accepted forged caller package");
                } catch (java.lang.reflect.InvocationTargetException expected) {
                    check(expected.getCause() instanceof SecurityException, "unexpected parent rejection");
                }
            } catch (Exception e) { throw new IllegalStateException(e); }
            // Death cleanup must not send unbind callbacks back to this dead process.
            check(bindService(intent(this, Child.class), new ServiceConnection() {
                public void onServiceConnected(ComponentName n, IBinder b) { selfDone = true; }
                public void onServiceDisconnected(ComponentName n) { }
            }, BIND_AUTO_CREATE), "child bind to itself failed");
            check(startService(intent(this, Root.class)) != null, "child startService failed");
            check(bindService(intent(this, Root.class), new ServiceConnection() {
                public void onServiceConnected(ComponentName n, IBinder b) {
                    android.content.BroadcastReceiver receiver = new android.content.BroadcastReceiver() {
                        public void onReceive(Context c, Intent i) { }
                    };
                    check(receiver.peekService(Child.this, intent(Child.this, Root.class)) != null, "child peekService failed");
                    unbindService(this);
                    check(stopService(intent(Child.this, Root.class)), "child stopService failed");
                    rootDone = true;
                }
                public void onServiceDisconnected(ComponentName n) { }
            }, BIND_AUTO_CREATE), "child bind to root failed");
            check(startService(intent(this, Helper.class)) != null, "child start helper failed");
            check(bindService(intent(this, Helper.class), new ServiceConnection() {
                public void onServiceConnected(ComponentName n, IBinder b) {
                    Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                    try {
                        check(b.transact(42, data, reply, 0), "nested service Binder failed");
                        helperPid = reply.readInt();
                        check(reply.readBoolean(), "nested service stopSelfResult failed");
                        helperDone = true;
                    } catch (Exception e) { throw new IllegalStateException(e); }
                    finally { data.recycle(); reply.recycle(); }
                }
                public void onServiceDisconnected(ComponentName n) { }
            }, BIND_AUTO_CREATE), "child bind to helper failed");
        }
        public IBinder onBind(Intent i) {
            return new Binder() {
                protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    if (code == 43) {
                        new Handler(Looper.getMainLooper()).postDelayed(() -> Process.killProcess(Process.myPid()), 50);
                        reply.writeInt(0); return true;
                    }
                    if (code != 42) return false;
                    reply.writeBoolean(rootDone && helperDone && selfDone);
                    reply.writeInt(Process.myPid()); reply.writeInt(helperPid); return true;
                }
            };
        }
    }
    private static void run() throws Exception {
        Looper.prepareMainLooper();
        Class<?> type = Class.forName("android.app.ActivityThread");
        type.getMethod("initializeMainlineModules").invoke(null);
        Method install = ManagedLauncher.class.getDeclaredMethod("installServices", boolean.class);
        install.setAccessible(true); install.invoke(null, false);
        Object thread = type.getMethod("systemMain").invoke(null);
        Context system = (Context) type.getMethod("getSystemContext").invoke(thread);
        Field field = ManagedLauncher.class.getDeclaredField("packages"); field.setAccessible(true);
        FrameworkPackages packages = (FrameworkPackages) field.get(null);
        PackageInfo pkg = new PackageInfo(); pkg.packageName = NAME;
        ApplicationInfo info = pkg.applicationInfo = new ApplicationInfo();
        info.packageName = info.processName = NAME; info.uid = Process.myUid();
        info.enabled = true; info.targetSdkVersion = 35;
        info.flags = ApplicationInfo.FLAG_HAS_CODE;
        info.sourceDir = info.publicSourceDir = "/data/app/org.bifrost.routing-fixture/base.apk";
        info.nativeLibraryDir = "/system/lib64";
        info.dataDir = "/data/user/0/" + NAME; new File(info.dataDir).mkdirs();
        Class<?>[] classes = {Root.class, Child.class, Helper.class}; pkg.services = new ServiceInfo[3];
        for (int i = 0; i < 3; i++) {
            ServiceInfo service = pkg.services[i] = new ServiceInfo();
            service.name = classes[i].getName(); service.packageName = NAME; service.applicationInfo = info;
            service.enabled = true; service.exported = false; service.processName = i == 0 ? NAME : NAME + ":" + i;
        }
        packages.install(system, pkg);
        Class<?> compat = Class.forName("android.content.res.CompatibilityInfo");
        Object loaded = type.getMethod("getPackageInfoNoCheck", ApplicationInfo.class, compat)
                .invoke(thread, info, compat.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
        field = loaded.getClass().getDeclaredField("mClassLoader"); field.setAccessible(true);
        field.set(loaded, FrameworkServiceRoutingProbe.class.getClassLoader());
        Method bind = ManagedLauncher.class.getDeclaredMethod("bindApplicationMetadata", Object.class, Context.class, ApplicationInfo.class, Object.class);
        bind.setAccessible(true); bind.invoke(null, thread, system, info, loaded);
        Method make = loaded.getClass().getDeclaredMethod("makeApplicationInner", boolean.class, android.app.Instrumentation.class);
        make.setAccessible(true); Context context = (Context) make.invoke(loaded, false, null);
        field = type.getDeclaredField("mInitialApplication"); field.setAccessible(true); field.set(thread, context);
        field = ManagedLauncher.class.getDeclaredField("serviceRuntime"); field.setAccessible(true);
        FrameworkServiceRuntime runtime = (FrameworkServiceRuntime) field.get(null); runtime.attach(thread, info);
        marker(context).delete();
        new Thread(() -> {
            try {
                CountDownLatch connected = new CountDownLatch(1); IBinder[] endpoint = new IBinder[1];
                context.bindService(intent(context, Child.class), new ServiceConnection() {
                    public void onServiceConnected(ComponentName n, IBinder b) { endpoint[0] = b; connected.countDown(); }
                    public void onServiceDisconnected(ComponentName n) { }
                }, Context.BIND_AUTO_CREATE);
                check(connected.await(60, TimeUnit.SECONDS), "child did not connect");
                long deadline = android.os.SystemClock.uptimeMillis() + 60000;
                while (true) {
                    Parcel data = Parcel.obtain(), reply = Parcel.obtain(); boolean ready;
                    try {
                        check(endpoint[0].transact(42, data, reply, 0), "child Binder failed");
                        ready = reply.readBoolean(); int child = reply.readInt(), helper = reply.readInt();
                        if (ready) check(child != Process.myPid() && helper != child && helper != Process.myPid(), "services did not run in separate processes");
                    } finally { data.recycle(); reply.recycle(); }
                    if (ready && rootDestroyed) break;
                    check(android.os.SystemClock.uptimeMillis() < deadline, "nested service calls timed out");
                    Thread.sleep(20);
                }
                context.startService(intent(context, Root.class));
                // An existing unexported record must not bypass caller permission checks.
                ServiceInfo foreignInfo = new ServiceInfo(pkg.services[0]); foreignInfo.packageName = "org.bifrost.foreign";
                FrameworkProcessRuntime.Child foreign = new FrameworkProcessRuntime.Child(foreignInfo, foreignInfo.packageName);
                foreign.uid = 10001; foreign.applicationThread = new Object();
                Parcel request = Parcel.obtain(), response = Parcel.obtain();
                try {
                    request.writeTypedObject(intent(context, Root.class), 0); request.writeString(foreignInfo.packageName); request.setDataPosition(0);
                    try { runtime.fromChild(foreign, FrameworkProcessRuntime.SERVICE_PEEK, request, response);
                        throw new AssertionError("existing record bypassed export checks");
                    } catch (SecurityException expected) { }
                    foreign.uid = 99000;
                    request.setDataPosition(0);
                    try { runtime.fromChild(foreign, FrameworkProcessRuntime.SERVICE_PEEK, request, response);
                        throw new AssertionError("isolated child could route services");
                    } catch (SecurityException expected) { }
                } finally { request.recycle(); response.recycle(); }
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try { endpoint[0].transact(43, data, reply, 0); }
                finally { data.recycle(); reply.recycle(); }
                deadline = android.os.SystemClock.uptimeMillis() + 30000;
                while (!marker(context).isFile()) {
                    check(android.os.SystemClock.uptimeMillis() < deadline, "dead child's outgoing binding leaked"); Thread.sleep(20);
                }
                System.out.println("SERVICE_CHILD_ROUTING_OK"); System.out.println("FRAMEWORK_OK"); System.exit(0);
            } catch (Throwable e) { e.printStackTrace(); System.exit(1); }
        }, "routing-checks").start();
        Looper.loop();
    }
    public static void main(String[] args) {
        try { run(); } catch (Throwable e) { e.printStackTrace(); System.exit(1); }
    }
}
