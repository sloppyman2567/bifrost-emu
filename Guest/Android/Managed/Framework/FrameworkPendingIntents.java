// Process-local PendingIntentController/Record contracts, Android 15.
// Sender objects are local IIntentSenders attached to real Binder identities.
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.Process;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

final class FrameworkPendingIntents implements FrameworkServices.Call {
    private enum Contract {
        getIntentSender, getIntentSenderWithFeature, sendIntentSender, cancelIntentSender,
        getInfoForIntentSender, getTagForIntentSender, getIntentForIntentSender,
        registerIntentSenderCancelListenerEx, unregisterIntentSenderCancelListener,
        isIntentSenderAnActivity, isIntentSenderTargetedToPackage
    }
    @Override public String[] methods() {
        Contract[] contracts = Contract.values(); String[] names = new String[contracts.length];
        for (int i = 0; i < names.length; i++) names[i] = contracts[i].name();
        return names;
    }
    boolean handles(String method) {
        try { Contract.valueOf(method); return true; } catch (IllegalArgumentException unknown) { return false; }
    }

    private final FrameworkServices services;
    private final FrameworkPackages packages;
    private final FrameworkActivities activities;
    private final FrameworkServiceRuntime runtime;
    private final FrameworkBroadcasts broadcasts;
    private final Map<Key, Record> records = new HashMap<>();
    private final Map<IBinder, Record> identities = new java.util.WeakHashMap<>();

    private static Intent copy(Intent intent) {
        if (intent == null) return null;
        Parcel parcel = Parcel.obtain();
        try { intent.writeToParcel(parcel, 0); parcel.setDataPosition(0); return Intent.CREATOR.createFromParcel(parcel); }
        finally { parcel.recycle(); }
    }
    private static final class Key {
        final int type, requestCode, flags, user;
        final String owner, feature, who, resolvedType;
        final IBinder activity;
        final Intent filter;
        Key(Object[] a, int flags) {
            type = (Integer) a[0]; owner = (String) a[1]; feature = (String) a[2]; activity = (IBinder) a[3];
            who = (String) a[4]; requestCode = (Integer) a[5]; this.flags = flags; user = (Integer) a[10];
            Intent[] intents = (Intent[]) a[6]; String[] types = (String[]) a[7];
            filter = intents == null ? null : new Intent(intents[intents.length - 1]).cloneFilter();
            resolvedType = types == null ? null : types[types.length - 1];
        }
        @Override public boolean equals(Object value) {
            if (!(value instanceof Key)) return false;
            Key other = (Key) value;
            return type == other.type && requestCode == other.requestCode && flags == other.flags && user == other.user
                    && activity == other.activity && Objects.equals(owner, other.owner) && Objects.equals(feature, other.feature)
                    && Objects.equals(who, other.who) && Objects.equals(resolvedType, other.resolvedType)
                    && (filter == null ? other.filter == null : filter.filterEquals(other.filter));
        }
        @Override public int hashCode() {
            return Objects.hash(type, requestCode, flags, user, activity, owner, feature, who, resolvedType,
                    filter == null ? 0 : filter.filterHashCode());
        }
    }
    private static final class Record {
        final Key key;
        Intent[] intents;
        String[] types;
        Bundle options;
        Object sender;
        boolean cancelled;
        final Map<IBinder, Object> listeners = new LinkedHashMap<>();
        Record(Key key, Object[] a) { this.key = key; update(a, false); }
        void update(Object[] a, boolean existing) {
            Intent last = intents == null ? null : intents[intents.length - 1];
            Intent[] incoming = (Intent[]) a[6];
            if (existing && last != null) last.replaceExtras(incoming == null ? (Intent) null : copy(incoming[incoming.length - 1]));
            intents = incoming == null ? null : new Intent[incoming.length];
            if (incoming != null) {
                for (int i = 0; i < incoming.length; i++) intents[i] = copy(incoming[i]);
                if (existing && last != null) intents[intents.length - 1] = last;
            }
            types = a[7] == null ? null : ((String[]) a[7]).clone();
            if (!existing) options = a[9] == null ? null : new Bundle((Bundle) a[9]);
        }
    }

    FrameworkPendingIntents(FrameworkServices services, FrameworkPackages packages, FrameworkActivities activities,
            FrameworkServiceRuntime runtime, FrameworkBroadcasts broadcasts) {
        this.services = services; this.packages = packages; this.activities = activities; this.runtime = runtime; this.broadcasts = broadcasts;
    }

    private Record record(Object sender) {
        return sender instanceof IInterface ? identities.get(((IInterface) sender).asBinder()) : null;
    }

    private synchronized Object create(Object[] arguments) throws Exception {
        Object[] a = arguments.clone();
        int user = (Integer) a[10], type = (Integer) a[0];
        if (packages.packageUid((String) a[1]) != Process.myUid() || user != 0 && user != -2)
            throw new SecurityException("Foreign PendingIntent owner/user");
        Intent[] intents = (Intent[]) a[6]; String[] types = (String[]) a[7];
        if (type < 1 || type > 5 || intents != null && intents.length == 0
                || types != null && (intents == null || intents.length != types.length))
            throw new IllegalArgumentException("Invalid PendingIntent request");
        if (intents != null) for (Intent intent : intents) {
            if (intent == null || intent.hasFileDescriptors()) throw new IllegalArgumentException("Invalid PendingIntent intent");
            if (type == 1 && (intent.getFlags() & 0x02000000) != 0)
                throw new IllegalArgumentException("FLAG_RECEIVER_BOOT_UPGRADE in PendingIntent");
            int flags = (Integer) a[8];
            if (type != 3 && packages.targetSdk() >= 34 && (flags & PendingIntent.FLAG_MUTABLE) != 0
                    && (flags & (PendingIntent.FLAG_NO_CREATE | 0x01000000)) == 0
                    && intent.getComponent() == null && intent.getPackage() == null)
                throw new IllegalArgumentException("Mutable implicit PendingIntent requires FLAG_ALLOW_UNSAFE_IMPLICIT_INTENT");
        }
        if (type == 3 && !activities.contains((IBinder) a[3])) return null;
        int flags = (Integer) a[8];
        if ((flags & PendingIntent.FLAG_IMMUTABLE) != 0 && (flags & PendingIntent.FLAG_MUTABLE) != 0)
            throw new IllegalArgumentException("Both immutable and mutable flags");
        Key key = new Key(a, flags & ~(PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_UPDATE_CURRENT));
        Record record = records.get(key);
        if (record != null) {
            if ((flags & PendingIntent.FLAG_CANCEL_CURRENT) != 0) {
                cancel(record);
                if ((flags & PendingIntent.FLAG_NO_CREATE) != 0) return record.sender;
            }
            else {
                if ((flags & PendingIntent.FLAG_UPDATE_CURRENT) != 0) record.update(a, true);
                return record.sender;
            }
        }
        if ((flags & PendingIntent.FLAG_NO_CREATE) != 0) return null;
        Record created = new Record(key, a);
        created.sender = services.local("intent_sender", "android.content.IIntentSender", (method, args) -> {
            if (!method.equals("send")) throw new UnsupportedOperationException("intent_sender." + method);
            send(created, (Integer) args[0], (Intent) args[1], (String) args[2], args[4], (String) args[5], (Bundle) args[6]);
            return null;
        });
        records.put(key, created); identities.put(((IInterface) created.sender).asBinder(), created);
        return created.sender;
    }

    private void cancel(Record record) throws Exception {
        if (record.cancelled) return;
        record.cancelled = true; records.remove(record.key, record);
        ArrayList<Object> listeners = new ArrayList<>(record.listeners.values()); record.listeners.clear();
        for (Object listener : listeners) Class.forName("com.android.internal.os.IResultReceiver")
                .getMethod("send", int.class, Bundle.class).invoke(listener, 0, null);
    }

    private int send(Record record, int code, Intent fill, String resolvedType, Object finished, String permission, Bundle options) throws Exception {
        Intent intent;
        Intent[] intents;
        String[] types;
        Bundle launchOptions;
        synchronized (this) {
            if (record == null || record.cancelled) return -96; // START_CANCELED
            if (record.key.type == 3 && !activities.contains(record.key.activity)) { cancel(record); return -96; }
            intent = record.intents == null ? new Intent() : copy(record.intents[record.intents.length - 1]);
            if ((record.key.flags & PendingIntent.FLAG_IMMUTABLE) == 0 && fill != null) {
                int changed = intent.fillIn(copy(fill), record.key.flags);
                if ((changed & Intent.FILL_IN_DATA) == 0) resolvedType = record.key.resolvedType;
            } else resolvedType = record.key.resolvedType;
            if ((record.key.flags & PendingIntent.FLAG_ONE_SHOT) != 0) cancel(record);
            intents = record.intents == null ? new Intent[] {intent} : new Intent[record.intents.length];
            for (int i = 0; i < intents.length - 1; i++) intents[i] = copy(record.intents[i]);
            intents[intents.length - 1] = intent;
            types = record.types == null ? null : record.types.clone();
            launchOptions = record.options == null ? new Bundle() : new Bundle(record.options);
            if (options != null) launchOptions.putAll(options);
        }
        int result = 0;
        switch (record.key.type) {
            case 1:
                return broadcasts.send(intent, resolvedType, finished, code, null, null,
                        permission == null ? null : new String[] {permission}, null, null, finished != null, false, record.key.user);
            case 2:
                for (int i = 0; i < intents.length; i++) result = activities.start(new Object[] {null, record.key.owner, record.key.feature,
                        intents[i], i == intents.length - 1 ? resolvedType : types == null ? null : types[i],
                        null, null, -1, 0, null, launchOptions});
                break;
            case 3: activities.result(record.key.activity, record.key.who, record.key.requestCode, code, intent); break;
            case 4:
            case 5: runtime.startPending(intent, resolvedType, record.key.owner, record.key.type == 5); break;
            default: throw new AssertionError(record.key.type);
        }
        if (result != -96 && finished != null) FrameworkBroadcasts.receive(finished, intent, 0, null, null, false, false);
        return result;
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("activity." + method); }
        if (contract == Contract.getIntentSenderWithFeature) return create(a);
        if (contract == Contract.getIntentSender) return create(new Object[] {a[0], a[1], null, a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]});
        if (contract == Contract.sendIntentSender) return send(record(a[1]), (Integer) a[3], (Intent) a[4], (String) a[5], a[6], (String) a[7], (Bundle) a[8]);
        Record record = record(a[0]);
        switch (contract) {
            case cancelIntentSender: if (record != null) cancel(record); return null;
            case getInfoForIntentSender:
                return Class.forName("android.app.ActivityManager$PendingIntentInfo").getConstructor(String.class, int.class, boolean.class, int.class)
                        .newInstance(record == null ? null : record.key.owner, record == null ? -1 : Process.myUid(),
                                record != null && (record.key.flags & PendingIntent.FLAG_IMMUTABLE) != 0, record == null ? 0 : record.key.type);
            case getIntentForIntentSender: return record == null || record.intents == null ? null : copy(record.intents[record.intents.length - 1]);
            case getTagForIntentSender:
                if (record == null || record.key.filter == null) return null;
                Intent filter = record.key.filter;
                return (a[1] == null ? "" : (String) a[1]) + (filter.getAction() != null ? filter.getAction()
                        : filter.getComponent() != null ? filter.getComponent().flattenToShortString() : "?");
            case registerIntentSenderCancelListenerEx:
                if (record == null || record.cancelled) return false;
                record.listeners.put(((IInterface) a[1]).asBinder(), a[1]); return true;
            case unregisterIntentSenderCancelListener:
                if (record != null) record.listeners.remove(((IInterface) a[1]).asBinder()); return null;
            case isIntentSenderAnActivity: return record != null && record.key.type == 2;
            case isIntentSenderTargetedToPackage:
                if (record == null || record.intents == null) return false;
                for (Intent item : record.intents) if (item.getPackage() != null && item.getComponent() != null) return false;
                return true;
            default: throw new UnsupportedOperationException("activity." + method);
        }
    }
}
