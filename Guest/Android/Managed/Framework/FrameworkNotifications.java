// App-owned Android 15 notification channels/groups. Keep deleted channels:
// recreating an id restores its settings instead of resetting user choices.
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.Process;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

final class FrameworkNotifications implements FrameworkServices.Call {
    private enum Contract {
        areNotificationsEnabled,
        isNotificationPolicyAccessGranted,
        setNotificationDelegate,
        getNotificationDelegate,
        canNotifyAsPackage,
        createNotificationChannels,
        createNotificationChannelGroups,
        getNotificationChannel,
        getConversationNotificationChannel,
        getNotificationChannels,
        deleteNotificationChannel,
        getNotificationChannelGroup,
        getNotificationChannelGroups,
        deleteNotificationChannelGroup,
        cancelAllNotifications,
        cancelNotificationWithTag,
        getAppActiveNotifications
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final Map<String, NotificationChannel> channels = new LinkedHashMap<>();
    private final Map<String, NotificationChannelGroup> groups = new LinkedHashMap<>();
    private static final int MAGIC = 0x41534e32, MAX_BYTES = 8 * 1024 * 1024;
    private String delegate;
    private int delegateUid = -1;
    // FileChannel.lock does not wait for overlapping locks held by another
    // instance in this JVM. Serialize those before taking the process lock.
    private static final Object STORE_LOCK = new Object();

    FrameworkNotifications(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("notification", "android.app.INotificationManager", this);
    }

    private PackageInfo owner(String name) {
        PackageInfo pkg = packages.processPackage(Objects.requireNonNull(name, "package"));
        if (pkg == null || pkg.applicationInfo.uid != Process.myUid())
            throw new SecurityException("Notification package does not belong to caller");
        return pkg;
    }

    private void user(int user) {
        if (user != Process.myUid() / 100000)
            throw new SecurityException("Notification access to another user");
    }

    private static <T extends Parcelable> T copy(T value, Parcelable.Creator<T> creator) {
        if (value == null) return null;
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0); parcel.setDataPosition(0);
            return creator.createFromParcel(parcel);
        } finally { parcel.recycle(); }
    }

    private static Object slice(List<?> list) throws Exception {
        return Class.forName("android.content.pm.ParceledListSlice")
                .getConstructor(List.class).newInstance(list);
    }

    private static List<?> list(Object slice) throws Exception {
        return (List<?>) slice.getClass().getMethod("getList").invoke(slice);
    }

    private void read(AtomicFile file) throws Exception {
        channels.clear(); groups.clear(); delegate = null; delegateUid = -1;
        if (!file.getBaseFile().exists() && !new File(file.getBaseFile() + ".bak").exists()) return;
        byte[] bytes;
        try (java.io.FileInputStream input = file.openRead()) {
            if (input.getChannel().size() > MAX_BYTES) throw new IllegalStateException("Notification store too large");
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] block = new byte[8192];
            for (int size; (size = input.read(block)) != -1;) {
                if (out.size() + size > MAX_BYTES) throw new IllegalStateException("Notification store too large");
                out.write(block, 0, size);
            }
            bytes = out.toByteArray();
        }
        Parcel p = Parcel.obtain();
        try {
            p.unmarshall(bytes, 0, bytes.length); p.setDataPosition(0);
            if (p.readInt() != MAGIC) throw new IllegalStateException("Unknown notification store");
            int count = p.readInt();
            if (count < 0 || count > 6000) throw new IllegalStateException("Invalid notification groups");
            for (int i = 0; i < count; i++) {
                NotificationChannelGroup group = NotificationChannelGroup.CREATOR.createFromParcel(p);
                groups.put(group.getId(), group);
            }
            count = p.readInt();
            if (count < 0 || count > 5000) throw new IllegalStateException("Invalid notification channels");
            for (int i = 0; i < count; i++) {
                NotificationChannel channel = NotificationChannel.CREATOR.createFromParcel(p);
                channels.put(channel.getId(), channel);
            }
            delegate = p.readString(); delegateUid = p.readInt();
            p.enforceNoDataAvail();
        } finally { p.recycle(); }
    }

    private void write(AtomicFile file) throws Exception {
        Parcel p = Parcel.obtain();
        try {
            p.writeInt(MAGIC); p.writeInt(groups.size());
            for (NotificationChannelGroup group : groups.values()) group.writeToParcel(p, 0);
            p.writeInt(channels.size());
            for (NotificationChannel channel : channels.values()) channel.writeToParcel(p, 0);
            p.writeString(delegate); p.writeInt(delegateUid);
            byte[] bytes = p.marshall();
            if (bytes.length > MAX_BYTES) throw new IllegalStateException("Notification store too large");
            FileOutputStream output = file.startWrite();
            try { output.write(bytes); file.finishWrite(output); }
            catch (Exception error) { file.failWrite(output); throw error; }
        } finally { p.recycle(); }
    }

    private static boolean deleted(NotificationChannel channel) throws Exception {
        return (Boolean) NotificationChannel.class.getMethod("isDeleted").invoke(channel);
    }

    private static void deletion(NotificationChannel channel, boolean deleted) throws Exception {
        NotificationChannel.class.getMethod("setDeleted", boolean.class).invoke(channel, deleted);
        NotificationChannel.class.getMethod("setDeletedTimeMs", long.class)
                .invoke(channel, deleted ? System.currentTimeMillis() : -1L);
    }

    private NotificationChannel channel(String id) throws Exception {
        NotificationChannel value = channels.get(id);
        return value == null || deleted(value) ? null : copy(value, NotificationChannel.CREATOR);
    }

    private List<NotificationChannel> channels(String group, boolean filterGroup) throws Exception {
        List<NotificationChannel> result = new ArrayList<>();
        for (NotificationChannel value : channels.values())
            if (!deleted(value) && (!filterGroup || Objects.equals(group, value.getGroup())))
                result.add(copy(value, NotificationChannel.CREATOR));
        return result;
    }

    private NotificationChannelGroup group(String id) throws Exception {
        NotificationChannelGroup value = groups.get(id);
        if (value == null) return null;
        NotificationChannelGroup result = copy(value, NotificationChannelGroup.CREATOR);
        NotificationChannelGroup.class.getMethod("setChannels", List.class).invoke(result, channels(id, true));
        return result;
    }

    // PreferencesHelper.createNotificationChannel: app updates may lower
    // importance, rename and describe, but cannot reset sound/vibration or
    // move a channel out of an existing group. No DND privilege is granted.
    private void create(NotificationChannel supplied) throws Exception {
        NotificationChannel value = copy(Objects.requireNonNull(supplied), NotificationChannel.CREATOR);
        Objects.requireNonNull(value.getId(), "channel id");
        if (value.getName() == null || value.getName().length() == 0
                || value.getImportance() < NotificationManager.IMPORTANCE_NONE
                || value.getImportance() > NotificationManager.IMPORTANCE_MAX
                || "miscellaneous".equals(value.getId()))
            throw new IllegalArgumentException("Invalid notification channel");
        if (value.getGroup() != null && !groups.containsKey(value.getGroup()))
            throw new IllegalArgumentException("Notification channel group does not exist");
        NotificationChannel existing = channels.get(value.getId());
        if (existing != null) {
            deletion(existing, false);
            existing.setName(value.getName()); existing.setDescription(value.getDescription());
            existing.setBlockable(value.isBlockable());
            if (existing.getGroup() == null && value.getGroup() != null) existing.setGroup(value.getGroup());
            int locked = (Integer) NotificationChannel.class.getMethod("getUserLockedFields").invoke(existing);
            if (locked == 0 && value.getImportance() < existing.getImportance())
                existing.setImportance(value.getImportance());
            return;
        }
        if (channels.size() >= 5000) throw new IllegalStateException("Notification channel limit");
        if (value.getParentChannelId() != null && !channels.containsKey(value.getParentChannelId()))
            throw new IllegalArgumentException("Conversation parent does not exist");
        deletion(value, false);
        value.setBypassDnd(false); value.setLockscreenVisibility(-1000);
        NotificationChannel.class.getMethod("setAllowBubbles", int.class).invoke(value, -1);
        NotificationChannel.class.getMethod("setImportantConversation", boolean.class).invoke(value, false);
        int locked = (Integer) NotificationChannel.class.getMethod("getUserLockedFields").invoke(value);
        NotificationChannel.class.getMethod("unlockFields", int.class).invoke(value, locked);
        NotificationChannel.class.getMethod("setOriginalImportance", int.class).invoke(value, value.getImportance());
        channels.put(value.getId(), value);
    }

    private void create(NotificationChannelGroup supplied) throws Exception {
        NotificationChannelGroup value = copy(Objects.requireNonNull(supplied), NotificationChannelGroup.CREATOR);
        Objects.requireNonNull(value.getId(), "group id");
        if (value.getName() == null || value.getName().length() == 0)
            throw new IllegalArgumentException("Notification group name is empty");
        NotificationChannelGroup existing = groups.get(value.getId());
        if (existing == null && groups.size() >= 6000) throw new IllegalStateException("Notification group limit");
        NotificationChannelGroup.class.getMethod("setBlocked", boolean.class)
                .invoke(value, existing != null && existing.isBlocked());
        NotificationChannelGroup.class.getMethod("setChannels", List.class).invoke(value, new ArrayList<>());
        groups.put(value.getId(), value);
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("notification." + method); }
        // These are app APIs. Administrative ForPackage/listener operations
        // remain unavailable; POST_NOTIFICATIONS never gates channel creation.
        PackageInfo pkg = owner((String) a[0]);
        if (method.equals("areNotificationsEnabled"))
            return packages.checkPermission("android.permission.POST_NOTIFICATIONS", pkg.applicationInfo.uid,
                    android.content.Context.DEVICE_ID_DEFAULT) == PackageManager.PERMISSION_GRANTED;
        if (method.equals("isNotificationPolicyAccessGranted")) return false;
        if (contract == Contract.canNotifyAsPackage) {
            user((Integer) a[2]);
            if (pkg.packageName.equals(a[1])) return true;
            pkg = packages.processPackage((String) a[1]);
            if (pkg == null || pkg.applicationInfo.dataDir == null) return false;
        }
        if (method.equals("getNotificationChannel") || method.equals("getConversationNotificationChannel")) {
            user((Integer) a[1]); owner((String) a[2]);
            if (!pkg.packageName.equals(a[2])) throw new SecurityException("Foreign notification package");
        }
        if (method.equals("getNotificationChannels")) {
            owner((String) a[1]); user((Integer) a[2]);
            if (!pkg.packageName.equals(a[1])) throw new SecurityException("Foreign notification package");
        }
        // Nothing is ever posted here (desktop delivery is unsupported), so
        // cancellation finds no notification and the active list is empty.
        switch (contract) {
            case cancelAllNotifications: user((Integer) a[1]); return null;
            case cancelNotificationWithTag: owner((String) a[1]); user((Integer) a[4]); return null;
            case getAppActiveNotifications: user((Integer) a[1]); return slice(new ArrayList<Object>());
            default: break;
        }
        File directory = new File(pkg.applicationInfo.dataDir, ".asgard-notifications");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IllegalStateException("Notification storage unavailable");
        // Service children can open the same app's metadata. Reload under a
        // cross-process lock, then atomically publish changes.
        synchronized (STORE_LOCK) {
            try (RandomAccessFile lock = new RandomAccessFile(new File(directory, "lock"), "rw");
                    FileLock held = lock.getChannel().lock()) {
                AtomicFile file = new AtomicFile(new File(directory, "channels"));
                read(file);
                switch (contract) {
                    case setNotificationDelegate: {
                        String requested = (String) a[1];
                        int uid = requested == null ? -1 : packages.packageUid(requested);
                        // NMS ignores a request naming an uninstalled delegate.
                        // In particular, Firebase cannot invent a GMS package.
                        if (requested == null || uid >= 0) {
                            delegate = requested; delegateUid = uid; write(file);
                        }
                        return null;
                    }
                    case getNotificationDelegate:
                        return delegate != null && packages.packageUid(delegate) == delegateUid ? delegate : null;
                    case canNotifyAsPackage:
                        return Objects.equals(delegate, a[0]) && delegateUid == Process.myUid();
                    case createNotificationChannels:
                        for (Object value : list(a[1])) create((NotificationChannel) value);
                        write(file); return null;
                    case createNotificationChannelGroups:
                        for (Object value : list(a[1])) create((NotificationChannelGroup) value);
                        write(file); return null;
                    case getNotificationChannel: return channel((String) a[3]);
                    case getConversationNotificationChannel: {
                        String parent = (String) a[3], conversation = (String) a[5];
                        if (conversation != null) for (NotificationChannel value : channels.values())
                            if (!deleted(value) && parent.equals(value.getParentChannelId())
                                    && conversation.equals(value.getConversationId()))
                                return copy(value, NotificationChannel.CREATOR);
                        return conversation == null || (Boolean) a[4] ? channel(parent) : null;
                    }
                    case getNotificationChannels: return slice(channels(null, false));
                    case deleteNotificationChannel: {
                        if ("miscellaneous".equals(a[1])) throw new IllegalArgumentException("Reserved channel id");
                        NotificationChannel value = channels.get(a[1]);
                        if (value != null) { deletion(value, true); write(file); }
                        return null;
                    }
                    case getNotificationChannelGroup: return group((String) a[1]);
                    case getNotificationChannelGroups: {
                        List<NotificationChannelGroup> result = new ArrayList<>();
                        for (String id : groups.keySet()) result.add(group(id));
                        return slice(result);
                    }
                    case deleteNotificationChannelGroup: {
                        groups.remove(a[1]);
                        for (NotificationChannel value : channels.values())
                            if (value.getGroup() != null && value.getGroup().equals(a[1])) deletion(value, true);
                        write(file); return null;
                    }
                    default: throw new UnsupportedOperationException("notification." + method);
                }
            }
        }
    }
}
