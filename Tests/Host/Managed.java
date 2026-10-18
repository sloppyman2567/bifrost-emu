package org.bifrost.fixture;

import android.app.Activity;
import android.app.Application;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.content.SharedPreferences;
import android.provider.Settings;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/** Real ART/framework fixture: persistent data, Surface callbacks and finish. */
public final class Managed extends Activity {
    private static int providerOrder, providerCreates;
    private static boolean applicationStarted;
    public static final class App extends Application {
        @Override public void onCreate() {
            super.onCreate();
            if (providerOrder != 2 || providerCreates != 1)
                throw new AssertionError("Providers did not precede Application.onCreate");
            applicationStarted = true;
            System.out.println("MANAGED_FIXTURE_APPLICATION_AFTER_PROVIDERS");
        }
    }
    public static class InitProvider extends ContentProvider {
        @Override public boolean onCreate() {
            if (applicationStarted || providerOrder != 1 || getContext() == null
                    || getContext().getApplicationContext() == null
                    || getContext().getSystemService(android.content.Context.CONNECTIVITY_SERVICE) == null)
                throw new AssertionError("Provider context/order is incorrect");
            providerOrder = 2;
            providerCreates++;
            System.out.println("MANAGED_FIXTURE_PROVIDER_CREATED");
            // Returning false does not prevent ActivityThread publication.
            return false;
        }
        @Override public Bundle call(String method, String arg, Bundle extras) {
            Bundle result = new Bundle();
            result.putInt("creates", providerCreates);
            return result;
        }
        @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) { return null; }
        @Override public String getType(Uri u) { return "application/bifrost-fixture"; }
        @Override public Uri insert(Uri u, ContentValues v) { throw new UnsupportedOperationException(); }
        @Override public int delete(Uri u, String s, String[] a) { throw new UnsupportedOperationException(); }
        @Override public int update(Uri u, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
    }
    public static final class EarlyProvider extends InitProvider {
        @Override public boolean onCreate() {
            if (applicationStarted || providerOrder != 0) throw new AssertionError("Provider initOrder ignored");
            providerOrder = 1;
            return true;
        }
    }
    public static final class DisabledProvider extends InitProvider {
        @Override public boolean onCreate() { throw new AssertionError("Disabled provider started"); }
    }
    public static final class RemoteProvider extends InitProvider {
        @Override public boolean onCreate() { throw new AssertionError("Remote provider started in main process"); }
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (!applicationStarted) throw new AssertionError("Activity preceded Application.onCreate");
        for (String authority : new String[] {"org.bifrost.fixture.init", "org.bifrost.fixture.alias"}) {
            Bundle result = getContentResolver().call(Uri.parse("content://" + authority), "probe", null, null);
            if (result == null || result.getInt("creates") != 1)
                throw new AssertionError("Provider authority did not resolve to the local instance");
        }
        System.out.println("MANAGED_FIXTURE_PROVIDER_ALIASES_OK");
        android.content.Intent absent = new android.content.Intent("org.bifrost.fixture.UNCREATED")
                .setPackage(getPackageName());
        int flags = android.app.PendingIntent.FLAG_NO_CREATE | android.app.PendingIntent.FLAG_IMMUTABLE;
        if (android.app.PendingIntent.getBroadcast(this, 71, absent, flags) != null
                || android.app.PendingIntent.getActivity(this, 71, absent, flags) != null
                || android.app.PendingIntent.getService(this, 71, absent, flags) != null)
            throw new AssertionError("NO_CREATE created or invented a PendingIntent");
        System.out.println("MANAGED_FIXTURE_PENDING_LOOKUP_OK");
        android.app.PendingIntent sender = android.app.PendingIntent.getBroadcast(this, 71, absent, android.app.PendingIntent.FLAG_IMMUTABLE);
        if (sender == null || !sender.equals(android.app.PendingIntent.getBroadcast(this, 71, absent,
                android.app.PendingIntent.FLAG_IMMUTABLE | android.app.PendingIntent.FLAG_NO_CREATE)))
            throw new AssertionError("PendingIntent identity lookup failed");
        sender.cancel();
        System.out.println("MANAGED_FIXTURE_PENDING_CREATE_OK");
        if ("activity".equals(System.getProperty("bifrost.apk.trace.mode"))) {
            for (int i = 0; i < 2; i++) android.app.ActivityManager.getMyMemoryState(
                    new android.app.ActivityManager.RunningAppProcessInfo());
            try {
                ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getAppTasks();
                throw new AssertionError("Missing Activity API silently succeeded");
            } catch (UnsupportedOperationException expected) {
                System.out.println("MANAGED_FIXTURE_ACTIVITY_TRACE_FAILURE_OK");
            }
        }
        SharedPreferences prefs = getSharedPreferences("fixture", MODE_PRIVATE);
        String id = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
        if (id == null || !id.matches("[0-9a-f]{16}")
                || !id.equals(prefs.getString("android_id", id)))
            throw new IllegalStateException("Android ID missing or changed across launches");
        if (!prefs.edit().putString("android_id", id).commit())
            throw new IllegalStateException("Android ID preference commit failed");
        if (Settings.Global.getString(getContentResolver(), "bifrost.missing") != null
                || Settings.Secure.getInt(getContentResolver(), "bifrost.missing", 37) != 37)
            throw new IllegalStateException("Settings missing/default semantics incorrect");
        System.out.println("MANAGED_FIXTURE_SETTINGS_OK " + id);
        checkActivityResolution();
        checkBackgroundPreferenceCommit();
        checkTelephonyRegistry();
        checkNetworkInterfaces();
        checkPackageQueries();
        checkAppTasks();
        checkLoudness();
        int starts = prefs.getInt("starts", 0) + 1;
        if (Settings.System.getInt(getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT, -1) != (starts == 3 ? 12345 : 60000))
            throw new AssertionError("System defaults or preserved override incorrect");
        if (!prefs.edit().putInt("starts", starts).commit())
            throw new IllegalStateException("preference commit failed");
        System.out.println("MANAGED_FIXTURE_START " + starts);
        // ART maps the install-time odex only after its checksum, boot class
        // path and class loader context checks accept it.
        boolean odex = false;
        try (java.io.BufferedReader maps = new java.io.BufferedReader(new java.io.FileReader("/proc/self/maps"))) {
            for (String line; (line = maps.readLine()) != null && !odex; )
                odex = line.endsWith(getApplicationInfo().sourceDir.replace("base.apk", "oat/arm64/base.odex"));
        } catch (java.io.IOException e) {
            throw new IllegalStateException(e);
        }
        System.out.println("MANAGED_FIXTURE_ODEX " + odex);
        // ART starts its "Profile Saver" thread when the app registers its profiles.
        // Runtime-attached threads join the system group; enumerate without suspending them.
        ThreadGroup group = Thread.currentThread().getThreadGroup();
        while (group.getParent() != null) group = group.getParent();
        Thread[] threads = new Thread[group.activeCount() + 16];
        boolean saver = false;
        for (int i = 0, n = group.enumerate(threads); i < n; i++)
            saver |= "Profile Saver".equals(threads[i].getName());
        System.out.println("MANAGED_FIXTURE_PROFILE_SAVER " + saver);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder holder) {
                System.out.println("MANAGED_FIXTURE_SURFACE_CREATED");
            }
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
            public void surfaceDestroyed(SurfaceHolder holder) {
                System.out.println("MANAGED_FIXTURE_SURFACE_DESTROYED");
            }
        });
        setContentView(view);
    }
    // PackageManager.resolveActivity: explicit, implicit within the package,
    // MATCH_DEFAULT_ONLY (the launcher filter has no DEFAULT category), none.
    private void checkActivityResolution() {
        android.content.pm.PackageManager pm = getPackageManager();
        android.content.Intent launcher = new android.content.Intent(android.content.Intent.ACTION_MAIN)
                .addCategory(android.content.Intent.CATEGORY_LAUNCHER).setPackage(getPackageName());
        android.content.pm.ResolveInfo main = pm.resolveActivity(launcher, 0);
        android.content.pm.ResolveInfo explicit = pm.resolveActivity(new android.content.Intent(this, Managed.class), 0);
        if (main == null || !Managed.class.getName().equals(main.activityInfo.name)
                || explicit == null || !Managed.class.getName().equals(explicit.activityInfo.name)
                || pm.resolveActivity(launcher, android.content.pm.PackageManager.MATCH_DEFAULT_ONLY) != null
                || pm.resolveActivity(new android.content.Intent("org.bifrost.fixture.NOTHING")
                        .setPackage(getPackageName()), 0) != null)
            throw new AssertionError("activity resolution incorrect");
        System.out.println("MANAGED_FIXTURE_RESOLVE_ACTIVITY_OK");
    }

    // SharedPreferences holds listeners weakly.
    private SharedPreferences.OnSharedPreferenceChangeListener preferenceListener;

    // A commit from another thread notifies listeners on the main thread
    // (ActivityThread.sMainThreadHandler).
    private void checkBackgroundPreferenceCommit() {
        SharedPreferences watched = getSharedPreferences("fixture-listener", MODE_PRIVATE);
        Thread main = Thread.currentThread();
        preferenceListener = (shared, key) -> System.out.println("MANAGED_FIXTURE_PREFS_LISTENER " + key
                + " main=" + (Thread.currentThread() == main));
        watched.registerOnSharedPreferenceChangeListener(preferenceListener);
        Thread writer = new Thread(() -> {
            if (!watched.edit().putLong("background", System.nanoTime()).commit())
                throw new AssertionError("background preference commit failed");
        }, "FixturePrefsWriter");
        writer.start();
        try { writer.join(); } catch (InterruptedException error) { throw new AssertionError(error); }
    }

    private static final class DisplayInfo extends android.telephony.TelephonyCallback
            implements android.telephony.TelephonyCallback.DisplayInfoListener {
        @Override public void onDisplayInfoChanged(android.telephony.TelephonyDisplayInfo info) {
            throw new AssertionError("radio-less device reported display info");
        }
    }

    private static final class CellInfo extends android.telephony.TelephonyCallback
            implements android.telephony.TelephonyCallback.CellInfoListener {
        @Override public void onCellInfoChanged(java.util.List<android.telephony.CellInfo> info) {
            throw new AssertionError("cell info delivered without location permission");
        }
    }

    // TelephonyRegistry on a device without a radio: callbacks register and
    // unregister without events; cell info needs a location permission.
    private void checkTelephonyRegistry() {
        android.telephony.TelephonyManager telephony = getSystemService(android.telephony.TelephonyManager.class);
        DisplayInfo display = new DisplayInfo();
        telephony.registerTelephonyCallback(getMainExecutor(), display);
        telephony.unregisterTelephonyCallback(display);
        try {
            telephony.registerTelephonyCallback(getMainExecutor(), new CellInfo());
            throw new AssertionError("cell info listener registered without location permission");
        } catch (SecurityException expected) {
            System.out.println("MANAGED_FIXTURE_TELEPHONY_REGISTRY_OK");
        }
    }

    // libcore's NetworkInterface keeps only interfaces bionic's
    // if_nametoindex (SIOCGIFINDEX) resolves; none gives a null enumeration.
    private void checkNetworkInterfaces() {
        try {
            java.util.Enumeration<java.net.NetworkInterface> all = java.net.NetworkInterface.getNetworkInterfaces();
            boolean loopback = false;
            while (all != null && all.hasMoreElements()) loopback |= all.nextElement().isLoopback();
            if (!loopback) throw new AssertionError("no loopback network interface");
        } catch (java.net.SocketException error) {
            throw new AssertionError(error);
        }
        System.out.println("MANAGED_FIXTURE_NETWORK_INTERFACES_OK");
    }

    // PackageManager queries ad SDKs make: queryIntentActivities,
    // getApplicationEnabledSetting and requestChecksums (whole-file SHA-256,
    // fs-verity root and 1 MiB partial root of the base APK).
    private void checkPackageQueries() {
        android.content.pm.PackageManager pm = getPackageManager();
        java.util.List<android.content.pm.ResolveInfo> main = pm.queryIntentActivities(
                new android.content.Intent(android.content.Intent.ACTION_MAIN).setPackage(getPackageName()),
                android.content.pm.PackageManager.GET_SIGNATURES);
        if (main.size() != 1 || !Managed.class.getName().equals(main.get(0).activityInfo.name))
            throw new AssertionError("queryIntentActivities incorrect: " + main);
        if (pm.getApplicationEnabledSetting(getPackageName())
                != android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DEFAULT)
            throw new AssertionError("application enabled setting incorrect");
        try {
            pm.getApplicationEnabledSetting("org.bifrost.fixture.missing");
            throw new AssertionError("unknown package has an enabled setting");
        } catch (IllegalArgumentException expected) {
        }
        int required = android.content.pm.Checksum.TYPE_WHOLE_SHA256
                | android.content.pm.Checksum.TYPE_WHOLE_MERKLE_ROOT_4K_SHA256
                | android.content.pm.Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA256;
        java.util.concurrent.CountDownLatch ready = new java.util.concurrent.CountDownLatch(1);
        java.util.Map<Integer, byte[]> found = new java.util.concurrent.ConcurrentHashMap<>();
        try {
            pm.requestChecksums(getPackageName(), false, required,
                    android.content.pm.PackageManager.TRUST_NONE, checksums -> {
                        for (android.content.pm.ApkChecksum checksum : checksums)
                            if (checksum.getSplitName() == null) found.put(checksum.getType(), checksum.getValue());
                        ready.countDown();
                    });
            if (!ready.await(60, java.util.concurrent.TimeUnit.SECONDS))
                throw new AssertionError("checksums were not delivered");
            java.security.MessageDigest sha256 = java.security.MessageDigest.getInstance("SHA-256");
            try (java.io.InputStream in = new java.io.FileInputStream(getApplicationInfo().sourceDir)) {
                byte[] buffer = new byte[65536];
                for (int read; (read = in.read(buffer)) != -1; ) sha256.update(buffer, 0, read);
            }
            if (!java.util.Arrays.equals(found.get(android.content.pm.Checksum.TYPE_WHOLE_SHA256), sha256.digest())
                    || found.get(android.content.pm.Checksum.TYPE_WHOLE_MERKLE_ROOT_4K_SHA256) == null
                    || found.get(android.content.pm.Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA256) == null)
                throw new AssertionError("package checksums incorrect: " + found.keySet());
        } catch (Exception error) {
            throw new AssertionError(error);
        }
        System.out.println("MANAGED_FIXTURE_PACKAGE_QUERIES_OK");
    }

    // ActivityManager.getAppTasks: the launcher task, matching getTaskId().
    private void checkAppTasks() {
        java.util.List<android.app.ActivityManager.AppTask> tasks =
                ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getAppTasks();
        if (tasks.size() != 1) throw new AssertionError("app tasks: " + tasks.size());
        android.app.ActivityManager.RecentTaskInfo info = tasks.get(0).getTaskInfo();
        if (info.taskId != getTaskId() || info.numActivities < 1
                || !new android.content.ComponentName(this, Managed.class).equals(info.baseActivity))
            throw new AssertionError("app task info incorrect");
        System.out.println("MANAGED_FIXTURE_APP_TASKS_OK");
    }

    // LoudnessCodecController (ExoPlayer on Android 15): an AAC-LC decoder on
    // the built-in speaker gets CTA-2075 "small" parameters, from the
    // controller's query and from the update addMediaCodec triggers.
    private void checkLoudness() {
        android.media.MediaCodec codec = null;
        try {
            java.util.concurrent.CountDownLatch updated = new java.util.concurrent.CountDownLatch(1);
            Bundle[] update = new Bundle[1];
            android.media.LoudnessCodecController controller = android.media.LoudnessCodecController.create(1,
                    Runnable::run, new android.media.LoudnessCodecController.OnLoudnessCodecUpdateListener() {
                        @Override public Bundle onLoudnessCodecUpdate(android.media.MediaCodec mediaCodec, Bundle params) {
                            update[0] = new Bundle(params);
                            updated.countDown();
                            return params;
                        }
                    });
            android.media.MediaFormat format = android.media.MediaFormat.createAudioFormat(
                    android.media.MediaFormat.MIMETYPE_AUDIO_AAC, 44100, 2);
            format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(new byte[] {0x12, 0x10})); // AAC-LC 44.1 kHz stereo
            codec = android.media.MediaCodec.createDecoderByType(android.media.MediaFormat.MIMETYPE_AUDIO_AAC);
            codec.configure(format, null, null, 0);
            if (!controller.addMediaCodec(codec)) throw new AssertionError("loudness codec rejected");
            Bundle params = controller.getLoudnessCodecParams(codec);
            if (!updated.await(10, java.util.concurrent.TimeUnit.SECONDS)) throw new AssertionError("no loudness update");
            for (Bundle bundle : new Bundle[] {params, update[0]})
                if (bundle.getInt(android.media.MediaFormat.KEY_AAC_DRC_TARGET_REFERENCE_LEVEL) != 64
                        || bundle.getInt(android.media.MediaFormat.KEY_AAC_DRC_HEAVY_COMPRESSION) != 1)
                    throw new AssertionError("loudness parameters " + bundle);
            controller.removeMediaCodec(codec);
            controller.close();
        } catch (java.io.IOException | InterruptedException error) {
            throw new AssertionError("loudness check failed", error);
        } finally {
            if (codec != null) codec.release();
        }
        System.out.println("MANAGED_FIXTURE_LOUDNESS_OK");
        // ExoPlayer watches the Spatializer: none exists, and it stays so.
        android.media.Spatializer spatializer = ((android.media.AudioManager) getSystemService(AUDIO_SERVICE)).getSpatializer();
        android.media.Spatializer.OnSpatializerStateChangedListener listener =
                new android.media.Spatializer.OnSpatializerStateChangedListener() {
                    @Override public void onSpatializerEnabledChanged(android.media.Spatializer s, boolean enabled) {}
                    @Override public void onSpatializerAvailableChanged(android.media.Spatializer s, boolean available) {}
                };
        spatializer.addOnSpatializerStateChangedListener(Runnable::run, listener);
        if (spatializer.getImmersiveAudioLevel() != android.media.Spatializer.SPATIALIZER_IMMERSIVE_LEVEL_NONE
                || spatializer.isAvailable() || spatializer.isEnabled())
            throw new AssertionError("spatializer state");
        spatializer.removeOnSpatializerStateChangedListener(listener);
        System.out.println("MANAGED_FIXTURE_SPATIALIZER_OK");
        // GMA's video view creates a MediaPlayer: PlayerBase registers with
        // IAudioService and reports attribute changes and release there.
        android.media.MediaPlayer player = new android.media.MediaPlayer();
        player.setAudioAttributes(new android.media.AudioAttributes.Builder()
                .setUsage(android.media.AudioAttributes.USAGE_MEDIA).build());
        player.release();
        System.out.println("MANAGED_FIXTURE_PLAYER_TRACKING_OK");
    }

    @Override public void onResume() {
        super.onResume();
        new Handler().postDelayed(() -> finish(), 1000);
    }
    @Override public void onPause() { super.onPause(); System.out.println("MANAGED_FIXTURE_PAUSED"); }
    @Override public void onStop() { super.onStop(); System.out.println("MANAGED_FIXTURE_STOPPED"); }
    @Override public void onDestroy() { super.onDestroy(); System.out.println("MANAGED_FIXTURE_DESTROYED"); }
}
