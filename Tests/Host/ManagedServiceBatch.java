// Real Android 15 NotificationManager/AudioManager/PowerManager clients.
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ServiceInfo;
import android.os.Bundle;
import android.provider.Settings;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.Process;
import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class ManagedServiceBatch {
    private static final String NAME = "org.bifrost.servicebatch";
    private static int checks;
    private static volatile CountDownLatch manifestDelivery, serviceDelivery, resultDelivery;
    private static volatile Intent manifestIntent, serviceIntent, resultIntent;
    private static final List<Long> manifestTimes = java.util.Collections.synchronizedList(new ArrayList<>());
    public static final class ProbeReceiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent) {
            manifestIntent = intent;
            manifestTimes.add(android.os.SystemClock.elapsedRealtime());
            if (isOrderedBroadcast()) setResultCode(getResultCode() + 1);
            manifestDelivery.countDown();
        }
    }
    public static final class ProbeService extends android.app.Service {
        @Override public IBinder onBind(Intent intent) { return null; }
        @Override public int onStartCommand(Intent intent, int flags, int id) {
            serviceIntent = intent; serviceDelivery.countDown(); return START_NOT_STICKY;
        }
    }
    public static final class ProbeActivity extends android.app.Activity {
        @Override protected void onActivityResult(int request, int result, Intent data) {
            resultIntent = data;
            if (request == 17 && result == 23) resultDelivery.countDown();
        }
    }

    private static Object launcherField(String name) throws Exception {
        java.lang.reflect.Field field = ManagedLauncher.class.getDeclaredField(name);
        field.setAccessible(true); return field.get(null);
    }

    private static void usersAndIdle(Context context) throws Exception {
        android.os.UserManager users = context.getSystemService(android.os.UserManager.class);
        check(users != null && users.isUserUnlocked() && users.isUserRunning(android.os.Process.myUserHandle()),
                "session user is not running and unlocked");
        check(users.getUserProfiles().equals(Arrays.asList(android.os.Process.myUserHandle()))
                && !users.isManagedProfile() && !users.isDemoUser(), "single-user profile differs");
        check(users.getSerialNumberForUser(android.os.Process.myUserHandle()) == 0
                && android.os.Process.myUserHandle().equals(users.getUserForSerialNumber(0))
                && users.getUserForSerialNumber(7) == null, "user serial mapping differs");
        android.os.Bundle restrictions = users.getUserRestrictions();
        restrictions.putBoolean(android.os.UserManager.DISALLOW_INSTALL_APPS, true);
        check(users.getUserRestrictions().isEmpty() && !users.hasUserRestriction(android.os.UserManager.DISALLOW_INSTALL_APPS),
                "restriction replies alias service state");
        Object remote = endpoint("user", "android.os.IUserManager");
        check((Boolean) invoke(remote, "isUserForeground", 0) && (Boolean) invoke(remote, "isUserVisible", 0)
                && (Boolean) invoke(remote, "isAdminUser", 0), "session user flags differ");
        int[] ids = (int[]) invoke(remote, "getProfileIds", 0, true);
        ids[0] = 10;
        check(Arrays.equals((int[]) invoke(remote, "getProfileIds", 0, false), new int[] {0}),
                "profile id replies alias service state");
        List<?> profiles = (List<?>) invoke(remote, "getProfiles", 0, true);
        Object info = profiles.get(0);
        check(info.getClass().getField("name").get(info) == null && info.getClass().getField("iconPath").get(info) == null,
                "unprivileged profile reply leaked personal fields");
        info.getClass().getField("id").setInt(info, 10);
        Object copied = ((List<?>) invoke(remote, "getProfiles", 0, true)).get(0);
        check(copied.getClass().getField("id").getInt(copied) == 0,
                "profile replies alias service state");
        for (String method : new String[] {"isUserRunning", "isUserUnlocked", "isUserForeground",
                "isUserVisible", "getUserRestrictions"}) denied(() -> invoke(remote, method, 10));
        denied(() -> invoke(remote, "getProfileIds", 10, true));
        denied(() -> invoke(remote, "getUserInfo", 0));
        check(!(Boolean) invoke(remote, "hasUserRestriction", android.os.UserManager.DISALLOW_INSTALL_APPS, 10),
                "missing user restriction must be false before permission checking");
        android.os.PowerManager power = context.getSystemService(android.os.PowerManager.class);
        check(!power.isIgnoringBatteryOptimizations(NAME) && !power.isDeviceIdleMode(),
                "device idle state/exemption disagrees");
        check(power.getLocationPowerSaveMode() == PowerManager.LOCATION_MODE_NO_CHANGE
                && power.isExemptFromLowPowerStandby()
                && power.isAllowedInLowPowerStandby(0)
                && power.isAllowedInLowPowerStandby("org.bifrost.feature"),
                "disabled power policies disagree across manager queries");
        Object powerRemote = endpoint("power", "android.os.IPowerManager");
        Object policy = invoke(powerRemote, "getPowerSaveState", 7);
        check(!(Boolean) policy.getClass().getField("batterySaverEnabled").get(policy)
                && !(Boolean) policy.getClass().getField("globalBatterySaverEnabled").get(policy)
                && policy.getClass().getField("brightnessFactor").getFloat(policy) == 0.5f,
                "unsupported battery saver did not return AOSP's default builder state");
        Object idle = endpoint("deviceidle", "android.os.IDeviceIdleController");
        check(((String[]) invoke(idle, "getFullPowerWhitelist")).length == 0
                && ((int[]) invoke(idle, "getAppIdTempWhitelist")).length == 0,
                "power exemptions invented");
        denied(() -> invoke(idle, "addPowerSaveWhitelistApp", NAME));
        denied(() -> invoke(idle, "addPowerSaveTempWhitelistApp", NAME, 1000L, 0, 0, "probe"));
        denied(() -> invoke(idle, "exitIdle", "probe"));
        System.out.println("MANAGED_USERS_IDLE_OK");
    }

    private static void settings(Context context, Context system, FrameworkPackages packages, PackageInfo pkg) throws Exception {
        check(Settings.System.getInt(context.getContentResolver(), Settings.System.ACCELEROMETER_ROTATION) == 0,
                "accelerometer_rotation default missing");
        check(Settings.System.getInt(context.getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT) == 60000,
                "screen timeout default missing");
        check(Settings.Secure.getInt(context.getContentResolver(), Settings.Secure.LOCATION_MODE) == 3
                && Settings.Secure.getInt(context.getContentResolver(), "notification_bubbles") == 1,
                "fresh-device upgrade defaults missing");
        check(Settings.Secure.getString(context.getContentResolver(), "limit_ad_tracking") == null,
                "Google setting fabricated");
        FrameworkSettings provider = (FrameworkSettings) launcherField("settings");
        check(provider.call("LIST_system", null, null, null).getStringArrayList("result_settings_list")
                .contains("accelerometer_rotation=0"), "LIST_system protocol incorrect");
        check(provider.call("GET_config", "unknown/name", null, null).getString("value") == null,
                "DeviceConfig overrides invented");
        Bundle other = new Bundle(); other.putInt("_user", 1);
        denied(() -> provider.call("GET_system", "accelerometer_rotation", other, null));
        denied(() -> Settings.System.putInt(context.getContentResolver(), Settings.System.ACCELEROMETER_ROTATION, 1));
        check(!Settings.System.canWrite(context), "modern WRITE_SETTINGS granted");
        denied(() -> Settings.Secure.putInt(context.getContentResolver(), "location_mode", 0));
        denied(() -> Settings.Global.putInt(context.getContentResolver(), "airplane_mode_on", 1));
        String[] original = pkg.requestedPermissions;
        pkg.requestedPermissions = Arrays.copyOf(original, original.length + 1);
        pkg.requestedPermissions[original.length] = "android.permission.WRITE_SETTINGS";
        pkg.requestedPermissionsFlags = new int[pkg.requestedPermissions.length];
        pkg.applicationInfo.targetSdkVersion = 22; packages.install(system, pkg);
        check(packages.granted("android.permission.WRITE_SETTINGS") && Settings.System.canWrite(context), "legacy WRITE_SETTINGS grant disagrees with canWrite");
        Bundle tracking = new Bundle(); tracking.putString("_track_generation", null);
        Bundle tracked = provider.call("GET_system", "accelerometer_rotation", tracking, null);
        Object memory = tracked.getParcelable("_track_generation");
        int index = tracked.getInt("_generation_index"), before = tracked.getInt("_generation");
        CountDownLatch observed = new CountDownLatch(1);
        android.database.ContentObserver observer = new android.database.ContentObserver(new android.os.Handler(Looper.getMainLooper())) {
            @Override public void onChange(boolean self, Uri uri) { if (Settings.System.getUriFor("accelerometer_rotation").equals(uri)) observed.countDown(); }
        };
        context.getContentResolver().registerContentObserver(Settings.System.getUriFor("accelerometer_rotation"), false, observer);
        check(Settings.System.putInt(context.getContentResolver(), Settings.System.ACCELEROMETER_ROTATION, 1)
                && Settings.System.getInt(context.getContentResolver(), Settings.System.ACCELEROMETER_ROTATION) == 1,
                "legacy WRITE_SETTINGS or cached read incorrect");
        check((Integer) memory.getClass().getMethod("get", int.class).invoke(memory, index) > before,
                "settings generation not incremented");
        check(observed.await(5, TimeUnit.SECONDS), "Settings observer not notified");
        context.getContentResolver().unregisterContentObserver(observer);
        memory.getClass().getMethod("close").invoke(memory);
        invalid(() -> Settings.System.putInt(context.getContentResolver(), Settings.System.ACCELEROMETER_ROTATION, 2));
        invalid(() -> Settings.System.putString(context.getContentResolver(), "bifrost.private", "x"));
        try (FrameworkSettings reopened = new FrameworkSettings(packages, new FrameworkContent())) {
            check(reopened.call("GET_system", "accelerometer_rotation", null, null).getString("value").equals("1"), "System write not persistent");
        }
        Settings.System.putInt(context.getContentResolver(), Settings.System.ACCELEROMETER_ROTATION, 0);
        pkg.applicationInfo.targetSdkVersion = 35;
        pkg.requestedPermissions = original; pkg.requestedPermissionsFlags = new int[original.length]; packages.install(system, pkg);
    }

    private static void pendingIntents(Context context, Object thread, Object loaded, FrameworkPackages packages) throws Exception {
        FrameworkBroadcasts broadcasts = (FrameworkBroadcasts) launcherField("broadcasts");
        broadcasts.attach(thread, NAME);
        ((FrameworkServiceRuntime) launcherField("serviceRuntime")).attach(thread, context.getApplicationInfo());
        FrameworkActivities activities = (FrameworkActivities) launcherField("activities");
        Binder token = new Binder();
        java.util.concurrent.FutureTask<android.app.Activity> root = new java.util.concurrent.FutureTask<>(() -> {
            ActivityInfo info = new ActivityInfo(); info.applicationInfo = context.getApplicationInfo();
            info.packageName = NAME; info.name = ProbeActivity.class.getName();
            android.app.Activity instance = new android.app.Instrumentation().newActivity(ProbeActivity.class, context, token,
                    (android.app.Application) context, new Intent().setComponent(new ComponentName(NAME, info.name)),
                    info, "PendingIntent probe", null, null, null);
            FrameworkActivities.attachThread(instance, thread);
            return instance;
        });
        new android.os.Handler(Looper.getMainLooper()).post(root);
        activities.attach(thread, loaded, new android.app.Instrumentation(), (android.app.Application) context, root.get(5, TimeUnit.SECONDS), token);
        Object activity = endpoint("activity", "android.app.IActivityManager");
        BroadcastReceiver systemReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) { }
        };
        IntentFilter protectedFilter = new IntentFilter(Intent.ACTION_SCREEN_ON);
        protectedFilter.addAction(Intent.ACTION_SCREEN_OFF);
        context.registerReceiver(systemReceiver, protectedFilter);
        context.unregisterReceiver(systemReceiver);
        check(true, "protected-only receiver registration failed");
        denied(() -> context.registerReceiver(systemReceiver, new IntentFilter(NAME + ".UNPROTECTED")));
        protectedFilter.addAction(NAME + ".UNPROTECTED");
        denied(() -> context.registerReceiver(systemReceiver, protectedFilter));
        Intent base = new Intent(NAME + ".DYNAMIC").setPackage(NAME).putExtra("value", 1);
        int immutable = PendingIntent.FLAG_IMMUTABLE;
        check(PendingIntent.getBroadcast(context, 1, base, immutable | PendingIntent.FLAG_NO_CREATE) == null,
                "NO_CREATE created a sender");
        PendingIntent first = PendingIntent.getBroadcast(context, 1, base, immutable);
        check(first.getCreatorPackage().equals(NAME) && first.getCreatorUid() == Process.myUid()
                && first.isImmutable() && first.isBroadcast(), "PendingIntent metadata incorrect");
        android.os.Parcel parcel = android.os.Parcel.obtain();
        try {
            first.writeToParcel(parcel, 0); parcel.setDataPosition(0);
            check(first.equals(PendingIntent.CREATOR.createFromParcel(parcel)), "PendingIntent Binder identity lost in parcel");
        } finally { parcel.recycle(); }
        denied(() -> invoke(activity, "getIntentSenderWithFeature", 1, "foreign", null, null, null, 1, new Intent[] {base}, null, immutable, null, 0));
        invalid(() -> invoke(activity, "getIntentSenderWithFeature", 1, NAME, null, null, null, 1, new Intent[] {new Intent("implicit")}, null, PendingIntent.FLAG_MUTABLE, null, 0));
        check(first.equals(PendingIntent.getBroadcast(context, 1, new Intent(base).putExtra("value", 2), immutable)),
                "extras affect sender identity");
        check(!first.equals(PendingIntent.getBroadcast(context, 2, base, immutable)), "requestCode omitted from identity");
        CountDownLatch delivered = new CountDownLatch(1);
        Intent[] received = new Intent[1];
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) { received[0] = intent; delivered.countDown(); }
        };
        context.registerReceiver(receiver, new IntentFilter(base.getAction()), Context.RECEIVER_NOT_EXPORTED);
        first = PendingIntent.getBroadcast(context, 1, new Intent(base).putExtra("value", 7), immutable | PendingIntent.FLAG_UPDATE_CURRENT);
        first.send(context, 0, new Intent("injected").putExtra("value", 9));
        check(delivered.await(5, TimeUnit.SECONDS) && received[0].getIntExtra("value", 0) == 7
                && base.getAction().equals(received[0].getAction()), "UPDATE_CURRENT or immutable fill-in incorrect");
        context.unregisterReceiver(receiver);
        PendingIntent old = first;
        CountDownLatch cancelled = new CountDownLatch(1);
        Object cancelListener = new FrameworkServices().local("cancel-probe", "com.android.internal.os.IResultReceiver", (method, a) -> {
            if (!method.equals("send")) throw new UnsupportedOperationException(method);
            cancelled.countDown(); return null;
        });
        Object oldTarget = PendingIntent.class.getMethod("getTarget").invoke(old);
        check((Boolean) invoke(activity, "registerIntentSenderCancelListenerEx", oldTarget, cancelListener), "cancel listener registration failed");
        first = PendingIntent.getBroadcast(context, 1, base, immutable | PendingIntent.FLAG_CANCEL_CURRENT);
        check(!old.equals(first) && cancelled.await(5, TimeUnit.SECONDS), "CANCEL_CURRENT failed");
        try { old.send(); throw new AssertionError("Cancelled sender sent"); } catch (PendingIntent.CanceledException expected) { checks++; }
        PendingIntent once = PendingIntent.getBroadcast(context, 3, base, immutable | PendingIntent.FLAG_ONE_SHOT);
        once.send();
        try { once.send(); throw new AssertionError("ONE_SHOT sent twice"); } catch (PendingIntent.CanceledException expected) { checks++; }
        CountDownLatch mutableDelivery = new CountDownLatch(1);
        BroadcastReceiver mutableReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) { received[0] = intent; mutableDelivery.countDown(); }
        };
        context.registerReceiver(mutableReceiver, new IntentFilter(base.getAction()), Context.RECEIVER_NOT_EXPORTED);
        PendingIntent mutable = PendingIntent.getBroadcast(context, 4, base, PendingIntent.FLAG_MUTABLE);
        mutable.send(context, 0, new Intent().putExtra("added", 8));
        check(mutableDelivery.await(5, TimeUnit.SECONDS) && received[0].getIntExtra("added", 0) == 8
                && received[0].getIntExtra("value", 0) == 1, "mutable extras merge failed");
        context.unregisterReceiver(mutableReceiver);
        CountDownLatch copies = new CountDownLatch(2);
        boolean[] isolated = {false};
        BroadcastReceiver mutate = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) { intent.getIntArrayExtra("array")[0] = 9; copies.countDown(); }
        };
        BroadcastReceiver inspect = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) { isolated[0] = intent.getIntArrayExtra("array")[0] == 1; copies.countDown(); }
        };
        context.registerReceiver(mutate, new IntentFilter(base.getAction()), Context.RECEIVER_NOT_EXPORTED);
        context.registerReceiver(inspect, new IntentFilter(base.getAction()), Context.RECEIVER_NOT_EXPORTED);
        int[] originalArray = {1};
        context.sendBroadcast(new Intent(base).putExtra("array", originalArray));
        check(copies.await(5, TimeUnit.SECONDS) && isolated[0] && originalArray[0] == 1, "broadcast nested extras were shared");
        context.unregisterReceiver(mutate); context.unregisterReceiver(inspect);
        manifestDelivery = new CountDownLatch(1);
        Intent manifest = new Intent(NAME + ".MANIFEST").setComponent(new ComponentName(NAME, ProbeReceiver.class.getName()));
        PendingIntent manifestSender = PendingIntent.getBroadcast(context, 5, manifest, immutable);
        manifestSender.send();
        check(manifestDelivery.await(5, TimeUnit.SECONDS) && manifest.getAction().equals(manifestIntent.getAction()), "manifest receiver delivery failed");
        CountDownLatch orderedDelivery = new CountDownLatch(1);
        context.sendOrderedBroadcast(manifest, null, new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) { if (getResultCode() == 8) orderedDelivery.countDown(); }
        }, new android.os.Handler(Looper.getMainLooper()), 7, null, null);
        check(orderedDelivery.await(5, TimeUnit.SECONDS), "finishReceiver lost ordered result");
        serviceDelivery = new CountDownLatch(1);
        Intent service = new Intent().setComponent(new ComponentName(NAME, ProbeService.class.getName())).putExtra("service", 11);
        PendingIntent.getService(context, 6, service, immutable).send();
        check(serviceDelivery.await(5, TimeUnit.SECONDS) && serviceIntent.getIntExtra("service", 0) == 11, "service sender delivery failed");
        try { PendingIntent.getForegroundService(context, 7, service, immutable).send(); throw new AssertionError("Foreground service silently accepted"); }
        catch (UnsupportedOperationException expected) { checks++; }
        resultDelivery = new CountDownLatch(1);
        Object resultSender = invoke(activity, "getIntentSenderWithFeature", 3, NAME, null, token, null, 17, new Intent[] {new Intent("result")}, null, immutable, null, 0);
        check((Integer) invoke(activity, "sendIntentSender", null, resultSender, null, 23, null, null, null, null, null) == 0
                && resultDelivery.await(5, TimeUnit.SECONDS) && "result".equals(resultIntent.getAction()), "Activity-result sender delivery failed");
        PendingIntent activitySender = PendingIntent.getActivity(context, 8, new Intent("unresolved"), immutable);
        check(activitySender.isActivity(), "Activity sender type lost");
        try { activitySender.send(); throw new AssertionError("Unresolved Activity send succeeded"); }
        catch (PendingIntent.CanceledException expected) { checks++; }
        android.app.AlarmManager alarm = context.getSystemService(android.app.AlarmManager.class);
        denied(() -> alarm.setExact(android.app.AlarmManager.ELAPSED_REALTIME, 0, manifestSender));
        manifestDelivery = new CountDownLatch(1);
        alarm.set(android.app.AlarmManager.ELAPSED_REALTIME, android.os.SystemClock.elapsedRealtime(), manifestSender);
        check(manifestDelivery.await(15, TimeUnit.SECONDS), "PendingIntent alarm did not deliver");
        manifestDelivery = new CountDownLatch(1);
        alarm.set(android.app.AlarmManager.ELAPSED_REALTIME, android.os.SystemClock.elapsedRealtime(), manifestSender); alarm.cancel(manifestSender);
        check(!manifestDelivery.await(6, TimeUnit.SECONDS), "remove(operation) did not cancel alarm");
        manifestTimes.clear(); manifestDelivery = new CountDownLatch(2);
        alarm.setRepeating(android.app.AlarmManager.ELAPSED_REALTIME, android.os.SystemClock.elapsedRealtime(), 1000, manifestSender);
        check(manifestDelivery.await(75, TimeUnit.SECONDS) && manifestTimes.size() == 2
                && manifestTimes.get(1) - manifestTimes.get(0) >= 59900, "repeating PendingIntent alarm or 60-second minimum incorrect");
        alarm.cancel(manifestSender);
        first.cancel(); mutable.cancel(); manifestSender.cancel(); activitySender.cancel();
        check(PendingIntent.getBroadcast(context, 1, base, immutable | PendingIntent.FLAG_NO_CREATE) == null, "cancelled sender remains in registry");
    }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static Object invoke(Object endpoint, String name, Object... args) throws Exception {
        for (Method method : endpoint.getClass().getInterfaces()[0].getMethods())
            if (method.getName().equals(name)) {
                try { return method.invoke(endpoint, args); }
                catch (InvocationTargetException error) {
                    if (error.getCause() instanceof Exception) throw (Exception) error.getCause();
                    throw error;
                }
            }
        throw new AssertionError(name);
    }
    private static Object endpoint(String service, String iface) throws Exception {
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, service);
        return binder.queryLocalInterface(iface);
    }
    private interface Action { void run() throws Exception; }
    private static final class DeathToken extends Binder {
        private final List<IBinder.DeathRecipient> recipients = new ArrayList<>();
        private boolean alive = true;
        @Override public boolean pingBinder() { return alive; }
        @Override public void linkToDeath(IBinder.DeathRecipient recipient, int flags) {
            recipients.add(recipient);
        }
        @Override public boolean unlinkToDeath(IBinder.DeathRecipient recipient, int flags) {
            return recipients.remove(recipient);
        }
        void die() {
            alive = false;
            for (IBinder.DeathRecipient recipient : new ArrayList<>(recipients)) recipient.binderDied();
        }
    }
    private static void denied(Action action) throws Exception {
        try { action.run(); throw new AssertionError("foreign/privileged operation allowed"); }
        catch (SecurityException expected) { checks++; }
    }
    private static void invalid(Action action) throws Exception {
        try { action.run(); throw new AssertionError("invalid input allowed"); }
        catch (IllegalArgumentException expected) { checks++; }
    }

    private static void notifications(Context context, FrameworkPackages packages) throws Exception {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        NotificationChannelGroup group = new NotificationChannelGroup("games", "Games");
        manager.createNotificationChannelGroup(group);
        NotificationChannel channel = new NotificationChannel("rewards", "Rewards", NotificationManager.IMPORTANCE_HIGH);
        channel.setGroup("games"); channel.setDescription("Original");
        Uri sound = Uri.parse("content://org.bifrost.servicebatch/sound");
        channel.setSound(sound, new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION).build());
        channel.setVibrationPattern(new long[] {1, 2, 3});
        manager.createNotificationChannel(channel);
        channel.setDescription("Caller mutation");
        NotificationChannel found = manager.getNotificationChannel("rewards");
        check(found != null && found.getDescription().equals("Original") && found.getSound().equals(sound), "channel creation/copy failed");
        found.setDescription("Reply mutation");
        check(manager.getNotificationChannel("rewards").getDescription().equals("Original"), "channel reply aliases service");
        NotificationChannel update = new NotificationChannel("rewards", "Renamed", NotificationManager.IMPORTANCE_LOW);
        update.setSound(null, null); update.setVibrationPattern(new long[] {99});
        manager.createNotificationChannel(update);
        found = manager.getNotificationChannel("rewards");
        check(found.getName().equals("Renamed") && found.getImportance() == NotificationManager.IMPORTANCE_LOW,
                "channel rename/downgrade failed");
        check(found.getSound().equals(sound) && Arrays.equals(found.getVibrationPattern(), new long[] {1, 2, 3}),
                "channel update reset immutable settings");
        update = new NotificationChannel("rewards", "Raised", NotificationManager.IMPORTANCE_HIGH);
        manager.createNotificationChannel(update);
        check(manager.getNotificationChannel("rewards").getImportance() == NotificationManager.IMPORTANCE_LOW,
                "app raised existing importance");
        check(manager.getNotificationChannelGroups().size() == 1
                && manager.getNotificationChannelGroup("games").getChannels().size() == 1, "group population failed");
        manager.deleteNotificationChannel("rewards");
        check(manager.getNotificationChannel("rewards") == null && manager.getNotificationChannels().isEmpty(),
                "deleted channel still visible");
        manager.createNotificationChannel(new NotificationChannel("rewards", "Restored", NotificationManager.IMPORTANCE_HIGH));
        found = manager.getNotificationChannel("rewards");
        check(found.getImportance() == NotificationManager.IMPORTANCE_LOW && found.getSound().equals(sound),
                "deleted channel lost settings on recreation");
        invalid(() -> manager.createNotificationChannel(new NotificationChannel("miscellaneous", "Reserved", 3)));
        invalid(() -> {
            NotificationChannel missing = new NotificationChannel("missing-group", "Missing", 3);
            missing.setGroup("absent"); manager.createNotificationChannel(missing);
        });
        check(!manager.areNotificationsEnabled(), "runtime notification permission was invented");
        manager.setNotificationDelegate(NAME);
        check(NAME.equals(manager.getNotificationDelegate()), "notification delegate not recorded");
        manager.setNotificationDelegate("com.google.android.gms");
        check(NAME.equals(manager.getNotificationDelegate()), "uninstalled delegate replaced an existing delegate");
        // Reconstruct the service: persisted state must survive its Java objects.
        new FrameworkNotifications(packages).install(new FrameworkServices());
        Object remote = endpoint("notification", "android.app.INotificationManager");
        found = (NotificationChannel) invoke(remote, "getNotificationChannel", NAME, 0, NAME, "rewards");
        check(found.getSound().equals(sound) && found.getImportance() == 2, "channel persistence failed");
        check(NAME.equals(invoke(remote, "getNotificationDelegate", NAME)), "delegate persistence failed");
        check((Boolean) invoke(remote, "canNotifyAsPackage", NAME, NAME, 0)
                && !(Boolean) invoke(remote, "canNotifyAsPackage", NAME, "foreign.package", 0),
                "notification delegation ownership incorrect");
        denied(() -> invoke(remote, "canNotifyAsPackage", NAME, NAME, 10));
        invoke(remote, "setNotificationDelegate", NAME, null);
        check(invoke(remote, "getNotificationDelegate", NAME) == null, "delegate revocation failed");
        manager.createNotificationChannel(new NotificationChannel("ungrouped", "Ungrouped", 3));
        manager.deleteNotificationChannelGroup(null);
        check(manager.getNotificationChannel("ungrouped") != null, "null group deletion removed ungrouped channels");
        manager.deleteNotificationChannel("ungrouped");
        denied(() -> invoke(remote, "getNotificationChannel", NAME, 10, NAME, "rewards"));
        denied(() -> invoke(remote, "getNotificationChannel", NAME, 0, "foreign.package", "rewards"));
        manager.deleteNotificationChannelGroup("games");
        check(manager.getNotificationChannelGroup("games") == null && manager.getNotificationChannel("rewards") == null,
                "group deletion did not delete its channels");
    }

    private static void audio(Context context) throws Exception {
        AudioManager manager = context.getSystemService(AudioManager.class);
        int stereo = android.media.AudioTrack.getMinBufferSize(48000,
                android.media.AudioFormat.CHANNEL_OUT_STEREO, android.media.AudioFormat.ENCODING_PCM_16BIT);
        int mono = android.media.AudioTrack.getMinBufferSize(48000,
                android.media.AudioFormat.CHANNEL_OUT_MONO, android.media.AudioFormat.ENCODING_PCM_16BIT);
        check(stereo > 0 && stereo == mono * 2, "native AudioTrack buffer-size query failed");
        check(android.media.AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC) > 0,
                "native AudioTrack sample-rate query failed");
        check(manager.requestAudioFocus(null, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN) == 1,
                "public AudioManager focus request failed");
        check(manager.abandonAudioFocus(null) == 1, "public AudioManager focus abandonment failed");
        android.os.HandlerThread listenerThread = new android.os.HandlerThread("audio-focus-client");
        listenerThread.start();
        List<Integer> delivered = java.util.Collections.synchronizedList(new ArrayList<>());
        java.util.concurrent.CountDownLatch loss = new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.CountDownLatch regain = new java.util.concurrent.CountDownLatch(1);
        android.media.AudioFocusRequest original = new android.media.AudioFocusRequest.Builder(1)
                .setOnAudioFocusChangeListener(value -> {
                    delivered.add(value);
                    if (value == -2) loss.countDown();
                    if (value == 1) regain.countDown();
                }, new android.os.Handler(listenerThread.getLooper())).build();
        android.media.AudioFocusRequest temporary = new android.media.AudioFocusRequest.Builder(2).build();
        try {
            check(manager.requestAudioFocus(original) == 1 && manager.requestAudioFocus(temporary) == 1,
                    "public transient focus requests failed");
            check(loss.await(3, java.util.concurrent.TimeUnit.SECONDS), "public focus loss callback missing");
            manager.abandonAudioFocusRequest(temporary);
            check(regain.await(3, java.util.concurrent.TimeUnit.SECONDS)
                    && delivered.equals(Arrays.asList(-2, 1)), "public focus regain callback missing");
        } finally { manager.abandonAudioFocusRequest(original); listenerThread.quitSafely(); }
        Object remote = endpoint("audio", "android.media.IAudioService");
        List<String> events = new ArrayList<>();
        FrameworkServices services = new FrameworkServices();
        Object dispatcher = services.local("focus-test", "android.media.IAudioFocusDispatcher", (method, args) -> {
            events.add(args[1] + ":" + args[0]); return null;
        });
        AudioAttributes attrs = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
        Binder first = new Binder(), second = new Binder(), third = new Binder();
        check((Integer) invoke(remote, "requestAudioFocus", attrs, 1, first, dispatcher, "first", NAME, null, 0, null, 35) == 1,
                "first owner rejected");
        invoke(remote, "requestAudioFocus", attrs, 2, second, dispatcher, "second", NAME, null, 0, null, 35);
        check(events.equals(Arrays.asList("first:-2")), "transient loss not delivered");
        invoke(remote, "requestAudioFocus", attrs, 3, third, dispatcher, "third", NAME, null, 0, null, 35);
        check(events.equals(Arrays.asList("first:-2", "second:-3")), "duck weakened existing loss");
        invoke(remote, "abandonAudioFocus", dispatcher, "third", attrs, NAME);
        check(events.get(events.size() - 1).equals("second:1"), "previous owner not restored");
        invoke(remote, "abandonAudioFocus", dispatcher, "second", attrs, NAME);
        check(events.get(events.size() - 1).equals("first:1"), "original owner not restored");
        int count = events.size();
        invoke(remote, "requestAudioFocus", attrs, 1, first, dispatcher, "first", NAME, null, 0, null, 35);
        check(events.size() == count, "duplicate request sent focus change");
        invoke(remote, "requestAudioFocus", attrs, 1, second, dispatcher, "second", NAME, null, 0, null, 35);
        check(events.get(events.size() - 1).equals("first:-1"), "permanent loss not delivered");
        invoke(remote, "abandonAudioFocus", dispatcher, "second", attrs, NAME);
        check((Integer) invoke(remote, "getCurrentAudioFocus") == 0, "permanently displaced owner returned");
        denied(() -> invoke(remote, "requestAudioFocus", attrs, 1, first, dispatcher, "bad", "foreign", null, 0, null, 35));
        denied(() -> invoke(remote, "requestAudioFocus", attrs, 1, first, dispatcher, "bad", NAME, null, 4, null, 35));
        invalid(() -> invoke(remote, "getStreamVolume", -1));
        DeathToken dying = new DeathToken();
        invoke(remote, "requestAudioFocus", attrs, 1, first, dispatcher, "first", NAME, null, 0, null, 35);
        invoke(remote, "requestAudioFocus", attrs, 2, dying, dispatcher, "dying", NAME, null, 0, null, 35);
        dying.die();
        check(events.get(events.size() - 1).equals("first:1"), "dead focus owner did not restore predecessor");
        check((Integer) invoke(remote, "requestAudioFocus", attrs, 2, dying, dispatcher, "dead", NAME, null, 0, null, 35) == 0,
                "dead focus client was granted focus");
        invoke(remote, "abandonAudioFocus", dispatcher, "first", attrs, NAME);
    }

    private static void power(Context context) throws Exception {
        PowerManager manager = context.getSystemService(PowerManager.class);
        check(manager.isWakeLockLevelSupported(PowerManager.PARTIAL_WAKE_LOCK), "partial wake lock unsupported");
        PowerManager.WakeLock lock = manager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "servicebatch");
        lock.acquire(); lock.acquire();
        check(lock.isHeld(), "wake lock acquisition failed");
        lock.release(); check(lock.isHeld(), "reference-counted lock released too soon");
        lock.release(); check(!lock.isHeld(), "wake lock release failed");
        Object remote = endpoint("power", "android.os.IPowerManager");
        Binder token = new Binder();
        List<Boolean> states = new ArrayList<>();
        Object callback = new FrameworkServices().local("wake-test", "android.os.IWakeLockCallback", (name, args) -> {
            states.add((Boolean) args[0]); return null;
        });
        invoke(remote, "acquireWakeLock", token, 1, "test", NAME, null, null, -1, callback);
        invoke(remote, "releaseWakeLock", token, 0);
        check(states.equals(Arrays.asList(true, false)), "wake lock callbacks incorrect");
        denied(() -> invoke(remote, "acquireWakeLock", token, 1, "test", "foreign", null, null, -1, null));
        invalid(() -> invoke(remote, "acquireWakeLock", token, 1, "test", NAME, null, null, 1234, null));
        DeathToken dying = new DeathToken();
        invoke(remote, "acquireWakeLock", dying, 1, "test", NAME, null, null, -1, callback);
        dying.die();
        check(states.equals(Arrays.asList(true, false, true, false)), "dead wake lock client retained lock");
    }

    private static void wifi(Context context) throws Exception {
        android.net.wifi.WifiManager manager = context.getSystemService(android.net.wifi.WifiManager.class);
        check(manager != null && manager.isWifiEnabled(), "virtual Wi-Fi service unavailable");
        android.net.wifi.WifiInfo info = manager.getConnectionInfo();
        check(info.getMacAddress().equals("02:00:00:00:00:00") && info.getNetworkId() == -1
                && info.getBSSID() == null, "virtual Wi-Fi invented private access-point details");
        check(manager.getScanResults().isEmpty(), "virtual Wi-Fi invented scan results");
        check(!manager.setWifiEnabled(false) && manager.isWifiEnabled(), "virtual radio changed host state");
        Object remote = endpoint("wifi", "android.net.wifi.IWifiManager");
        denied(() -> invoke(remote, "getConnectionInfo", "foreign", null));
        android.net.DhcpInfo dhcp = manager.getDhcpInfo();
        check(dhcp.ipAddress == 0 && dhcp.gateway == 0, "virtual DHCP invented host addresses");
    }

    private static void uiMode(Context context) throws Exception {
        android.app.UiModeManager manager = context.getSystemService(android.app.UiModeManager.class);
        check(manager != null, "UiModeManager unavailable");
        check(manager.getCurrentModeType() == android.content.res.Configuration.UI_MODE_TYPE_NORMAL
                && manager.getNightMode() == android.app.UiModeManager.MODE_NIGHT_NO
                && manager.getContrast() == 0f, "UI mode defaults differ from AOSP resources");
        check(FrameworkUiMode.CONFIGURATION_UI_MODE == (android.content.res.Configuration.UI_MODE_TYPE_NORMAL
                | android.content.res.Configuration.UI_MODE_NIGHT_NO), "configuration uiMode differs from the service");
        manager.setNightMode(android.app.UiModeManager.MODE_NIGHT_YES);
        check(manager.getNightMode() == android.app.UiModeManager.MODE_NIGHT_NO, "locked night mode changed");
        manager.setApplicationNightMode(android.app.UiModeManager.MODE_NIGHT_NO);
        try {
            manager.setApplicationNightMode(android.app.UiModeManager.MODE_NIGHT_YES);
            throw new AssertionError("per-app dark mode claimed without a configuration change");
        } catch (UnsupportedOperationException expected) { checks++; }
        Object remote = endpoint("uimode", "android.app.IUiModeManager");
        invalid(() -> invoke(remote, "setNightMode", 7));
        denied(() -> invoke(remote, "getActiveProjectionTypes"));
    }

    private static void permissions(Context context) throws Exception {
        android.content.pm.PackageManager pm = context.getPackageManager();
        int granted = android.content.pm.PackageManager.PERMISSION_GRANTED;
        check(pm.checkPermission("android.permission.WAKE_LOCK", NAME) == granted, "requested normal permission denied");
        check(pm.checkPermission("android.permission.CAMERA", NAME) != granted, "runtime permission invented");
        check(pm.checkPermission("android.permission.WAKE_LOCK", "foreign.package") != granted, "foreign package granted");
        android.content.pm.PermissionInfo camera = pm.getPermissionInfo("android.permission.CAMERA", 0);
        check(camera.getProtection() == android.content.pm.PermissionInfo.PROTECTION_DANGEROUS
                && "android".equals(camera.packageName), "platform permission definition incorrect");
        camera.protectionLevel = 0;
        check(pm.getPermissionInfo("android.permission.CAMERA", 0).getProtection()
                == android.content.pm.PermissionInfo.PROTECTION_DANGEROUS, "permission reply aliases service");
        try {
            pm.getPermissionInfo("org.bifrost.absent", 0);
            throw new AssertionError("unknown permission resolved");
        } catch (android.content.pm.PackageManager.NameNotFoundException expected) { checks++; }
        // System APIs absent from the compile-time SDK jar.
        Class<?> managerType = Class.forName("android.permission.PermissionManager");
        Object manager = context.getSystemService(managerType);
        java.util.List<?> splits = (java.util.List<?>) managerType.getMethod("getSplitPermissions").invoke(manager);
        check(splits.size() == 24, "split permission table incomplete");
        Method self = Class.forName("android.content.PermissionChecker")
                .getMethod("checkSelfPermissionForPreflight", Context.class, String.class);
        check((Integer) self.invoke(null, context, "android.permission.WAKE_LOCK") == 0
                && (Integer) self.invoke(null, context, "android.permission.CAMERA") == 2,
                "permission checker results incorrect");
        Object remote = endpoint("permissionmgr", "android.permission.IPermissionManager");
        check(!(Boolean) invoke(remote, "shouldShowRequestPermissionRationale", NAME, "android.permission.CAMERA", 0, 0),
                "rationale shown without a user denial");
        android.content.AttributionSource own = new android.content.AttributionSource.Builder(Process.myUid())
                .setPackageName(NAME).build();
        Object state = android.content.AttributionSource.class.getMethod("asState").invoke(own);
        invoke(remote, "registerAttributionSource", state);
        check((Boolean) invoke(remote, "isRegisteredAttributionSource", state), "attribution source not registered");
        Object foreign = android.content.AttributionSource.class.getMethod("asState").invoke(
                new android.content.AttributionSource.Builder(Process.myUid() + 1).setPackageName(NAME).build());
        denied(() -> invoke(remote, "registerAttributionSource", foreign));
        denied(() -> invoke(remote, "getRegisteredAttributionSourceCount", Process.myUid()));
    }

    private static void battery(Context context) throws Exception {
        android.os.BatteryManager manager = context.getSystemService(android.os.BatteryManager.class);
        check(manager != null && !manager.isCharging(), "BatteryManager unavailable or charging without a battery");
        check(manager.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY) == 100
                && manager.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CURRENT_NOW) == Integer.MIN_VALUE,
                "battery properties invented hardware values");
        android.content.IntentFilter filter = new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED);
        android.content.Intent sticky = context.registerReceiver(null, filter);
        check(sticky != null && sticky.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1) == 100
                && sticky.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, -1) == 100
                && sticky.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, -1) == android.os.BatteryManager.BATTERY_PLUGGED_AC
                && !sticky.getBooleanExtra(android.os.BatteryManager.EXTRA_PRESENT, true), "sticky battery intent incorrect");
        check(context.registerReceiver(null, new android.content.IntentFilter("org.bifrost.none")) == null,
                "sticky intent returned for an unrelated filter");
        android.os.HandlerThread thread = new android.os.HandlerThread("battery-receiver");
        thread.start();
        java.util.concurrent.CountDownLatch received = new java.util.concurrent.CountDownLatch(1);
        android.content.BroadcastReceiver receiver = new android.content.BroadcastReceiver() {
            @Override public void onReceive(Context c, android.content.Intent intent) {
                if (isInitialStickyBroadcast() && intent.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1) == 100)
                    received.countDown();
            }
        };
        context.registerReceiver(receiver, filter, null, new android.os.Handler(thread.getLooper()));
        check(received.await(10, java.util.concurrent.TimeUnit.SECONDS), "sticky battery intent not delivered");
        context.unregisterReceiver(receiver);
        thread.quitSafely();
    }

    private static void vibratorAndKeyguard(Context context) throws Exception {
        android.os.Vibrator vibrator = context.getSystemService(android.os.Vibrator.class);
        check(vibrator != null && !vibrator.hasVibrator(), "vibrator invented");
        vibrator.vibrate(android.os.VibrationEffect.createOneShot(20, android.os.VibrationEffect.DEFAULT_AMPLITUDE));
        vibrator.cancel();
        Object manager = context.getSystemService(Class.forName("android.os.VibratorManager"));
        check(((int[]) manager.getClass().getMethod("getVibratorIds").invoke(manager)).length == 0, "vibrator ids invented");
        android.app.KeyguardManager keyguard = context.getSystemService(android.app.KeyguardManager.class);
        check(keyguard != null && !keyguard.isKeyguardLocked() && !keyguard.isKeyguardSecure()
                && !keyguard.isDeviceLocked() && !keyguard.isDeviceSecure()
                && !keyguard.inKeyguardRestrictedInputMode(), "keyguard state invented a lock");
        android.app.KeyguardManager.KeyguardLock lock = keyguard.newKeyguardLock("batch");
        lock.disableKeyguard(); lock.reenableKeyguard();
        boolean[] exited = new boolean[1];
        keyguard.exitKeyguardSecurely(success -> exited[0] = success);
        check(exited[0], "unlocked keyguard exit not reported");
        Object trust = endpoint("trust", "android.app.trust.ITrustManager");
        denied(() -> invoke(trust, "isDeviceLocked", 10, 0));
        denied(() -> invoke(trust, "isTrustUsuallyManaged", 0));
    }

    private static void clipboard(Context context, FrameworkPackages packages) throws Exception {
        android.content.ClipboardManager manager = context.getSystemService(android.content.ClipboardManager.class);
        manager.setPrimaryClip(android.content.ClipData.newPlainText("code", "unfocused"));
        check(manager.getPrimaryClip() == null && !manager.hasPrimaryClip(), "clipboard readable without window focus");
        // A focused instance: the play window holds focus while it is open.
        new FrameworkClipboard(packages, () -> true).install(new FrameworkServices());
        Object remote = endpoint("clipboard", "android.content.IClipboard");
        int[] changes = new int[1];
        Class<?> listenerType = Class.forName("android.content.IOnPrimaryClipChangedListener");
        android.os.Binder listenerBinder = new android.os.Binder();
        Object listener = java.lang.reflect.Proxy.newProxyInstance(listenerType.getClassLoader(),
                new Class<?>[] {listenerType}, (self, method, args) -> {
                    if (method.getName().equals("asBinder")) return listenerBinder;
                    if (method.getName().equals("dispatchPrimaryClipChanged")) changes[0]++;
                    return null;
                });
        invoke(remote, "addPrimaryClipChangedListener", listener, NAME, null, 0, 0);
        android.content.ClipData clip = android.content.ClipData.newPlainText("code", "GIFT-1234");
        invoke(remote, "setPrimaryClip", clip, NAME, null, 0, 0);
        android.content.ClipData read = (android.content.ClipData) invoke(remote, "getPrimaryClip", NAME, null, 0, 0);
        check(read != null && read != clip && "GIFT-1234".contentEquals(read.getItemAt(0).getText())
                && read.getDescription().getTimestamp() > 0, "focused clipboard read failed or aliased");
        check((Boolean) invoke(remote, "hasClipboardText", NAME, null, 0, 0) && changes[0] == 1,
                "clipboard text/listener state incorrect");
        invoke(remote, "clearPrimaryClip", NAME, null, 0, 0);
        check(!(Boolean) invoke(remote, "hasPrimaryClip", NAME, null, 0, 0) && changes[0] == 2, "clipboard clear failed");
        denied(() -> invoke(remote, "setPrimaryClip", clip, "foreign.package", null, 0, 0));
        denied(() -> invoke(remote, "getPrimaryClip", NAME, null, 10, 0));
    }

    private static void alarmsAccountsLocation(Context context) throws Exception {
        android.app.AlarmManager alarms = context.getSystemService(android.app.AlarmManager.class);
        check(alarms != null && !alarms.canScheduleExactAlarms() && alarms.getNextAlarmClock() == null,
                "AlarmManager unavailable or exact alarms granted without permission");
        android.os.HandlerThread thread = new android.os.HandlerThread("alarm-listener");
        thread.start();
        android.os.Handler handler = new android.os.Handler(thread.getLooper());
        java.util.concurrent.CountDownLatch fired = new java.util.concurrent.CountDownLatch(1);
        boolean[] cancelledFired = new boolean[1];
        android.app.AlarmManager.OnAlarmListener cancelled = () -> cancelledFired[0] = true;
        long start = android.os.SystemClock.elapsedRealtime();
        alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, start, "cancelled", cancelled, handler);
        alarms.cancel(cancelled);
        // An exact listener alarm needs no permission; app alarms fire no
        // sooner than MIN_FUTURITY (5 s) after scheduling.
        alarms.setExact(android.app.AlarmManager.ELAPSED_REALTIME, start, "fired", fired::countDown, handler);
        check(fired.await(15, java.util.concurrent.TimeUnit.SECONDS)
                && android.os.SystemClock.elapsedRealtime() - start >= 5000 && !cancelledFired[0],
                "listener alarm timing or cancellation incorrect");
        thread.quitSafely();
        Object alarmRemote = endpoint("alarm", "android.app.IAlarmManager");
        Class<?> listenerType = Class.forName("android.app.IAlarmListener");
        Binder listenerBinder = new Binder();
        Object listener = java.lang.reflect.Proxy.newProxyInstance(listenerType.getClassLoader(),
                new Class<?>[] {listenerType}, (self, method, args) ->
                        method.getName().equals("asBinder") ? listenerBinder : null);
        invalid(() -> invoke(alarmRemote, "set", NAME, 3, 0L, 0L, 60000L, 0, null, listener, null, null, null));
        denied(() -> invoke(alarmRemote, "setTime", 0L));

        android.accounts.AccountManager accounts = android.accounts.AccountManager.get(context);
        check(accounts.getAccounts().length == 0 && accounts.getAccountsByType("com.google").length == 0
                && accounts.getAuthenticatorTypes().length == 0, "accounts or authenticators invented");
        denied(() -> accounts.addAccountExplicitly(new android.accounts.Account("player", "com.google"), null, null));

        android.location.LocationManager location = context.getSystemService(android.location.LocationManager.class);
        check(location != null && location.getAllProviders().equals(Arrays.asList("passive"))
                && location.isLocationEnabled() && location.getProviders(true).isEmpty()
                && location.getBestProvider(new android.location.Criteria(), true) == null,
                "location providers differ from a device without GNSS or Play services");
        denied(() -> location.getLastKnownLocation("passive"));

        NotificationManager notifications = context.getSystemService(NotificationManager.class);
        notifications.cancelAll();
        notifications.cancel("tag", 7);
        check(notifications.getActiveNotifications().length == 0, "active notifications invented");
    }

    private static void run() throws Exception {
        Looper.prepareMainLooper();
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        threadType.getMethod("initializeMainlineModules").invoke(null);
        Method install = ManagedLauncher.class.getDeclaredMethod("installServices", boolean.class);
        install.setAccessible(true);
        install.invoke(null, false);
        java.lang.reflect.Field packagesField = ManagedLauncher.class.getDeclaredField("packages");
        packagesField.setAccessible(true);
        FrameworkPackages packages = (FrameworkPackages) packagesField.get(null);
        Object thread = threadType.getMethod("systemMain").invoke(null);
        Context system = (Context) threadType.getMethod("getSystemContext").invoke(thread);
        PackageInfo pkg = new PackageInfo(); pkg.packageName = NAME;
        ApplicationInfo info = pkg.applicationInfo = new ApplicationInfo();
        info.packageName = NAME; info.processName = NAME; info.uid = Process.myUid();
        info.enabled = true; info.targetSdkVersion = 35;
        info.sourceDir = info.publicSourceDir = "/system/framework/framework-res.apk";
        info.dataDir = "/data/user/0/" + NAME;
        ActivityInfo receiver = new ActivityInfo();
        receiver.name = ProbeReceiver.class.getName(); receiver.packageName = NAME; receiver.applicationInfo = info;
        receiver.enabled = true; receiver.exported = false; receiver.processName = NAME;
        pkg.receivers = new ActivityInfo[] {receiver};
        ServiceInfo service = new ServiceInfo();
        service.name = ProbeService.class.getName(); service.packageName = NAME; service.applicationInfo = info;
        service.enabled = true; service.exported = false; service.processName = NAME;
        pkg.services = new ServiceInfo[] {service};
        new File(info.dataDir).mkdirs();
        pkg.requestedPermissions = new String[] {"android.permission.WAKE_LOCK", "android.permission.MODIFY_AUDIO_SETTINGS",
                "android.permission.ACCESS_WIFI_STATE", "android.permission.CHANGE_WIFI_STATE",
                "android.permission.VIBRATE", "android.permission.DISABLE_KEYGUARD"};
        pkg.requestedPermissionsFlags = new int[pkg.requestedPermissions.length];
        packages.install(system, pkg);
        Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
        Object loaded = threadType.getMethod("getPackageInfoNoCheck", ApplicationInfo.class, compatibility)
                .invoke(thread, info, compatibility.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
        java.lang.reflect.Field loader = loaded.getClass().getDeclaredField("mClassLoader");
        loader.setAccessible(true); loader.set(loaded, ManagedServiceBatch.class.getClassLoader());
        Method bind = ManagedLauncher.class.getDeclaredMethod("bindApplicationMetadata", Object.class,
                Context.class, ApplicationInfo.class, Object.class);
        bind.setAccessible(true); bind.invoke(null, thread, system, info, loaded);
        Method make = loaded.getClass().getDeclaredMethod("makeApplicationInner", boolean.class,
                android.app.Instrumentation.class);
        make.setAccessible(true);
        Context context = (Context) make.invoke(loaded, false, null);
        java.lang.reflect.Field initial = threadType.getDeclaredField("mInitialApplication");
        initial.setAccessible(true); initial.set(thread, context);
        new Thread(() -> {
        try {
        usersAndIdle(context);
        notifications(context, packages); audio(context); power(context); wifi(context);
        uiMode(context); permissions(context);
        battery(context); vibratorAndKeyguard(context); clipboard(context, packages);
        alarmsAccountsLocation(context);
        settings(context, system, packages, pkg);
        System.out.println("MANAGED_SETTINGS_OK");
        pendingIntents(context, thread, loaded, packages);
        System.out.println("MANAGED_PENDING_INTENTS_OK");
        pkg.requestedPermissions = new String[0]; pkg.requestedPermissionsFlags = new int[0];
        packages.install(system, pkg);
        Object wifi = endpoint("wifi", "android.net.wifi.IWifiManager");
        check((Integer) invoke(wifi, "getVerboseLoggingLevel") == 0, "Wi-Fi construction query requires a permission");
        denied(() -> invoke(wifi, "getConnectionInfo", NAME, null));
        Object power = endpoint("power", "android.os.IPowerManager");
        denied(() -> invoke(power, "acquireWakeLock", new Binder(), 1, "denied", NAME, null, null, -1, null));
        Object vibrators = endpoint("vibrator_manager", "android.os.IVibratorManagerService");
        denied(() -> invoke(vibrators, "cancelVibrate", -1, new Binder()));
        Object window = endpoint("window", "android.view.IWindowManager");
        denied(() -> invoke(window, "disableKeyguard", new Binder(), "denied", 0));
        System.out.println("MANAGED_SERVICE_BATCH_OK checks=" + checks);
        System.out.println("FRAMEWORK_OK");
        System.exit(0);
        } catch (Throwable error) { error.printStackTrace(); System.exit(1); }
        }, "service-checks").start();
        Looper.loop();
    }

    public static void main(String[] args) {
        try { run(); System.exit(0); }
        catch (Throwable error) { error.printStackTrace(); System.exit(1); }
    }
}
