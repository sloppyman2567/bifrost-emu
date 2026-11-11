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
import android.os.Parcel;
import android.os.PowerManager;
import android.os.Process;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.View;
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
        ActivityInfo info = new ActivityInfo(); info.applicationInfo = context.getApplicationInfo();
        info.packageName = NAME; info.name = ProbeActivity.class.getName();
        Intent launch = new Intent().setComponent(new ComponentName(NAME, info.name));
        java.util.concurrent.FutureTask<android.app.Activity> root = new java.util.concurrent.FutureTask<>(() -> {
            android.app.Activity instance = new android.app.Instrumentation().newActivity(ProbeActivity.class, context, token,
                    (android.app.Application) context, launch,
                    info, "PendingIntent probe", null, null, null);
            FrameworkActivities.attachThread(instance, thread);
            return instance;
        });
        new android.os.Handler(Looper.getMainLooper()).post(root);
        activities.attach(thread, loaded, new android.app.Instrumentation(), (android.app.Application) context, root.get(5, TimeUnit.SECONDS), info, launch, token);
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
        runningServices(context);
        // startForegroundService sends are delivered like any start; the startForeground() they owe is the service's.
        serviceDelivery = new CountDownLatch(1);
        PendingIntent.getForegroundService(context, 7, service, immutable).send();
        check(serviceDelivery.await(5, TimeUnit.SECONDS), "foreground service sender delivery failed");
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
    // FrameworkCursors: PointerIcon types to host cursors, and what a mouse hover sends to the host
    // (through a real View and real PointerIcons: only a changed shape is sent, a refusal is contained).
    private static void cursors(Context context) {
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_NULL) == FrameworkCursors.HIDDEN, "TYPE_NULL is not hidden");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_ARROW) == FrameworkCursors.DEFAULT, "arrow is not the default cursor");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_HAND) == FrameworkCursors.POINTER, "hand is not the pointer");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_TEXT) == FrameworkCursors.TEXT
                && FrameworkCursors.hostCursor(PointerIcon.TYPE_VERTICAL_TEXT) == FrameworkCursors.TEXT, "text cursors");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_WAIT) == FrameworkCursors.WAIT, "wait cursor");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_CROSSHAIR) == FrameworkCursors.CROSSHAIR
                && FrameworkCursors.hostCursor(PointerIcon.TYPE_CELL) == FrameworkCursors.CROSSHAIR, "crosshair cursors");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_ALL_SCROLL) == FrameworkCursors.MOVE
                && FrameworkCursors.hostCursor(PointerIcon.TYPE_GRAB) == FrameworkCursors.MOVE
                && FrameworkCursors.hostCursor(PointerIcon.TYPE_GRABBING) == FrameworkCursors.MOVE, "move cursors");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_NO_DROP) == FrameworkCursors.NOT_ALLOWED, "no-drop cursor");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_HORIZONTAL_DOUBLE_ARROW) == FrameworkCursors.EW_RESIZE
                && FrameworkCursors.hostCursor(PointerIcon.TYPE_VERTICAL_DOUBLE_ARROW) == FrameworkCursors.NS_RESIZE
                && FrameworkCursors.hostCursor(PointerIcon.TYPE_TOP_RIGHT_DIAGONAL_DOUBLE_ARROW) == FrameworkCursors.NESW_RESIZE
                && FrameworkCursors.hostCursor(PointerIcon.TYPE_TOP_LEFT_DIAGONAL_DOUBLE_ARROW) == FrameworkCursors.NWSE_RESIZE,
                "resize cursors");
        check(FrameworkCursors.hostCursor(PointerIcon.TYPE_HELP) == FrameworkCursors.DEFAULT
                && FrameworkCursors.hostCursor(-1) == FrameworkCursors.DEFAULT, "unmapped types fall back to the arrow");
        List<Integer> sent = new ArrayList<>();
        FrameworkCursors shown = new FrameworkCursors(shape -> { sent.add(shape); return 0; });
        View view = new View(context);
        MotionEvent hover = MotionEvent.obtain(0, 0, MotionEvent.ACTION_HOVER_MOVE, 5f, 5f, 0);
        shown.hover(() -> view, hover);
        check(sent.isEmpty(), "a view with no pointer icon changed the host's arrow");
        view.setPointerIcon(PointerIcon.getSystemIcon(context, PointerIcon.TYPE_HAND));
        shown.hover(() -> view, hover); shown.hover(() -> view, hover);
        check(sent.equals(Arrays.asList(FrameworkCursors.POINTER)), "hand was not sent exactly once: " + sent);
        view.setPointerIcon(PointerIcon.getSystemIcon(context, PointerIcon.TYPE_NULL));
        shown.hover(() -> view, hover);
        check(sent.equals(Arrays.asList(FrameworkCursors.POINTER, FrameworkCursors.HIDDEN)), "hidden pointer was not sent: " + sent);
        view.setPointerIcon(null);
        shown.hover(() -> view, hover);
        check(sent.equals(Arrays.asList(FrameworkCursors.POINTER, FrameworkCursors.HIDDEN, FrameworkCursors.DEFAULT)),
                "clearing the icon did not restore the arrow: " + sent);
        shown.hover(() -> null, hover);  // no decor view yet
        // A host that refuses a shape turns the feature off for good, without throwing into input delivery.
        List<Integer> refused = new ArrayList<>();
        FrameworkCursors refusing = new FrameworkCursors(shape -> { refused.add(shape); return -95; });
        view.setPointerIcon(PointerIcon.getSystemIcon(context, PointerIcon.TYPE_HAND));
        refusing.hover(() -> view, hover); refusing.hover(() -> view, hover);
        check(refused.size() == 1, "a refused cursor was retried: " + refused);
        hover.recycle();
        System.out.println("MANAGED_CURSORS_OK");
    }
    // Host gamepads (FrameworkGamepads): a device record reaches InputManager's listener as (id, generation) pairs
    // and becomes an Xbox-style InputDevice; unhandled buttons fall back through Generic.kcm (B is BACK, X is DEL,
    // MODE's HOME is withheld); a joystick record carries the axes; an unhandled hat moves the d-pad focus and a
    // handled motion cancels the synthetic key.
    private static void gamepads() throws Exception {
        FrameworkGamepads pads = new FrameworkGamepads();
        List<int[]> changes = new ArrayList<>();
        Class<?> listenerType = Class.forName("android.hardware.input.IInputDevicesChangedListener");
        Object listener = java.lang.reflect.Proxy.newProxyInstance(listenerType.getClassLoader(),
                new Class<?>[] {listenerType}, (proxy, method, args) -> {
                    if (method.getName().equals("onInputDevicesChanged")) changes.add((int[]) args[0]);
                    return null;
                });
        pads.register(listener);
        long[] device = new long[55];
        device[0] = 0x101; device[4] = 1; device[6] = 2; device[7] = 0x045e; device[8] = 0x028e; device[9] = 1;
        device[13] = 3; device[14] = 'P' | 'a' << 8 | 'd' << 16;
        pads.deviceEvent(device, -1);
        check(changes.size() == 1 && Arrays.equals(changes.get(0), new int[] {-1, 0, 2, 1}),
                "device change pairs: " + (changes.isEmpty() ? "none" : Arrays.toString(changes.get(0))));
        check(Arrays.equals(pads.deviceIds(-1), new int[] {-1, 2}), "gamepad missing from the device ids");
        android.view.InputDevice pad = pads.device(2);
        check(pad != null && pad.getName().equals("Pad") && pad.getVendorId() == 0x045e && pad.getProductId() == 0x028e
                && pad.getControllerNumber() == 1 && pad.isExternal() && !pad.isVirtual(), "gamepad identity");
        check(pad.supportsSource(android.view.InputDevice.SOURCE_GAMEPAD)
                && pad.supportsSource(android.view.InputDevice.SOURCE_JOYSTICK)
                && pad.getKeyboardType() == android.view.InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC, "gamepad sources");
        android.view.InputDevice.MotionRange z = pad.getMotionRange(MotionEvent.AXIS_Z,
                android.view.InputDevice.SOURCE_JOYSTICK);
        android.view.InputDevice.MotionRange trigger = pad.getMotionRange(MotionEvent.AXIS_LTRIGGER,
                android.view.InputDevice.SOURCE_JOYSTICK);
        check(z != null && z.getMin() == -1f && z.getMax() == 1f && Math.abs(z.getFlat() - 0.125f) < 0.001f
                && trigger != null && trigger.getMin() == 0f && trigger.getFlat() == 0f, "gamepad motion ranges");
        check(pads.device(3) == null, "an unknown id has a device");
        // IInputManager.hasKeys, which Chromium's gamepad list asks of each pad: the pad's buttons, not a keyboard's keys, and
        // nothing for a device that does not exist.
        final int[] asked = {android.view.KeyEvent.KEYCODE_BUTTON_A, android.view.KeyEvent.KEYCODE_BUTTON_START,
                android.view.KeyEvent.KEYCODE_DPAD_UP, android.view.KeyEvent.KEYCODE_BUTTON_L2,
                android.view.KeyEvent.KEYCODE_A, android.view.KeyEvent.KEYCODE_ENTER};
        boolean[] padHas = new boolean[asked.length], keyboardHas = new boolean[asked.length], nobodyHas = new boolean[asked.length];
        check(pads.hasKeys(2, asked, padHas, -1) && Arrays.equals(padHas, new boolean[] {true, true, true, false, false, false}),
                "gamepad hasKeys: " + Arrays.toString(padHas));
        check(pads.hasKeys(-1, asked, keyboardHas, -1)
                && Arrays.equals(keyboardHas, new boolean[] {false, false, true, false, true, true}),
                "keyboard hasKeys: " + Arrays.toString(keyboardHas));
        check(!pads.hasKeys(9, asked, nobodyHas, -1) && Arrays.equals(nobodyHas, new boolean[asked.length]),
                "an unknown device has keys");
        final int keys = android.view.InputDevice.SOURCE_KEYBOARD | android.view.InputDevice.SOURCE_GAMEPAD;
        List<android.view.KeyEvent> sent = new ArrayList<>();
        FrameworkGamepads.KeyTarget unhandled = event -> { sent.add(event); return false; };
        FrameworkGamepads.KeyTarget handled = event -> { sent.add(event); return true; };
        java.util.function.BiConsumer<Integer, FrameworkGamepads.KeyTarget> press = (code, target) -> {
            try {
                pads.key(new android.view.KeyEvent(1, 1, android.view.KeyEvent.ACTION_DOWN, code, 0, 0, 2, 0, 0, keys), target);
                pads.key(new android.view.KeyEvent(1, 2, android.view.KeyEvent.ACTION_UP, code, 0, 0, 2, 0, 0, keys), target);
            } catch (Exception error) { throw new RuntimeException(error); }
        };
        java.util.function.Function<List<android.view.KeyEvent>, String> codes = list -> {
            StringBuilder text = new StringBuilder();
            for (android.view.KeyEvent event : list)
                text.append(android.view.KeyEvent.keyCodeToString(event.getKeyCode())).append(event.getAction() == 0 ? "+" : "-")
                        .append((event.getFlags() & android.view.KeyEvent.FLAG_FALLBACK) != 0 ? "f " : " ");
            return text.toString().trim();
        };
        press.accept(android.view.KeyEvent.KEYCODE_BUTTON_B, unhandled);
        check(codes.apply(sent).equals("KEYCODE_BUTTON_B+ KEYCODE_BACK+f KEYCODE_BUTTON_B- KEYCODE_BACK-f"),
                "B fallback: " + codes.apply(sent));
        sent.clear();
        press.accept(android.view.KeyEvent.KEYCODE_BUTTON_X, unhandled);
        check(codes.apply(sent).equals("KEYCODE_BUTTON_X+ KEYCODE_DEL+f KEYCODE_BUTTON_X- KEYCODE_DEL-f"),
                "X fallback (Generic.kcm): " + codes.apply(sent));
        sent.clear();
        press.accept(android.view.KeyEvent.KEYCODE_BUTTON_A, handled);
        check(codes.apply(sent).equals("KEYCODE_BUTTON_A+ KEYCODE_BUTTON_A-"), "a handled A fell back: " + codes.apply(sent));
        sent.clear();
        press.accept(android.view.KeyEvent.KEYCODE_BUTTON_MODE, unhandled);
        check(codes.apply(sent).equals("KEYCODE_BUTTON_MODE+ KEYCODE_BUTTON_MODE-"), "MODE became HOME: " + codes.apply(sent));
        sent.clear();
        long[] axes = new long[55];
        axes[0] = 2; axes[5] = android.view.InputDevice.SOURCE_JOYSTICK; axes[6] = 2; axes[11] = axes[12] = 5000000; axes[13] = 1;
        axes[20 + 2] = Float.floatToIntBits(0.5f);   // Z
        axes[20 + 6] = Float.floatToIntBits(-1f);    // HAT_X
        MotionEvent left = FrameworkGamepads.motion(axes);
        check(left.getSource() == android.view.InputDevice.SOURCE_JOYSTICK && left.getDeviceId() == 2
                && left.getAxisValue(MotionEvent.AXIS_Z) == 0.5f && left.getAxisValue(MotionEvent.AXIS_HAT_X) == -1f
                && left.getAxisValue(MotionEvent.AXIS_X) == 0f && left.getEventTime() == 5, "joystick axes");
        pads.joystick(left, false, unhandled, () -> true);
        axes[20 + 6] = Float.floatToIntBits(0f);
        MotionEvent centered = FrameworkGamepads.motion(axes);
        pads.joystick(centered, false, unhandled, () -> true);
        pads.joystick(left, false, unhandled, () -> true);
        pads.joystick(centered, true, unhandled, () -> true);  // handled: the held synthetic key is canceled
        check(codes.apply(sent).equals("KEYCODE_DPAD_LEFT+f KEYCODE_DPAD_LEFT-f KEYCODE_DPAD_LEFT+f KEYCODE_DPAD_LEFT-f")
                && (sent.get(3).getFlags() & android.view.KeyEvent.FLAG_CANCELED) != 0, "synthetic d-pad: " + codes.apply(sent));
        left.recycle(); centered.recycle();
        device[4] = 0;
        pads.deviceEvent(device, -1);
        check(changes.size() == 2 && Arrays.equals(changes.get(1), new int[] {-1, 0}) && pads.device(2) == null,
                "gamepad removal");
        System.out.println("MANAGED_GAMEPADS_OK");
    }
    // PackageManager.queryBroadcastReceivers (IPackageManager.queryIntentReceivers): an explicit component of
    // the batch package, an implicit intent no manifest filter matches, and components that do not exist.
    private static void receivers(Context context) {
        android.content.pm.PackageManager pm = context.getPackageManager();
        ComponentName probe = new ComponentName(NAME, ProbeReceiver.class.getName());
        List<android.content.pm.ResolveInfo> found = pm.queryBroadcastReceivers(new Intent(NAME + ".MANIFEST").setComponent(probe), 0);
        check(found.size() == 1 && found.get(0).activityInfo != null
                && found.get(0).activityInfo.name.equals(ProbeReceiver.class.getName())
                && found.get(0).activityInfo.packageName.equals(NAME), "explicit receiver query: " + found);
        check(pm.queryBroadcastReceivers(new Intent("org.bifrost.none"), 0).isEmpty(), "implicit receiver query matched");
        check(pm.queryBroadcastReceivers(new Intent("org.bifrost.none").setPackage(NAME), 0).isEmpty(),
                "package-restricted receiver query matched");
        check(pm.queryBroadcastReceivers(new Intent("org.bifrost.none").setPackage("org.bifrost.absent"), 0).isEmpty(),
                "receiver query in an absent package matched");
        check(pm.queryBroadcastReceivers(new Intent().setComponent(new ComponentName(NAME, "Missing")), 0).isEmpty(),
                "a missing receiver component was found");
    }
    // FrameworkSharedLibraries against fixture <library> declarations: SystemConfig's name order and "file must
    // exist", PackageManagerService's resolution (required before optional, org.apache.http.legacy added below API
    // 28 unless already declared), and the SharedLibraryInfo objects LoadedApk builds the libraries' loaders from.
    private static void sharedLibraries() throws Exception {
        File directory = new File("/data/local/tmp/shared-libraries-test");
        for (File old : directory.listFiles() == null ? new File[0] : directory.listFiles()) old.delete();
        directory.mkdirs();
        String http = new File(directory, "http.jar").getPath(), other = new File(directory, "other.jar").getPath();
        for (String jar : new String[] {http, other}) java.nio.file.Files.write(new File(jar).toPath(), new byte[] {1});
        java.nio.file.Files.write(new File(directory, "a.xml").toPath(), ("<permissions>"
                + "<library name=\"org.apache.http.legacy\" file=\"" + http + "\"/>"
                + "<library name=\"fixture.ghost\" file=\"" + directory + "/absent.jar\"/></permissions>").getBytes());
        java.nio.file.Files.write(new File(directory, "b.xml").toPath(), ("<permissions>"
                + "<library name=\"fixture.other\" file=\"" + other + "\"/>"
                + "<library name=\"org.apache.http.legacy\" file=\"" + other + "\"/></permissions>").getBytes());
        java.nio.file.Files.write(new File(directory, "c.xml").toPath(), "<permissions><library".getBytes());  // malformed
        FrameworkSharedLibraries libraries = new FrameworkSharedLibraries(directory);
        check(libraries.has("org.apache.http.legacy") && libraries.has("fixture.other") && !libraries.has("fixture.ghost"),
                "declared libraries: a library whose file is absent must be ignored");
        List<String> none = new ArrayList<>(), apache = Arrays.asList("org.apache.http.legacy");
        check(new ArrayList<>(libraries.resolve(35, none, apache).values()).equals(Arrays.asList(http)),
                "the first declaration of a library (a.xml) must win: " + libraries.resolve(35, none, apache));
        check(libraries.resolve(35, none, none).isEmpty(), "an app targeting API 35 gets no library it did not ask for");
        check(libraries.resolve(28, none, none).isEmpty(), "API 28 does not get the Apache HTTP legacy library");
        check(libraries.resolve(27, none, none).keySet().equals(new java.util.HashSet<>(apache)),
                "an app targeting API 27 gets org.apache.http.legacy");
        check(libraries.resolve(27, none, apache).size() == 1, "org.apache.http.legacy was added though declared optional");
        check(libraries.resolve(27, apache, none).size() == 1, "org.apache.http.legacy was added though declared required");
        check(new ArrayList<>(libraries.resolve(35, Arrays.asList("fixture.other"), apache).keySet())
                .equals(Arrays.asList("fixture.other", "org.apache.http.legacy")), "required libraries must come first");
        check(libraries.resolve(35, Arrays.asList("fixture.ghost"), none).isEmpty(), "an unavailable required library");
        check(!new FrameworkSharedLibraries(new File("/data/local/tmp/absent-directory")).has("fixture.other"),
                "a missing permissions directory");
        android.content.pm.ApplicationInfo app = new android.content.pm.ApplicationInfo();
        FrameworkSharedLibraries.apply(app, libraries.resolve(35, Arrays.asList("fixture.other"), apache));
        check(Arrays.equals(app.sharedLibraryFiles, new String[] {other, http}), "sharedLibraryFiles: " + Arrays.toString(app.sharedLibraryFiles));
        List<?> infos = (List<?>) android.content.pm.ApplicationInfo.class.getField("sharedLibraryInfos").get(app);
        check(infos != null && infos.size() == 2, "one SharedLibraryInfo per library");
        android.content.pm.SharedLibraryInfo first = (android.content.pm.SharedLibraryInfo) infos.get(0);
        check(first.getName().equals("fixture.other") && first.getType() == android.content.pm.SharedLibraryInfo.TYPE_BUILTIN
                && first.getVersion() == -1 && other.equals(first.getClass().getMethod("getPath").invoke(first))
                && ((List<?>) first.getClass().getMethod("getAllCodePaths").invoke(first)).equals(Arrays.asList(other)),
                "a built-in SharedLibraryInfo (name, type, undefined version, the jar as its path)");
        FrameworkSharedLibraries.apply(app, new java.util.LinkedHashMap<>());
        check(app.sharedLibraryFiles == null && android.content.pm.ApplicationInfo.class.getField("sharedLibraryInfos").get(app) == null,
                "no libraries must clear both fields");
        for (File file : directory.listFiles()) file.delete();
        directory.delete();
    }
    // AudioManager.addOnModeChangedListener (registerModeDispatcher): accepted, removable and re-addable; the mode is
    // always MODE_NORMAL, so the listener is never called.
    private static void audioMode(Context context) {
        android.media.AudioManager audio = context.getSystemService(android.media.AudioManager.class);
        android.media.AudioManager.OnModeChangedListener listener = mode -> { throw new AssertionError("the audio mode never changes"); };
        audio.addOnModeChangedListener(Runnable::run, listener);
        check(audio.getMode() == android.media.AudioManager.MODE_NORMAL, "audio mode");
        audio.removeOnModeChangedListener(listener);
        audio.addOnModeChangedListener(Runnable::run, listener);
        audio.removeOnModeChangedListener(listener);
    }
    // The media_session service through the real MediaSession, MediaController and VolumeProvider: the state an app
    // publishes reaches its controller (and the controller's callbacks), a controller's transport commands reach the
    // app's callback, the volume model, queue, destruction and the per-package session limit.
    private static void mediaSessions(Context context) throws Exception {
        android.os.HandlerThread thread = new android.os.HandlerThread("media-session-test");
        thread.start();
        android.os.Handler handler = new android.os.Handler(thread.getLooper());
        android.media.session.MediaSession session = new android.media.session.MediaSession(context, "bifrost-test");
        android.media.session.MediaController controller = session.getController();
        check(controller.getPackageName().equals(NAME) && "bifrost-test".equals(controller.getTag()),
                "controller identity: " + controller.getPackageName() + "/" + controller.getTag());
        CountDownLatch metadataChanged = new CountDownLatch(1), stateChanged = new CountDownLatch(1),
                queueChanged = new CountDownLatch(1), destroyed = new CountDownLatch(1), volumeChanged = new CountDownLatch(1);
        controller.registerCallback(new android.media.session.MediaController.Callback() {
            @Override public void onMetadataChanged(android.media.MediaMetadata metadata) { metadataChanged.countDown(); }
            @Override public void onPlaybackStateChanged(android.media.session.PlaybackState state) { stateChanged.countDown(); }
            @Override public void onQueueChanged(List<android.media.session.MediaSession.QueueItem> queue) { queueChanged.countDown(); }
            @Override public void onAudioInfoChanged(android.media.session.MediaController.PlaybackInfo info) { volumeChanged.countDown(); }
            @Override public void onSessionDestroyed() { destroyed.countDown(); }
        }, handler);
        // Transport commands and media buttons reach the session's callback.
        List<String> received = java.util.Collections.synchronizedList(new ArrayList<>());
        CountDownLatch commands = new CountDownLatch(6);
        session.setCallback(new android.media.session.MediaSession.Callback() {
            @Override public void onPlay() { received.add("play"); commands.countDown(); }
            @Override public void onSkipToQueueItem(long id) { received.add("skip " + id); commands.countDown(); }
            @Override public void onSeekTo(long position) { received.add("seek " + position); commands.countDown(); }
            @Override public void onCustomAction(String action, Bundle extras) {
                received.add("custom " + action + " " + extras.getInt("n")); commands.countDown(); }
            @Override public boolean onMediaButtonEvent(Intent button) {
                received.add("button " + ((android.view.KeyEvent) button.getParcelableExtra(Intent.EXTRA_KEY_EVENT)).getKeyCode());
                commands.countDown(); return true; }
            @Override public void onSetRating(android.media.Rating rating) { received.add("rating " + rating.getStarRating()); commands.countDown(); }
        }, handler);
        session.setActive(true);
        session.setMetadata(new android.media.MediaMetadata.Builder().putString(android.media.MediaMetadata.METADATA_KEY_TITLE, "Song").build());
        check(metadataChanged.await(5, TimeUnit.SECONDS) && "Song".equals(controller.getMetadata().getString(android.media.MediaMetadata.METADATA_KEY_TITLE)),
                "metadata did not reach the controller");
        session.setPlaybackState(new android.media.session.PlaybackState.Builder()
                .setState(android.media.session.PlaybackState.STATE_PAUSED, 1000, 1f).build());
        check(stateChanged.await(5, TimeUnit.SECONDS) && controller.getPlaybackState().getState() == android.media.session.PlaybackState.STATE_PAUSED
                && controller.getPlaybackState().getPosition() == 1000, "a paused state keeps its position");
        session.setPlaybackState(new android.media.session.PlaybackState.Builder()
                .setState(android.media.session.PlaybackState.STATE_PLAYING, 1000, 1f, android.os.SystemClock.elapsedRealtime()).build());
        Thread.sleep(300);
        long moved = controller.getPlaybackState().getPosition();
        check(moved >= 1250 && moved < 3000, "a playing position moves on with the clock: " + moved);
        List<android.media.session.MediaSession.QueueItem> queue = new ArrayList<>();
        for (int i = 0; i < 2; i++) queue.add(new android.media.session.MediaSession.QueueItem(
                new android.media.MediaDescription.Builder().setMediaId("m" + i).setTitle("T" + i).build(), 7 + i));
        session.setQueue(queue);
        session.setQueueTitle("Up next");
        check(queueChanged.await(5, TimeUnit.SECONDS) && controller.getQueue().size() == 2
                && controller.getQueue().get(1).getQueueId() == 8 && "Up next".contentEquals(controller.getQueueTitle()), "the queue did not round trip");
        Bundle extras = new Bundle(); extras.putString("k", "v");
        session.setExtras(extras);
        session.setRatingType(android.media.Rating.RATING_5_STARS);
        check("v".equals(controller.getExtras().getString("k")) && controller.getRatingType() == android.media.Rating.RATING_5_STARS, "extras and rating type");
        // Volume: local playback follows the system stream, remote playback is the app's own.
        android.media.session.MediaController.PlaybackInfo local = controller.getPlaybackInfo();
        check(local.getPlaybackType() == android.media.session.MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL && local.getMaxVolume() > 0,
                "local playback info");
        session.setPlaybackToRemote(new android.media.VolumeProvider(android.media.VolumeProvider.VOLUME_CONTROL_ABSOLUTE, 20, 5) {});
        check(volumeChanged.await(5, TimeUnit.SECONDS) && controller.getPlaybackInfo().getPlaybackType()
                == android.media.session.MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE && controller.getPlaybackInfo().getMaxVolume() == 20
                && controller.getPlaybackInfo().getCurrentVolume() == 5, "remote playback info");
        // A controller's commands reach the session callback, in order, with the right arguments.
        android.media.session.MediaController.TransportControls controls = controller.getTransportControls();
        controls.play(); controls.skipToQueueItem(8); controls.seekTo(4000);
        Bundle action = new Bundle(); action.putInt("n", 3);
        controls.sendCustomAction("boost", action);
        controls.setRating(android.media.Rating.newStarRating(android.media.Rating.RATING_5_STARS, 4f));
        check(controller.dispatchMediaButtonEvent(new android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, android.view.KeyEvent.KEYCODE_MEDIA_PAUSE)),
                "a media button the session accepts");
        check(commands.await(5, TimeUnit.SECONDS) && received.equals(Arrays.asList("play", "skip 8", "seek 4000", "custom boost 3", "rating 4.0", "button 127")),
                "commands reached the session callback: " + received);
        // Platform limits.
        denied(() -> session.setFlags(1 << 16));  // FLAG_EXCLUSIVE_GLOBAL_PRIORITY needs MODIFY_PHONE_STATE
        // Destruction tells the controllers, and a controller that registers later is told at once.
        session.release();
        check(destroyed.await(5, TimeUnit.SECONDS), "the controller was not told of the destroyed session");
        CountDownLatch late = new CountDownLatch(1);
        new android.media.session.MediaController(context, session.getSessionToken()).registerCallback(
                new android.media.session.MediaController.Callback() { @Override public void onSessionDestroyed() { late.countDown(); } }, handler);
        check(late.await(5, TimeUnit.SECONDS), "a controller registered on a destroyed session was not told");
        // One package may hold 100 sessions; releasing one frees a slot.
        List<android.media.session.MediaSession> many = new ArrayList<>();
        for (int i = 0; i < 100; i++) many.add(new android.media.session.MediaSession(context, "many-" + i));
        try { new android.media.session.MediaSession(context, "one-too-many"); throw new AssertionError("session limit not enforced"); }
        catch (RuntimeException expected) { checks++; }
        many.get(0).release();
        new android.media.session.MediaSession(context, "after-release").release();
        for (android.media.session.MediaSession each : many) each.release();
        thread.quitSafely();
        System.out.println("MANAGED_MEDIA_SESSIONS_OK");
    }
    // ActivityManager.getDeviceConfigurationInfo (ATMS.getDeviceConfigurationInfo): a touchscreen device with no
    // physical keyboard or navigation, hence no input features, and the property ro.opengles.version (ES 3.0).
    private static void deviceConfiguration(Context context) {
        android.content.pm.ConfigurationInfo info = context.getSystemService(android.app.ActivityManager.class)
                .getDeviceConfigurationInfo();
        check(info.reqTouchScreen == android.content.res.Configuration.TOUCHSCREEN_FINGER
                && info.reqKeyboardType == android.content.res.Configuration.KEYBOARD_NOKEYS
                && info.reqNavigation == android.content.res.Configuration.NAVIGATION_NONAV
                && info.reqInputFeatures == 0, "device configuration: " + info);
        check(info.reqGlEsVersion == 0x30000 && info.getGlEsVersion().equals("3.0"), "reqGlEsVersion: " + info.reqGlEsVersion);
    }
    // ActivityManager.getRunningServices for a plain app: the app's own created services, up to maxNum.
    private static void runningServices(Context context) {
        android.app.ActivityManager manager = context.getSystemService(android.app.ActivityManager.class);
        ComponentName probe = new ComponentName(NAME, ProbeService.class.getName());
        android.app.ActivityManager.RunningServiceInfo found = null;
        for (android.app.ActivityManager.RunningServiceInfo info : manager.getRunningServices(20))
            if (info.service.equals(probe)) found = info;
        check(found != null && found.started && found.uid == Process.myUid() && found.pid == Process.myPid()
                && found.process.equals(NAME) && (found.flags & android.app.ActivityManager.RunningServiceInfo.FLAG_STARTED) != 0
                && found.activeSince > 0 && found.lastActivityTime >= found.activeSince, "running service not reported");
        check(manager.getRunningServices(0).isEmpty(), "maxNum 0 returned services");
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
        check(manager.getCurrentInterruptionFilter() == NotificationManager.INTERRUPTION_FILTER_ALL,
                "Do Not Disturb is not off (getZenMode)");
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

    // android.media.audiofx loads, reports no effects, and refuses one the way a device without the engine does.
    private static void audioEffects() throws Exception {
        android.media.audiofx.AudioEffect.Descriptor[] effects = android.media.audiofx.AudioEffect.queryEffects();
        check(effects == null, "no audio effect engine to report");  // null, not empty: android_media_AudioEffect.cpp
        try {
            new android.media.audiofx.Equalizer(0, 0);
            throw new AssertionError("an equalizer with no effect engine");
        } catch (UnsupportedOperationException | IllegalArgumentException error) {
            throw error;  // apps guard the documented RuntimeException below
        } catch (RuntimeException expected) {
            check(expected.getMessage().startsWith("Cannot initialize effect engine"), "equalizer failure");
        }
    }

    private static void audio(Context context) throws Exception {
        AudioManager manager = context.getSystemService(AudioManager.class);
        java.util.List<?> outputs = (java.util.List<?>) invoke(endpoint("audio", "android.media.IAudioService"),
                "getDevicesForAttributesUnprotected", new android.media.AudioAttributes.Builder().build());
        check(outputs.size() == 1 && (Integer) outputs.get(0).getClass().getMethod("getType").invoke(outputs.get(0))
                == android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER, "audio output for attributes");
        // Bionic and libcore share persist.sys.timezone; a zone the image's tzdata lacks would read as GMT.
        String zone = (String) Class.forName("android.os.SystemProperties").getMethod("get", String.class)
                .invoke(null, "persist.sys.timezone");
        check(zone.isEmpty() || java.util.TimeZone.getDefault().getID().equals(zone), "default time zone is the host's");
        System.out.println("MANAGED_TIMEZONE property=" + zone + " java=" + java.util.TimeZone.getDefault().getID()
                + " now=" + new java.util.Date());
        Object activity = endpoint("activity", "android.app.IActivityManager");
        check(invoke(activity, "getProcessesInErrorState") == null, "a process reported in error");
        android.os.Debug.MemoryInfo[] memory = (android.os.Debug.MemoryInfo[]) invoke(activity,
                "getProcessMemoryInfo", (Object) new int[] {android.os.Process.myPid(), 1});
        check(memory.length == 2 && memory[0].getTotalPss() > 0 && memory[1].getTotalPss() == 0,
                "process memory info: own pid filled, foreign pid empty");
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

    // Native platform clients (C++ AudioSystem and AudioTrack, ViewRootImpl) reach these through
    // Binder and ServiceManager rather than the Java service cache. Each one used to log an error
    // in every game: the policy call, the missing native "audio" service and the missing
    // sensitive-content service.
    private static int audioServiceCode(String method) throws Exception {
        java.lang.reflect.Field field = Class.forName("android.media.IAudioService$Stub")
                .getDeclaredField("TRANSACTION_" + method);
        field.setAccessible(true);
        return field.getInt(null);
    }
    private static void nativeServices() throws Exception {
        // media.audio_policy transaction 3 is IAudioPolicyService.getDeviceConnectionState: a nonnull
        // marker and a size-prefixed AudioDevice in, an AudioPolicyDeviceState out.
        Binder policy = (Binder) launcherField("audioPolicy");
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken("android.media.IAudioPolicyService");
            data.writeInt(1); data.writeInt(16); data.writeInt(0); data.writeInt(0); data.writeInt(0);
            check(policy.transact(3, data, reply, 0), "getDeviceConnectionState is not handled");
            reply.setDataPosition(0); reply.readException();
            check(reply.readInt() == 0 && reply.dataAvail() == 0, "no device is connected: UNAVAILABLE expected");
        } finally { data.recycle(); reply.recycle(); }
        invalid(() -> {
            Parcel bad = Parcel.obtain(), out = Parcel.obtain();
            try {
                bad.writeInterfaceToken("android.media.IAudioPolicyService"); bad.writeInt(0);  // null device
                policy.transact(3, bad, out, 0);
            } finally { bad.recycle(); out.recycle(); }
        });
        invalid(() -> {
            Parcel bad = Parcel.obtain(), out = Parcel.obtain();
            try {
                bad.writeInterfaceToken("android.media.IAudioPolicyService");
                bad.writeInt(1); bad.writeInt(4000);  // a size beyond the parcel
                policy.transact(3, bad, out, 0);
            } finally { bad.recycle(); out.recycle(); }
        });

        // Native AudioTrack looks "audio" up with checkService (not the Java cache) and reports its
        // port id with the one-way playerEvent; native IAudioManager::trackPlayer returns an id.
        Class<?> manager = Class.forName("android.os.IServiceManager");
        java.lang.reflect.Method getManager = Class.forName("android.os.ServiceManager")
                .getDeclaredMethod("getIServiceManager");
        getManager.setAccessible(true);
        IBinder nativeAudio = (IBinder) manager.getMethod("checkService", String.class)
                .invoke(getManager.invoke(null), "audio");
        check(nativeAudio != null, "native clients cannot find the audio service");
        int[] piid = new int[2];
        for (int i = 0; i < 2; i++) {
            Parcel in = Parcel.obtain(), out = Parcel.obtain();
            try {
                in.writeInterfaceToken("android.media.IAudioService");
                in.writeInt(1);  // a PlayerIdCard follows; the service does not read it
                check(nativeAudio.transact(audioServiceCode("trackPlayer"), in, out, 0), "trackPlayer failed");
                out.readException(); piid[i] = out.readInt();
            } finally { in.recycle(); out.recycle(); }
        }
        check(piid[0] > 0 && piid[1] == piid[0] + 1, "player ids " + piid[0] + ", " + piid[1]);
        for (String oneway : new String[] {"playerEvent", "playerAttributes", "releasePlayer", "playerSessionId"}) {
            Parcel in = Parcel.obtain();
            try {
                in.writeInterfaceToken("android.media.IAudioService");
                in.writeInt(piid[0]); in.writeInt(7); in.writeInt(42);
                check(nativeAudio.transact(audioServiceCode(oneway), in, null, IBinder.FLAG_ONEWAY),
                        oneway + " rejected");
            } finally { in.recycle(); }
        }

        // ViewRootImpl asks for this service for every window.
        Object protection = endpoint("sensitive_content_protection_service",
                "android.view.ISensitiveContentProtectionManager");
        check(protection != null, "sensitive content protection service is missing");
        invoke(protection, "setSensitiveContentProtection", new Binder(), NAME, true);
        invoke(protection, "setSensitiveContentProtection", new Binder(), NAME, false);
        try { invoke(protection, "setSensitiveContentProtection", null, NAME, true); throw new AssertionError("null token"); }
        catch (NullPointerException expected) { checks++; }

        // A call no stand-in answers is named once in the log, as an exception from a Java service
        // and as an unknown transaction from a native one; the app still sees the failure.
        java.io.PrintStream saved = System.err;
        java.io.ByteArrayOutputStream captured = new java.io.ByteArrayOutputStream();
        System.setErr(new java.io.PrintStream(captured, true));
        try {
            Object game = endpoint("game", "android.app.IGameManagerService");
            for (int i = 0; i < 3; i++) {
                try { invoke(game, "getAvailableGameModes", NAME, 0); throw new AssertionError("unsupported call answered"); }
                catch (UnsupportedOperationException expected) { checks++; }
            }
            Parcel in = Parcel.obtain(), out = Parcel.obtain();
            try {
                for (int i = 0; i < 3; i++) {
                    in.setDataPosition(0);
                    in.writeInterfaceToken("android.media.IAudioPolicyService");
                    check(!policy.transact(9999, in, out, 0), "an unknown policy call was accepted");
                }
                check(policy.transact(IBinder.INTERFACE_TRANSACTION, Parcel.obtain(), out, 0), "interface query refused");
            } finally { in.recycle(); out.recycle(); }
        } finally { System.setErr(saved); }
        String log = captured.toString();
        check(count(log, "SERVICE_CALL_UNSUPPORTED game getAvailableGameModes\n") == 1, "Java call report: " + log);
        check(count(log, "SERVICE_CALL_UNSUPPORTED android.media.IAudioPolicyService transaction 9999\n") == 1,
                "native call report: " + log);
        check(count(log, "SERVICE_CALL_UNSUPPORTED") == 2, "binder's own codes must not be reported: " + log);
    }
    private static int count(String text, String part) {
        int n = 0;
        for (int at = text.indexOf(part); at >= 0; at = text.indexOf(part, at + part.length())) n++;
        return n;
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
        check(FrameworkUiMode.configurationUiMode() == (android.content.res.Configuration.UI_MODE_TYPE_NORMAL
                | android.content.res.Configuration.UI_MODE_NIGHT_NO), "configuration uiMode differs from the service");
        manager.setNightMode(android.app.UiModeManager.MODE_NIGHT_YES);
        check(manager.getNightMode() == android.app.UiModeManager.MODE_NIGHT_NO, "locked night mode changed");
        manager.setApplicationNightMode(android.app.UiModeManager.MODE_NIGHT_NO);
        try {
            manager.setApplicationNightMode(android.app.UiModeManager.MODE_NIGHT_YES);
            throw new AssertionError("per-app dark mode claimed without a configuration change");
        } catch (UnsupportedOperationException expected) { checks++; }
        // The system's night mode is the desktop's colour scheme: dark makes the service, the configuration's uiMode and
        // a request for the mode the system is in agree, and a request for the other one is still refused.
        check(FrameworkUiMode.setNight(true) && !FrameworkUiMode.setNight(true), "night mode change was not reported once");
        try {
            check(manager.getNightMode() == android.app.UiModeManager.MODE_NIGHT_YES
                    && manager.getCurrentModeType() == android.content.res.Configuration.UI_MODE_TYPE_NORMAL,
                    "a dark desktop is not the night mode");
            check(FrameworkUiMode.configurationUiMode() == (android.content.res.Configuration.UI_MODE_TYPE_NORMAL
                    | android.content.res.Configuration.UI_MODE_NIGHT_YES), "dark configuration uiMode differs from the service");
            manager.setApplicationNightMode(android.app.UiModeManager.MODE_NIGHT_YES);
            manager.setApplicationNightMode(android.app.UiModeManager.MODE_NIGHT_AUTO);
            manager.setNightMode(android.app.UiModeManager.MODE_NIGHT_NO);
            check(manager.getNightMode() == android.app.UiModeManager.MODE_NIGHT_YES, "locked night mode changed while dark");
            try {
                manager.setApplicationNightMode(android.app.UiModeManager.MODE_NIGHT_NO);
                throw new AssertionError("per-app light mode claimed without a configuration change");
            } catch (UnsupportedOperationException expected) { checks++; }
        } finally {
            check(FrameworkUiMode.setNight(false), "night mode did not return to light");
        }
        check(manager.getNightMode() == android.app.UiModeManager.MODE_NIGHT_NO, "light desktop is not the default night mode");
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

    // ---- app ops: the whole Android 15 IAppOpsService against AppOpsService's contracts ----
    private static int op(String name) throws Exception {
        return android.app.AppOpsManager.class.getField(name).getInt(null);
    }
    private static Object hidden(String name, Class<?>[] types, Object... args) throws Exception {
        return android.app.AppOpsManager.class.getMethod(name, types).invoke(null, args);
    }
    private static int defaultMode(int code) throws Exception {
        return (Integer) hidden("opToDefaultMode", new Class<?>[] {int.class}, code);
    }
    private static int switchOf(int code) { return FrameworkAppOps.switchOp(code); }
    private static boolean restrictsRead(int code) throws Exception {
        return (Boolean) hidden("opRestrictsRead", new Class<?>[] {int.class}, code);
    }
    private static int modeOf(Object syncNoted) throws Exception { return (Integer) syncNoted.getClass().getMethod("getOpMode").invoke(syncNoted); }
    private static final class Recorder {
        final java.util.concurrent.LinkedBlockingQueue<Object[]> events = new java.util.concurrent.LinkedBlockingQueue<>();
        final Object callback;
        Recorder(String iface) throws Exception {
            callback = new FrameworkServices().local("appops-test", iface, (method, args) -> { events.add(args); return null; });
        }
        Object[] next() throws Exception {
            Object[] event = events.poll(10, TimeUnit.SECONDS);
            if (event == null) throw new AssertionError("callback not delivered");
            return event;
        }
        void quiet(String message) throws Exception {
            check(events.poll(300, TimeUnit.MILLISECONDS) == null, message);
        }
    }
    private static Object remoteCallback(java.util.concurrent.LinkedBlockingQueue<Bundle> results) throws Exception {
        Class<?> listener = Class.forName("android.os.RemoteCallback$OnResultListener");
        Object proxy = java.lang.reflect.Proxy.newProxyInstance(listener.getClassLoader(), new Class<?>[] {listener},
                (self, method, args) -> { results.add((Bundle) args[0]); return null; });
        return Class.forName("android.os.RemoteCallback").getConstructor(listener).newInstance(proxy);
    }

    private static void appOps(Context context, FrameworkPackages packages) throws Exception {
        FrameworkAppOps service = (FrameworkAppOps) launcherField("appOps");
        Object ops = endpoint("appops", "com.android.internal.app.IAppOpsService");
        int uid = Process.myUid(), count = FrameworkAppOps.numOp();
        int camera = op("OP_CAMERA"), usage = op("OP_GET_USAGE_STATS"), alert = op("OP_SYSTEM_ALERT_WINDOW"),
                write = op("OP_WRITE_SETTINGS"), vibrate = op("OP_VIBRATE"), audio = op("OP_PLAY_AUDIO");

        // Every method of the framework interface is implemented, and the contract names nothing else.
        java.util.Set<String> declared = new java.util.TreeSet<>();
        for (Method method : Class.forName("com.android.internal.app.IAppOpsService").getMethods())
            if (method.getDeclaringClass() != Object.class && !method.getName().equals("asBinder")) declared.add(method.getName());
        java.util.Set<String> covered = new java.util.TreeSet<>(Arrays.asList(FrameworkAppOps.contractMethods()));
        check(declared.equals(covered), "IAppOpsService methods differ from the contract: " + declared + " vs " + covered);

        // The failing Battle Cats / gms / Hungry Shark calls: ops 24 and 43 answer their default mode, never throw.
        android.app.AppOpsManager manager = context.getSystemService(android.app.AppOpsManager.class);
        check(alert == 24 && usage == 43, "op numbering differs from the reported failures");
        check(manager.checkOpNoThrow(android.app.AppOpsManager.OPSTR_GET_USAGE_STATS, uid, NAME) == defaultMode(usage),
                "OP_GET_USAGE_STATS did not answer its default mode");
        check(manager.checkOpNoThrow(android.app.AppOpsManager.OPSTR_SYSTEM_ALERT_WINDOW, uid, NAME) == defaultMode(alert),
                "OP_SYSTEM_ALERT_WINDOW did not answer its default mode");
        check(manager.checkOpNoThrow(android.app.AppOpsManager.OPSTR_WRITE_SETTINGS, uid, NAME) == defaultMode(write),
                "OP_WRITE_SETTINGS did not answer its default mode");

        // Every op: default mode without an override; restricted-read ops need a privileged permission.
        int restricted = 0;
        for (int code = 0; code < count; code++) {
            final int op = code;
            if (restrictsRead(code)) { restricted++; denied(() -> invoke(ops, "checkOperation", op, uid, NAME)); continue; }
            // AOSP returns the stored default unevaluated until the op has a record; MODE_FOREGROUND
            // (OP_TAKE_AUDIO_FOCUS) then evaluates to allowed once the app has used the op.
            int def = defaultMode(switchOf(code)), now = (Integer) invoke(ops, "checkOperation", code, uid, NAME);
            check(now == def || def == FrameworkAppOps.MODE_FOREGROUND && now == FrameworkAppOps.MODE_ALLOWED,
                    "op " + code + " default mode " + now + " != " + def);
            check((Integer) invoke(ops, "checkOperationRaw", code, uid, NAME, null) == defaultMode(switchOf(code)),
                    "op " + code + " raw default mode");
        }
        check(restricted > 0, "no restricted-read op exercised");
        invalid(() -> invoke(ops, "checkOperation", -1, uid, NAME));
        invalid(() -> invoke(ops, "checkOperation", count, uid, NAME));
        invalid(() -> invoke(ops, "noteOperation", count, uid, NAME, null, false, "m", false));
        invalid(() -> invoke(ops, "shouldCollectNotes", -1));
        invalid(() -> invoke(ops, "startWatchingActive", new int[] {count}, null));
        invalid(() -> invoke(ops, "startWatchingNoted", new int[0], new Recorder("com.android.internal.app.IAppOpsNotedCallback").callback));
        invalid(() -> invoke(ops, "startWatchingStarted", new int[] {-1}, new Recorder("com.android.internal.app.IAppOpsStartedCallback").callback));
        invalid(() -> invoke(ops, "startWatchingMode", count, NAME, new Recorder("com.android.internal.app.IAppOpsCallback").callback));
        // Package validation: an unknown package keeps the default mode for checks and errors for notes;
        // a name that resolves to nothing is ignored; another uid's package fails verification.
        check((Integer) invoke(ops, "checkOperation", camera, uid, "org.bifrost.absent") == defaultMode(camera), "unknown package check");
        check(modeOf(invoke(ops, "noteOperation", camera, uid, "org.bifrost.absent", null, false, "m", false)) == FrameworkAppOps.MODE_ERRORED, "unknown package note");
        check(modeOf(invoke(ops, "noteOperation", camera, uid, null, null, false, "m", false)) == FrameworkAppOps.MODE_IGNORED, "null package note");
        check((Integer) invoke(ops, "checkOperationForDevice", camera, uid, NAME, 5) == FrameworkAppOps.MODE_IGNORED, "invalid device check");
        check(modeOf(invoke(ops, "noteOperationForDevice", camera, uid, NAME, null, 5, false, "m", false)) == FrameworkAppOps.MODE_IGNORED, "invalid device note");
        check((Integer) invoke(ops, "checkPackage", uid, NAME) == FrameworkAppOps.MODE_ALLOWED
                && (Integer) invoke(ops, "checkPackage", uid, "org.bifrost.absent") == FrameworkAppOps.MODE_ERRORED, "checkPackage");
        denied(() -> invoke(ops, "noteOperation", camera, uid + 1, NAME, null, false, "m", false));  // verifyIncomingUid
        check((Integer) invoke(ops, "permissionToOpCode", "android.permission.CAMERA") == camera
                && (Integer) invoke(ops, "permissionToOpCode", new Object[] {null}) == -1, "permissionToOpCode");
        check((Boolean) invoke(ops, "shouldCollectNotes", camera), "shouldCollectNotes for a dangerous permission");

        // note: allowed access is recorded and fires noted watchers; a rejected one fires with its mode.
        Recorder noted = new Recorder("com.android.internal.app.IAppOpsNotedCallback");
        invoke(ops, "startWatchingNoted", new int[] {camera}, noted.callback);
        long before = System.currentTimeMillis();
        check(modeOf(invoke(ops, "noteOperation", camera, uid, NAME, null, false, "m", false)) == FrameworkAppOps.MODE_ALLOWED, "note allowed");
        Object[] event = noted.next();
        check((Integer) event[0] == camera && (Integer) event[1] == uid && NAME.equals(event[2]) && (Integer) event[4] == 0
                && (Integer) event[5] == 1 && (Integer) event[6] == FrameworkAppOps.MODE_ALLOWED, "noted callback arguments");
        service.setModeUnchecked(camera, uid, NAME, FrameworkAppOps.MODE_IGNORED);
        check((Integer) invoke(ops, "checkOperation", camera, uid, NAME) == FrameworkAppOps.MODE_IGNORED
                && modeOf(invoke(ops, "noteOperation", camera, uid, NAME, null, false, "m", false)) == FrameworkAppOps.MODE_IGNORED, "package override");
        check((Integer) noted.next()[6] == FrameworkAppOps.MODE_IGNORED, "rejected note callback");
        service.setModeUnchecked(camera, uid, NAME, FrameworkAppOps.MODE_FOREGROUND);
        check((Integer) invoke(ops, "checkOperation", camera, uid, NAME) == FrameworkAppOps.MODE_ALLOWED
                && (Integer) invoke(ops, "checkOperationRaw", camera, uid, NAME, null) == FrameworkAppOps.MODE_FOREGROUND, "foreground evaluates to allowed");
        service.setUidModeUnchecked(camera, uid, FrameworkAppOps.MODE_ERRORED);
        check((Integer) invoke(ops, "checkOperation", camera, uid, NAME) == FrameworkAppOps.MODE_ERRORED, "uid override wins over package mode");
        service.setUidModeUnchecked(camera, uid, defaultMode(camera));
        service.setModeUnchecked(camera, uid, NAME, defaultMode(camera));
        check((Integer) invoke(ops, "checkOperation", camera, uid, NAME) == defaultMode(camera), "overrides cleared");
        invoke(ops, "stopWatchingNoted", noted.callback);
        invoke(ops, "noteOperation", camera, uid, NAME, null, false, "m", false);
        noted.events.clear();
        noted.quiet("stopped noted watcher still called");

        // Mode watchers: by op, by package, with the switch-op flag; a stopped one is silent.
        Recorder mode = new Recorder("com.android.internal.app.IAppOpsCallback"), byPackage = new Recorder("com.android.internal.app.IAppOpsCallback");
        invoke(ops, "startWatchingMode", camera, null, mode.callback);
        invoke(ops, "startWatchingModeWithFlags", op("OP_NONE"), NAME, 0, byPackage.callback);
        service.setModeUnchecked(camera, uid, NAME, FrameworkAppOps.MODE_IGNORED);
        event = mode.next();
        check((Integer) event[0] == camera && (Integer) event[1] == uid && NAME.equals(event[2]), "mode watcher arguments");
        check(byPackage.next().length == 4, "package watcher notified");
        service.setUidModeUnchecked(camera, uid, FrameworkAppOps.MODE_ERRORED);
        check((Integer) mode.next()[0] == camera, "uid change notifies op watcher");
        invoke(ops, "stopWatchingMode", mode.callback);
        service.setUidModeUnchecked(camera, uid, defaultMode(camera));
        service.setModeUnchecked(camera, uid, NAME, defaultMode(camera));
        byPackage.events.clear(); mode.events.clear();
        mode.quiet("stopped mode watcher still called");
        invoke(ops, "stopWatchingMode", byPackage.callback);

        // start/finish: active and started watchers fire on 0 -> 1 and back; nested starts and clients are counted.
        Recorder active = new Recorder("com.android.internal.app.IAppOpsActiveCallback"), started = new Recorder("com.android.internal.app.IAppOpsStartedCallback");
        invoke(ops, "startWatchingActive", new int[] {camera}, active.callback);
        invoke(ops, "startWatchingStarted", new int[] {camera}, started.callback);
        IBinder first = new Binder(), second = new Binder();
        check(modeOf(invoke(ops, "startOperation", first, camera, uid, NAME, null, false, false, "m", false, 0, -1)) == FrameworkAppOps.MODE_ALLOWED, "start allowed");
        event = active.next();
        check((Integer) event[0] == camera && (Integer) event[1] == uid && (Boolean) event[5], "active callback on start");
        event = started.next();
        check((Integer) event[6] == FrameworkAppOps.MODE_ALLOWED && (Integer) event[7] == 1, "started callback arguments");
        check((Boolean) invoke(ops, "isOperationActive", camera, uid, NAME), "op active after start");
        invoke(ops, "startOperation", first, camera, uid, NAME, null, false, false, "m", false, 0, -1);
        invoke(ops, "startOperation", second, camera, uid, NAME, null, false, false, "m", false, 0, -1);
        started.next(); started.next();
        active.quiet("active callback fired again for a nested start");
        invoke(ops, "finishOperation", first, camera, uid, NAME, null);
        invoke(ops, "finishOperation", first, camera, uid, NAME, null);
        active.quiet("op became inactive while another client still ran");
        check((Boolean) invoke(ops, "isOperationActive", camera, uid, NAME), "op inactive with a running client");
        invoke(ops, "finishOperation", second, camera, uid, NAME, null);
        check(!(Boolean) active.next()[5] && !(Boolean) invoke(ops, "isOperationActive", camera, uid, NAME), "inactive after last finish");
        // A client that dies without finishing releases its start.
        DeathToken client = new DeathToken();
        invoke(ops, "startOperation", client, camera, uid, NAME, null, false, false, "m", false, 0, -1);
        check((Boolean) active.next()[5], "active callback for the dying client");
        started.next();
        client.die();
        check(!(Boolean) active.next()[5] && !(Boolean) invoke(ops, "isOperationActive", camera, uid, NAME), "dead client's start released");
        // A rejected start reports the mode, fires started with START_TYPE_FAILED and starts nothing.
        service.setModeUnchecked(camera, uid, NAME, FrameworkAppOps.MODE_IGNORED);
        check(modeOf(invoke(ops, "startOperation", first, camera, uid, NAME, null, false, false, "m", false, 0, -1)) == FrameworkAppOps.MODE_IGNORED, "rejected start");
        event = started.next();
        check((Integer) event[6] == FrameworkAppOps.MODE_IGNORED && (Integer) event[7] == 0
                && !(Boolean) invoke(ops, "isOperationActive", camera, uid, NAME), "rejected start state");
        service.setModeUnchecked(camera, uid, NAME, defaultMode(camera));
        // MODE_DEFAULT starts only when asked to.
        service.setModeUnchecked(camera, uid, NAME, FrameworkAppOps.MODE_DEFAULT);
        check(modeOf(invoke(ops, "startOperation", first, camera, uid, NAME, null, false, false, "m", false, 0, -1)) == FrameworkAppOps.MODE_DEFAULT, "default mode start without startIfModeDefault");
        started.next();
        check(modeOf(invoke(ops, "startOperation", first, camera, uid, NAME, null, true, false, "m", false, 0, -1)) == FrameworkAppOps.MODE_ALLOWED, "default mode start with startIfModeDefault");
        started.next(); active.next();
        invoke(ops, "finishOperation", first, camera, uid, NAME, null);
        active.next();
        service.setModeUnchecked(camera, uid, NAME, defaultMode(camera));
        invoke(ops, "stopWatchingActive", active.callback);
        invoke(ops, "stopWatchingStarted", started.callback);
        invoke(ops, "startOperation", first, camera, uid, NAME, null, false, false, "m", false, 0, -1);
        invoke(ops, "finishOperation", first, camera, uid, NAME, null);
        active.events.clear(); started.events.clear();
        active.quiet("stopped active watcher still called");
        // Unstarted finish is logged and ignored, as AOSP does.
        invoke(ops, "finishOperation", first, camera, uid, NAME, null);

        // Async noted ops: forwarded to a watcher, else queued for extractAsyncOps once.
        check(invoke(ops, "extractAsyncOps", NAME) == null, "async queue not empty");
        invoke(ops, "noteOperation", camera, uid, NAME, null, true, "queued", true);
        java.util.List<?> queued = (java.util.List<?>) invoke(ops, "extractAsyncOps", NAME);
        check(queued != null && queued.size() == 1 && "queued".equals(queued.get(0).getClass().getMethod("getMessage").invoke(queued.get(0)))
                && invoke(ops, "extractAsyncOps", NAME) == null, "async op not queued once");
        Recorder async = new Recorder("com.android.internal.app.IAppOpsAsyncNotedCallback");
        invoke(ops, "startWatchingAsyncNoted", NAME, async.callback);
        invoke(ops, "noteOperation", camera, uid, NAME, null, true, "forwarded", true);
        Object forwarded = async.next()[0];
        check("forwarded".equals(forwarded.getClass().getMethod("getMessage").invoke(forwarded)), "async op not forwarded");
        invoke(ops, "stopWatchingAsyncNoted", NAME, async.callback);
        denied(() -> invoke(ops, "startWatchingAsyncNoted", "org.bifrost.absent", async.callback));
        try { invoke(ops, "noteOperation", camera, uid, NAME, null, true, null, true); throw new AssertionError("null message collected"); }
        catch (NullPointerException expected) { checks++; }

        // Queries: the app sees its own entries with access time and activity; foreign uids need GET_APP_OPS_STATS.
        java.util.List<?> own = (java.util.List<?>) invoke(ops, "getOpsForPackage", uid, NAME, new int[] {camera});
        Object entry = own == null ? null : own.get(0);
        check(entry != null && NAME.equals(entry.getClass().getMethod("getPackageName").invoke(entry))
                && ((java.util.List<?>) entry.getClass().getMethod("getOps").invoke(entry)).size() == 1, "own package ops");
        Object opEntry = ((java.util.List<?>) entry.getClass().getMethod("getOps").invoke(entry)).get(0);
        check((Long) opEntry.getClass().getMethod("getLastAccessTime", int.class).invoke(opEntry, 31) >= before
                && !(Boolean) opEntry.getClass().getMethod("isRunning").invoke(opEntry), "op entry access time or activity");
        check(invoke(ops, "getOpsForPackage", uid, NAME, new int[] {vibrate}) == null, "ops listed that never happened");
        check(((java.util.List<?>) invoke(ops, "getPackagesForOps", new Object[] {null})).size() == 1
                && ((java.util.List<?>) invoke(ops, "getPackagesForOpsForDevice", new int[] {camera}, "default:0")).size() == 1, "packages for ops");
        denied(() -> invoke(ops, "getOpsForPackage", uid + 1, NAME, null));
        denied(() -> invoke(ops, "getUidOps", uid, null));

        // History: the caller's own, as a Bundle with HistoricalOps; other uids need privileged permissions.
        java.util.concurrent.LinkedBlockingQueue<Bundle> results = new java.util.concurrent.LinkedBlockingQueue<>();
        invoke(ops, "getHistoricalOps", uid, null, null, null, 0, 1, before - 1000, System.currentTimeMillis() + 60000, 31, remoteCallback(results));
        Bundle history = results.poll(10, TimeUnit.SECONDS);
        Object historical = history == null ? null : history.getParcelable("historical_ops");
        check(historical != null && (Integer) historical.getClass().getMethod("getUidCount").invoke(historical) == 1, "own historical ops");
        invoke(ops, "getHistoricalOps", uid + 1, null, null, null, 0, 1, 0L, 10L, 31, remoteCallback(results));
        check(results.poll(10, TimeUnit.SECONDS).isEmpty(), "foreign history without GET_HISTORICAL_APP_OPS_STATS");
        invalid(() -> invoke(ops, "getHistoricalOps", uid, NAME, null, null, 0, 1, 0L, 10L, 31, remoteCallback(results)));
        invalid(() -> invoke(ops, "getHistoricalOps", uid, null, null, null, 0, 1, 10L, 10L, 31, remoteCallback(results)));
        denied(() -> invoke(ops, "getHistoricalOpsFromDiskRaw", uid, null, null, null, 0, 1, 0L, 10L, 31, remoteCallback(results)));

        // Proxy operations: a self proxy is trusted; note and start/finish go through the same state.
        android.content.AttributionSource chain = new android.content.AttributionSource.Builder(uid).setPackageName(NAME)
                .setNext(new android.content.AttributionSource.Builder(uid).setPackageName(NAME).build()).build();
        check(modeOf(invoke(ops, "noteProxyOperation", camera, chain, false, "m", false, false)) == FrameworkAppOps.MODE_ALLOWED, "proxy note");
        check(modeOf(invoke(ops, "noteProxyOperationWithState", camera, android.content.AttributionSource.class.getMethod("asState").invoke(chain),
                false, "m", false, true)) == FrameworkAppOps.MODE_ALLOWED, "proxy note with state");
        check(modeOf(invoke(ops, "startProxyOperation", first, camera, chain, false, false, "m", false, false, 0, 0, -1)) == FrameworkAppOps.MODE_ALLOWED
                && (Boolean) invoke(ops, "isOperationActive", camera, uid, NAME), "proxy start");
        invoke(ops, "finishProxyOperation", first, camera, chain, false);
        check(!(Boolean) invoke(ops, "isOperationActive", camera, uid, NAME), "proxy finish");
        android.content.AttributionSource foreignChain = new android.content.AttributionSource.Builder(uid + 1).setPackageName(NAME)
                .setNext(new android.content.AttributionSource.Builder(uid).setPackageName(NAME).build()).build();
        denied(() -> invoke(ops, "noteProxyOperation", camera, foreignChain, false, "m", false, false));

        // User restrictions: privileged to set, applied to checks and notes as IGNORED, lifted when cleared.
        Bundle restrictions = new Bundle();
        IBinder token = new Binder();
        denied(() -> invoke(ops, "setUserRestrictions", restrictions, token, 0));
        denied(() -> invoke(ops, "setUserRestriction", camera, true, token, 0, null));
        denied(() -> invoke(ops, "removeUser", 0));
        service.setUserRestrictionUnchecked(camera, true, token, uid / 100000, null);
        check((Integer) invoke(ops, "checkOperation", camera, uid, NAME) == FrameworkAppOps.MODE_IGNORED
                && modeOf(invoke(ops, "noteOperation", camera, uid, NAME, null, false, "m", false)) == FrameworkAppOps.MODE_IGNORED, "user restriction");
        service.setUserRestrictionUnchecked(camera, false, token, uid / 100000, null);
        check((Integer) invoke(ops, "checkOperation", camera, uid, NAME) == defaultMode(camera), "user restriction lifted");

        // Audio: unrestricted usages answer the op's own mode; restriction setters are privileged.
        check((Integer) invoke(ops, "checkAudioOperation", audio, 1, uid, NAME) == (Integer) invoke(ops, "checkOperation", audio, uid, NAME), "audio check");
        denied(() -> invoke(ops, "setAudioRestriction", audio, 1, uid, FrameworkAppOps.MODE_IGNORED, new String[0]));
        denied(() -> invoke(ops, "setCameraAudioRestriction", 1));

        // Telemetry hooks answer without sampling anything.
        check(invoke(ops, "collectRuntimeAppOpAccessMessage") == null, "runtime message collected for an app");
        check(invoke(ops, "reportRuntimeAppOpAccessMessageAndGetConfig", NAME, invoke(ops, "noteOperation", camera, uid, NAME, null, false, "m", false), "m") != null, "sampling config");
        invoke(ops, "collectNoteOpCallsForValidation", "trace", camera, NAME, 1L);

        // Privileged methods: SecurityException for the app, with no pid bypass.
        denied(() -> invoke(ops, "setMode", camera, uid, NAME, FrameworkAppOps.MODE_ERRORED));
        denied(() -> invoke(ops, "setUidMode", camera, uid, FrameworkAppOps.MODE_ERRORED));
        denied(() -> invoke(ops, "resetAllModes", 0, NAME));
        denied(() -> invoke(ops, "resetAllModes", 10, null));
        denied(() -> invoke(ops, "offsetHistory", 1L));
        denied(() -> invoke(ops, "setHistoryParameters", 0, 1L, 1));
        denied(() -> invoke(ops, "addHistoricalOps", new Object[] {null}));
        denied(() -> invoke(ops, "resetHistoryParameters"));
        denied(() -> invoke(ops, "resetPackageOpsNoHistory", NAME));
        denied(() -> invoke(ops, "clearHistory"));
        denied(() -> invoke(ops, "rebootHistory", 0L));
        denied(() -> invoke(ops, "reloadNonHistoricalState"));
        check((Integer) invoke(ops, "checkOperation", camera, uid, NAME) == defaultMode(camera), "privileged call changed state");
    }

    // ShortcutManager, which Firefox clears at startup: no launcher exists, so nothing is stored or pinnable.
    private static void shortcuts(Context context) throws Exception {
        android.content.pm.ShortcutManager manager = context.getSystemService(android.content.pm.ShortcutManager.class);
        check(manager != null, "ShortcutManager is published");
        manager.removeDynamicShortcuts(java.util.Collections.singletonList("absent"));
        manager.removeAllDynamicShortcuts();
        manager.reportShortcutUsed("absent");
        check(manager.getDynamicShortcuts().isEmpty() && manager.getPinnedShortcuts().isEmpty(), "no shortcuts stored");
        check(!manager.isRequestPinShortcutSupported(), "no launcher confirms pinned shortcuts");
        check(manager.getMaxShortcutCountPerActivity() == 15, "default shortcut limit");
        Object service = endpoint("shortcut", "android.content.pm.IShortcutService");
        denied(() -> invoke(service, "removeAllDynamicShortcuts", "org.bifrost.absent", 0));
        denied(() -> invoke(service, "removeAllDynamicShortcuts", NAME, 10));
        checks++;
    }

    // RoleManager, which Firefox reads for its default-browser prompt: this device defines no roles.
    private static void roles(Context context) throws Exception {
        android.app.role.RoleManager manager = context.getSystemService(android.app.role.RoleManager.class);
        check(manager != null, "RoleManager is published");
        check(!manager.isRoleAvailable(android.app.role.RoleManager.ROLE_BROWSER), "no role is available");
        check(!manager.isRoleHeld(android.app.role.RoleManager.ROLE_BROWSER), "no role is held");
        Object service = endpoint("role", "android.app.role.IRoleManager");
        denied(() -> invoke(service, "isRoleAvailableAsUser", "role", 10));
        denied(() -> invoke(service, "isRoleHeldAsUser", "role", "org.bifrost.absent", 0));
        checks++;
    }

    // AppWidgetManager.getInstalledProvidersForProfile, which Roblox calls: the caller's own profile lists no providers.
    private static void appWidgets(Context context) throws Exception {
        Object widgets = endpoint("appwidget", "com.android.internal.appwidget.IAppWidgetService");
        Object slice = invoke(widgets, "getInstalledProvidersForProfile", 1, 0, NAME);
        check(slice != null && ((java.util.List<?>) slice.getClass().getMethod("getList").invoke(slice)).isEmpty(), "provider list for the own profile");
        check(invoke(widgets, "getInstalledProvidersForProfile", 1, 10, NAME) == null, "provider list for a disabled profile");
        denied(() -> invoke(widgets, "getInstalledProvidersForProfile", 1, 0, "org.bifrost.absent"));
        check(Boolean.FALSE.equals(invoke(widgets, "isRequestPinAppWidgetSupported")), "no launcher confirms pinned widgets");
        // notifyProviderInheritance: a null array or entry ends the call, the own package has no provider to
        // mark, and a foreign package is refused before any lookup.
        invoke(widgets, "notifyProviderInheritance", (Object) null);
        invoke(widgets, "notifyProviderInheritance", (Object) new ComponentName[] {new ComponentName(NAME, "Provider")});
        invoke(widgets, "notifyProviderInheritance", (Object) new ComponentName[] {null, new ComponentName("org.bifrost.absent", "P")});
        denied(() -> invoke(widgets, "notifyProviderInheritance",
                (Object) new ComponentName[] {new ComponentName("org.bifrost.absent", "P")}));
        checks++;
    }

    // The battery follows the host (see batteryFromSupplies for the model): a host without a battery is a full one on
    // AC, a host with one reports its own state. Either way BatteryManager, the sticky intent and the registered
    // receiver agree.
    private static void battery(Context context) throws Exception {
        boolean hostBattery = false;
        String[] supplies = new File("/sys/class/power_supply").list();
        if (supplies != null)
            for (String supply : supplies) {
                File type = new File("/sys/class/power_supply/" + supply + "/type"), scope = new File("/sys/class/power_supply/" + supply + "/scope");
                if (type.isFile() && new String(java.nio.file.Files.readAllBytes(type.toPath())).trim().equals("Battery")
                        && !(scope.isFile() && new String(java.nio.file.Files.readAllBytes(scope.toPath())).trim().equals("Device")))
                    hostBattery = true;
            }
        android.os.BatteryManager manager = context.getSystemService(android.os.BatteryManager.class);
        check(manager != null, "BatteryManager unavailable");
        int capacity = manager.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY);
        check(capacity >= 0 && capacity <= 100, "capacity out of range: " + capacity);
        android.content.IntentFilter filter = new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED);
        android.content.Intent sticky = context.registerReceiver(null, filter);
        check(sticky != null && sticky.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, -1) == 100, "sticky battery intent incorrect");
        int level = sticky.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1);
        check(Math.abs(level - capacity) <= 1, "sticky level " + level + " differs from the capacity property " + capacity);
        if (!hostBattery) {
            check(manager.isCharging() && capacity == 100 && manager.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CURRENT_NOW) == Integer.MIN_VALUE
                    && level == 100 && sticky.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, -1) == android.os.BatteryManager.BATTERY_PLUGGED_AC
                    && sticky.getIntExtra(android.os.BatteryManager.EXTRA_STATUS, -1) == android.os.BatteryManager.BATTERY_STATUS_FULL
                    && sticky.getBooleanExtra(android.os.BatteryManager.EXTRA_PRESENT, false),
                    "a host without a battery is not a full battery on AC: " + sticky.getExtras());
        } else {
            check(sticky.getBooleanExtra(android.os.BatteryManager.EXTRA_PRESENT, false)
                    || sticky.getIntExtra(android.os.BatteryManager.EXTRA_STATUS, -1) == android.os.BatteryManager.BATTERY_STATUS_UNKNOWN,
                    "host battery missing from the sticky intent: " + sticky.getExtras());
        }
        check(context.registerReceiver(null, new android.content.IntentFilter("org.bifrost.none")) == null,
                "sticky intent returned for an unrelated filter");
        android.os.HandlerThread thread = new android.os.HandlerThread("battery-receiver");
        thread.start();
        java.util.concurrent.CountDownLatch received = new java.util.concurrent.CountDownLatch(1);
        android.content.BroadcastReceiver receiver = new android.content.BroadcastReceiver() {
            @Override public void onReceive(Context c, android.content.Intent intent) {
                if (isInitialStickyBroadcast() && Math.abs(intent.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1) - level) <= 1)
                    received.countDown();
            }
        };
        context.registerReceiver(receiver, filter, null, new android.os.Handler(thread.getLooper()));
        check(received.await(10, java.util.concurrent.TimeUnit.SECONDS), "sticky battery intent not delivered");
        context.unregisterReceiver(receiver);
        thread.quitSafely();
    }

    // UsageStatsManager and NetworkStatsManager against the "usagestats" and "netstats" services: an app without
    // PACKAGE_USAGE_STATS sees only itself (ACTIVE, no history); a wrong package or a system-only call is refused as
    // the service refuses it. No traffic or usage is recorded, so queries are empty and kernel counters unsupported.
    private static void usageAndNetworkStats(Context context) throws Exception {
        android.app.usage.UsageStatsManager usage = context.getSystemService(android.app.usage.UsageStatsManager.class);
        check(usage != null, "UsageStatsManager is unavailable");
        long now = System.currentTimeMillis();
        check(usage.getAppStandbyBucket() == android.app.usage.UsageStatsManager.STANDBY_BUCKET_ACTIVE, "app is not ACTIVE");
        check(!usage.isAppInactive(NAME), "app is inactive");
        check(usage.queryUsageStats(android.app.usage.UsageStatsManager.INTERVAL_DAILY, 0, now).isEmpty(), "usage stats without permission");
        check(!usage.queryEvents(0, now).hasNextEvent() && !usage.queryEventsForSelf(0, now).hasNextEvent(), "events invented");
        Class<?> iface = Class.forName("android.app.usage.IUsageStatsManager");
        Object service = Class.forName("android.app.usage.IUsageStatsManager$Stub").getMethod("asInterface", IBinder.class)
                .invoke(null, Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "usagestats"));
        check((Boolean) iface.getMethod("isAppStandbyEnabled").invoke(service), "standby disabled");
        try { iface.getMethod("getAppStandbyBuckets", String.class, int.class).invoke(service, NAME, 0); throw new AssertionError("buckets listed without PACKAGE_USAGE_STATS"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof SecurityException, "getAppStandbyBuckets: " + expected.getCause()); }
        try { iface.getMethod("setAppInactive", String.class, boolean.class, int.class).invoke(service, NAME, true, 0); throw new AssertionError("idle state changed"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof SecurityException, "setAppInactive: " + expected.getCause()); }
        Binder activity = new Binder();
        Method start = iface.getMethod("reportUsageStart", IBinder.class, String.class, String.class);
        Method stop = iface.getMethod("reportUsageStop", IBinder.class, String.class, String.class);
        start.invoke(service, activity, "t", NAME);
        try { start.invoke(service, activity, "t", NAME); throw new AssertionError("token started twice"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof IllegalArgumentException, "double start: " + expected.getCause()); }
        stop.invoke(service, activity, "t", NAME);
        try { stop.invoke(service, activity, "t", NAME); throw new AssertionError("token stopped twice"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof IllegalArgumentException, "double stop: " + expected.getCause()); }

        android.app.usage.NetworkStatsManager net = context.getSystemService(android.app.usage.NetworkStatsManager.class);
        check(net != null, "NetworkStatsManager is unavailable");
        int wifi = android.net.ConnectivityManager.TYPE_WIFI;
        check(net.querySummaryForDevice(wifi, null, 0, now).getRxBytes() == 0, "device traffic invented");
        android.app.usage.NetworkStats details = net.queryDetailsForUid(wifi, null, 0, now, Process.myUid());
        check(!details.hasNextBucket(), "uid traffic invented");
        details.close();
        android.app.usage.NetworkStats summary = net.querySummary(wifi, null, 0, now);
        check(!summary.hasNextBucket(), "summary traffic invented");
        summary.close();
        android.app.usage.NetworkStatsManager.UsageCallback callback = new android.app.usage.NetworkStatsManager.UsageCallback() {
            @Override public void onThresholdReached(int networkType, String subscriberId) { throw new AssertionError("threshold reached"); }
        };
        net.registerUsageCallback(wifi, null, 1, callback);
        net.unregisterUsageCallback(callback);
        Object netService = Class.forName("android.net.INetworkStatsService$Stub").getMethod("asInterface", IBinder.class)
                .invoke(null, Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "netstats"));
        Class<?> netIface = Class.forName("android.net.INetworkStatsService");
        check((Long) netIface.getMethod("getUidStats", int.class, int.class).invoke(netService, Process.myUid(), 0) == -1L, "kernel counters invented");
        check(((String[]) netIface.getMethod("getMobileIfaces").invoke(netService)).length == 0, "mobile interfaces invented");
        try { netIface.getMethod("forceUpdate").invoke(netService); throw new AssertionError("stats forced by an app"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof SecurityException, "forceUpdate: " + expected.getCause()); }
        try { netIface.getMethod("openSessionForUsageStats", int.class, String.class).invoke(netService, 0, "org.bifrost.other"); throw new AssertionError("foreign package session"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof SecurityException, "foreign session: " + expected.getCause()); }
        System.out.println("MANAGED_USAGE_NET_STATS_OK");
    }
    // The battery follows the host's power supplies: a battery with a charger, a peripheral's battery that is
    // not the device's, transitions broadcast as BatteryService does, and a host with none is a full battery on AC.
    private static void batteryFromSupplies(Context context) throws Exception {
        File root = new File(context.getCacheDir(), "power_supply-" + System.nanoTime());
        File battery = new File(root, "BAT0"), mains = new File(root, "AC"), mouse = new File(root, "hidpp_battery_0");
        for (File dir : new File[] {battery, mains, mouse}) check(dir.mkdirs(), "fake supply directory");
        String saved = FrameworkBattery.suppliesRoot;
        FrameworkBattery.suppliesRoot = root.getPath();
        try {
            // No supplies at all: a full battery on mains.
            FrameworkBattery none = new FrameworkBattery(null);
            File empty = new File(context.getCacheDir(), "power_supply-empty-" + System.nanoTime());
            check(empty.mkdirs(), "empty supplies");
            FrameworkBattery.suppliesRoot = empty.getPath();
            none = new FrameworkBattery(null);
            Intent full = none.stickyIntent();
            check(full.getIntExtra("level", -1) == 100 && full.getIntExtra("status", -1) == 5 && full.getIntExtra("plugged", -1) == 1
                    && full.getIntExtra("health", -1) == 2 && full.getBooleanExtra("present", false), "no battery is not full on AC: " + full.getExtras());
            FrameworkBattery.suppliesRoot = root.getPath();
            write(battery, "type", "Battery"); write(battery, "status", "Charging"); write(battery, "capacity", "42");
            write(battery, "health", "Good"); write(battery, "present", "1"); write(battery, "technology", "Li-ion");
            write(battery, "voltage_now", "11800000"); write(battery, "temp", "301"); write(battery, "cycle_count", "77");
            write(battery, "charge_counter", "3100000"); write(battery, "current_now", "-1500000"); write(battery, "energy_now", "20000000");
            write(mains, "type", "Mains"); write(mains, "online", "1");
            write(mouse, "type", "Battery"); write(mouse, "scope", "Device"); write(mouse, "capacity", "3");
            FrameworkBattery fb = new FrameworkBattery(null);
            Intent sticky = fb.stickyIntent();
            check(sticky.getIntExtra("level", -1) == 42 && sticky.getIntExtra("status", -1) == 2 && sticky.getIntExtra("plugged", -1) == 1
                    && sticky.getIntExtra("health", -1) == 2 && sticky.getIntExtra("voltage", -1) == 11800 && sticky.getIntExtra("temperature", -1) == 301
                    && "Li-ion".equals(sticky.getStringExtra("technology")) && sticky.getIntExtra("charge_counter", -1) == 3100000
                    && sticky.getIntExtra("android.os.extra.CYCLE_COUNT", -1) == 77, "host battery not reflected: " + sticky.getExtras());
            check(!sticky.getBooleanExtra("battery_low", true), "low battery while charging");
            // Unplugged and low: disconnect, changed (new sequence), low.
            write(battery, "status", "Discharging"); write(battery, "capacity", "14"); write(mains, "online", "0");
            List<String> actions = new ArrayList<>();
            List<Intent> sent = new ArrayList<>();
            fb.refresh(intent -> { actions.add(intent.getAction()); sent.add(intent); });
            check(actions.equals(Arrays.asList(Intent.ACTION_POWER_DISCONNECTED, Intent.ACTION_BATTERY_CHANGED, Intent.ACTION_BATTERY_LOW)),
                    "transition broadcasts: " + actions);
            Intent changed = sent.get(1);
            check(changed.getIntExtra("level", -1) == 14 && changed.getIntExtra("plugged", -1) == 0 && changed.getIntExtra("status", -1) == 3
                    && changed.getBooleanExtra("battery_low", false) && changed.getIntExtra("seq", 0) > sticky.getIntExtra("seq", 0), "changed intent: " + changed.getExtras());
            actions.clear();
            fb.refresh(intent -> actions.add(intent.getAction()));
            check(actions.isEmpty(), "an unchanged host was broadcast again: " + actions);
            // Plugged back in at 25%: connect, changed, OK.
            write(battery, "status", "Charging"); write(battery, "capacity", "25"); write(mains, "online", "1");
            fb.refresh(intent -> actions.add(intent.getAction()));
            check(actions.equals(Arrays.asList(Intent.ACTION_POWER_CONNECTED, Intent.ACTION_BATTERY_CHANGED, Intent.ACTION_BATTERY_OKAY)),
                    "reconnect broadcasts: " + actions);
            // BatteryManager's properties come from the same files; an attribute the battery lacks is not found.
            Object registrar = Class.forName("android.os.IBatteryPropertiesRegistrar$Stub").getMethod("asInterface", IBinder.class)
                    .invoke(null, Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "batteryproperties"));
            check(registrar != null, "batteryproperties unavailable");
            System.out.println("MANAGED_BATTERY_SUPPLIES_OK");
        } finally { FrameworkBattery.suppliesRoot = saved; }
    }
    private static void write(File dir, String name, String value) throws java.io.IOException {
        java.nio.file.Files.write(new File(dir, name).toPath(), (value + "\n").getBytes("UTF-8"));
    }
    // The device services YouTube looked up at start: media_metrics (session ids, reports dropped), locale (app
    // locales read back; a change is refused), legacy_permission (device identifiers denied to an app) and auth (no
    // biometric sensors, no screen lock), each as the Android 15 service answers an app.
    private static void mediaLocaleLegacyAuth(Context context) throws Exception {
        android.media.metrics.MediaMetricsManager metrics = context.getSystemService(android.media.metrics.MediaMetricsManager.class);
        check(metrics != null, "MediaMetricsManager is unavailable");
        android.media.metrics.PlaybackSession first = metrics.createPlaybackSession(), second = metrics.createPlaybackSession();
        String id = first.getSessionId().getStringId();
        check(id.length() == 16 && !id.equals(second.getSessionId().getStringId()) && id.matches("[A-Za-z0-9_-]+"), "session ids: " + id);
        first.reportPlaybackStateEvent(new android.media.metrics.PlaybackStateEvent.Builder().setState(android.media.metrics.PlaybackStateEvent.STATE_PLAYING).build());
        first.close(); second.close();

        android.app.LocaleManager locales = context.getSystemService(android.app.LocaleManager.class);
        check(locales != null && locales.getApplicationLocales().isEmpty() && locales.getSystemLocales().size() > 0, "locale manager");
        locales.setApplicationLocales(android.os.LocaleList.getEmptyLocaleList());
        try { locales.setApplicationLocales(android.os.LocaleList.forLanguageTags("fr")); throw new AssertionError("app locale applied silently"); }
        catch (UnsupportedOperationException expected) { checks++; }

        Class<?> legacy = Class.forName("android.permission.ILegacyPermissionManager");
        Object service = Class.forName("android.permission.ILegacyPermissionManager$Stub").getMethod("asInterface", IBinder.class)
                .invoke(null, Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "legacy_permission"));
        Method identifier = legacy.getMethod("checkDeviceIdentifierAccess", String.class, String.class, String.class, int.class, int.class);
        check((Integer) identifier.invoke(service, NAME, "test", null, Process.myPid(), Process.myUid()) == -1, "device identifier access granted to an app");
        try { identifier.invoke(service, NAME, "test", null, Process.myPid(), Process.myUid() + 1); throw new AssertionError("another uid checked"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof SecurityException, "foreign uid: " + expected.getCause()); }
        try { legacy.getMethod("grantDefaultPermissionsToCarrierServiceApp", String.class, int.class).invoke(service, NAME, 0); throw new AssertionError("default grants by an app"); }
        catch (InvocationTargetException expected) { check(expected.getCause() instanceof SecurityException, "default grant: " + expected.getCause()); }

        android.hardware.biometrics.BiometricManager biometrics = context.getSystemService(android.hardware.biometrics.BiometricManager.class);
        check(biometrics != null, "BiometricManager is unavailable");
        boolean permitted = context.checkSelfPermission("android.permission.USE_BIOMETRIC") == android.content.pm.PackageManager.PERMISSION_GRANTED;
        try {
            int strong = biometrics.canAuthenticate(android.hardware.biometrics.BiometricManager.Authenticators.BIOMETRIC_STRONG);
            int credential = biometrics.canAuthenticate(android.hardware.biometrics.BiometricManager.Authenticators.DEVICE_CREDENTIAL);
            check(permitted && strong == android.hardware.biometrics.BiometricManager.BIOMETRIC_ERROR_NO_HARDWARE
                    && credential == android.hardware.biometrics.BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED, "canAuthenticate " + strong + "/" + credential);
        } catch (SecurityException refused) { check(!permitted && refused.getMessage().contains("USE_BIOMETRIC"), "canAuthenticate: " + refused); }
        System.out.println("MANAGED_MEDIA_LOCALE_LEGACY_AUTH_OK permitted=" + permitted);
    }
    // The framework's AndroidKeyStore provider over FrameworkKeystore (keystore2 + software KeyMint).
    private static void keystore() throws Exception {
        java.security.KeyStore store = java.security.KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        for (String alias : new String[] {"batch-aes", "batch-encrypt-only", "batch-import", "batch-hmac"})
            store.deleteEntry(alias);
        javax.crypto.KeyGenerator generator = javax.crypto.KeyGenerator.getInstance("AES", "AndroidKeyStore");
        generator.init(new android.security.keystore.KeyGenParameterSpec.Builder("batch-aes",
                android.security.keystore.KeyProperties.PURPOSE_ENCRYPT | android.security.keystore.KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes("GCM").setEncryptionPaddings("NoPadding").setKeySize(256).build());
        javax.crypto.SecretKey key = generator.generateKey();
        byte[] plain = "bifrost keystore".getBytes(java.nio.charset.StandardCharsets.UTF_8), aad = {1, 2, 3};
        javax.crypto.Cipher encrypt = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
        encrypt.init(javax.crypto.Cipher.ENCRYPT_MODE, key);
        encrypt.updateAAD(aad);
        byte[] sealed = encrypt.doFinal(plain), iv = encrypt.getIV();
        check(iv != null && iv.length == 12 && sealed.length == plain.length + 16, "GCM nonce or tag size");
        javax.crypto.Cipher decrypt = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
        decrypt.init(javax.crypto.Cipher.DECRYPT_MODE, key, new javax.crypto.spec.GCMParameterSpec(128, iv));
        decrypt.updateAAD(aad);
        check(Arrays.equals(decrypt.doFinal(sealed), plain), "GCM round trip");
        sealed[0] ^= 1;
        decrypt.init(javax.crypto.Cipher.DECRYPT_MODE, key, new javax.crypto.spec.GCMParameterSpec(128, iv));
        decrypt.updateAAD(aad);
        try { decrypt.doFinal(sealed); check(false, "tampered GCM ciphertext accepted"); }
        catch (javax.crypto.AEADBadTagException expected) { checks++; }
        android.security.keystore.KeyInfo info = (android.security.keystore.KeyInfo) javax.crypto.SecretKeyFactory
                .getInstance("AES", "AndroidKeyStore").getKeySpec(key, android.security.keystore.KeyInfo.class);
        check(info.getKeySize() == 256 && !info.isInsideSecureHardware()
                && info.getOrigin() == android.security.keystore.KeyProperties.ORIGIN_GENERATED, "KeyInfo");
        // KeyMint INCOMPATIBLE_PURPOSE.
        generator.init(new android.security.keystore.KeyGenParameterSpec.Builder("batch-encrypt-only",
                android.security.keystore.KeyProperties.PURPOSE_ENCRYPT)
                .setBlockModes("GCM").setEncryptionPaddings("NoPadding").build());
        javax.crypto.SecretKey encryptOnly = generator.generateKey();
        try {
            decrypt.init(javax.crypto.Cipher.DECRYPT_MODE, encryptOnly, new javax.crypto.spec.GCMParameterSpec(128, iv));
            check(false, "decryption with an encrypt-only key");
        } catch (java.security.InvalidKeyException expected) { checks++; }
        // Imported material gives the same results as the software implementation.
        byte[] material = new byte[16];
        for (int i = 0; i < material.length; i++) material[i] = (byte) i;
        javax.crypto.spec.SecretKeySpec raw = new javax.crypto.spec.SecretKeySpec(material, "AES");
        store.setEntry("batch-import", new java.security.KeyStore.SecretKeyEntry(raw),
                new android.security.keystore.KeyProtection.Builder(android.security.keystore.KeyProperties.PURPOSE_DECRYPT)
                        .setBlockModes("CBC").setEncryptionPaddings("PKCS7Padding").build());
        byte[] cbcIv = new byte[16];
        javax.crypto.Cipher software = javax.crypto.Cipher.getInstance("AES/CBC/PKCS5Padding", "AndroidOpenSSL");
        software.init(javax.crypto.Cipher.ENCRYPT_MODE, raw, new javax.crypto.spec.IvParameterSpec(cbcIv));
        byte[] cbc = software.doFinal(plain);
        decrypt = javax.crypto.Cipher.getInstance("AES/CBC/PKCS7Padding");
        decrypt.init(javax.crypto.Cipher.DECRYPT_MODE, store.getKey("batch-import", null),
                new javax.crypto.spec.IvParameterSpec(cbcIv));
        check(Arrays.equals(decrypt.doFinal(cbc), plain), "imported AES-CBC key");
        javax.crypto.spec.SecretKeySpec hmacRaw = new javax.crypto.spec.SecretKeySpec(material, "HmacSHA256");
        store.setEntry("batch-hmac", new java.security.KeyStore.SecretKeyEntry(hmacRaw),
                new android.security.keystore.KeyProtection.Builder(android.security.keystore.KeyProperties.PURPOSE_SIGN).build());
        javax.crypto.Mac mac = javax.crypto.Mac.getInstance("HmacSHA256");
        mac.init(store.getKey("batch-hmac", null));
        javax.crypto.Mac reference = javax.crypto.Mac.getInstance("HmacSHA256", "AndroidOpenSSL");
        reference.init(hmacRaw);
        check(Arrays.equals(mac.doFinal(plain), reference.doFinal(plain)), "imported HMAC key");
        // A fresh provider instance reads the persisted entries; deletion removes them.
        java.security.KeyStore reloaded = java.security.KeyStore.getInstance("AndroidKeyStore");
        reloaded.load(null);
        List<String> aliases = java.util.Collections.list(reloaded.aliases());
        check(aliases.equals(Arrays.asList("batch-aes", "batch-encrypt-only", "batch-hmac", "batch-import"))
                && reloaded.size() == 4 && reloaded.getKey("batch-aes", null) != null, "persisted aliases " + aliases);
        for (String alias : aliases) reloaded.deleteEntry(alias);
        check(!reloaded.containsAlias("batch-aes") && reloaded.size() == 0, "keystore deletion");
        System.out.println("MANAGED_KEYSTORE_OK");
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
        // Toast.show used to die on unsupported notification.enqueueToast.
        if (Looper.myLooper() == null) Looper.prepare();
        android.widget.Toast toast = android.widget.Toast.makeText(context, "toast", android.widget.Toast.LENGTH_SHORT);
        toast.show();
        toast.cancel();
        // Uptodown dereferenced a null NsdManager ("servicediscovery" unpublished).
        android.net.nsd.NsdManager nsd = context.getSystemService(android.net.nsd.NsdManager.class);
        check(nsd != null, "NsdManager missing");
        java.util.concurrent.CountDownLatch started = new java.util.concurrent.CountDownLatch(1),
                stopped = new java.util.concurrent.CountDownLatch(1);
        boolean[] found = new boolean[1];
        android.net.nsd.NsdManager.DiscoveryListener nsdListener = new android.net.nsd.NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String type, int code) {}
            @Override public void onStopDiscoveryFailed(String type, int code) {}
            @Override public void onDiscoveryStarted(String type) { started.countDown(); }
            @Override public void onDiscoveryStopped(String type) { stopped.countDown(); }
            @Override public void onServiceFound(android.net.nsd.NsdServiceInfo info) { found[0] = true; }
            @Override public void onServiceLost(android.net.nsd.NsdServiceInfo info) {}
        };
        nsd.discoverServices("_http._tcp", android.net.nsd.NsdManager.PROTOCOL_DNS_SD, nsdListener);
        check(started.await(10, java.util.concurrent.TimeUnit.SECONDS), "NSD discovery never started");
        nsd.stopServiceDiscovery(nsdListener);
        check(stopped.await(10, java.util.concurrent.TimeUnit.SECONDS) && !found[0], "NSD discovery did not stop cleanly");
    }

    // Without the managed libaaudio adapter (no --managed-graphics) the host stream opens through Android's
    // own libaaudio, which plays through AudioTrack and so through createTrack again. That used to recurse
    // until the stack overflowed and killed the process; now the AudioTrack fails to initialize.
    private static boolean noAudioAdapter;
    private static void audioTrackWithoutAdapter() throws Exception {
        android.media.AudioFormat format = new android.media.AudioFormat.Builder()
                .setSampleRate(48000).setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).build();
        android.media.AudioTrack track = null;
        try {
            track = new android.media.AudioTrack.Builder()
                    .setAudioFormat(format).setTransferMode(android.media.AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(android.media.AudioTrack.getMinBufferSize(48000,
                            android.media.AudioFormat.CHANNEL_OUT_STEREO, android.media.AudioFormat.ENCODING_PCM_16BIT) * 2)
                    .build();
        } catch (UnsupportedOperationException refused) { checks++; }
        check(track == null || track.getState() != android.media.AudioTrack.STATE_INITIALIZED,
                "an AudioTrack was created without a host stream");
        if (track != null) track.release();
        System.out.println("MANAGED_AUDIO_TRACK_REFUSED_OK");
        System.out.println("MANAGED_SERVICE_BATCH_OK checks=" + checks + " mode=no-audio-adapter");
        System.out.println("FRAMEWORK_OK");
        System.exit(0);
    }

    // A real android.media.AudioTrack plays through AudioFlinger.createTrack: the client writes
    // PCM into the shared ring and waits on its futex, and the native server consumes it.
    private static void audioTracks() throws Exception {
        if (noAudioAdapter) { audioTrackWithoutAdapter(); return; }
        final int rate = 48000, frames = rate / 4;
        android.media.AudioFormat format = new android.media.AudioFormat.Builder()
                .setSampleRate(rate).setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).build();
        android.media.AudioTrack track = new android.media.AudioTrack.Builder()
                .setAudioAttributes(new android.media.AudioAttributes.Builder()
                        .setUsage(android.media.AudioAttributes.USAGE_GAME)
                        .setContentType(android.media.AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(format).setTransferMode(android.media.AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(android.media.AudioTrack.getMinBufferSize(rate,
                        android.media.AudioFormat.CHANNEL_OUT_STEREO, android.media.AudioFormat.ENCODING_PCM_16BIT) * 2)
                .build();
        check(track.getState() == android.media.AudioTrack.STATE_INITIALIZED, "AudioTrack was not created");
        // Left at a quarter of full scale, right at a half: channel order, sample size and the
        // ring's start all show in the server's per-channel peaks.
        short[] pcm = new short[frames * 2];
        for (int i = 0; i < frames; i++) {
            double wave = Math.sin(2 * Math.PI * 440 * i / rate);
            pcm[2 * i] = (short) (wave * 8192);
            pcm[2 * i + 1] = (short) (wave * 16384);
        }
        track.play();
        check(track.write(pcm, 0, pcm.length) == pcm.length, "blocking write did not complete");
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (track.getPlaybackHeadPosition() < frames && System.nanoTime() < deadline) Thread.sleep(10);
        check(track.getPlaybackHeadPosition() == frames, "server consumed " + track.getPlaybackHeadPosition()
                + " of " + frames + " frames");
        long[] seen = FrameworkAudioTracks.snapshot(track.getAudioSessionId());
        check(seen != null, "no server counters for the track");
        check(seen[3] > 240_000 && seen[3] <= 250_100 && seen[4] > 490_000 && seen[4] <= 500_100,
                "channel peaks " + seen[3] + "/" + seen[4] + " (expected about 250000/500000)");
        track.stop();
        // Stop, then play again from the same track: the server resumes past the stop position.
        // The start threshold defaults to the whole buffer; lower it so 2,000 frames start playing.
        track.setStartThresholdInFrames(1000);
        track.play();
        check(track.write(pcm, 0, 4000) == 4000, "write after restart");
        deadline = System.nanoTime() + 5_000_000_000L;
        while (track.getPlaybackHeadPosition() < 2000 && System.nanoTime() < deadline) Thread.sleep(10);
        // AudioTrack::start after a stop counts from zero again.
        check(track.getPlaybackHeadPosition() == 2000, "restart consumed " + track.getPlaybackHeadPosition()
                + " of 2000 frames");
        track.pause();
        track.flush();
        track.release();
        System.out.println("MANAGED_AUDIO_TRACK_OK");
    }

    // Waits until the track's playback head reaches `frames`, then long enough to see it go past.
    private static long settledHead(android.media.AudioTrack track, long frames) throws InterruptedException {
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (track.getPlaybackHeadPosition() < frames && System.nanoTime() < deadline) Thread.sleep(10);
        Thread.sleep(150);
        return track.getPlaybackHeadPosition();
    }

    // A MODE_STATIC AudioTrack (SoundPool's kind) plays the client's own shared buffer: once to its
    // end, then from the start with two loops (three passes), then at twice its sample rate, which
    // the server resamples to its stream's rate (half as many frames reach the host).
    private static void staticAudioTracks() throws Exception {
        final int rate = 48000, frames = 4800;
        android.media.AudioFormat format = new android.media.AudioFormat.Builder()
                .setSampleRate(rate).setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).build();
        android.media.AudioTrack track = new android.media.AudioTrack.Builder()
                .setAudioAttributes(new android.media.AudioAttributes.Builder()
                        .setUsage(android.media.AudioAttributes.USAGE_GAME).build())
                .setAudioFormat(format).setTransferMode(android.media.AudioTrack.MODE_STATIC)
                .setBufferSizeInBytes(frames * 4).build();
        short[] pcm = new short[frames * 2];
        for (int i = 0; i < frames; i++) {
            double wave = Math.sin(2 * Math.PI * 440 * i / rate);
            pcm[2 * i] = (short) (wave * 8192);
            pcm[2 * i + 1] = (short) (wave * 16384);
        }
        check(track.write(pcm, 0, pcm.length) == pcm.length, "static write");
        check(track.getState() == android.media.AudioTrack.STATE_INITIALIZED, "static AudioTrack state " + track.getState());
        final int session = track.getAudioSessionId();
        track.play();
        long head = settledHead(track, frames);
        check(head == frames, "static track played " + head + " of " + frames + " frames");
        long[] seen = FrameworkAudioTracks.snapshot(session);
        check(seen != null && seen[0] == frames && seen[5] == frames, "static server counters " + Arrays.toString(seen));
        check(seen[3] > 240_000 && seen[3] <= 250_100 && seen[4] > 490_000 && seen[4] <= 500_100,
                "static channel peaks " + seen[3] + "/" + seen[4] + " (expected about 250000/500000)");
        track.stop();
        check(track.setLoopPoints(0, frames, 2) == android.media.AudioTrack.SUCCESS, "setLoopPoints");
        track.play();
        head = settledHead(track, 3L * frames);
        check(head == 3L * frames, "looped static track played " + head + " of " + 3 * frames + " frames");
        track.stop();
        check(track.setPlaybackRate(rate * 2) == android.media.AudioTrack.SUCCESS, "setPlaybackRate");
        long before = FrameworkAudioTracks.snapshot(session)[5];
        track.play();
        head = settledHead(track, frames);
        seen = FrameworkAudioTracks.snapshot(session);
        check(head == frames && seen[0] == 5L * frames, "static track at twice its rate read " + head + " frames, "
                + seen[0] + " in all");
        check(Math.abs(seen[5] - before - frames / 2) <= 2, "static track at twice its rate gave the host "
                + (seen[5] - before) + " frames (expected about " + frames / 2 + ")");
        track.release();
        System.out.println("MANAGED_AUDIO_STATIC_TRACK_OK");
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
        cursors(context);
        gamepads();
        receivers(context);
        sharedLibraries();
        deviceConfiguration(context);
        audioMode(context);
        mediaSessions(context);
        audioTracks();
        staticAudioTracks();
        audioEffects();
        notifications(context, packages); audio(context); nativeServices(); power(context); wifi(context);
        uiMode(context); permissions(context); appOps(context, packages); appWidgets(context); roles(context); shortcuts(context);
        battery(context); vibratorAndKeyguard(context); usageAndNetworkStats(context); mediaLocaleLegacyAuth(context); batteryFromSupplies(context); clipboard(context, packages);
        alarmsAccountsLocation(context);
        keystore();
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
        noAudioAdapter = args.length > 0 && args[0].equals("no-audio-adapter");
        try { run(); System.exit(0); }
        catch (Throwable error) { error.printStackTrace(); System.exit(1); }
    }
}
