// Managed installed-Activity runtime for Bifrost. Uses the real guest
// ActivityThread, Instrumentation, LoadedApk and main Looper with a bounded
// in-process service profile. ManagedLauncher --app starts and resumes an app;
// ActivityProbe delegates to probe() for framework assertions and checkpoints.
// Host input, resize and teardown are owned here; every Activity, including
// the launcher one, is a FrameworkWindows window. There is no system_server.
import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.app.GameManager;
import android.app.Instrumentation;
import android.content.AttributionSource;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.Signature;
import android.content.res.Configuration;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.media.AudioManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Process;
import android.os.PowerManager;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.os.RemoteException;
import android.os.UserHandle;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.telephony.TelephonyManager;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.io.File;
import java.io.DataInputStream;
import java.io.FileInputStream;
import java.io.FileDescriptor;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import android.system.Os;
import android.system.OsConstants;
import android.system.ErrnoException;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.Map;
import java.util.UUID;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

public final class ManagedLauncher {
    private static final FrameworkServices services = new FrameworkServices();
    private static final FrameworkPackages packages = new FrameworkPackages();
    private static final FrameworkServiceRuntime serviceRuntime = new FrameworkServiceRuntime(packages);
    private static final FrameworkConnectivity connectivity = new FrameworkConnectivity();
    private static final FrameworkContent content = new FrameworkContent();
    private static boolean diagnostic;
    private static boolean created;
    private static volatile Activity activeActivity;
    private static volatile boolean windowOpen;
    private static final Map<IBinder, Long> fullyDrawn = new ConcurrentHashMap<>();
    // The package installed by app mode; the only package that exists here.
    private static volatile PackageInfo installed;
    private static final FrameworkAudio audio = new FrameworkAudio(packages);
    private static final FrameworkNotifications notifications = new FrameworkNotifications(packages);
    private static final FrameworkPower power = new FrameworkPower(packages, () -> windowOpen);
    private static final FrameworkWifi wifi = new FrameworkWifi(packages, connectivity::online);
    private static final FrameworkInputMethod inputMethod = new FrameworkInputMethod((name, uid) ->
            installed != null && installed.packageName.equals(name) && installed.applicationInfo.uid == uid);
    private static String androidId;
    // ActivityRecord's requested orientation is initialized from its manifest.
    private static final Map<IBinder, Integer> orientations = new ConcurrentHashMap<>();
    private static final FrameworkDisplay displays = new FrameworkDisplay();
    private static final FrameworkWindows windows = new FrameworkWindows(displays,
            new FrameworkSurfaces(new FrameworkSurfaces.Host() {
                @Override public long prepare(android.view.Surface surface, boolean opaque) {
                    return prepareSurface(surface, opaque);
                }
                @Override public void release(long window) { releaseSurface(window); }
            }));
    private static final FrameworkActivities activities =
            new FrameworkActivities(orientations, displays, packages);
    private static boolean play;
    private static final FrameworkJobs jobs = new FrameworkJobs(packages, () -> windowOpen);
    private static final FrameworkAppOps appOps = new FrameworkAppOps(packages);
    // Guest/Android/Adapters/activity_window.c: Bifrost syscall 0x100E ops 2 and 3.
    private static native int openWindow(int width, int height);
    private static native int setWindowIdentity(byte[] titleUtf8, int[] argb, int width, int height);
    private static native int pollWindow(long[] event);
    private static native int windowMetrics(long[] metrics); // pixel width, height, density dpi
    private static native int displaySize(long[] size); // host display pixel width, height
    static native int audioConfig(long[] config); // also FrameworkNativeServices.AudioFlinger
    private static native int activeAudioOutputs();

    // The launcher's JNI bridge; loading the same path again is a no-op.
    private static void loadActivityLibrary() {
        System.load("/data/local/tmp/libbifrost-activity.so");
    }
    private static native long prepareSurface(android.view.Surface surface, boolean opaque);
    private static native void releaseSurface(long window);

    private static void checkServiceRegistration() throws Exception {
        if (!(Boolean) Class.forName("android.os.SystemProperties")
                .getMethod("getBoolean", String.class, boolean.class)
                .invoke(null, "servicemanager.ready", false))
            return;
        Class<?> manager = Class.forName("android.os.ServiceManager");
        Method add = manager.getMethod("addService", String.class, IBinder.class);
        Method check = manager.getMethod("checkService", String.class);
        // Exercise real libbinder Parcel recycling and local Binder unflattening,
        // independently of the Java service cache used by the framework stubs.
        for (int i = 0; i < 8; ++i) {
            Binder binder = new Binder();
            add.invoke(null, "bifrost.registration_probe", binder);
            if (check.invoke(null, "bifrost.registration_probe") != binder)
                throw new AssertionError("registered Binder identity changed");
        }
        System.out.println("ACTIVITY_SERVICE_REGISTRATION_OK");
    }

    public static final class ProbeActivity extends Activity {
        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            created = true;
            System.out.println("ACTIVITY_CREATED");
        }
    }

    private static InputDevice virtualKeyboard;
    // The virtual keyboard EventHub always creates (id -1, classes
    // KEYBOARD|ALPHAKEY|DPAD, name "Virtual"): KeyCharacterMap.load falls
    // back to it, and Unity's render thread loads it during startup. The
    // Builder and the empty map are hidden APIs, reached by reflection like
    // the other hidden framework calls in this probe.
    private static synchronized InputDevice virtualKeyboardDevice() throws Exception {
        if (virtualKeyboard != null) return virtualKeyboard;
        Object map = call(Class.forName("android.view.KeyCharacterMap"),
                "obtainEmptyMap", new Class<?>[] {int.class},
                KeyCharacterMap.VIRTUAL_KEYBOARD);
        Object builder = Class.forName("android.view.InputDevice$Builder")
                .getConstructor().newInstance();
        call(builder, "setId", new Class<?>[] {int.class},
                KeyCharacterMap.VIRTUAL_KEYBOARD);
        call(builder, "setName", new Class<?>[] {String.class}, "Virtual");
        call(builder, "setSources", new Class<?>[] {int.class},
                InputDevice.SOURCE_KEYBOARD | InputDevice.SOURCE_DPAD);
        call(builder, "setKeyboardType", new Class<?>[] {int.class},
                InputDevice.KEYBOARD_TYPE_ALPHABETIC);
        call(builder, "setKeyCharacterMap",
                new Class<?>[] {Class.forName("android.view.KeyCharacterMap")}, map);
        virtualKeyboard = (InputDevice) call(builder, "build", new Class<?>[0]);
        return virtualKeyboard;
    }

    // Native platform Binder services (audio policy/flinger, sensors and the
    // SurfaceFlinger display-event channel): Framework/FrameworkNativeServices.java.
    private static final Binder audioPolicy = new FrameworkNativeServices.AudioPolicy();
    private static final Binder audioFlinger = new FrameworkNativeServices.AudioFlinger();
    private static final Binder sensorService = new FrameworkNativeServices.SensorService();
    private static boolean sensorServiceRegistered;
    private static final Binder surfaceComposer = new FrameworkNativeServices.SurfaceComposer();
    private static final Binder transactionComposer = new FrameworkNativeServices.TransactionComposer();

    private static ByteBuffer readDisplayEvent(ParcelFileDescriptor channel, boolean wait) throws Exception {
        long until = System.nanoTime() + 2000000000L;
        byte[] packet = new byte[216];
        do {
            try {
                if (Os.read(channel.getFileDescriptor(), packet, 0, packet.length) != packet.length)
                    throw new AssertionError("wrong display event size");
                return ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN);
            } catch (ErrnoException error) {
                if (error.errno != OsConstants.EAGAIN) throw error;
                if (!wait) return null;
            }
            Thread.sleep(2);
        } while (System.nanoTime() < until);
        throw new AssertionError("display channel timed out");
    }

    private static void checkDisplayConnection() throws Exception {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        FrameworkNativeServices.DisplayEventConnection connection = null;
        ParcelFileDescriptor receive = null, send = null;
        try {
            data.writeInterfaceToken(FrameworkNativeServices.SurfaceComposer.DESCRIPTOR);
            data.writeInt(0); data.writeInt(0); data.writeStrongBinder(null);
            if (!surfaceComposer.transact(2, data, reply, 0)) throw new AssertionError("no display connection");
            reply.readException();
            connection = (FrameworkNativeServices.DisplayEventConnection) reply.readStrongBinder();
            data.setDataSize(0); reply.setDataSize(0);
            data.setDataPosition(0); reply.setDataPosition(0);
            data.writeInterfaceToken(FrameworkNativeServices.DisplayEventConnection.DESCRIPTOR);
            connection.transact(1, data, reply, 0);
            reply.readException();
            if (reply.readInt() != 1) throw new AssertionError("null BitTube");
            receive = reply.readFileDescriptor(); send = reply.readFileDescriptor();
            ByteBuffer hotplug = readDisplayEvent(receive, true);
            if (hotplug.getInt(0) != 0x706c7567 || hotplug.getLong(8) != FrameworkNativeServices.PHYSICAL_DISPLAY_ID || hotplug.get(24) != 1)
                throw new AssertionError("missing initial hotplug");
            for (int step = 0; step < 4; step++) {
                data.setDataSize(0); reply.setDataSize(0);
                data.setDataPosition(0); reply.setDataPosition(0);
                data.writeInterfaceToken(FrameworkNativeServices.DisplayEventConnection.DESCRIPTOR);
                if (step == 0) { // query future frame timeline
                    connection.transact(4, data, reply, 0);
                    reply.readException();
                    if (reply.readInt() != 1 || reply.readLong() != FrameworkNativeServices.DISPLAY_PERIOD_NS ||
                            reply.readInt() != 0 || reply.readInt() != 1)
                        throw new AssertionError("invalid vsync timeline");
                    long id = reply.readLong(), deadline = reply.readLong(), presentation = reply.readLong();
                    if (id <= 0 || deadline >= presentation || presentation - deadline != 2000000)
                        throw new AssertionError("invalid frame deadline");
                    Thread.sleep(40);
                    if (readDisplayEvent(receive, false) != null) throw new AssertionError("unsolicited vsync");
                } else if (step == 1) {
                    connection.transact(3, data, null, IBinder.FLAG_ONEWAY);
                    ByteBuffer frame = readDisplayEvent(receive, true);
                    if (frame.getInt(0) != 0x7673796e || frame.getLong(32) != FrameworkNativeServices.DISPLAY_PERIOD_NS || frame.getInt(44) != 1)
                        throw new AssertionError("invalid one-shot vsync");
                    Thread.sleep(40);
                    if (readDisplayEvent(receive, false) != null) throw new AssertionError("one-shot repeated");
                } else {
                    data.writeInt(step == 2 ? 2 : 0);
                    connection.transact(2, data, reply, 0);
                    reply.readException();
                    if (step == 2) {
                        ByteBuffer first = readDisplayEvent(receive, true), second = readDisplayEvent(receive, true);
                        if ((first.getInt(24) & 1) != 0 || (second.getInt(24) & 1) != 0 ||
                                second.getLong(16) <= first.getLong(16))
                            throw new AssertionError("invalid periodic vsync rate");
                    } else {
                        while (readDisplayEvent(receive, false) != null) { }
                        Thread.sleep(40);
                        if (readDisplayEvent(receive, false) != null) throw new AssertionError("vsync rate zero still active");
                    }
                }
            }
        } finally {
            if (connection != null) connection.close();
            if (receive != null) receive.close();
            if (send != null) send.close();
            data.recycle(); reply.recycle();
        }
    }

    private static void checkFrameworkVsync() throws Exception {
        if (!sensorServiceRegistered) return;
        checkDisplayConnection();
        CountDownLatch done = new CountDownLatch(2);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        HandlerThread[] threads = new HandlerThread[2];
        for (int i = 0; i < threads.length; i++) {
            HandlerThread owner = new HandlerThread("FrameworkVsyncProbe" + i);
            threads[i] = owner;
            owner.start();
            new Handler(owner.getLooper()).post(() -> {
                try {
                    android.view.Choreographer choreographer = android.view.Choreographer.getInstance();
                    android.view.Choreographer.FrameCallback cancelled = time -> {
                        failure.compareAndSet(null, new AssertionError("cancelled frame callback ran"));
                    };
                    choreographer.postFrameCallback(cancelled);
                    choreographer.removeFrameCallback(cancelled);
                    choreographer.postFrameCallback(new android.view.Choreographer.FrameCallback() {
                        private int count;
                        private long previous;
                        @Override public void doFrame(long time) {
                            try {
                                if (Thread.currentThread() != owner || time <= previous || time > System.nanoTime())
                                    throw new AssertionError("wrong frame owner or timestamp");
                                previous = time;
                                if (++count < 4) choreographer.postFrameCallback(this);
                                else new Handler(owner.getLooper()).post(() -> {
                                    try {
                                        android.view.Choreographer.class.getDeclaredMethod("releaseInstance")
                                                .invoke(null);
                                    } catch (Throwable error) { failure.compareAndSet(null, error); }
                                    done.countDown();
                                });
                            } catch (Throwable error) {
                                failure.compareAndSet(null, error);
                                done.countDown();
                            }
                        }
                    });
                } catch (Throwable error) {
                    failure.compareAndSet(null, error);
                    done.countDown();
                }
            });
        }
        boolean completed;
        try { completed = done.await(20, TimeUnit.SECONDS); }
        finally { for (HandlerThread thread : threads) thread.quitSafely(); }
        if (!completed) throw new AssertionError("framework vsync timed out");
        if (failure.get() != null) throw new AssertionError("framework vsync failed", failure.get());
        System.out.println("ACTIVITY_FRAMEWORK_VSYNC_OK");
    }

    private static void checkSensorService(Context context) throws Exception {
        if (!sensorServiceRegistered) return;
        IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("checkService", String.class).invoke(null, "sensorservice");
        if (binder == null || !FrameworkNativeServices.SensorService.DESCRIPTOR.equals(binder.getInterfaceDescriptor()))
            throw new AssertionError("native sensor service was not published");
        // Check the raw native reply layout, including non-default device ids.
        for (int code : new int[] {1, 4, 7, 3, 8, 9, 2}) {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(FrameworkNativeServices.SensorService.DESCRIPTOR);
                if (code == 1 || code == 4 || code == 7) data.writeString(context.getPackageName());
                if (code == 7) data.writeInt(42);
                boolean handled = binder.transact(code, data, reply, 0);
                if (code == 2) {
                    if (handled) throw new AssertionError("unsupported sensor connection accepted");
                } else if (!handled || reply.dataSize() != 4 || reply.readInt() != 0) {
                    throw new AssertionError("invalid native sensor reply: " + code);
                }
            } finally { data.recycle(); reply.recycle(); }
        }
        for (boolean badToken : new boolean[] {true, false}) {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(badToken ? "invalid.sensor.interface" : FrameworkNativeServices.SensorService.DESCRIPTOR);
                if (!badToken) data.writeInt(123); // injection queries have no arguments
                try {
                    binder.transact(3, data, reply, 0);
                    reply.readException(); // remote Binder serializes the rejection
                    throw new AssertionError("malformed sensor query accepted");
                } catch (SecurityException | android.os.BadParcelableException expected) {
                    // Local Binder throws directly; remote Binder writes an exception.
                }
            } finally { data.recycle(); reply.recycle(); }
        }
        // This goes through SystemSensorManager JNI and the real libsensor
        // client, so a cache-only Java service cannot make this check pass.
        SensorManager sensors = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        if (sensors == null || !sensors.getSensorList(Sensor.TYPE_ALL).isEmpty()
                || !sensors.getDynamicSensorList(Sensor.TYPE_ALL).isEmpty()
                || sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null
                || sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null)
            throw new AssertionError("sensorless device reported sensors");
        System.out.println("ACTIVITY_SENSOR_SERVICE_OK");
    }

    // Unity's render thread loads the virtual keyboard's character map on
    // startup; without this service that lookup crashes the process.
    private static void checkInputService(Context context) throws Exception {
        InputManager inputs = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        if (inputs == null) throw new AssertionError("input service unavailable");
        boolean virtual = false;
        for (int id : inputs.getInputDeviceIds())
            virtual |= id == KeyCharacterMap.VIRTUAL_KEYBOARD;
        if (!virtual) throw new AssertionError("virtual keyboard missing from input devices");
        InputDevice device = inputs.getInputDevice(KeyCharacterMap.VIRTUAL_KEYBOARD);
        if (device == null || device.getKeyCharacterMap() == null)
            throw new AssertionError("virtual keyboard has no key character map");
        if (KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD) == null)
            throw new AssertionError("virtual key character map did not load");
        System.out.println("ACTIVITY_INPUT_SERVICE_OK");
    }

    // Exercise the same active-network/capabilities/callback path Unity uses.
    private static void checkGameServices(Context context) throws Exception {
        GameManager games = (GameManager) context.getSystemService(Context.GAME_SERVICE);
        if (games == null || games.getGameMode() != GameManager.GAME_MODE_STANDARD)
            throw new AssertionError("game service did not report standard mode");
        connectivity.check(context);
        System.out.println("ACTIVITY_GAME_SERVICES_OK");
    }

    private static UnsupportedOperationException unsupported(String service, String method) {
        return new UnsupportedOperationException(service + "." + method);
    }

    private static boolean activityResumed() throws Exception {
        Activity activity = activeActivity;
        return activity != null && (Boolean) call(activity, "isResumed", new Class<?>[0]);
    }

    private static void memoryState(ActivityManager.RunningAppProcessInfo info) throws Exception {
        if (info == null) throw new IllegalArgumentException("outState is null");
        boolean resumed = activityResumed();
        info.pid = Process.myPid();
        info.uid = Process.myUid();
        info.importance = resumed ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                : ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED;
        // One app process, no trim callbacks or OOM adjustment dependencies.
        info.lastTrimLevel = 0;
        info.lru = 0;
        info.importanceReasonCode = ActivityManager.RunningAppProcessInfo.REASON_UNKNOWN;
    }

    // What ProcessList.getMemoryInfo reports: totals from /proc/meminfo,
    // the home-app OOM level as the threshold, low-memory by comparison.
    private static void memoryInfo(ActivityManager.MemoryInfo info) throws Exception {
        if (info == null) throw new IllegalArgumentException("outInfo is null");
        long totalKb = 0, availKb = 0;
        java.io.BufferedReader reader =
                new java.io.BufferedReader(new java.io.FileReader("/proc/meminfo"));
        try {
            for (String line = reader.readLine(); line != null; line = reader.readLine()) {
                String[] parts = line.split("\\s+");
                if (parts.length == 3 && parts[2].equals("kB")) {
                    if (parts[0].equals("MemTotal:")) totalKb = Long.parseLong(parts[1]);
                    if (parts[0].equals("MemAvailable:")) availKb = Long.parseLong(parts[1]);
                }
            }
        } finally {
            reader.close();
        }
        if (totalKb <= 0 || availKb <= 0)
            throw new IllegalStateException("unreadable /proc/meminfo");
        info.totalMem = totalKb * 1024;
        info.availMem = availKb * 1024;
        info.threshold = info.totalMem / 16;
        info.lowMemory = info.availMem < info.threshold;
    }

    private static void appDirectories(String packageName, String path) throws Exception {
        if (installed == null || !installed.packageName.equals(packageName))
            throw new SecurityException("Unknown mkdirs package: " + packageName);
        File directory = new File(path).getCanonicalFile();
        String canonical = directory.getPath();
        boolean allowed = false;
        for (String type : new String[] {"data", "obb", "media"}) {
            String root = "/storage/emulated/0/Android/" + type + "/" + packageName;
            allowed |= canonical.equals(root) || canonical.startsWith(root + "/");
        }
        if (!allowed) throw new SecurityException("Invalid mkdirs path: " + path);
        if (!directory.isDirectory() && !directory.mkdirs() && !directory.isDirectory())
            throw new IllegalStateException("Failed to prepare " + path);
    }

    // Public AudioManager queries the app SDKs use; the probe starts no stream.
    static boolean hostAudioActive() {
        loadActivityLibrary();
        int active = activeAudioOutputs();
        if (active < 0) throw new IllegalStateException("host audio state unavailable: " + active);
        return active > 0;
    }

    private static void checkAudioService(Context context) throws Exception {
        AudioManager audio = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (audio == null || audio.getMode() != AudioManager.MODE_NORMAL || audio.isVolumeFixed()
                || audio.isMusicActive() || activeAudioOutputs() != 0)
            throw new AssertionError("audio service state differs from an idle device");
        System.out.println("ACTIVITY_AUDIO_SERVICE_OK");
    }

    private static void checkRuntimeServices(Context context, boolean resumed) throws Exception {
        checkSensorService(context);
        checkAudioService(context);
        checkInputService(context);
        inputMethod.check(context);
        checkGameServices(context);
        TelephonyManager telephony = (TelephonyManager)
                context.getSystemService(Context.TELEPHONY_SERVICE);
        // A device without a radio has no country, but its per-process
        // TelephonyServiceManager must still be initialized (as in ActivityThread.main).
        if (telephony == null || !telephony.getNetworkCountryIso().isEmpty())
            throw new AssertionError("virtual device unexpectedly reports a mobile network");
        System.out.println("ACTIVITY_TELEPHONY_SERVICE_OK");
        try (android.database.CursorWindow window = new android.database.CursorWindow("bifrost-probe")) {
            if (!window.setNumColumns(1) || !window.allocRow() ||
                    !window.putLong(42, 0, 0) || window.getLong(0, 0) != 42)
                throw new AssertionError("CursorWindow shared memory round-trip failed");
        }
        System.out.println("ACTIVITY_SHARED_MEMORY_OK");
        ActivityManager activityManager = (ActivityManager)
                context.getSystemService(Context.ACTIVITY_SERVICE);
        if (!activityManager.getHistoricalProcessExitReasons(null, 0, 0).isEmpty()
                || !activityManager.getHistoricalProcessExitReasons(
                        context.getPackageName(), Process.myPid(), 1).isEmpty())
            throw new AssertionError("launcher unexpectedly reports historical exits");
        try {
            activityManager.getHistoricalProcessExitReasons("bifrost.foreign.package", 0, 0);
            throw new AssertionError("foreign exit history query was permitted");
        } catch (SecurityException expected) {
            // Only the launcher's own package/UID is available to this caller.
        }
        System.out.println("ACTIVITY_EXIT_HISTORY_OK");
        if (!activityManager.getHistoricalProcessStartReasons(0).isEmpty())
            throw new AssertionError("launcher unexpectedly reports historical starts");
        Object historyService = call(ActivityManager.class, "getService", new Class<?>[0]);
        Method historyQuery = Class.forName("android.app.IActivityManager").getMethod(
                "getHistoricalProcessStartReasons", String.class, int.class, int.class);
        Object ownHistory = historyQuery.invoke(historyService, context.getPackageName(), 1, 0);
        if (!((List<?>) call(ownHistory, "getList", new Class<?>[0])).isEmpty())
            throw new AssertionError("launcher unexpectedly reports package start history");
        try {
            historyQuery.invoke(historyService, "bifrost.foreign.package", 1, 0);
            throw new AssertionError("foreign start history query was permitted");
        } catch (java.lang.reflect.InvocationTargetException expected) {
            if (!(expected.getCause() instanceof SecurityException)) throw expected;
            // Start history follows the same package/UID boundary as exit history.
        }
        for (int user : new int[] {-1, -2, 1}) {
            try {
                historyQuery.invoke(historyService, null, 1, user);
                throw new AssertionError("invalid/foreign start history user was permitted");
            } catch (java.lang.reflect.InvocationTargetException expected) {
                Throwable cause = expected.getCause();
                if (user < 0 ? !(cause instanceof IllegalArgumentException)
                        : !(cause instanceof SecurityException))
                    throw new AssertionError("incorrect start history user exception", cause);
            }
        }
        System.out.println("ACTIVITY_START_HISTORY_OK");
        ActivityManager.RunningAppProcessInfo info = new ActivityManager.RunningAppProcessInfo();
        info.lastTrimLevel = 99;
        info.importanceReasonCode = 99;
        ActivityManager.getMyMemoryState(info);
        int importance = resumed ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                : ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED;
        if (info.pid != Process.myPid() || info.uid != Process.myUid()
                || info.importance != importance || info.lastTrimLevel != 0
                || info.lru != 0 || info.importanceReasonCode != 0)
            throw new AssertionError("process memory state differs from launcher state");
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        ((ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(memory);
        if (memory.totalMem <= 0 || memory.availMem <= 0 || memory.lowMemory)
            throw new AssertionError("device memory info reports pressure");
        System.out.println("APP_MEMORY_OK totalMb=" + memory.totalMem / 1048576);
        PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
        if (power == null || power.isInteractive() != windowOpen || power.isPowerSaveMode()
                || power.isDeviceIdleMode() || power.getCurrentThermalStatus() != 0
                || !Float.isNaN(power.getThermalHeadroom(0)))
            throw new AssertionError("virtual-device power state differs from window state");
        int[] status = {-1};
        PowerManager.OnThermalStatusChangedListener listener = value -> status[0] = value;
        power.addThermalStatusListener(Runnable::run, listener);
        if (status[0] != 0) throw new AssertionError("thermal listener missed initial status");
        power.removeThermalStatusListener(listener);
        if (installed != null) {
            List<ActivityManager.RunningAppProcessInfo> processes = ((ActivityManager)
                    context.getSystemService(Context.ACTIVITY_SERVICE)).getRunningAppProcesses();
            String processName = installed.applicationInfo.processName != null
                    ? installed.applicationInfo.processName : installed.packageName;
            if (processes == null || processes.size() != 1)
                throw new AssertionError("launcher process missing from process list");
            ActivityManager.RunningAppProcessInfo process = processes.get(0);
            if (process.pid != Process.myPid() || process.uid != Process.myUid()
                    || process.importance != importance || !processName.equals(process.processName)
                    || !Arrays.equals(process.pkgList, new String[] {installed.packageName}))
                throw new AssertionError("process list differs from launcher state");
            System.out.println("APP_PROCESS_LIST_OK " + process.processName);
            boolean notify = context.checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                    == PackageManager.PERMISSION_GRANTED;
            if (context.getSystemService(android.app.NotificationManager.class).areNotificationsEnabled() != notify)
                throw new AssertionError("notification enablement differs from POST_NOTIFICATIONS grant");
            // Firebase measurement verifies its caller through AppOpsManager.checkPackage.
            android.app.AppOpsManager appOps = context.getSystemService(android.app.AppOpsManager.class);
            if (appOps == null) throw new AssertionError("AppOpsManager is unavailable");
            appOps.checkPackage(Process.myUid(), installed.packageName);
            for (String foreign : new String[] {"bifrost.missing.package", "root", "com.android.shell"}) {
                try {
                    appOps.checkPackage(Process.myUid(), foreign);
                    throw new AssertionError("checkPackage accepted " + foreign + " for the app uid");
                } catch (SecurityException expected) {
                    // AppOpsService returns MODE_ERRORED for packages the uid does not own.
                }
            }
            System.out.println("APP_APP_OPS_OK");
            File cache = context.getExternalCacheDir();
            if (cache == null || !cache.isDirectory())
                throw new AssertionError("external app cache was not created");
            try {
                appDirectories(installed.packageName, "/storage/emulated/0/Android/data/"
                        + installed.packageName + "/../other-package/cache");
                throw new AssertionError("mkdirs allowed a path outside the app directory");
            } catch (SecurityException expected) {
                // Canonicalization must reject traversal before filesystem mutation.
            }
        }
        System.out.println("APP_RUNTIME_SERVICES_OK resumed=" + resumed);
    }

    private static void installServices(boolean serviceChild) throws Exception {
        jobs.install(services);
        appOps.install(services);
        services.diagnostic = diagnostic;
        connectivity.install(services);
        wifi.install(services);
        inputMethod.install(services);
        // The runner exposes both credential and device protected storage
        // for user 0, so this single user is running and unlocked.
        services.register("user", "android.os.IUserManager", (method, a) -> {
            switch (method) {
                case "isUserUnlockingOrUnlocked":
                case "isUserUnlocked": return ((Integer) a[0]) == 0;
                case "getApplicationRestrictions":
                case "getApplicationRestrictionsForUser": {
                    int user = method.equals("getApplicationRestrictionsForUser") ? (Integer) a[1] : 0;
                    if (user != 0 || installed == null || !installed.packageName.equals(a[0])
                            || installed.applicationInfo.uid != Process.myUid())
                        throw new SecurityException("Application restrictions for another app/user");
                    // No system administrator provisions restrictions in this
                    // profile. AOSP returns a fresh empty Bundle without XML.
                    return new Bundle();
                }
                case "setApplicationRestrictions":
                    throw new SecurityException("Application restrictions require system/root");
                default: throw unsupported("user", method);
            }
        });
        // The runner provisions one writable emulated volume for user 0.
        // Android 15's VolumeInfo.buildStorageVolume uses the same path for
        // both app and internal access to an emulated volume.
        File storage = new File("/storage/emulated/0");
        StorageVolume volume = StorageVolume.class.getConstructor(String.class, File.class,
                File.class, String.class, boolean.class, boolean.class, boolean.class,
                boolean.class, boolean.class, long.class, UserHandle.class, UUID.class,
                String.class, String.class).newInstance("emulated;0", storage, storage,
                        "Internal shared storage", true, false, true, false, false, 0L,
                        UserHandle.class.getConstructor(int.class).newInstance(0),
                        Class.forName("android.os.storage.StorageManager")
                                .getField("UUID_DEFAULT").get(null), null, "mounted");
        services.register("mount", "android.os.storage.IStorageManager", (method, a) -> {
            if (method.equals("getVolumeList"))
                return ((Integer) a[0]) == 0 ? new StorageVolume[] {volume} : new StorageVolume[0];
            if (method.equals("mkdirs")) {
                appDirectories((String) a[0], (String) a[1]);
                return null;
            }
            throw unsupported("mount", method);
        });
        displays.install(services);
        // No physical input devices exist here, so the id list holds only
        // the virtual keyboard. Host key/touch events do not pass through
        // this service: the play loop pumps them from the window backend and
        // dispatches them to the Activity directly, bypassing ViewRootImpl's
        // input stages (which are not attached here).
        services.register("input", "android.hardware.input.IInputManager", (method, a) -> {
            switch (method) {
                // DeviceConfig holds no velocity-tracker override, as on a
                // fresh device; the client falls back to its default strategy.
                case "getVelocityTrackerStrategy": return null;
                // The device set never changes, so the listener never fires.
                case "registerInputDevicesChangedListener": return null;
                case "getInputDeviceIds":
                    return new int[] {KeyCharacterMap.VIRTUAL_KEYBOARD};
                case "getInputDevice":
                    return ((Integer) a[0]) == KeyCharacterMap.VIRTUAL_KEYBOARD
                            ? virtualKeyboardDevice() : null;
                default: throw unsupported("input", method);
            }
        });
        // The installed package is marked as a game with no mode override;
        // the platform default is standard mode. Game-state listeners are
        // kept nowhere: nothing observes here.
        services.register("game", "android.app.IGameManagerService", (method, a) -> {
            switch (method) {
                case "getGameMode": return GameManager.GAME_MODE_STANDARD;
                case "setGameState": return null;
                default: throw unsupported("game", method);
            }
        });
        packages.register(services);
        // Service children share the session's Binder registry. These are
        // device services (system_server/surfaceflinger on Android): a child
        // must not replace the session's entries, or their clients see a
        // death notification and an empty registry when the child exits.
        if (!serviceChild && (Boolean) Class.forName("android.os.SystemProperties")
                .getMethod("getBoolean", String.class, boolean.class)
                .invoke(null, "servicemanager.ready", false)) {
            // Register the VM before publishing Java-backed native services:
            // an SDK's own pthread may call them before an Activity opens.
            loadActivityLibrary();
            Class.forName("android.os.ServiceManager").getMethod(
                    "addService", String.class, IBinder.class)
                    .invoke(null, "sensorservice", sensorService);
            sensorServiceRegistered = true;
            Class.forName("android.os.ServiceManager").getMethod(
                    "addService", String.class, IBinder.class)
                    .invoke(null, "SurfaceFlingerAIDL", surfaceComposer);
            Class.forName("android.os.ServiceManager").getMethod(
                    "addService", String.class, IBinder.class)
                    .invoke(null, "SurfaceFlinger", transactionComposer);
            Method add = Class.forName("android.os.ServiceManager").getMethod(
                    "addService", String.class, IBinder.class);
            add.invoke(null, "media.audio_policy", audioPolicy);
            add.invoke(null, "media.audio_flinger", audioFlinger);
        }
        audio.install(services);
        services.register("content", "android.content.IContentService", content::call);
        services.register("window", "android.view.IWindowManager", (method, a) -> {
            switch (method) {
                case "hasNavigationBar": return false;
                case "getCurrentAnimatorScale": return 1f;
                case "getWindowInsets": return displays.windowInsets((Integer) a[0], a[2]);
                case "attachWindowContextToDisplayArea":
                    return displays.attachWindowContext(a[0], (IBinder) a[1], (Integer) a[2], (Integer) a[3]);
                case "detachWindowContext":
                    displays.detachWindowContext((IBinder) a[0]);
                    return null;
                // Dialogs and popup windows (WindowManagerGlobal.getWindowSession).
                case "openSession": return windows.openSession(services);
                default: throw unsupported("window", method);
            }
        });
        power.install(services);
        notifications.install(services);
        Map<IBinder, Object> thermalEvents = new ConcurrentHashMap<>();
        Map<IBinder, Object> thermalStatuses = new ConcurrentHashMap<>();
        // No virtual thermal HAL: no temperature/cooling samples and no
        // forecast. NONE is the initial status, not a host-temperature claim.
        services.register("thermalservice", "android.os.IThermalService", (method, a) -> {
            switch (method) {
                case "getCurrentThermalStatus": return 0;
                case "getThermalHeadroom": return Float.NaN;
                case "getThermalHeadroomThresholds": return new float[0];
                case "getCurrentTemperatures":
                case "getCurrentTemperaturesWithType":
                    return Array.newInstance(Class.forName("android.os.Temperature"), 0);
                case "getCurrentCoolingDevices":
                case "getCurrentCoolingDevicesWithType":
                    return Array.newInstance(Class.forName("android.os.CoolingDevice"), 0);
                case "registerThermalEventListener":
                case "registerThermalEventListenerWithType":
                    return thermalEvents.putIfAbsent(((IInterface) a[0]).asBinder(), a[0]) == null;
                case "unregisterThermalEventListener":
                    return thermalEvents.remove(((IInterface) a[0]).asBinder()) != null;
                case "registerThermalStatusListener": {
                    IBinder token = ((IInterface) a[0]).asBinder();
                    if (thermalStatuses.putIfAbsent(token, a[0]) != null) return false;
                    try {
                        call(a[0], "onStatusChange", new Class<?>[] {int.class}, 0);
                    } catch (Exception error) {
                        thermalStatuses.remove(token);
                        throw error;
                    }
                    return true;
                }
                case "unregisterThermalStatusListener":
                    return thermalStatuses.remove(((IInterface) a[0]).asBinder()) != null;
                default: throw unsupported("thermalservice", method);
            }
        });
        Object client = services.register("bifrost.activity_client",
                "android.app.IActivityClientController", (method, a) -> {
                    switch (method) {
                        case "setTaskDescription": return null;
                        // Record the first completion for a known Activity;
                        // unknown tokens are ignored, like ActivityRecord lookup.
                        case "reportActivityFullyDrawn":
                            if (orientations.containsKey((IBinder) a[0]))
                                fullyDrawn.putIfAbsent((IBinder) a[0], System.nanoTime());
                            return null;
                        // ActivityClientController.onBackPressed: the launcher
                        // root (a MAIN Intent started from home) moves its task to
                        // the back instead of finishing; with no home screen here
                        // it stays in front. Other Activities are asked to finish.
                        case "onBackPressed":
                            if (orientations.containsKey((IBinder) a[0]) && !activities.isRoot((IBinder) a[0]))
                                Class.forName("android.app.IRequestFinishCallback")
                                        .getMethod("requestFinish").invoke(a[1]);
                            return null;
                        // getTaskForActivity(token, onlyRoot): every Activity here
                        // belongs to the one launcher task; -1 is INVALID_TASK_ID.
                        case "getTaskForActivity":
                            if (!orientations.containsKey((IBinder) a[0])) return -1;
                            return (Boolean) a[1] && !activities.isRoot((IBinder) a[0]) ? -1 : 1;
                        // Only an Activity started for a result has a caller.
                        case "getCallingActivity":
                            return activities.callingActivity((IBinder) a[0]);
                        case "getCallingPackage": {
                            android.content.ComponentName caller = activities.callingActivity((IBinder) a[0]);
                            return caller == null ? null : caller.getPackageName();
                        }
                        case "finishActivity":
                            // Activity.finish() marks a known Activity finishing
                            // after this acknowledgement; the UI pump delivers
                            // its teardown and any result on the next turn.
                            if (!orientations.containsKey((IBinder) a[0])) return false;
                            activities.finishing((IBinder) a[0], (Integer) a[1], (Intent) a[2]);
                            return true;
                        case "getRequestedOrientation":
                            return orientations.getOrDefault((IBinder) a[0],
                                    ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
                        case "setRequestedOrientation":
                            if (orientations.containsKey((IBinder) a[0]))
                                orientations.put((IBinder) a[0], (Integer) a[1]);
                            return null;
                        default: throw unsupported("activity_client", method);
                    }
                });
        services.register("activity_task", "android.app.IActivityTaskManager", (method, a) -> {
            if (method.equals("getActivityClientController")) return client;
            // startActivityAsUser appends the user id to the same arguments.
            if (method.equals("startActivity") || method.equals("startActivityAsUser"))
                return activities.start(a);
            throw unsupported("activity_task", method);
        });
        Object provider = services.register("bifrost.settings", "android.content.IContentProvider",
                (method, a) -> {
                    if (method.equals("call") && "settings".equals(a[1])) {
                        AttributionSource caller = (AttributionSource) a[0];
                        if (caller == null || caller.getUid() != Process.myUid()
                                || (installed != null && !installed.packageName.equals(caller.getPackageName())
                                    && !"android".equals(caller.getPackageName())))
                            throw new SecurityException("Foreign settings caller");
                        return settingsCall((String) a[2], (String) a[3], (Bundle) a[4]);
                    }
                    throw unsupported("settings", method);
                });
        ProviderInfo providerInfo = new ProviderInfo();
        providerInfo.authority = "settings";
        providerInfo.name = "BifrostSettings";
        providerInfo.packageName = "android";
        providerInfo.applicationInfo = new ApplicationInfo();
        providerInfo.applicationInfo.packageName = "android";
        Class<?> holderType = Class.forName("android.app.ContentProviderHolder");
        Object holder = holderType.getConstructor(ProviderInfo.class).newInstance(providerInfo);
        holderType.getField("provider").set(holder, provider);
        holderType.getField("noReleaseNeeded").setBoolean(holder, true);
        services.register("activity", "android.app.IActivityManager", (method, a) -> {
            switch (method) {
                case "addPackageDependency":
                    if (packages.systemPackage((String) a[0]) == null)
                        throw new IllegalArgumentException("Unknown system package dependency: " + a[0]);
                    // Provisioned system packages are immutable for this session.
                    return null;
                case "getHistoricalProcessExitReasons": {
                    String packageName = (String) a[0];
                    String ownPackage = installed == null ? "android" : installed.packageName;
                    if ((Integer) a[3] != 0 || (packageName != null && !packageName.isEmpty()
                            && !ownPackage.equals(packageName)))
                        throw new SecurityException("Foreign process exit history");
                    // This launcher has no persistent exit-info tracker. The
                    // current live process is not a historical exit record.
                    return Class.forName("android.content.pm.ParceledListSlice")
                            .getConstructor(List.class).newInstance(java.util.Collections.emptyList());
                }
                case "getHistoricalProcessStartReasons": {
                    String packageName = (String) a[0];
                    int userId = (Integer) a[2];
                    if (userId == -1 || userId == -2)
                        throw new IllegalArgumentException("Unsupported userId");
                    String ownPackage = installed == null ? "android" : installed.packageName;
                    if (userId != 0 || (packageName != null && !packageName.isEmpty()
                            && !ownPackage.equals(packageName)))
                        throw new SecurityException("Foreign process start history");
                    // No start-info tracker is provisioned. Do not invent launch
                    // reasons or timestamps for the current or earlier processes.
                    return Class.forName("android.content.pm.ParceledListSlice")
                            .getConstructor(List.class).newInstance(java.util.Collections.emptyList());
                }
                case "startService": return serviceRuntime.start(a);
                case "stopService": return serviceRuntime.stop(a);
                case "stopServiceToken": return serviceRuntime.stopSelf(
                        (android.content.ComponentName) a[0], (IBinder) a[1], (Integer) a[2]);
                case "bindService": return serviceRuntime.bind(a, false);
                case "bindServiceInstance": return serviceRuntime.bind(a, true);
                case "unbindService": return serviceRuntime.unbind(a[0]);
                case "peekService": return serviceRuntime.peek((Intent) a[0], (String) a[2]);
                case "publishService": serviceRuntime.publish((IBinder) a[0], (Intent) a[1], (IBinder) a[2]); return null;
                case "unbindFinished": serviceRuntime.unbindFinished((IBinder) a[0], (Intent) a[1], (Boolean) a[2]); return null;
                case "serviceDoneExecuting": serviceRuntime.done((IBinder) a[0], (Integer) a[1], (Intent) a[4]); return null;
            }
            if (method.equals("checkPermissionForDevice"))
                return packages.checkPermission((String) a[0], (Integer) a[2], (Integer) a[3]);
            if (method.equals("checkPermission"))
                return packages.checkPermission((String) a[0], (Integer) a[2], Context.DEVICE_ID_DEFAULT);
            if (method.equals("getContentProvider") && Arrays.asList(a).contains("settings"))
                return holder;
            // AOSP ContentProviderHelper returns no holder when no package
            // declares the authority (e.g. Play services providers that
            // Firebase probes); ActivityThread then logs "Failed to find
            // provider info" and ContentResolver returns null.
            if (method.equals("getContentProvider") && !packages.declaresProvider((String) a[2])) {
                System.out.println("APP_PROVIDER_MISSING " + a[2]);
                return null;
            }
            // Log.wtf report; false means "do not kill this process".
            if (method.equals("handleApplicationWtf")) return false;
            if (method.equals("handleApplicationCrash")) {
                // Record the supplied report, then let RuntimeInit perform
                // its normal fatal-process termination.
                Object crash = a[1];
                for (String field : new String[] {"exceptionClassName", "exceptionMessage", "stackTrace"})
                    System.err.println("APP_CRASH " + field + "="
                            + crash.getClass().getField(field).get(crash));
                return null;
            }
            if (method.equals("getMyMemoryState")) {
                memoryState((ActivityManager.RunningAppProcessInfo) a[0]);
                return null;
            }
            if (method.equals("getRunningAppProcesses")) {
                if (installed == null) return null;
                String name = installed.applicationInfo.processName != null
                        ? installed.applicationInfo.processName : installed.packageName;
                ActivityManager.RunningAppProcessInfo process =
                        new ActivityManager.RunningAppProcessInfo(name, Process.myPid(),
                                new String[] {installed.packageName});
                memoryState(process);
                return Arrays.asList(process);
            }
            if (method.equals("getMemoryInfo")) {
                memoryInfo((ActivityManager.MemoryInfo) a[0]);
                return null;
            }
            // Nothing broadcasts here: no sticky Intent matches a new receiver.
            if (method.equals("registerReceiverWithFeature")) return null;
            if (method.equals("unregisterReceiver")) return null;
            // Scheduling hint for HardwareRenderer's thread; no priority policy here.
            if (method.equals("setRenderThread")) return null;
            throw unsupported("activity", method);
        });
    }

    private static Object call(Object target, String name, Class<?>[] types, Object... arguments)
            throws Exception {
        Class<?> type = target instanceof Class ? (Class<?>) target : target.getClass();
        Method method = null;
        for (Class<?> c = type; method == null && c != null; c = c.getSuperclass()) {
            try {
                method = c.getDeclaredMethod(name, types);
            } catch (NoSuchMethodException next) {
                // Keep searching superclasses.
            }
        }
        if (method == null) throw new NoSuchMethodException(type.getName() + "." + name);
        method.setAccessible(true);
        return method.invoke(target instanceof Class ? null : target, arguments);
    }

    static void setField(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    // Android 15 SettingsProvider.generateSsaidLocked: HMAC the length-prefixed
    // signing certificates with a device/user key, then keep the first 64 bits.
    // Package names do not participate: apps signed alike share the scoped ID.
    private static String scopedAndroidId(byte[] key, Signature[] signatures) throws Exception {
        if (key.length != 32 || signatures == null || signatures.length == 0)
            throw new IllegalStateException("Android ID requires a user key and APK signatures");
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, mac.getAlgorithm()));
        for (Signature signature : signatures) {
            byte[] certificate = signature.toByteArray();
            mac.update(ByteBuffer.allocate(4).putInt(certificate.length).array());
            mac.update(certificate);
        }
        byte[] digest = mac.doFinal();
        char[] hex = "0123456789abcdef".toCharArray();
        StringBuilder result = new StringBuilder(16);
        for (int i = 0; i < 8; i++)
            result.append(hex[(digest[i] & 255) >>> 4]).append(hex[digest[i] & 15]);
        return result.toString();
    }

    private static byte[] settingsKey(int user) throws IOException {
        byte[] key = new byte[32];
        try (DataInputStream input = new DataInputStream(new FileInputStream(
                "/data/system/users/" + user + "/bifrost-ssaid-key"))) {
            input.readFully(key);
            if (input.read() != -1) throw new IOException("Invalid settings user key");
        }
        return key;
    }

    private static Bundle settingsCall(String method, String name, Bundle extras) {
        // This profile provides user 0 and immutable settings. Missing values
        // remain absent, so the real Settings client applies its own defaults.
        int user = extras == null ? 0 : extras.getInt("_user", 0);
        if (user != 0 && user != -2 && user != -3)
            throw new SecurityException("Settings for another user are unavailable");
        // Settings.Config lists a namespace using a null setting name and
        // extras._prefix. This immutable profile has no DeviceConfig overrides.
        // Match SettingsProvider's serialized map response, allowing clients
        // such as AutofillFeatureFlags to apply their platform defaults.
        if (method.equals("LIST_config")) {
            Bundle result = new Bundle();
            result.putSerializable("value", new java.util.HashMap<String, String>());
            return result;
        }
        if (name == null) throw new IllegalArgumentException("Missing setting name");
        switch (method) {
            case "GET_secure":
            case "GET_system":
            case "GET_global":
                Bundle result = new Bundle();
                result.putString("value", method.equals("GET_secure")
                        && name.equals(Settings.Secure.ANDROID_ID) ? androidId : null);
                return result;
            case "PUT_secure":
            case "PUT_global":
            case "PUT_system":
                throw new SecurityException("Settings writes are not granted by this profile");
            default: throw unsupported("settings", method);
        }
    }

    private static void checkSettings(Context context) throws Exception {
        String id = Settings.Secure.getString(context.getContentResolver(), Settings.Secure.ANDROID_ID);
        if (id == null || !id.matches("[0-9a-f]{16}") || !id.equals(androidId)
                || !id.equals(Settings.Secure.getString(context.getContentResolver(), Settings.Secure.ANDROID_ID)))
            throw new AssertionError("Scoped Android ID unavailable through Settings.Secure");
        for (String namespace : new String[] {"GET_secure", "GET_system", "GET_global"}) {
            if (settingsCall(namespace, "bifrost.missing", null).getString("value") != null)
                throw new AssertionError("Missing settings must stay absent");
        }
        if (settingsCall("GET_global", Settings.Secure.ANDROID_ID, null).getString("value") != null
                || settingsCall("GET_system", Settings.Secure.ANDROID_ID, null).getString("value") != null
                || Settings.Global.getInt(context.getContentResolver(), "bifrost.missing", 37) != 37)
            throw new AssertionError("Settings namespace/default mismatch");
        Bundle other = new Bundle();
        other.putInt("_user", 1);
        try {
            settingsCall("GET_secure", Settings.Secure.ANDROID_ID, other);
            throw new AssertionError("Foreign settings user accepted");
        } catch (SecurityException expected) {}
        try {
            Settings.Secure.putString(context.getContentResolver(), Settings.Secure.ANDROID_ID, "changed");
            throw new AssertionError("Android ID write accepted");
        } catch (SecurityException expected) {}
        byte[] key = new byte[32];
        for (int i = 0; i < key.length; i++) key[i] = (byte) i;
        Signature[] first = {new Signature(new byte[] {1, 2, 3})};
        // Independent Python HMAC oracle, using AOSP's big-endian length prefix.
        if (!scopedAndroidId(key, first).equals("ea90c345cb4ad8ff"))
            throw new AssertionError("SSAID derivation differs from AOSP");
        if (scopedAndroidId(key, first).equals(scopedAndroidId(key,
                new Signature[] {new Signature(new byte[] {1, 2, 4})})))
            throw new AssertionError("Signing certificate did not scope Android ID");
        key[0] ^= 1;
        if (scopedAndroidId(key, first).equals("ea90c345cb4ad8ff"))
            throw new AssertionError("Device/user key did not scope Android ID");
        System.out.println("APP_SETTINGS_OK");
    }

    // args: --app <package> <code dir> <launcher class> [<split>=<apk>]...
    private static void bindApplicationMetadata(Object thread, Context system, ApplicationInfo app,
                                                Object loaded) throws Exception {
        // handleBindApplication installs this before creating the Application.
        // setArgV0 alone names /proc but leaves Application.getProcessName()
        // null; Chromium records that Java name in its data-directory lock.
        Class<?> bindType = Class.forName("android.app.ActivityThread$AppBindData");
        java.lang.reflect.Constructor<?> constructor = bindType.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object binding = constructor.newInstance();
        String processName = app.processName != null ? app.processName : app.packageName;
        setField(binding, "processName", processName);
        setField(binding, "appInfo", app);
        setField(binding, "info", loaded);
        setField(binding, "config", new Configuration(system.getResources().getConfiguration()));
        setField(binding, "compatInfo", Class.forName("android.content.res.CompatibilityInfo")
                .getField("DEFAULT_COMPATIBILITY_INFO").get(null));
        setField(thread, "mBoundApplication", binding);
        displays.attachApplicationThread(thread);
        call(Process.class, "setArgV0", new Class<?>[] {String.class}, processName);
        if (!processName.equals(Application.getProcessName()))
            throw new AssertionError("Java process identity differs from bound application");
    }

    private static void checkApplicationRestrictions(Context context) {
        android.os.UserManager manager = context.getSystemService(android.os.UserManager.class);
        Bundle restrictions = manager.getApplicationRestrictions(context.getPackageName());
        if (!restrictions.isEmpty()) throw new AssertionError("unexpected application restrictions");
        restrictions.putString("local", "mutation");
        if (!manager.getApplicationRestrictions(context.getPackageName()).isEmpty())
            throw new AssertionError("application restrictions leaked caller mutation");
        try {
            manager.getApplicationRestrictions("org.bifrost.foreign");
            throw new AssertionError("foreign application restrictions allowed");
        } catch (SecurityException expected) {}
        System.out.println("ACTIVITY_APPLICATION_RESTRICTIONS_OK");
    }

    private static void runApp(Object thread, Context system, String[] args) throws Exception {
        String packageName = args[1], code = args[2], launcher = args[3];
        PackageInfo packageInfo = system.getPackageManager().getPackageArchiveInfo(
                code + "/base.apk", PackageManager.GET_ACTIVITIES | PackageManager.GET_RECEIVERS
                        | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS | PackageManager.GET_META_DATA
                        | PackageManager.GET_CONFIGURATIONS | PackageManager.GET_INSTRUMENTATION
                        | PackageManager.GET_URI_PERMISSION_PATTERNS
                        | PackageManager.GET_PERMISSIONS
                        | PackageManager.GET_SIGNATURES | PackageManager.GET_SIGNING_CERTIFICATES
                        | PackageManager.MATCH_DISABLED_COMPONENTS
                        | PackageManager.MATCH_DIRECT_BOOT_AWARE | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        if (packageInfo == null || !packageName.equals(packageInfo.packageName))
            throw new IllegalStateException("Cannot parse " + code + "/base.apk");
        ApplicationInfo app = packageInfo.applicationInfo;
        ActivityInfo activity = null;
        for (ActivityInfo candidate : packageInfo.activities)
            if (candidate.name.equals(launcher)) activity = candidate;
        if (activity == null) throw new IllegalStateException("No activity " + launcher);
        String[] splitNames = new String[args.length - 4];
        String[] splitPaths = new String[args.length - 4];
        for (int i = 4; i < args.length; i++) {
            String[] split = args[i].split("=", 2);
            splitNames[i - 4] = split[0];
            splitPaths[i - 4] = code + "/" + split[1];
        }
        // What PackageManagerService records for an installed arm64 package.
        app.sourceDir = app.publicSourceDir = code + "/base.apk";
        app.splitNames = splitNames;
        app.splitSourceDirs = app.splitPublicSourceDirs = splitPaths;
        // The installed PackageInfo lists the same sorted splits; Play Asset
        // Delivery finds install-time asset packs through it.
        packageInfo.splitNames = splitNames;
        packageInfo.splitRevisionCodes = new int[splitNames.length];
        app.nativeLibraryDir = code + "/lib/arm64";
        setField(app, "nativeLibraryRootDir", code + "/lib");
        setField(app, "primaryCpuAbi", "arm64-v8a");
        app.dataDir = "/data/user/0/" + packageName;
        setField(app, "credentialProtectedDataDir", app.dataDir);
        setField(app, "deviceProtectedDataDir", "/data/user_de/0/" + packageName);
        app.uid = Process.myUid();
        // The installed ApplicationInfo marks a base APK with classes.dex as
        // code; the archive-parsed one here does not, and LoadedApk then builds
        // a class loader without the APK's dex files.
        app.flags |= ApplicationInfo.FLAG_HAS_CODE;
        packages.install(system, packageInfo);
        installed = packageInfo;
        androidId = scopedAndroidId(settingsKey(app.uid / 100000), packageInfo.signatures);
        if (diagnostic) {
            packages.checkPackageServices(system);
            checkSettings(system);
        }
        System.out.println("APP_PARSED " + app.className + " " + activity.name);
        int width = 0, height = 0;
        if (play) {
            boolean landscape = Arrays.asList(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
                    ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE,
                    ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE)
                    .contains(activity.screenOrientation);
            // The window opens at 1280x720 desktop units; on a scaled host
            // display its native buffer has more pixels at a higher density.
            loadActivityLibrary();
            int error = openWindow(landscape ? 1280 : 720, landscape ? 720 : 1280);
            if (error != 0) throw new IllegalStateException("host window: " + error);
            long[] metrics = new long[3];
            error = windowMetrics(metrics);
            if (error != 0) throw new IllegalStateException("host window metrics: " + error);
            width = (int) metrics[0];
            height = (int) metrics[1];
            displays.setMetrics(width, height, (int) metrics[2]);
            long[] display = new long[2];
            error = displaySize(display);
            if (error != 0) throw new IllegalStateException("host display size: " + error);
            displays.setMaxBounds((int) display[0], (int) display[1]);
            // What ActivityThread.handleConfigurationChanged applies: the
            // process and system resources use the display's density before
            // the Application and Activity contexts are created.
            Object resources = Class.forName("android.app.ResourcesManager").getMethod("getInstance").invoke(null);
            call(resources, "applyConfigurationToResources",
                    new Class<?>[] {Configuration.class, Class.forName("android.content.res.CompatibilityInfo")},
                    displays.processConfiguration(), null);
            windowOpen = true;
            System.out.println("APP_WINDOW_OPEN " + width + "x" + height + " density=" + metrics[2]
                    + " max=" + display[0] + "x" + display[1]);
        }

        Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
        Object loaded = call(thread, "getPackageInfoNoCheck",
                new Class<?>[] {ApplicationInfo.class, compatibility}, app,
                compatibility.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
        ClassLoader loader = (ClassLoader) call(loaded, "getClassLoader", new Class<?>[0]);
        Class<?> launcherClass = loader.loadClass(launcher);
        System.out.println("APP_CLASSLOADER_OK " + launcherClass.getName());

        bindApplicationMetadata(thread, system, app, loaded);
        Instrumentation instrumentation =
                (Instrumentation) call(thread, "getInstrumentation", new Class<?>[0]);
        Application application = (Application) call(loaded, "makeApplicationInner",
                new Class<?>[] {boolean.class, Instrumentation.class}, false, null);
        setField(thread, "mInitialApplication", application);
        if (diagnostic) checkApplicationRestrictions(application);
        System.out.println("APP_APPLICATION_CREATED " + application.getClass().getName());
        if (play) {
            PackageManager pm = application.getPackageManager();
            String label = activity.loadLabel(pm).toString();
            android.graphics.drawable.Drawable icon = pm.getDrawable(packageName, activity.getIconResource(), app);
            if (icon == null) icon = pm.getDefaultActivityIcon();
            android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(
                    128, 128, android.graphics.Bitmap.Config.ARGB_8888);
            icon.setBounds(0, 0, 128, 128);
            icon.draw(new android.graphics.Canvas(bitmap));
            int[] pixels = new int[128 * 128];
            bitmap.getPixels(pixels, 0, 128, 0, 0, 128, 128);
            byte[] titleUtf8 = label.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            if (titleUtf8.length >= 1024)
                titleUtf8 = packageName.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            int result = setWindowIdentity(titleUtf8, pixels, 128, 128);
            bitmap.recycle();
            if (result != 0 && result != -95) // Unsupported compositor/backend icon API.
                throw new IllegalStateException("window identity: " + result);
            System.out.println("APP_WINDOW_IDENTITY " + label + " icon=" + (result == 0));
        }
        // Exercise the framework path Unity uses, including volume lookup
        // and creation of the installed package's external files directory.
        File external = application.getExternalFilesDir(null);
        File expected = new File("/storage/emulated/0/Android/data/" + packageName + "/files");
        if (!expected.equals(external) || !external.isDirectory() || !external.canWrite())
            throw new IllegalStateException("external files directory unavailable: " + external);
        System.out.println("APP_STORAGE_READY " + external);
        // SharedPreferences goes through UserManager's credential-storage
        // check on Android 15. Verify the real framework persistence path.
        if (diagnostic) {
        android.content.SharedPreferences preferences = application.getSharedPreferences(
                "bifrost_probe", Context.MODE_PRIVATE);
        if (!preferences.edit().putString("storage", "ready").commit() ||
                !"ready".equals(preferences.getString("storage", null)))
            throw new IllegalStateException("SharedPreferences storage unavailable");
        System.out.println("APP_PREFERENCES_READY");
        }
        if (play) {
            android.media.AudioManager audio = (android.media.AudioManager)
                    application.getSystemService(Context.AUDIO_SERVICE);
            if (audio.getDevices(android.media.AudioManager.GET_DEVICES_ALL).length != 0)
                throw new AssertionError("unexpected Android HAL audio ports");
            long[] config = new long[3];
            if (audioConfig(config) != 0 ||
                    Integer.parseInt(audio.getProperty(android.media.AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)) != config[0] ||
                    Integer.parseInt(audio.getProperty(android.media.AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)) != config[1])
                throw new AssertionError("AudioFlinger output differs from host backend");
            System.out.println("APP_AUDIO_ROUTING_READY " + config[0] + " " + config[1]);
        }
        serviceRuntime.attach(thread, app);
        jobs.attach(application);
        instrumentation.callApplicationOnCreate(application);
        System.out.println("APP_APPLICATION_STARTED");

        Binder token = new Binder();
        orientations.put(token, activity.screenOrientation);
        Context activityContext = (Context) call(Class.forName("android.app.ContextImpl"),
                "createActivityContext", new Class<?>[] {thread.getClass(), loaded.getClass(),
                        ActivityInfo.class, IBinder.class, int.class, Configuration.class},
                thread, loaded, activity, token, 0, displays.activityOverride());
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(packageName, launcher);
        Activity instance = instrumentation.newActivity(launcherClass, activityContext, token,
                application, intent, activity, activity.loadLabel(system.getPackageManager()),
                null, null, null);
        FrameworkActivities.attachThread(instance, thread);
        activeActivity = instance;
        call(activityContext, "setOuterContext", new Class<?>[] {Context.class}, instance);
        if (activity.getThemeResource() != 0) instance.setTheme(activity.getThemeResource());
        if (instance.getRequestedOrientation() != activity.screenOrientation)
            throw new IllegalStateException("Activity orientation differs from its manifest");
        System.out.println("APP_ACTIVITY_ATTACHED");
        instrumentation.callActivityOnCreate(instance, null);
        System.out.println("APP_ACTIVITY_CREATED");
        System.out.println("FRAMEWORK_OK");
        if (play) {
            activities.attach(thread, loaded, instrumentation, application, instance, token);
            play(instrumentation, instance, width, height);
            return;
        }
        System.exit(0);
    }

    // Converts one op-3 event record (activity_window.c) into a framework
    // input event and dispatches it like ViewRootImpl's input stages.
    // Popup windows (FrameworkWindows) above the Activity receive input first.
    private static void dispatch(Activity activity, long[] e) throws Exception {
        final long ms = 1000000;
        if (e[0] == 1) {  // AINPUT_EVENT_TYPE_KEY
            KeyEvent event = new KeyEvent(e[11] / ms, e[12] / ms, (int) e[4],
                    (int) e[8], (int) e[10], (int) e[7], (int) e[6], (int) e[9],
                    (int) e[19], (int) e[5]);
            if (!windows.dispatchKey(event)) activity.dispatchKeyEvent(event);
        } else if (e[0] == 2) {  // AINPUT_EVENT_TYPE_MOTION, first pointer only
            MotionEvent event = MotionEvent.obtain(e[11] / ms, e[12] / ms, (int) e[4],
                    Float.intBitsToFloat((int) e[14]), Float.intBitsToFloat((int) e[15]),
                    Float.intBitsToFloat((int) e[16]), Float.intBitsToFloat((int) e[17]),
                    0, 1f, 1f, (int) e[6], 0);
            event.setSource((int) e[5]);
            if ((e[5] & InputDevice.SOURCE_CLASS_POINTER) != 0) {
                if (!windows.dispatchPointer(event)) activity.dispatchTouchEvent(event);
            } else {
                activity.dispatchGenericMotionEvent(event);
            }
            event.recycle();
        }
    }

    private static void closeActivity(Activity activity) throws Exception {
        windowOpen = false;
        activity.onWindowFocusChanged(false);
        call(activity, "performPause", new Class<?>[0]);
        System.out.println("APP_PAUSED");
        windows.stop();
        call(activity, "performStop", new Class<?>[] {boolean.class, String.class}, false, "bifrost");
        System.out.println("APP_STOPPED");
        call(Class.forName("android.app.QueuedWork"), "waitToFinish", new Class<?>[0]);
        call(activity, "performDestroy", new Class<?>[0]);
        FrameworkActivities.removeWindow(activity);
        call(activity.getBaseContext(), "scheduleFinalCleanup", new Class<?>[] {String.class, String.class},
                activity.getClass().getName(), "Activity");
        System.out.println("APP_DESTROYED");
        System.out.println("APP_WINDOW_CLOSED");
    }

    private static void play(Instrumentation instrumentation, Activity activity, int width,
            int height) throws Exception {
        // ActivityThread.handleStartActivity, then handleResumeActivity.
        call(activity, "performStart", new Class<?>[] {String.class}, "bifrost");
        instrumentation.callActivityOnPostCreate(activity, null);
        call(activity, "performResume", new Class<?>[] {boolean.class, String.class},
                false, "bifrost");
        if (diagnostic) checkRuntimeServices(activity, true);
        System.out.println("APP_RESUMED");
        // The decor becomes a window like any other Activity's; ViewRootImpl
        // lays it out and FrameworkWindows draws it and its SurfaceViews.
        FrameworkActivities.show(activity);
        System.out.println("APP_WINDOW_ADDED");
        Handler handler = new Handler(Looper.getMainLooper());
        long[] event = new long[20];
        int[] size = {width, height};
        handler.post(new Runnable() {
            @Override
            public void run() {
                try {
                    for (int i = 0; i < 64; i++) {
                        int error = pollWindow(event);
                        if (error != 0) throw new IllegalStateException("window poll: " + error);
                        if (event[1] != 0 || activity.isFinishing()) {
                            boolean hostClosed = event[1] != 0;
                            // Apps may kill their own process during teardown
                            // (Process.killProcess); the session reports this
                            // marker plus SIGKILL as a user close.
                            if (hostClosed) System.out.println("APP_HOST_CLOSED");
                            activities.closeAll();
                            closeActivity(activity);
                            jobs.close();
                            if (hostClosed) serviceRuntime.shutdown(() -> System.exit(0));
                            else serviceRuntime.whenIdle(() -> System.exit(0));
                            return;
                        }
                        if (event[2] != size[0] || event[3] != size[1]) {
                            size[0] = (int) event[2];
                            size[1] = (int) event[3];
                            displays.setSize(size[0], size[1]);
                            // ActivityThread applies a WMS bounds change this way.
                            Object resources = Class.forName("android.app.ResourcesManager")
                                    .getMethod("getInstance").invoke(null);
                            call(resources, "updateResourcesForActivity",
                                    new Class<?>[] {IBinder.class, Configuration.class, int.class},
                                    call(activity, "getActivityToken", new Class<?>[0]),
                                    displays.activityOverride(), 0);
                            // ViewConfiguration caches 4 * maxBounds area as the
                            // software drawing-cache limit, per density, forever
                            // (a phone display never grows). Rebuild it from the
                            // new bounds, or full-window software layers such as
                            // ad WebViews stop drawing after the window grows.
                            Class.forName("android.view.ViewConfiguration")
                                    .getMethod("resetCacheForTesting").invoke(null);
                            windows.displayChanged();
                        }
                        if (event[0] == 0) break;
                        dispatch(activity, event);
                    }
                    activities.update();
                    windows.render();
                } catch (Exception error) {
                    error.printStackTrace();
                    System.exit(1);
                }
                handler.postDelayed(this, 4);
            }
        });
        Looper.loop();
    }

    private static void run(String[] args) throws Exception {
        boolean inventory = args.length == 1 && args[0].equals("--service-inventory");
        if (inventory) args = new String[0];
        boolean nativeChoreographer = diagnostic && args.length == 1
                && args[0].equals("--native-choreographer");
        if (nativeChoreographer) args = new String[0];
        boolean gralloc = diagnostic && (args.length == 1 || args.length == 2) && args[0].equals("--gralloc");
        int grallocDelay = gralloc && args.length == 2 ? Integer.parseInt(args[1]) : 0;
        if (grallocDelay < 0 || grallocDelay > 30) throw new IllegalArgumentException("Gralloc repeat delay");
        if (gralloc) args = new String[0];
        boolean probeRenderer = diagnostic && args.length == 1 && args[0].equals("--webview-renderer");
        boolean probeWebView = probeRenderer || (diagnostic && args.length == 1 && args[0].equals("--webview"));
        if (probeWebView) args = new String[0];
        if (args.length > 0 && args[0].equals("--play")) {
            play = true;
            args = Arrays.copyOfRange(args, 1, args.length);
        }
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        // Match ActivityThread.main before any SDK can access mainline APIs.
        // systemMain() alone does not initialize their per-process service managers.
        threadType.getMethod("initializeMainlineModules").invoke(null);
        Looper.prepareMainLooper();
        boolean serviceChild = args.length == 3 && args[0].equals("--service-child");
        if (diagnostic) checkServiceRegistration();
        installServices(serviceChild || inventory);
        if (gralloc) {
            ActivityProbe.GrallocProbe.checkDeclarations();
            int line = ActivityProbe.GrallocProbe.check();
            if (line != 0) throw new AssertionError("Gralloc contract failed at gralloc_probe.c:" + line);
            if (grallocDelay != 0) {
                System.out.println("ANDROID_GRALLOC_CLIENT_READY");
                Thread.sleep(grallocDelay * 1000L);
                line = ActivityProbe.GrallocProbe.check();
                if (line != 0) throw new AssertionError("Gralloc after peer exit failed at gralloc_probe.c:" + line);
                System.out.println("ANDROID_GRALLOC_PEER_EXIT_OK");
            }
            System.out.println("ANDROID_GRALLOC_OK");
            System.out.println("FRAMEWORK_OK");
            System.exit(0);
            return;
        }
        if (inventory) {
            System.out.println(services.inventory());
            System.out.println("FRAMEWORK_OK");
            System.exit(0);
            return;
        }
        if (nativeChoreographer) {
            loadActivityLibrary();
            ActivityProbe.NativeLooperProbe.checkNativeFrames();
            System.out.println("FRAMEWORK_OK");
            System.exit(0);
            return;
        }
        if (diagnostic && !play) checkFrameworkVsync();
        System.out.println("ACTIVITY_BOOTSTRAP_BEGIN");
        Object thread = threadType.getMethod("systemMain").invoke(null);
        Context context = (Context) threadType.getMethod("getSystemContext").invoke(thread);
        System.out.println("ACTIVITY_CONTEXT_OK " + context);
        FrameworkProcessRuntime.Remote parent = serviceChild
                ? new FrameworkProcessRuntime.Remote(args[1], args[2]) : null;
        PackageInfo childPackage = parent == null ? null : parent.bootstrap();
        // The parent already parsed and validated the immutable provider APK.
        // Reuse its parcel snapshot so cold child startup fits Chromium's bind deadline.
        if (parent == null) installWebView(context);
        else if (parent.webViewProvider != null) installWebView(context, parent.webViewProvider);
        if (diagnostic) checkRuntimeServices(context, false);
        // ActivityThread.handleBindApplication's fallback when no serialized
        // font map arrives (lazy Typeface initialization). This probe has
        // no bindApplication, and a TextView needs the default typefaces.
        Class.forName("android.graphics.Typeface")
                .getMethod("loadPreinstalledSystemFontMap").invoke(null);
        System.out.println("ACTIVITY_FONTS_OK");
        if (serviceChild) {
            PackageInfo pkg = childPackage;
            ApplicationInfo app = pkg.applicationInfo;
            new File(app.dataDir).mkdirs();
            packages.install(context, pkg); installed = pkg;
            Class<?> compat = Class.forName("android.content.res.CompatibilityInfo");
            Object loaded = call(thread, "getPackageInfoNoCheck", new Class<?>[] {ApplicationInfo.class, compat},
                    app, compat.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
            bindApplicationMetadata(thread, context, app, loaded);
            Application application = (Application) call(loaded, "makeApplicationInner",
                    new Class<?>[] {boolean.class, Instrumentation.class}, false, null);
            setField(thread, "mInitialApplication", application);
            serviceRuntime.attach(thread, app); serviceRuntime.remote(parent);
            jobs.attach(application);
            parent.ready(call(thread, "getApplicationThread", new Class<?>[0]));
            Looper.loop();
            return;
        }
        if (args.length >= 4 && args[0].equals("--app")) {
            runApp(thread, context, args);
            return;
        }
        if (args.length != 0) throw new IllegalArgumentException("unexpected arguments");

        if (probeWebView) {
            // The system context uses Resources.getSystem(), which is outside
            // ResourcesManager's app-resource tracking. WebView appends its
            // shared assets to an application's tracked Resources instead.
            // Give this resource-only probe the same LoadedApk context path as
            // runApp, using the image's existing framework resources.
            ApplicationInfo probeInfo = new ApplicationInfo(context.getApplicationInfo());
            probeInfo.packageName = "org.bifrost.activity";
            probeInfo.processName = probeInfo.packageName;
            probeInfo.uid = Process.myUid();
            probeInfo.sourceDir = probeInfo.publicSourceDir = "/system/framework/framework-res.apk";
            probeInfo.nativeLibraryDir = "/data/local/tmp";
            probeInfo.dataDir = "/data/user/0/" + probeInfo.packageName;
            if (!new File(probeInfo.dataDir).mkdirs())
                throw new IllegalStateException("Cannot create probe data directory");
            setField(probeInfo, "credentialProtectedDataDir", probeInfo.dataDir);
            setField(probeInfo, "deviceProtectedDataDir", "/data/user_de/0/" + probeInfo.packageName);
            probeInfo.flags &= ~(ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_HAS_CODE);
            PackageInfo probePackage = new PackageInfo();
            probePackage.packageName = probeInfo.packageName;
            probePackage.applicationInfo = probeInfo;
            packages.install(context, probePackage);
            installed = probePackage;
            Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
            Object loaded = call(thread, "getPackageInfoNoCheck",
                    new Class<?>[] {ApplicationInfo.class, compatibility}, probeInfo,
                    compatibility.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
            bindApplicationMetadata(thread, context, probeInfo, loaded);
            Application application = (Application) call(loaded, "makeApplicationInner",
                    new Class<?>[] {boolean.class, Instrumentation.class}, false, null);
            setField(thread, "mInitialApplication", application);
            serviceRuntime.attach(thread, probeInfo);
            jobs.attach(application);
            context = application;
            checkApplicationRestrictions(context);
            android.view.Display display = context.getSystemService(android.hardware.display.DisplayManager.class)
                    .getDisplay(0);
            Context windowContext = context.createWindowContext(display,
                    android.view.WindowManager.LayoutParams.TYPE_APPLICATION, null);
            Configuration windowConfig = windowContext.getResources().getConfiguration();
            if (windowContext.getDisplay().getDisplayId() != 0 || windowConfig.densityDpi != 160
                    || windowConfig.screenWidthDp != display.getWidth()
                    || windowConfig.screenHeightDp != display.getHeight())
                throw new AssertionError("window context display/configuration mismatch");
            windowContext.getClass().getMethod("release").invoke(windowContext);
            System.out.println("ACTIVITY_WINDOW_CONTEXT_OK");
        }

        ActivityInfo info = new ActivityInfo();
        info.applicationInfo = context.getApplicationInfo();
        info.packageName = "org.bifrost.activity";
        info.name = ProbeActivity.class.getName();
        Intent intent = new Intent().setClassName(info.packageName, info.name);
        Instrumentation instrumentation = new Instrumentation();
        Binder token = new Binder();
        orientations.put(token, info.screenOrientation);
        Activity activity = instrumentation.newActivity(ProbeActivity.class, context,
                token, new Application(), intent, info, "Bifrost Activity",
                null, null, null);
        activeActivity = activity;
        if (activity.getRequestedOrientation() != info.screenOrientation)
            throw new IllegalStateException("initial Activity orientation differs from its manifest");
        activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        if (activity.getRequestedOrientation() != ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)
            throw new IllegalStateException("requested Activity orientation was not retained");
        activity.setRequestedOrientation(info.screenOrientation);
        instrumentation.callActivityOnCreate(activity, null);
        if (!created || activity.getWindow() == null ||
                activity.getWindow().peekDecorView() == null)
            throw new IllegalStateException("onCreate did not complete with a decor view");
        System.out.println("FRAMEWORK_OK");
        if (probeWebView) {
            if (probeRenderer) {
                loadActivityLibrary();
                int error = openWindow(640, 480);
                if (error != 0) throw new IllegalStateException("renderer probe window: " + error);
            }
            System.out.println("ACTIVITY_HEAP_POLICY maxMb=" + Runtime.getRuntime().maxMemory() / 1048576);
            if (android.webkit.WebView.getWebViewClassLoader() == null)
                throw new AssertionError("real WebView provider startup failed");
            if (android.webkit.CookieManager.getInstance() == null)
                throw new AssertionError("real WebView cookie manager unavailable");
            if (android.webkit.WebSettings.getDefaultUserAgent(context).isEmpty())
                throw new AssertionError("real WebView user agent unavailable");
            android.webkit.WebView view = new android.webkit.WebView(activity);
            if (view.getSettings() == null) throw new AssertionError("WebView settings unavailable");
            if (probeRenderer) {
                Handler timeout = new Handler(Looper.getMainLooper());
                timeout.postDelayed(() -> {
                    System.err.println("WebView renderer did not finish the local page: url="
                            + view.getUrl() + " progress=" + view.getProgress()
                            + " height=" + view.getContentHeight());
                    System.exit(1);
                }, 60000);
                view.getSettings().setJavaScriptEnabled(true);
                view.setWebViewClient(new android.webkit.WebViewClient() {
                    @Override public void onPageStarted(android.webkit.WebView page, String url,
                            android.graphics.Bitmap favicon) {
                        System.out.println("ACTIVITY_WEBVIEW_PAGE_STARTED " + url);
                        timeout.postDelayed(() -> page.evaluateJavascript(
                                "JSON.stringify({state:document.readyState,url:location.href,"
                                + "html:document.documentElement&&document.documentElement.outerHTML})",
                                value -> System.out.println("ACTIVITY_WEBVIEW_DOCUMENT_STATE " + value)), 2000);
                    }
                    @Override public void onReceivedError(android.webkit.WebView page,
                            android.webkit.WebResourceRequest request,
                            android.webkit.WebResourceError error) {
                        System.err.println("ACTIVITY_WEBVIEW_PAGE_ERROR " + error.getErrorCode()
                                + " " + error.getDescription());
                    }
                    @Override public boolean onRenderProcessGone(android.webkit.WebView page,
                            android.webkit.RenderProcessGoneDetail detail) {
                        System.err.println("ACTIVITY_WEBVIEW_RENDERER_GONE crashed=" + detail.didCrash());
                        System.exit(1);
                        return true;
                    }
                    @Override public void onPageFinished(android.webkit.WebView page, String url) {
                        System.out.println("ACTIVITY_WEBVIEW_PAGE_FINISHED " + url);
                        page.evaluateJavascript("document.getElementById('probe').textContent + ':' + (6 * 7)", value -> {
                            if (!"\"asgard:42\"".equals(value)) {
                                System.err.println("Unexpected WebView DOM result: " + value);
                                System.exit(1);
                            }
                            System.out.println("ACTIVITY_WEBVIEW_RENDERER_DOM_OK " + value);
                            page.postVisualStateCallback(1, new android.webkit.WebView.VisualStateCallback() {
                                @Override public void onComplete(long requestId) {
                                    android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(
                                            640, 480, android.graphics.Bitmap.Config.ARGB_8888);
                                    page.draw(new android.graphics.Canvas(bitmap));
                                    int foreground = bitmap.getPixel(40, 40);
                                    int background = bitmap.getPixel(500, 400);
                                    if (foreground != 0xffe63946 || background != 0xff1d3557) {
                                        System.err.println("Unexpected WebView pixels: "
                                                + Integer.toHexString(foreground) + " "
                                                + Integer.toHexString(background));
                                        System.exit(1);
                                    }
                                    System.out.println("ACTIVITY_WEBVIEW_RENDERER_PIXELS_OK");
                                    bitmap.recycle();
                                    page.destroy();
                                    System.exit(0);
                                }
                            });
                        });
                    }
                });
                view.measure(android.view.View.MeasureSpec.makeMeasureSpec(640,
                                android.view.View.MeasureSpec.EXACTLY),
                        android.view.View.MeasureSpec.makeMeasureSpec(480,
                                android.view.View.MeasureSpec.EXACTLY));
                view.layout(0, 0, 640, 480);
                view.loadDataWithBaseURL("https://asgard.invalid/", "<!doctype html><html><head>"
                        + "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                        + "<style>html,body{margin:0;background:#1d3557}"
                        + "#probe{width:120px;height:120px;background:#e63946;color:#e63946}</style>"
                        + "</head><body>"
                        + "<div id='probe'>asgard</div><input id='editor'></body></html>",
                        "text/html", "UTF-8", null);
                System.out.println("ACTIVITY_WEBVIEW_CONSTRUCTION_OK");
                System.out.println("ACTIVITY_WEBVIEW_OK heapMaxMb=" + Runtime.getRuntime().maxMemory() / 1048576);
                Looper.loop();
                return;
            }
            view.destroy();
            System.out.println("ACTIVITY_WEBVIEW_CONSTRUCTION_OK");
            System.out.println("ACTIVITY_WEBVIEW_OK heapMaxMb=" + Runtime.getRuntime().maxMemory() / 1048576);
        }
        // Exercise the report transport without invoking RuntimeInit's
        // uncaught-exception handler (which would correctly kill the process).
        Object manager = call(ActivityManager.class, "getService", new Class<?>[0]);
        Class<?> crashType = Class.forName("android.app.ApplicationErrorReport$ParcelableCrashInfo");
        Object crash = crashType.getConstructor(Throwable.class)
                .newInstance(new IllegalStateException("intentional service-report check"));
        call(manager, "handleApplicationCrash", new Class<?>[] {IBinder.class, crashType}, null, crash);
        System.out.println("ACTIVITY_CRASH_REPORTING_OK");
        System.exit(0);
    }

    private static void installWebView(Context system) throws Exception {
        String apk = "/product/app/webview/webview.apk";
        if (!new File(apk).isFile()) return;
        PackageInfo pkg = system.getPackageManager().getPackageArchiveInfo(apk,
                PackageManager.GET_META_DATA | PackageManager.GET_SIGNATURES
                        | PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_SERVICES
                        | PackageManager.MATCH_DISABLED_COMPONENTS);
        if (pkg == null || !"com.android.webview".equals(pkg.packageName)
                || pkg.applicationInfo.metaData == null
                || pkg.applicationInfo.metaData.getString("com.android.webview.WebViewLibrary") == null)
            throw new IllegalStateException("Invalid same-image WebView provider");
        installWebView(system, pkg);
    }

    private static void installWebView(Context system, PackageInfo pkg) throws Exception {
        String apk = "/product/app/webview/webview.apk";
        ApplicationInfo app = pkg.applicationInfo;
        app.sourceDir = app.publicSourceDir = apk;
        app.nativeLibraryDir = "/product/app/webview/lib/arm64";
        setField(app, "nativeLibraryRootDir", "/product/app/webview/lib");
        setField(app, "primaryCpuAbi", "arm64-v8a");
        app.dataDir = "/data/user/0/" + pkg.packageName;
        setField(app, "credentialProtectedDataDir", app.dataDir);
        setField(app, "deviceProtectedDataDir", "/data/user_de/0/" + pkg.packageName);
        app.uid = 10001;
        app.flags |= ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_HAS_CODE | ApplicationInfo.FLAG_INSTALLED;
        if (pkg.services != null)
            for (android.content.pm.ServiceInfo service : pkg.services) service.applicationInfo = app;
        packages.installSystem(pkg);
        services.register("webviewupdate", "android.webkit.IWebViewUpdateService", (method, a) -> {
            switch (method) {
                case "waitForAndGetProvider":
                    // No zygote-created RELRO: AOSP accepts this status and
                    // loads the real provider library without shared RELRO.
                    return Class.forName("android.webkit.WebViewProviderResponse")
                            .getConstructor(PackageInfo.class, int.class)
                            .newInstance(packages.systemPackage(pkg.packageName), 3);
                case "getCurrentWebViewPackage": return packages.systemPackage(pkg.packageName);
                case "getCurrentWebViewPackageName": return pkg.packageName;
                case "isMultiProcessEnabled": return false;
                default: throw unsupported("webviewupdate", method);
            }
        });
        if (diagnostic) {
            PackageInfo lookup = system.getPackageManager().getPackageInfo(pkg.packageName,
                    PackageManager.GET_META_DATA | PackageManager.GET_SIGNATURES);
            if (!system.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WEBVIEW)
                    || system.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WEBVIEW, 1)
                    || lookup.signatures == null || lookup.signatures.length == 0
                    || !apk.equals(lookup.applicationInfo.sourceDir))
                throw new AssertionError("WebView provider package contract failed");
            boolean listed = false;
            for (android.content.pm.FeatureInfo feature : system.getPackageManager().getSystemAvailableFeatures())
                if (PackageManager.FEATURE_WEBVIEW.equals(feature.name) && feature.version == 0) listed = true;
            if (!listed) throw new AssertionError("WebView feature missing from feature list");
            ComponentName component = new ComponentName(pkg.packageName,
                    "org.chromium.android_webview.devui.DeveloperModeState");
            if (system.getPackageManager().getComponentEnabledSetting(component)
                    != PackageManager.COMPONENT_ENABLED_STATE_DEFAULT)
                throw new AssertionError("unexpected system component override");
            try {
                system.getPackageManager().setComponentEnabledSetting(component,
                        PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
                throw new AssertionError("foreign component mutation allowed");
            } catch (IllegalArgumentException expected) {
                // This app may read the system provider, but does not own it.
            }
            System.out.println("ACTIVITY_WEBVIEW_PACKAGE_OK " + pkg.versionName);
        }
    }

    public static void main(String[] args) {
        if (args.length == 1 && args[0].equals("--service-inventory")) {
            execute(args);
            return;
        }
        if (args.length == 3 && args[0].equals("--service-child")) {
            execute(args);
            return;
        }
        if (args.length < 4 || !args[0].equals("--app")) {
            System.err.println("ManagedLauncher requires --app <package> <code> <activity> [split=apk]...");
            System.exit(2);
        }
        String[] playArgs = new String[args.length + 1];
        playArgs[0] = "--play";
        System.arraycopy(args, 0, playArgs, 1, args.length);
        execute(playArgs);
    }

    public static void probe(String[] args) {
        diagnostic = true;
        execute(args);
    }

    private static void execute(String[] args) {
        try {
            FrameworkTrace.start();
            run(args);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
