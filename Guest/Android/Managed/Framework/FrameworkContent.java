// Content notifications used by real ContentObservers and JobScheduler triggers.
import android.net.Uri;
import android.os.IBinder;
import android.os.IInterface;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

final class FrameworkContent {
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
