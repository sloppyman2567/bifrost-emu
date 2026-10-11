// Fresh ART service processes, controlled through the realm's real Binder transport.
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.ServiceInfo;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.Process;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import org.json.JSONObject;

final class FrameworkProcessRuntime {
    private static final String DESCRIPTOR = "org.asgard.ServiceProcesses";
    private static final int BOOTSTRAP = 1, READY = 2, PUBLISH = 3, DONE = 4, UNBIND = 5;
    private final FrameworkPackages packages;
    private final FrameworkServiceRuntime services;
    private final Map<String, Child> children = new HashMap<>();
    private Handler main;
    private String registryName;

    static final class Child {
        final String key = UUID.randomUUID().toString().replace("-", "");
        final ServiceInfo info;
        final List<Runnable> pending = new ArrayList<>();
        final String processName;
        int pid, uid;
        Object applicationThread;
        boolean dead;
        Child(ServiceInfo info, String processName) { this.info = info; this.processName = processName; }
    }

    FrameworkProcessRuntime(FrameworkPackages packages, FrameworkServiceRuntime services) {
        this.packages = packages;
        this.services = services;
    }

    static void binderPool() throws Exception {
        Thread thread = new Thread(() -> {
            try {
                Class.forName("com.android.internal.os.BinderInternal").getMethod("joinThreadPool").invoke(null);
            } catch (ReflectiveOperationException error) {
                throw new IllegalStateException("Asgard Binder thread failed", error);
            }
        }, "AsgardBinder");
        thread.setDaemon(true);
        thread.start();
    }

    private static JSONObject host(JSONObject request) throws Exception {
        String token = System.getenv("ASGARD_PROCESS_TOKEN");
        if (token == null) throw new IllegalStateException("Android process supervisor is unavailable");
        request.put("token", token);
        try (LocalSocket socket = new LocalSocket()) {
            socket.connect(new LocalSocketAddress("/dev/socket/asgard_process", LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(5000);
            byte[] payload = request.toString().getBytes(StandardCharsets.UTF_8);
            DataOutputStream output = new DataOutputStream(socket.getOutputStream());
            output.writeInt(payload.length); output.write(payload); output.flush();
            DataInputStream input = new DataInputStream(socket.getInputStream());
            int length = input.readInt();
            if (length <= 0 || length > 8192) throw new IllegalStateException("Invalid process response");
            payload = new byte[length]; input.readFully(payload);
            JSONObject response = new JSONObject(new String(payload, StandardCharsets.UTF_8));
            if (response.has("error")) throw new IllegalStateException(response.getString("error"));
            return response;
        }
    }

    private void register() throws Exception {
        if (registryName != null) return;
        main = new Handler(android.os.Looper.getMainLooper());
        binderPool();
        registryName = "asgard.process." + Process.myPid();
        Binder controller = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                    throws android.os.RemoteException {
                if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
                data.enforceInterface(DESCRIPTOR);
                Child child;
                synchronized (FrameworkProcessRuntime.this) {
                    child = children.get(data.readString());
                    if (child == null || child.dead || child.pid != Binder.getCallingPid()
                            || child.uid != Binder.getCallingUid())
                        throw new SecurityException("Foreign Android service process");
                }
                try {
                    switch (code) {
                        case BOOTSTRAP: {
                            PackageInfo pkg = packages.processPackage(child.info.packageName);
                            if (pkg == null) {
                                pkg = new PackageInfo();
                                pkg.packageName = child.info.packageName;
                                pkg.applicationInfo = child.info.applicationInfo;
                            }
                            pkg.applicationInfo = new android.content.pm.ApplicationInfo(child.info.applicationInfo);
                            pkg.applicationInfo.processName = child.processName;
                            pkg.applicationInfo.uid = child.uid;
                            if (child.uid >= 99000) {
                                pkg.applicationInfo.dataDir = "/data/misc/asgard/" + child.key;
                                ManagedLauncher.setField(pkg.applicationInfo, "credentialProtectedDataDir", pkg.applicationInfo.dataDir);
                                ManagedLauncher.setField(pkg.applicationInfo, "deviceProtectedDataDir", pkg.applicationInfo.dataDir);
                            }
                            reply.writeNoException();
                            reply.writeTypedObject(pkg, 0);
                            reply.writeTypedObject(packages.systemPackage("com.android.webview"), 0);
                            return true;
                        }
                        case READY: {
                            IBinder endpoint = data.readStrongBinder();
                            Object thread = Class.forName("android.app.IApplicationThread$Stub")
                                    .getMethod("asInterface", IBinder.class).invoke(null, endpoint);
                            endpoint.linkToDeath(() -> died(child), 0);
                            synchronized (FrameworkProcessRuntime.this) { child.applicationThread = thread; }
                            main.post(() -> {
                                List<Runnable> pending;
                                synchronized (FrameworkProcessRuntime.this) {
                                    pending = new ArrayList<>(child.pending); child.pending.clear();
                                }
                                for (Runnable request : pending) request.run();
                            });
                            System.out.println("ASGARD_PROCESS_READY pid=" + child.pid + " uid=" + child.uid
                                    + " process=" + child.processName);
                            break;
                        }
                        case PUBLISH: services.publish(data.readStrongBinder(),
                                data.readTypedObject(Intent.CREATOR), data.readStrongBinder()); break;
                        case DONE: services.done(data.readStrongBinder(), data.readInt(), data.readTypedObject(Intent.CREATOR)); break;
                        case UNBIND: services.unbindFinished(data.readStrongBinder(),
                                data.readTypedObject(Intent.CREATOR), data.readBoolean()); break;
                        default: return false;
                    }
                    reply.writeNoException();
                    return true;
                } catch (RuntimeException error) { throw error; }
                catch (Exception error) { throw new IllegalStateException("Service process callback", error); }
            }
        };
        Class.forName("android.os.ServiceManager").getMethod("addService", String.class, IBinder.class)
                .invoke(null, registryName, controller);
    }

    synchronized Child start(ServiceInfo info, String instance) throws Exception {
        register();
        boolean isolated = (info.flags & ServiceInfo.FLAG_ISOLATED_PROCESS) != 0;
        String process = info.processName == null ? info.packageName : info.processName;
        if (instance != null) process += ":" + instance;
        for (Child child : children.values())
            if (!child.dead && child.processName.equals(process) && child.info.packageName.equals(info.packageName)
                    && (!isolated || child.info.name.equals(info.name)))
                return child;
        Child child = new Child(info, process);
        children.put(child.key, child);
        try {
            JSONObject result = host(new JSONObject().put("operation", "spawn").put("key", child.key)
                    .put("controller", registryName).put("isolated", isolated).put("uid", info.applicationInfo.uid));
            child.pid = result.getInt("pid"); child.uid = result.getInt("uid");
            main.postDelayed(() -> {
                synchronized (FrameworkProcessRuntime.this) {
                    if (!child.dead && child.applicationThread == null) died(child);
                }
            }, 30000);
            return child;
        } catch (Exception error) { children.remove(child.key); throw error; }
    }

    void schedule(Child child, String method, Class<?>[] types, Object... args) throws Exception {
        synchronized (this) {
            if (child.dead) throw new IllegalStateException("Service process has exited");
            if (child.applicationThread == null) {
                child.pending.add(() -> {
                    try { schedule(child, method, types, args); }
                    catch (Exception failure) { died(child); }
                });
                return;
            }
        }
        Class.forName("android.app.IApplicationThread").getMethod(method, types)
                .invoke(child.applicationThread, args);
    }

    private void died(Child child) {
        synchronized (this) {
            if (child.dead) return;
            child.dead = true; child.pending.clear();
        }
        main.post(() -> {
            try {
                host(new JSONObject().put("operation", "stop").put("key", child.key));
            } catch (Exception error) { android.util.Log.w("AsgardProcesses", "Process cleanup failed", error); }
            try { services.processDied(child); }
            catch (Exception error) { android.util.Log.w("AsgardProcesses", "Service cleanup failed", error); }
            synchronized (FrameworkProcessRuntime.this) { children.remove(child.key); }
        });
    }

    void stop(Child child) { died(child); }

    static final class Remote {
        final IBinder controller;
        final String key;
        PackageInfo webViewProvider;
        Remote(String name, String key) throws Exception {
            controller = (IBinder) Class.forName("android.os.ServiceManager")
                    .getMethod("getService", String.class).invoke(null, name);
            if (controller == null) throw new IllegalStateException("Parent process controller unavailable");
            this.key = key;
        }
        Parcel request() {
            Parcel parcel = Parcel.obtain(); parcel.writeInterfaceToken(DESCRIPTOR); parcel.writeString(key);
            return parcel;
        }
        Parcel call(int code, Parcel data) throws Exception {
            Parcel reply = Parcel.obtain();
            try {
                if (!controller.transact(code, data, reply, 0)) throw new IllegalStateException("Unknown process callback");
                reply.readException(); return reply;
            } catch (Exception error) { reply.recycle(); throw error; }
            finally { data.recycle(); }
        }
        PackageInfo bootstrap() throws Exception {
            Parcel reply = call(BOOTSTRAP, request());
            try {
                PackageInfo result = reply.readTypedObject(PackageInfo.CREATOR);
                webViewProvider = reply.readTypedObject(PackageInfo.CREATOR);
                return result;
            } finally { reply.recycle(); }
        }
        void ready(Object thread) throws Exception {
            binderPool();
            Parcel data = request(); data.writeStrongBinder(((IInterface) thread).asBinder());
            call(READY, data).recycle();
        }
        void publish(IBinder token, Intent intent, IBinder binder) throws Exception {
            Parcel data = request(); data.writeStrongBinder(token); data.writeTypedObject(intent, 0);
            data.writeStrongBinder(binder); call(PUBLISH, data).recycle();
        }
        void done(IBinder token, int type, Intent intent) throws Exception {
            Parcel data = request(); data.writeStrongBinder(token); data.writeInt(type);
            data.writeTypedObject(intent, 0); call(DONE, data).recycle();
        }
        void unbind(IBinder token, Intent intent, boolean rebind) throws Exception {
            Parcel data = request(); data.writeStrongBinder(token); data.writeTypedObject(intent, 0);
            data.writeBoolean(rebind); call(UNBIND, data).recycle();
        }
    }
}
