// AlarmManagerService ("alarm") for the running app process. AlarmManager is
// built from ServiceManager.getServiceOrThrow("alarm"), so without it
// getSystemService(ALARM_SERVICE) is null. Listener alarms (OnAlarmListener)
// fire on a scheduler thread through IAlarmListener.doAlarm, which AlarmManager
// posts to the caller's Handler. Alarms do not wake or restart a stopped
// process. PendingIntent alarms are unsupported: this runtime cannot yet
// create PendingIntents (IActivityManager.getIntentSenderWithFeature).
// Timing and permission rules follow Android 15's AlarmManagerService.
import android.os.IBinder;
import android.os.Process;
import android.os.SystemClock;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

final class FrameworkAlarms implements FrameworkServices.Call {
    private static final int RTC_WAKEUP = 0, ELAPSED_REALTIME = 3;  // AlarmManager types
    private static final long MIN_FUTURITY = 5000;  // DEFAULT_MIN_FUTURITY for non-core uids
    private static final int USER_CURRENT_OR_SELF = -3, USER_CURRENT = -2;
    private static final int TARGET_S = 31, TARGET_T = 33, TARGET_U = 34;

    private enum Contract {
        set,
        remove,
        removeAll,
        getNextWakeFromIdleTime,
        getNextAlarmClock,
        canScheduleExactAlarms,
        hasScheduleExactAlarm,
        setTime,
        setTimeZone
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final Map<IBinder, ScheduledFuture<?>> alarms = new HashMap<>();
    private ScheduledThreadPoolExecutor scheduler;

    FrameworkAlarms(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("alarm", "android.app.IAlarmManager", this);
    }

    private void enforce(String permission) {
        if (!packages.granted(permission)) throw new SecurityException(permission);
    }

    private void ownPackage(Object pkg) {
        if (pkg == null || packages.packageUid((String) pkg) != Process.myUid())
            throw new SecurityException("Package " + pkg + " does not belong to " + Process.myUid());
    }

    private static IBinder binder(Object listener) throws Exception {
        return (IBinder) listener.getClass().getMethod("asBinder").invoke(listener);
    }

    // AlarmManager's ListenerWrapper reports completion through this; nothing
    // else (wake lock, batching) waits for it here.
    private static Object completion() throws Exception {
        Class<?> type = Class.forName("android.app.IAlarmCompleteListener");
        android.os.Binder token = new android.os.Binder();
        return Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, (self, method, args) -> {
            switch (method.getName()) {
                case "asBinder": return token;
                case "equals": return self == args[0];
                case "hashCode": return System.identityHashCode(self);
                case "toString": return "AlarmComplete";
                default: return null;  // alarmComplete
            }
        });
    }

    // Targets 31+ need SCHEDULE_EXACT_ALARM (requested; denied by default
    // from 34, and no user grants it here) or, from 33, USE_EXACT_ALARM.
    private boolean exactAllowed() {
        int target = packages.targetSdk();
        if (target < TARGET_S) return true;
        boolean schedule = packages.requested("android.permission.SCHEDULE_EXACT_ALARM") && target < TARGET_U;
        boolean use = target >= TARGET_T && packages.granted("android.permission.USE_EXACT_ALARM");
        return schedule || use;
    }

    private synchronized void fire(IBinder key, Object listener) {
        if (alarms.remove(key) == null) return;  // removed or replaced after scheduling
        try {
            listener.getClass().getMethod("doAlarm", Class.forName("android.app.IAlarmCompleteListener"))
                    .invoke(listener, completion());
        } catch (Exception error) {
            System.out.println("ALARM_DELIVERY_FAILED " + error);
        }
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("alarm." + method); }
        switch (contract) {
            case set: {
                ownPackage(a[0]);
                int type = (Integer) a[1];
                long trigger = (Long) a[2], interval = (Long) a[4];
                Object operation = a[6], listener = a[7];
                if (interval != 0 && listener != null)
                    throw new IllegalArgumentException("Repeating alarms cannot use AlarmReceivers");
                if (a[9] != null) enforce("android.permission.UPDATE_DEVICE_STATS");  // workSource
                if (operation != null && listener == null)
                    throw new UnsupportedOperationException("alarm.set with a PendingIntent");
                // "Alarms must either supply a PendingIntent or an AlarmReceiver": dropped.
                if (operation != null || listener == null) return null;
                if (type < RTC_WAKEUP || type > ELAPSED_REALTIME)
                    throw new IllegalArgumentException("Invalid alarm type " + type);
                if (trigger < 0) trigger = 0;
                long now = SystemClock.elapsedRealtime();
                // convertToElapsed: RTC types are wall-clock times.
                if (type <= 1) trigger -= System.currentTimeMillis() - now;
                long when = Math.max(now + MIN_FUTURITY, trigger);
                IBinder key = binder(listener);
                ScheduledFuture<?> previous = alarms.remove(key);
                if (previous != null) previous.cancel(false);
                if (alarms.size() >= 500) throw new IllegalStateException("Maximum limit of concurrent alarms 500 reached");
                if (scheduler == null) {
                    scheduler = new ScheduledThreadPoolExecutor(1, task -> {
                        Thread thread = new Thread(task, "AlarmManager");
                        thread.setDaemon(true);
                        return thread;
                    });
                    scheduler.setRemoveOnCancelPolicy(true);
                }
                alarms.put(key, scheduler.schedule(() -> fire(key, listener), when - now, TimeUnit.MILLISECONDS));
                return null;
            }
            case remove: {
                if (a[1] == null) return null;  // PendingIntent alarms never exist here
                ScheduledFuture<?> alarm = alarms.remove(binder(a[1]));
                if (alarm != null) alarm.cancel(false);
                return null;
            }
            case removeAll: {
                ownPackage(a[0]);
                for (ScheduledFuture<?> alarm : alarms.values()) alarm.cancel(false);
                alarms.clear();
                return null;
            }
            // No FLAG_IDLE_UNTIL alarm and no AlarmClockInfo alarm exist.
            case getNextWakeFromIdleTime: return Long.MAX_VALUE;
            case getNextAlarmClock: {
                int user = (Integer) a[0];
                if (user != 0 && user != USER_CURRENT && user != USER_CURRENT_OR_SELF)
                    enforce("android.permission.INTERACT_ACROSS_USERS_FULL");
                return null;
            }
            case canScheduleExactAlarms: {
                if (packages.packageUid((String) a[0]) != Process.myUid())
                    throw new SecurityException("Uid " + Process.myUid() + " cannot query canScheduleExactAlarms for package " + a[0]);
                return exactAllowed();
            }
            case hasScheduleExactAlarm: {
                if ((Integer) a[1] != 0) enforce("android.permission.INTERACT_ACROSS_USERS_FULL");
                int uid = packages.packageUid((String) Objects.requireNonNull(a[0], "packageName"));
                if (uid != -1 && uid != Process.myUid())
                    throw new SecurityException("Uid " + Process.myUid() + " cannot query hasScheduleExactAlarm for package " + a[0]);
                // hasScheduleExactAlarmInternal: only apps requesting the permission are candidates.
                // Granted below target 31 or by the default op mode up to 33;
                // denied by default from 34 without a user grant.
                return uid > 0 && packages.requested("android.permission.SCHEDULE_EXACT_ALARM")
                        && packages.targetSdk() < TARGET_U;
            }
            case setTime: enforce("android.permission.SET_TIME"); throw new IllegalStateException("SET_TIME granted to an app");
            case setTimeZone: enforce("android.permission.SET_TIME_ZONE"); throw new IllegalStateException("SET_TIME_ZONE granted to an app");
            default: throw new UnsupportedOperationException("alarm." + method);
        }
    }
}
