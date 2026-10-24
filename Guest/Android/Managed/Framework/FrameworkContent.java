// Content notifications used by real ContentObservers and JobScheduler triggers.
import android.net.Uri;
import android.app.Application;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.ProviderInfo;
import android.os.IBinder;
import android.os.IInterface;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Map;
import java.util.LinkedHashMap;

final class FrameworkContent {
    // ActivityThread owns provider instances and its local authority cache.
    // This registry supplies the ActivityManager publication contract only;
    // remote-process providers are not instantiated in the caller's process.
    private final Map<String, ProviderInfo> localProviders = new LinkedHashMap<>();
    private final Map<String, Object> publishedProviders = new LinkedHashMap<>();
    private IBinder applicationThread;
    private String packageName;

    void installProviders(Object thread, Application application, PackageInfo pkg) throws Exception {
        packageName = pkg.packageName;
        applicationThread = ((IInterface) thread.getClass().getMethod("getApplicationThread")
                .invoke(thread)).asBinder();
        List<ProviderInfo> providers = new ArrayList<>();
        String process = Application.getProcessName();
        if (pkg.applicationInfo.enabled && pkg.providers != null) {
            for (ProviderInfo info : pkg.providers) {
                if (!info.enabled || !Objects.equals(process,
                        info.processName == null ? pkg.packageName : info.processName)) continue;
                if (info.authority == null || info.authority.isEmpty())
                    throw new IllegalArgumentException("Provider has no authority: " + info.name);
                providers.add(info);
                for (String authority : info.authority.split(";")) {
                    if (localProviders.putIfAbsent(authority, info) != null)
                        throw new IllegalArgumentException("Duplicate provider authority: " + authority);
                }
            }
        }
        // ActivityManager normally orders this list before binding the app.
        providers.sort((a, b) -> Integer.compare(b.initOrder, a.initOrder));
        if (providers.isEmpty()) return;
        java.lang.reflect.Method install = thread.getClass().getDeclaredMethod(
                "installContentProviders", Context.class, List.class);
        install.setAccessible(true);
        install.invoke(thread, application, providers);
        System.out.println("APP_PROVIDERS_STARTED " + providers.size());
    }

    void remote(FrameworkProcessRuntime.Remote remote) { this.remote = remote; }
    void processes(FrameworkProcessRuntime processes) { this.processes = processes; }

    void publishProviders(Object caller, List<?> holders) throws Exception {
        publishLocal(caller, holders);
        // A child process publishes to the session, which hands out its providers.
        if (remote != null) remote.publishProviders(holders);
    }

    private synchronized void publishLocal(Object caller, List<?> holders) throws Exception {
        if (!(caller instanceof IInterface)
                || ((IInterface) caller).asBinder() != applicationThread)
            throw new SecurityException("Foreign provider publisher");
        for (Object holder : holders) {
            ProviderInfo info = (ProviderInfo) holder.getClass().getField("info").get(holder);
            IInterface provider = (IInterface) holder.getClass().getField("provider").get(holder);
            if (info == null || info.authority == null || provider == null
                    || !Objects.equals(packageName, info.packageName))
                throw new SecurityException("Invalid local provider publication");
            for (String authority : info.authority.split(";")) {
                ProviderInfo declared = localProviders.get(authority);
                if (declared == null || !declared.name.equals(info.name))
                    throw new SecurityException("Undeclared local provider: " + authority);
                Object previous = publishedProviders.putIfAbsent(authority, holder);
                if (previous != null && previous != holder)
                    throw new IllegalStateException("Provider already published: " + authority);
            }
        }
    }

    synchronized Object publishedProvider(String authority, String caller, int user) {
        Object holder = publishedProviders.get(authority);
        if (holder != null && (user != android.os.Process.myUid() / 100000
                || !Objects.equals(packageName, caller)))
            throw new SecurityException("Foreign provider caller");
        return holder;
    }

    // ContentProviderHelper for providers hosted by child processes. Each
    // child installs its process's providers before Application.onCreate, as
    // bindApplication does, and publishes them on a Binder thread; acquirers
    // wait for that publication. One connection per provider carries this
    // client process's stable/unstable counts.
    private static final class Hosted {
        final FrameworkProcessRuntime.Child child;
        final IBinder provider;
        Hosted(FrameworkProcessRuntime.Child child, IBinder provider) { this.child = child; this.provider = provider; }
    }
    private static final class Connection {
        final android.os.Binder token = new android.os.Binder();
        final Hosted hosted;
        int stable, unstable;
        boolean dead;
        Connection(Hosted hosted) { this.hosted = hosted; }
    }
    private final Map<String, Hosted> hosted = new LinkedHashMap<>();
    private final Map<Hosted, Connection> connectionsByProvider = new LinkedHashMap<>();
    private final Map<IBinder, Connection> connections = new LinkedHashMap<>();
    private FrameworkProcessRuntime processes;
    private FrameworkProcessRuntime.Remote remote;

    private static Object holder(ProviderInfo info, Object provider, IBinder connection, boolean local, boolean noRelease)
            throws Exception {
        Class<?> type = Class.forName("android.app.ContentProviderHolder");
        Object holder = type.getConstructor(ProviderInfo.class).newInstance(new ProviderInfo(info));
        type.getField("provider").set(holder, provider);
        type.getField("connection").set(holder, connection);
        type.getField("mLocal").setBoolean(holder, local);
        type.getField("noReleaseNeeded").setBoolean(holder, noRelease);
        return holder;
    }

    // A same-uid multiprocess provider: the caller instantiates it itself
    // (ContentProviderHelper.getContentProviderImpl, canRunHere).
    static Object localHolder(ProviderInfo info) throws Exception { return holder(info, null, null, true, false); }

    // Starts (or reuses) the provider's process and waits for its publication.
    // null, as AOSP returns, when the process dies or the wait times out.
    Object acquireRemote(ProviderInfo info, String authority, boolean stable) throws Exception {
        if (remote != null) {
            // A child cannot start processes: the session acquires for it.
            IBinder[] reply = remote.acquireProvider(authority, stable);
            if (reply[0] == null) return null;
            Object provider = Class.forName("android.content.ContentProviderNative")
                    .getMethod("asInterface", IBinder.class).invoke(null, reply[0]);
            return holder(info, provider, reply[1], false, reply[1] == null);
        }
        long deadline = android.os.SystemClock.uptimeMillis() + android.content.ContentResolver.class
                .getField("CONTENT_PROVIDER_READY_TIMEOUT_MILLIS").getInt(null);
        FrameworkProcessRuntime.Child child;
        try {
            child = processes.start(info, null);
        } catch (Exception error) {
            // ContentProviderHelper: "Unable to launch app ... for provider".
            android.util.Log.w("AsgardContent", "Unable to launch " + info.packageName + " for provider " + authority, error);
            return null;
        }
        synchronized (this) {
            Hosted provider;
            while ((provider = hosted.get(child.key + "/" + info.name)) == null) {
                long wait = deadline - android.os.SystemClock.uptimeMillis();
                if (child.dead) {
                    android.util.Log.w("AsgardContent", "Provider process died before publishing " + authority);
                    return null;
                }
                if (wait <= 0) {
                    android.util.Log.wtf("AsgardContent", "Timeout waiting for provider "
                            + info.packageName + "/" + info.applicationInfo.uid + " for provider " + authority);
                    return null;
                }
                wait(wait);
            }
            Connection connection = connectionsByProvider.get(provider);
            if (connection == null) {
                connection = new Connection(provider);
                connectionsByProvider.put(provider, connection);
                connections.put(connection.token, connection);
            }
            if (stable) connection.stable++; else connection.unstable++;
            Object binder = Class.forName("android.content.ContentProviderNative")
                    .getMethod("asInterface", IBinder.class).invoke(null, provider.provider);
            return holder(info, binder, connection.token, false, false);
        }
    }

    // PROVIDERS from a child: provider class names and their Binders. The
    // session builds each holder from its own manifest record (the child's
    // ProviderInfo copies carry its whole ApplicationInfo, megabytes for
    // GmsCore); a name the session never resolves is never handed out.
    synchronized void publishRemote(FrameworkProcessRuntime.Child child, android.os.Parcel data) {
        int count = data.readInt();
        if (count < 0 || count > 1024) throw new SecurityException("Invalid child provider publication");
        for (int i = 0; i < count; i++) {
            String name = data.readString();
            IBinder binder = data.readStrongBinder();
            if (name == null || binder == null) throw new SecurityException("Invalid child provider publication");
            hosted.put(child.key + "/" + name, new Hosted(child, binder));
        }
        notifyAll();
    }

    synchronized void processDied(FrameworkProcessRuntime.Child child) {
        hosted.values().removeIf(entry -> entry.child == child);
        for (Connection connection : connections.values())
            if (connection.hosted.child == child) connection.dead = true;
        notifyAll();
    }

    synchronized boolean hosting(FrameworkProcessRuntime.Child child) {
        for (Connection connection : connections.values())
            if (connection.hosted.child == child && !connection.dead
                    && connection.stable + connection.unstable > 0) return true;
        return false;
    }

    // A child's connection is the session's: forward its counts there.
    private Connection connection(Object token, String method) {
        if (token == null) throw new NullPointerException("connection is null");
        Connection connection = connections.get((IBinder) token);
        if (connection == null && remote == null)
            throw new IllegalArgumentException(method + ": " + token + " not a ContentProviderConnection");
        return connection;
    }

    boolean refProvider(Object token, int stable, int unstable) throws Exception {
        synchronized (this) {
            Connection connection = connection(token, "refContentProvider");
            if (connection != null) {
                connection.stable = Math.max(0, connection.stable + stable);
                connection.unstable = Math.max(0, connection.unstable + unstable);
                if (connection.stable + connection.unstable == 0) processes.idleLater(connection.hosted.child);
                return !connection.dead;
            }
        }
        return remote.refProvider((IBinder) token, stable, unstable);
    }

    void removeProvider(Object token, boolean stable) throws Exception {
        synchronized (this) {
            Connection connection = connection(token, "removeContentProvider");
            if (connection != null) {
                if (stable && connection.stable > 0) connection.stable--;
                else if (!stable && connection.unstable > 0) connection.unstable--;
                if (connection.stable + connection.unstable == 0) processes.idleLater(connection.hosted.child);
                return;
            }
        }
        remote.refProvider((IBinder) token, stable ? -1 : 0, stable ? 0 : -1);
    }

    // The client saw a DeadObjectException; believe it only if the binder is dead.
    void unstableProviderDied(Object token) throws Exception {
        Connection connection;
        synchronized (this) { connection = connection(token, "unstableProviderDied"); }
        if (connection == null) { remote.providerDied((IBinder) token); return; }
        if (connection.hosted.provider.pingBinder()) return;
        processes.lost(connection.hosted.child);
    }

    // A provider the session app itself published, for a child's acquisition.
    synchronized IBinder sessionProvider(String authority) throws Exception {
        Object holder = publishedProviders.get(authority);
        return holder == null ? null
                : ((IInterface) holder.getClass().getField("provider").get(holder)).asBinder();
    }

    private final List<Observer> observers = new ArrayList<>();
    private final class Observer implements IBinder.DeathRecipient {
        final Uri uri;
        final boolean descendants;
        final IInterface endpoint;
        Observer(Uri uri, boolean descendants, IInterface endpoint) {
            this.uri = uri; this.descendants = descendants; this.endpoint = endpoint;
        }
        @Override public void binderDied() {
            synchronized (observers) { observers.remove(this); }
            endpoint.asBinder().unlinkToDeath(this, 0);
        }
        boolean matches(Uri changed, int flags) {
            if (!Objects.equals(uri.getScheme(), changed.getScheme())
                    || !Objects.equals(uri.getAuthority(), changed.getAuthority())) return false;
            List<String> prefix = uri.getPathSegments(), path = changed.getPathSegments();
            int common = Math.min(prefix.size(), path.size());
            if (!path.subList(0, common).equals(prefix.subList(0, common))) return false;
            if (prefix.size() < path.size()) return descendants;
            return !descendants || (flags & android.content.ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) == 0;
        }
    }
    private static void user(int user) {
        if (user != 0 && user != -2) throw new SecurityException("Foreign content user");
    }
    Object call(String method, Object[] a) throws Exception {
        switch (method) {
            case "registerContentObserver": {
                user((Integer) a[3]);
                if (a[0] == null || a[2] == null) throw new IllegalArgumentException("Missing content URI or observer");
                Observer observer = new Observer((Uri) a[0], (Boolean) a[1], (IInterface) a[2]);
                synchronized (observers) {
                    if (observers.size() >= 4096) throw new IllegalStateException("Content observer limit reached");
                    observer.endpoint.asBinder().linkToDeath(observer, 0); observers.add(observer);
                }
                return null;
            }
            case "unregisterContentObserver": {
                if (a[0] == null) throw new IllegalArgumentException("Missing content observer");
                IBinder binder = ((IInterface) a[0]).asBinder();
                synchronized (observers) {
                    for (Observer observer : new ArrayList<>(observers)) if (observer.endpoint.asBinder() == binder) {
                        observers.remove(observer); binder.unlinkToDeath(observer, 0);
                    }
                }
                return null;
            }
            case "notifyChange": {
                user((Integer) a[4]);
                Uri[] changes = (Uri[]) a[0];
                if (changes == null) throw new IllegalArgumentException("Missing changed URIs");
                for (Uri uri : changes) if (uri == null) throw new IllegalArgumentException("Null changed URI");
                IBinder origin = a[1] == null ? null : ((IInterface) a[1]).asBinder();
                List<Observer> snapshot;
                synchronized (observers) { snapshot = new ArrayList<>(observers); }
                for (Observer observer : snapshot) {
                    boolean self = observer.endpoint.asBinder() == origin;
                    if (self && !(Boolean) a[2]) continue;
                    List<Uri> matches = new ArrayList<>();
                    for (Uri uri : changes) {
                        if (observer.matches(uri, (Integer) a[3])) matches.add(uri);
                    }
                    if (!matches.isEmpty()) Class.forName("android.database.IContentObserver")
                            .getMethod("onChangeEtc", boolean.class, Uri[].class, int.class, int.class)
                            .invoke(observer.endpoint, self, matches.toArray(new Uri[0]), (Integer) a[3], 0);
                }
                return null;
            }
            default: throw new UnsupportedOperationException("content." + method);
        }
    }
}
