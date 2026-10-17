// Fresh ART service processes, controlled through the realm's real Binder transport.
import android.content.Intent;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
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
    private static final int BOOTSTRAP = 1, READY = 2, PUBLISH = 3, DONE = 4, UNBIND = 5, PROVIDERS = 6,
            ACQUIRE = 7, REF = 8, PROVIDER_DIED = 9;
    // Provisioned system packages a child receives as parsed snapshots when
    // the parent has already parsed them (ManagedLauncher.installMicroG).
    static final String[] SHARED_SYSTEM_PACKAGES = {"com.google.android.gms", "com.android.vending"};
    private final FrameworkPackages packages;
    private final FrameworkServiceRuntime services;
    private final Map<String, Child> children = new HashMap<>();
    private Handler main;
    private String registryName;
    private FrameworkContent content;

    static final class Child {
        final String key = UUID.randomUUID().toString().replace("-", "");
        // The component whose start created the process (a service or provider).
        final ComponentInfo info;
        final List<Runnable> pending = new ArrayList<>();
        final String processName;
        int pid, uid;
        Object applicationThread;
        volatile boolean dead;
        Child(ComponentInfo info, String processName) { this.info = info; this.processName = processName; }
    }

    FrameworkProcessRuntime(FrameworkPackages packages, FrameworkServiceRuntime services) {
        this.packages = packages;
        this.services = services;
    }

    void content(FrameworkContent content) { this.content = content; }

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
                            List<PackageInfo> provisioned = new ArrayList<>();
                            for (String name : SHARED_SYSTEM_PACKAGES) {
                                PackageInfo system = packages.loadedSystemPackage(name);
                                if (system != null) provisioned.add(system);
                            }
                            reply.writeTypedList(provisioned);
                            // A child hosting another package (microG) knows
                            // the session app that calls it.
                            PackageInfo session = packages.installedPackage();
                            reply.writeTypedObject(session == null
                                    || session.packageName.equals(child.info.packageName) ? null : session, 0);
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
                        case PROVIDERS: content.publishRemote(child, data); break;
                        case ACQUIRE: {
                            // The child resolved the provider itself; the session
                            // re-checks it for the child's uid before starting its host.
                            String authority = data.readString();
                            boolean stable = data.readBoolean();
                            ProviderInfo info = packages.resolveProvider(authority, PackageManager.GET_SHARED_LIBRARY_FILES
                                    | PackageManager.GET_URI_PERMISSION_PATTERNS, child.uid / 100000);
                            IBinder provider = null, connection = null;
                            if (info != null) {
                                packages.enforceProviderPermission(info, child.uid);
                                provider = content.sessionProvider(authority);
                                if (provider == null) {
                                    Object holder = content.acquireRemote(info, authority, stable);
                                    if (holder != null) {
                                        provider = ((IInterface) holder.getClass().getField("provider").get(holder)).asBinder();
                                        connection = (IBinder) holder.getClass().getField("connection").get(holder);
                                    }
                                }
                            }
                            reply.writeNoException();
                            reply.writeStrongBinder(provider);
                            reply.writeStrongBinder(connection);
                            return true;
                        }
                        case REF: {
                            boolean live = content.refProvider(data.readStrongBinder(), data.readInt(), data.readInt());
                            reply.writeNoException();
                            reply.writeBoolean(live);
                            return true;
                        }
                        case PROVIDER_DIED: content.unstableProviderDied(data.readStrongBinder()); break;
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

    synchronized Child start(ComponentInfo info, String instance) throws Exception {
        register();
        boolean isolated = info instanceof ServiceInfo
                && (((ServiceInfo) info).flags & ServiceInfo.FLAG_ISOLATED_PROCESS) != 0;
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
        // Provider waiters may block the main thread: wake them directly.
        if (content != null) content.processDied(child);
        main.post(() -> {
            try {
                host(new JSONObject().put("operation", "stop").put("key", child.key));
            } catch (Exception error) { android.util.Log.w("AsgardProcesses", "Process cleanup failed", error); }
            try { services.processDied(child); }
            catch (Exception error) { android.util.Log.w("AsgardProcesses", "Service cleanup failed", error); }
            synchronized (FrameworkProcessRuntime.this) { children.remove(child.key); }
        });
    }

    // A process whose providers have clients stays cached, as LRU-ranked
    // provider hosts do; the session's exit stops it.
    void lost(Child child) { died(child); }

    void stop(Child child) {
        if (content == null || !content.hosting(child)) died(child);
    }

    static final class Remote {
        final IBinder controller;
        final String key;
        PackageInfo webViewProvider;
        List<PackageInfo> systemPackages;
        PackageInfo session;
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
                systemPackages = reply.createTypedArrayList(PackageInfo.CREATOR);
                session = reply.readTypedObject(PackageInfo.CREATOR);
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
        void publishProviders(List<?> holders) throws Exception {
            Parcel data = request();
            data.writeInt(holders.size());
            for (Object holder : holders) {
                ProviderInfo info = (ProviderInfo) holder.getClass().getField("info").get(holder);
                data.writeString(info.name);
                data.writeStrongBinder(((IInterface) holder.getClass().getField("provider").get(holder)).asBinder());
            }
            call(PROVIDERS, data).recycle();
        }
        IBinder[] acquireProvider(String authority, boolean stable) throws Exception {
            Parcel data = request(); data.writeString(authority); data.writeBoolean(stable);
            Parcel reply = call(ACQUIRE, data);
            try { return new IBinder[] {reply.readStrongBinder(), reply.readStrongBinder()}; }
            finally { reply.recycle(); }
        }
        boolean refProvider(IBinder connection, int stable, int unstable) throws Exception {
            Parcel data = request(); data.writeStrongBinder(connection); data.writeInt(stable); data.writeInt(unstable);
            Parcel reply = call(REF, data);
            try { return reply.readBoolean(); } finally { reply.recycle(); }
        }
        void providerDied(IBinder connection) throws Exception {
            Parcel data = request(); data.writeStrongBinder(connection); call(PROVIDER_DIED, data).recycle();
        }
    }
}
