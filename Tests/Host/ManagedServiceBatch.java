// Real Android 15 NotificationManager/AudioManager/PowerManager clients.
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
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
        new File(info.dataDir).mkdirs();
        pkg.requestedPermissions = new String[] {"android.permission.WAKE_LOCK", "android.permission.MODIFY_AUDIO_SETTINGS",
                "android.permission.ACCESS_WIFI_STATE", "android.permission.CHANGE_WIFI_STATE",
                "android.permission.VIBRATE", "android.permission.DISABLE_KEYGUARD"};
        pkg.requestedPermissionsFlags = new int[pkg.requestedPermissions.length];
        packages.install(system, pkg);
        Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
        Object loaded = threadType.getMethod("getPackageInfoNoCheck", ApplicationInfo.class, compatibility)
                .invoke(thread, info, compatibility.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
        Method bind = ManagedLauncher.class.getDeclaredMethod("bindApplicationMetadata", Object.class,
                Context.class, ApplicationInfo.class, Object.class);
        bind.setAccessible(true); bind.invoke(null, thread, system, info, loaded);
        Method make = loaded.getClass().getDeclaredMethod("makeApplicationInner", boolean.class,
                android.app.Instrumentation.class);
        make.setAccessible(true);
        Context context = (Context) make.invoke(loaded, false, null);
        notifications(context, packages); audio(context); power(context); wifi(context);
        uiMode(context); permissions(context);
        battery(context); vibratorAndKeyguard(context); clipboard(context, packages);
        alarmsAccountsLocation(context);
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
    }

    public static void main(String[] args) {
        try { run(); System.exit(0); }
        catch (Throwable error) { error.printStackTrace(); System.exit(1); }
    }
}
