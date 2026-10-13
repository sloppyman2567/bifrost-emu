// Same-app dynamic/manifest broadcasts, Android 15 IIntentReceiver protocol.
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Process;
import android.os.Parcel;
import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Objects;

final class FrameworkBroadcasts {
    private final FrameworkPackages packages;
    private final FrameworkBattery battery;
    private final List<Receiver> receivers = new ArrayList<>();
    private Object thread;
    private String owner;
    private final ArrayDeque<Broadcast> ordered = new ArrayDeque<>();
    private Broadcast active;
    private boolean advancing;

    // Local interfaces bypass Binder's Parcel boundary. Preserve the same
    // isolation for nested extras, not just the Intent/Bundle containers.
    private static Intent copy(Intent intent) {
        Parcel parcel = Parcel.obtain();
        try { intent.writeToParcel(parcel, 0); parcel.setDataPosition(0); return Intent.CREATOR.createFromParcel(parcel); }
        finally { parcel.recycle(); }
    }
    private static Bundle copy(Bundle extras) {
        if (extras == null) return null;
        Parcel parcel = Parcel.obtain();
        try { parcel.writeBundle(extras); parcel.setDataPosition(0); return parcel.readBundle(extras.getClassLoader()); }
        finally { parcel.recycle(); }
    }

    private static final class Receiver {
        final IInterface endpoint;
        final IntentFilter filter;
        final String permission;
        final int flags;
        Receiver(IInterface endpoint, IntentFilter filter, String permission, int flags) {
            this.endpoint = endpoint; this.filter = new IntentFilter(filter); this.permission = permission; this.flags = flags;
        }
    }
    private static final class Broadcast {
        final Intent intent;
        final List<Object> targets;
        final Object finished;
        int code, index;
        String data;
        Bundle extras;
        IBinder waiting;
        Broadcast(Intent intent, List<Object> targets, Object finished, int code, String data, Bundle extras) {
            this.intent = copy(intent); this.targets = targets; this.finished = finished;
            this.code = code; this.data = data; this.extras = copy(extras);
        }
    }

    FrameworkBroadcasts(FrameworkPackages packages, FrameworkBattery battery) { this.packages = packages; this.battery = battery; }
    synchronized void attach(Object thread, String owner) { this.thread = thread; this.owner = owner; }
    private static void user(int user) { if (user != 0 && user != -2) throw new SecurityException("Foreign broadcast user"); }

    static void receive(Object receiver, Intent intent, int code, String data, Bundle extras, boolean ordered, boolean sticky) throws Exception {
        Class.forName("android.content.IIntentReceiver").getMethod("performReceive", Intent.class, int.class,
                String.class, Bundle.class, boolean.class, boolean.class, int.class).invoke(receiver,
                copy(intent), code, data, copy(extras), ordered, sticky, 0);
    }

    synchronized Intent register(Object[] a) throws Exception {
        user((Integer) a[7]);
        if (packages.packageUid((String) a[1]) != Process.myUid()) throw new SecurityException("Foreign receiver package");
        IInterface endpoint = (IInterface) a[4];
        IntentFilter filter = Objects.requireNonNull((IntentFilter) a[5], "filter");
        int flags = (Integer) a[8];
        if ((flags & 6) == 6) throw new IllegalArgumentException("Both RECEIVER_EXPORTED and RECEIVER_NOT_EXPORTED");
        boolean onlyProtected = filter.countActions() != 0;
        for (int i = 0; i < filter.countActions(); i++) onlyProtected &= packages.protectedBroadcast(filter.getAction(i));
        if ((flags & 5) == 5 && packages.targetSdk() >= 34)
            throw new IllegalArgumentException("Unexported receiver visible to instant apps");
        if (endpoint != null && !onlyProtected && packages.targetSdk() >= 34 && (flags & 6) == 0)
            throw new SecurityException("Receiver export flag required");
        if (endpoint != null) {
            if (receivers.size() >= 1000) throw new IllegalStateException("Receiver limit reached");
            receivers.add(new Receiver(endpoint, filter, (String) a[6], flags));
        }
        Intent sticky = battery.stickyIntent();
        if (filter.match(null, sticky, false, "AsgardSticky") < 0) return null;
        if (endpoint != null) registered(endpoint, sticky, 0, null, null, false, true);
        return copy(sticky);
    }

    synchronized void unregister(Object endpoint) {
        IBinder binder = ((IInterface) endpoint).asBinder();
        receivers.removeIf(receiver -> receiver.endpoint.asBinder() == binder);
    }

    private boolean allowed(String permission) { return permission == null || packages.granted(permission); }

    private void registered(Object receiver, Intent intent, int code, String data, Bundle extras, boolean serial, boolean sticky) throws Exception {
        if (thread == null) { receive(receiver, intent, code, data, extras, serial, sticky); return; }
        Object app = thread.getClass().getMethod("getApplicationThread").invoke(thread);
        Class.forName("android.app.IApplicationThread").getMethod("scheduleRegisteredReceiver", Class.forName("android.content.IIntentReceiver"),
                Intent.class, int.class, String.class, Bundle.class, boolean.class, boolean.class, boolean.class,
                int.class, int.class, int.class, String.class).invoke(app, receiver, copy(intent), code, data,
                copy(extras), serial, sticky, !serial, 0, 2, Process.myUid(), owner);
    }

    synchronized int send(Intent intent, String type, Object finished, int code, String data, Bundle extras,
            String[] permissions, String[] excludedPermissions, String[] excludedPackages, boolean serial, boolean sticky, int user) throws Exception {
        user(user); Objects.requireNonNull(intent, "intent");
        if (sticky) throw new SecurityException("android.permission.BROADCAST_STICKY");
        if (packages.protectedBroadcast(intent.getAction())) throw new SecurityException("Protected broadcast " + intent.getAction());
        List<Object> targets = new ArrayList<>();
        boolean permitted = intent.getPackage() == null || Objects.equals(owner, intent.getPackage());
        if (permissions != null) for (String permission : permissions) permitted &= allowed(permission);
        if (excludedPermissions != null) for (String permission : excludedPermissions) if (packages.granted(permission)) permitted = false;
        if (excludedPackages != null) for (String pkg : excludedPackages) if (Objects.equals(owner, pkg)) permitted = false;
        if (permitted) {
            if (intent.getComponent() == null) for (Receiver receiver : receivers)
                if (allowed(receiver.permission) && receiver.filter.match(intent.getAction(), type, intent.getScheme(),
                        intent.getData(), intent.getCategories(), "AsgardBroadcast") >= 0) targets.add(receiver);
            if ((intent.getFlags() & Intent.FLAG_RECEIVER_REGISTERED_ONLY) == 0)
                for (ActivityInfo info : packages.receivers(intent, type, 0)) {
                    if (!allowed(info.permission)) continue;
                    if (info.processName != null && !info.processName.equals(owner))
                        throw new UnsupportedOperationException("Broadcast receiver in another process: " + info.processName);
                    targets.add(info);
                }
        }
        if (serial) {
            // Dynamic receiver priorities are honored before manifest targets.
            targets.sort((left, right) -> Integer.compare(right instanceof Receiver ? ((Receiver) right).filter.getPriority() : 0,
                    left instanceof Receiver ? ((Receiver) left).filter.getPriority() : 0));
            ordered.add(new Broadcast(intent, targets, finished, code, data, extras));
            advance();
        } else {
            for (Object target : targets) deliver(target, intent, code, data, extras, false);
            if (finished != null) receive(finished, intent, code, data, extras, false, false);
        }
        return 0;
    }

    private IBinder deliver(Object target, Intent intent, int code, String data, Bundle extras, boolean serial) throws Exception {
        if (target instanceof Receiver) {
            Receiver receiver = (Receiver) target;
            registered(receiver.endpoint, intent, code, data, extras, serial, false);
            return receiver.endpoint.asBinder();
        }
        if (thread == null) throw new UnsupportedOperationException("Manifest receiver before application attach");
        Object app = thread.getClass().getMethod("getApplicationThread").invoke(thread);
        Class<?> compat = Class.forName("android.content.res.CompatibilityInfo");
        Class.forName("android.app.IApplicationThread").getMethod("scheduleReceiver", Intent.class, ActivityInfo.class,
                compat, int.class, String.class, Bundle.class, boolean.class, boolean.class,
                int.class, int.class, int.class, String.class).invoke(app, copy(intent), target,
                compat.getField("DEFAULT_COMPATIBILITY_INFO").get(null), code, data, copy(extras),
                serial, !serial, 0, 2, Process.myUid(), owner);
        return ((IInterface) app).asBinder();
    }

    private void advance() {
        if (active == null) active = ordered.poll();
        if (active == null || active.waiting != null || advancing) return;
        Broadcast current = active;
        advancing = true;
        new Handler(Looper.getMainLooper()).post(() -> {
            synchronized (FrameworkBroadcasts.this) {
                advancing = false;
                try {
                    if (current.index < current.targets.size()) {
                        current.waiting = deliver(current.targets.get(current.index++), current.intent, current.code, current.data, current.extras, true);
                    } else {
                        if (current.finished != null) receive(current.finished, current.intent, current.code, current.data, current.extras, false, false);
                        active = null; advance();
                    }
                } catch (Exception error) { throw new RuntimeException("Broadcast delivery failed", error); }
            }
        });
    }

    synchronized void finish(Object[] a) {
        if (active == null || active.waiting != a[0]) return;
        active.code = (Integer) a[1]; active.data = (String) a[2];
        active.extras = copy((Bundle) a[3]);
        if ((Boolean) a[4] && (active.intent.getFlags() & Intent.FLAG_RECEIVER_NO_ABORT) == 0) active.index = active.targets.size();
        active.waiting = null; advance();
    }
}
