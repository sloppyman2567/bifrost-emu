// Managed installed-Activity runtime for Bifrost. Uses the real guest
// ActivityThread, Instrumentation, LoadedApk and main Looper with a bounded
// in-process service profile. ManagedLauncher --app starts and resumes an app;
// ActivityProbe delegates to probe() for framework assertions and checkpoints.
// SurfaceView input, resize and teardown are owned here. There is no system_server.
import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.app.GameManager;
import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
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
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import java.io.File;
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
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.Map;
import java.util.UUID;

public final class ManagedLauncher {
    private static boolean diagnostic;
    private static boolean created;
    private static volatile Activity activeActivity;
    private static volatile boolean windowOpen;
    private static final Map<IBinder, Long> fullyDrawn = new ConcurrentHashMap<>();
    // The package installed by app mode; the only package that exists here.
    private static volatile PackageInfo installed;
    // ActivityRecord's requested orientation is initialized from its manifest.
    private static final Map<IBinder, Integer> orientations = new ConcurrentHashMap<>();
    private static Object display;  // the single DisplayInfo the display stub reports
    private static boolean play;

    // guest/android/activity_window.c: Bifrost syscall 0x100E ops 2 and 3.
    private static native int openWindow(int width, int height);
    private static native int setWindowIdentity(byte[] titleUtf8, int[] argb, int width, int height);
    private static native int pollWindow(long[] event);
    private static native int audioConfig(long[] config);
    private static native long prepareSurface(android.view.Surface surface, boolean opaque);
    private static native void releaseSurface(long window);
    private static final Map<SurfaceView, Long> surfaceWindows = new java.util.IdentityHashMap<>();

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

    interface Call {
        Object invoke(String method, Object[] arguments) throws Exception;
    }

    // Registers a local implementation of `iface` under `name`.
    // ServiceManager.getService consults sCache before any Binder lookup.
    @SuppressWarnings("unchecked")
    private static Object service(String name, String iface, Call call) throws Exception {
        Class<?> type = Class.forName(iface);
        Binder binder = new Binder();
        Object proxy = Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type},
                (self, method, arguments) -> {
                    if (method.getName().equals("asBinder")) return binder;
                    if (method.getName().equals("toString")) return "Bifrost:" + name;
                    if (diagnostic) System.out.println("LOCAL_SERVICE " + name + "." + method.getName());
                    return call.invoke(method.getName(), arguments);
                });
        binder.attachInterface((IInterface) proxy, iface);
        Field cache = Class.forName("android.os.ServiceManager").getDeclaredField("sCache");
        cache.setAccessible(true);
        ((Map<String, IBinder>) cache.get(null)).put(name, binder);
        return proxy;
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

    // Native AudioSystem clients use Binder, not ServiceManager's Java cache.
    // This profile has no Android HAL ports/patches; host audio is a separate backend.
    private static final class AudioPolicy extends Binder {
        private static final String DESCRIPTOR = "android.media.IAudioPolicyService";
        private final List<IBinder> clients = new ArrayList<>();
        private boolean portCallbacks, volumeCallbacks;
        private void readCount(Parcel data) {
            if (data.readInt() != 1) throw new IllegalArgumentException("null port count");
            int start = data.dataPosition(), size = data.readInt();
            if (size < 8 || size > data.dataSize() - start || data.readInt() < 0)
                throw new IllegalArgumentException("invalid port count");
            data.setDataPosition(start + size);
        }
        @Override protected synchronized boolean onTransact(int code, Parcel data,
                Parcel reply, int flags) throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            if (code != 44 && code != 49 && code != 51 && code != 52 && code != 53)
                return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            System.out.println("AUDIO_POLICY_TRANSACTION " + code);
            if (code == 51) {
                IBinder client = data.readStrongBinder();
                if (client == null) throw new IllegalArgumentException("null audio client");
                if (!clients.contains(client)) clients.add(client);
            } else if (code == 52 || code == 53) {
                boolean enabled = data.readBoolean();
                if (code == 52) portCallbacks = enabled; else volumeCallbacks = enabled;
            } else {
                if (code == 44) {
                    int role = data.readInt(), type = data.readInt();
                    if (role < 0 || role > 2 || type < 0 || type > 3)
                        throw new IllegalArgumentException("invalid audio port filter");
                }
                readCount(data);
                // C++ AIDL sends the capacity of an out array after inout count.
                int capacity = data.readInt();
                if (capacity < 0 || capacity > 4096)
                    throw new IllegalArgumentException("invalid audio array capacity");
            }
            data.enforceNoDataAvail();
            reply.writeNoException();
            if (code == 44 || code == 49) {
                reply.writeInt(1); // stable generation for the empty topology
                reply.writeInt(1); // nonnull android.media.audio.common.Int
                reply.writeInt(8); // parcelable size, including size field
                reply.writeInt(0); // count.value
                reply.writeInt(0); // empty ports/patches vector
            }
            return true;
        }
    }
    private static final Binder audioPolicy = new AudioPolicy();

    // Android 15 IAudioFlingerService: only the queries used by this profile.
    // Playback uses the managed AAudio adapter and Bifrost's host Audio engine.
    private static final class AudioFlinger extends Binder {
        private static final String DESCRIPTOR = "android.media.IAudioFlingerService";
        private IBinder client;
        @Override protected synchronized boolean onTransact(int code, Parcel data,
                Parcel reply, int flags) throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            if (code != 23 && code != 45 && code != 46)
                return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            System.out.println("AUDIO_FLINGER_TRANSACTION " + code);
            if (code == 23) {
                IBinder incoming = data.readStrongBinder();
                if (incoming == null) throw new IllegalArgumentException("null audio client");
                if (client == null) client = incoming; // AudioFlinger keeps the first per process.
            }
            data.enforceNoDataAvail();
            long[] config = new long[3];
            if (code != 23 && audioConfig(config) != 0)
                throw new IllegalStateException("host audio configuration unavailable");
            reply.writeNoException();
            if (code == 45) reply.writeInt((int) config[0]);
            if (code == 46) reply.writeLong(config[1]);
            return true;
        }
    }
    private static final Binder audioFlinger = new AudioFlinger();

    // Native libsensor uses the hand-written ISensorServer protocol, not AIDL:
    // replies start with the count/bool itself, with no exception header.
    // This virtual device exposes no physical, dynamic or virtual-device sensors.
    private static final class SensorService extends Binder {
        private static final String DESCRIPTOR = "android.gui.SensorServer";
        SensorService() { attachInterface(null, DESCRIPTOR); }
        @Override protected boolean onTransact(int code, Parcel data,
                Parcel reply, int flags) throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            switch (code) {
                case 1: // GET_SENSOR_LIST
                case 4: // GET_DYNAMIC_SENSOR_LIST
                case 7: // GET_RUNTIME_SENSOR_LIST
                case 3: // ENABLE_DATA_INJECTION (query, not a setter)
                case 8: // ENABLE_REPLAY_DATA_INJECTION
                case 9: // ENABLE_HAL_BYPASS_REPLAY_DATA_INJECTION
                    break;
                default: return super.onTransact(code, data, reply, flags);
            }
            data.enforceInterface(DESCRIPTOR);
            if (code == 1 || code == 4 || code == 7) {
                if (data.dataAvail() < 4 || data.readString() == null)
                    throw new IllegalArgumentException("missing sensor package name");
                if (code == 7) {
                    if (data.dataAvail() < 4)
                        throw new IllegalArgumentException("missing sensor device id");
                    data.readInt();
                }
            }
            data.enforceNoDataAvail();
            System.out.println("SENSOR_SERVICE_TRANSACTION " + code);
            reply.writeInt(0); // empty list, or injection disabled
            return true;
        }
    }
    private static final Binder sensorService = new SensorService();
    private static boolean sensorServiceRegistered;

    // Android 15 ISurfaceComposer / IDisplayEventConnection and BitTube wire
    // formats. This virtual display uses a monotonic 60 Hz clock, as does the
    // NDK adapter. Real libgui/libandroid_runtime own the receiver and deliver
    // callbacks on the client's MessageQueue; no Java Choreographer override.
    private static final long DISPLAY_PERIOD_NS = 1000000000L / 60;
    private static final long PHYSICAL_DISPLAY_ID = 1;
    private static final class DisplayClock implements Runnable {
        private final Handler handler;
        private static final class ConnectionRef extends WeakReference<DisplayEventConnection> {
            // Keep the resources alive until this clock observes client disposal,
            // then close explicitly rather than leaving them to PFD finalizers.
            private final ParcelFileDescriptor receive, send;
            ConnectionRef(DisplayEventConnection connection) {
                super(connection);
                receive = connection.receive;
                send = connection.send;
            }
            void close() {
                for (ParcelFileDescriptor fd : new ParcelFileDescriptor[] {receive, send})
                    try { fd.close(); } catch (IOException ignored) { }
            }
        }
        private final List<ConnectionRef> connections = new ArrayList<>();
        DisplayClock() {
            HandlerThread thread = new HandlerThread("BifrostDisplayEvents");
            thread.setDaemon(true);
            thread.start();
            handler = new Handler(thread.getLooper());
            handler.post(this);
        }
        synchronized void add(DisplayEventConnection connection) {
            connections.removeIf(ref -> {
                if (ref.get() != null) return false;
                ref.close();
                return true;
            });
            if (connections.size() >= 256) throw new IllegalStateException("display connection limit");
            connections.add(new ConnectionRef(connection));
        }
        @Override public synchronized void run() {
            long now = System.nanoTime();
            long tick = now / DISPLAY_PERIOD_NS;
            for (int i = connections.size() - 1; i >= 0; i--) {
                DisplayEventConnection connection = connections.get(i).get();
                if (connection == null || !connection.tick(tick)) {
                    connections.remove(i).close();
                }
            }
            long delay = ((tick + 1) * DISPLAY_PERIOD_NS - System.nanoTime() + 999999) / 1000000;
            handler.postAtTime(this, SystemClock.uptimeMillis() + Math.max(1, delay));
        }
    }
    private static final class DisplayEventConnection extends Binder {
        private static final String DESCRIPTOR = "android.gui.IDisplayEventConnection";
        private ParcelFileDescriptor receive, send;
        private boolean requested, closed;
        private int rate;
        private long lastTick = -1;
        DisplayEventConnection() throws Exception {
            attachInterface(null, DESCRIPTOR);
            FileDescriptor read = new FileDescriptor(), write = new FileDescriptor();
            Os.socketpair(OsConstants.AF_UNIX, OsConstants.SOCK_SEQPACKET, 0, read, write);
            try {
                Os.fcntlInt(read, OsConstants.F_SETFL, OsConstants.O_NONBLOCK);
                Os.fcntlInt(write, OsConstants.F_SETFL, OsConstants.O_NONBLOCK);
                receive = ParcelFileDescriptor.dup(read);
                send = ParcelFileDescriptor.dup(write);
            } catch (Exception error) {
                close();
                throw error;
            } finally {
                Os.close(read);
                Os.close(write);
            }
        }
        private synchronized void close() {
            closed = true;
            for (ParcelFileDescriptor fd : new ParcelFileDescriptor[] {receive, send}) {
                if (fd != null) try { fd.close(); } catch (IOException ignored) { }
            }
            receive = send = null;
        }
        private boolean sendEvent(byte[] packet) {
            try {
                int size = Os.sendto(send.getFileDescriptor(), packet, 0, packet.length,
                        0x40 | 0x4000, null, 0); // Linux MSG_DONTWAIT | MSG_NOSIGNAL (hidden SDK)
                if (size != packet.length) throw new IllegalStateException("partial display event");
                return true;
            } catch (ErrnoException error) {
                if (error.errno == OsConstants.EAGAIN || error.errno == OsConstants.EINTR) return false;
                close(); // Peer gone: stop ticking and release the service's endpoint.
                return false;
            } catch (java.net.SocketException error) {
                close();
                return false;
            }
        }
        private ByteBuffer event(int type, long timestamp) {
            // DisplayEventReceiver::Event is 216 bytes on both Android ABIs;
            // Header's displayId/timestamp and VSync::vsyncData are 8-aligned.
            ByteBuffer packet = ByteBuffer.allocate(216).order(ByteOrder.LITTLE_ENDIAN);
            packet.putInt(0, type);
            packet.putLong(8, PHYSICAL_DISPLAY_ID);
            packet.putLong(16, timestamp);
            return packet;
        }
        synchronized boolean tick(long tick) {
            if (closed) return false;
            if (receive != null || tick == lastTick) return true;
            lastTick = tick;
            if (rate == 0 ? !requested : tick % rate != 0) return true;
            long timestamp = tick * DISPLAY_PERIOD_NS;
            ByteBuffer packet = event(0x7673796e, timestamp); // fourcc('v','s','y','n')
            packet.putInt(24, (int) tick);
            packet.putLong(32, DISPLAY_PERIOD_NS);
            packet.putInt(40, 0); // preferred timeline
            packet.putInt(44, 1); // one valid timeline, capacity seven
            packet.putLong(48, tick);
            packet.putLong(56, timestamp + DISPLAY_PERIOD_NS - 2000000);
            packet.putLong(64, timestamp + DISPLAY_PERIOD_NS);
            if (sendEvent(packet.array())) requested = false;
            return !closed;
        }
        @Override protected synchronized boolean onTransact(int code, Parcel data,
                Parcel reply, int flags) throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            if (code < 1 || code > 4) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            int count = code == 2 ? data.readInt() : 0;
            data.enforceNoDataAvail();
            if (closed) throw new IllegalStateException("display event connection closed");
            if (code == 1) {
                if (receive == null) throw new IllegalStateException("channel already stolen");
                ByteBuffer hotplug = event(0x706c7567, System.nanoTime());
                hotplug.put(24, (byte) 1);
                if (!sendEvent(hotplug.array())) throw new IllegalStateException("initial hotplug failed");
                reply.writeNoException();
                reply.writeInt(1); // nonnull native BitTube parcelable
                reply.writeFileDescriptor(receive.getFileDescriptor());
                reply.writeFileDescriptor(send.getFileDescriptor());
                try { receive.close(); } catch (IOException error) { throw new IllegalStateException(error); }
                receive = null;
            } else if (code == 2) {
                if (count < 0) throw new IllegalArgumentException("negative vsync rate");
                rate = count;
                requested = false;
                reply.writeNoException();
            } else if (code == 3) {
                if (rate == 0) requested = true; // oneway, repeated requests coalesce
            } else {
                long tick = System.nanoTime() / DISPLAY_PERIOD_NS;
                long presentation = (tick + 1) * DISPLAY_PERIOD_NS;
                reply.writeNoException();
                reply.writeInt(1); // nonnull native ParcelableVsyncEventData
                reply.writeLong(DISPLAY_PERIOD_NS);
                reply.writeInt(0);
                reply.writeInt(1);
                reply.writeLong(tick);
                reply.writeLong(presentation - 2000000);
                reply.writeLong(presentation);
            }
            return true;
        }
    }
    private static final class SurfaceComposer extends Binder {
        private static final String DESCRIPTOR = "android.gui.ISurfaceComposer";
        private DisplayClock clock;
        SurfaceComposer() { attachInterface(null, DESCRIPTOR); }
        @Override protected synchronized boolean onTransact(int code, Parcel data,
                Parcel reply, int flags) throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            if (code != 2) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            int source = data.readInt(), registration = data.readInt();
            IBinder layer = data.readStrongBinder();
            data.enforceNoDataAvail();
            if (source < 0 || source > 1 || (registration & ~3) != 0 || layer != null)
                throw new IllegalArgumentException("unsupported display connection options");
            DisplayEventConnection connection = null;
            try {
                connection = new DisplayEventConnection();
                if (clock == null) clock = new DisplayClock();
                clock.add(connection);
            } catch (Exception error) {
                if (connection != null) connection.close();
                throw new IllegalStateException("display channel creation failed", error);
            }
            reply.writeNoException();
            reply.writeStrongBinder(connection);
            return true;
        }
    }
    private static final Binder surfaceComposer = new SurfaceComposer();

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
        DisplayEventConnection connection = null;
        ParcelFileDescriptor receive = null, send = null;
        try {
            data.writeInterfaceToken(SurfaceComposer.DESCRIPTOR);
            data.writeInt(0); data.writeInt(0); data.writeStrongBinder(null);
            if (!surfaceComposer.transact(2, data, reply, 0)) throw new AssertionError("no display connection");
            reply.readException();
            connection = (DisplayEventConnection) reply.readStrongBinder();
            data.setDataSize(0); reply.setDataSize(0);
            data.setDataPosition(0); reply.setDataPosition(0);
            data.writeInterfaceToken(DisplayEventConnection.DESCRIPTOR);
            connection.transact(1, data, reply, 0);
            reply.readException();
            if (reply.readInt() != 1) throw new AssertionError("null BitTube");
            receive = reply.readFileDescriptor(); send = reply.readFileDescriptor();
            ByteBuffer hotplug = readDisplayEvent(receive, true);
            if (hotplug.getInt(0) != 0x706c7567 || hotplug.getLong(8) != PHYSICAL_DISPLAY_ID || hotplug.get(24) != 1)
                throw new AssertionError("missing initial hotplug");
            for (int step = 0; step < 4; step++) {
                data.setDataSize(0); reply.setDataSize(0);
                data.setDataPosition(0); reply.setDataPosition(0);
                data.writeInterfaceToken(DisplayEventConnection.DESCRIPTOR);
                if (step == 0) { // query future frame timeline
                    connection.transact(4, data, reply, 0);
                    reply.readException();
                    if (reply.readInt() != 1 || reply.readLong() != DISPLAY_PERIOD_NS ||
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
                    if (frame.getInt(0) != 0x7673796e || frame.getLong(32) != DISPLAY_PERIOD_NS || frame.getInt(44) != 1)
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
        if (binder == null || !SensorService.DESCRIPTOR.equals(binder.getInterfaceDescriptor()))
            throw new AssertionError("native sensor service was not published");
        // Check the raw native reply layout, including non-default device ids.
        for (int code : new int[] {1, 4, 7, 3, 8, 9, 2}) {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(SensorService.DESCRIPTOR);
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
                data.writeInterfaceToken(badToken ? "invalid.sensor.interface" : SensorService.DESCRIPTOR);
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

    // Unity reads the game mode during startup and several SDKs require the
    // connectivity service to construct. Neither needs more than this.
    private static void checkGameServices(Context context) throws Exception {
        GameManager games = (GameManager) context.getSystemService(Context.GAME_SERVICE);
        if (games == null || games.getGameMode() != GameManager.GAME_MODE_STANDARD)
            throw new AssertionError("game service did not report standard mode");
        android.net.ConnectivityManager connectivity = (android.net.ConnectivityManager)
                context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (connectivity == null || connectivity.getActiveNetworkInfo() != null)
            throw new AssertionError("connectivity service did not report no network");
        System.out.println("ACTIVITY_GAME_SERVICES_OK");
    }

    private static final Map<ComponentName, Integer> componentStates = new ConcurrentHashMap<>();

    private static ComponentName ownComponent(Object argument) {
        ComponentName component = (ComponentName) argument;
        if (installed == null || !installed.packageName.equals(component.getPackageName()))
            throw new IllegalArgumentException("Unknown component: " + component);
        return component;
    }

    // PackageManagerService returns a manifest ServiceInfo, not a running
    // service. Firebase discovers its registrars through this metadata.
    private static boolean componentMatches(ComponentInfo info, long flags, int userId) {
        PackageInfo pkg = installed;
        if (pkg == null || userId != pkg.applicationInfo.uid / 100000
                || !pkg.packageName.equals(info.packageName)) return false;
        if ((flags & PackageManager.MATCH_SYSTEM_ONLY) != 0
                && (pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0) return false;
        int state = componentStates.getOrDefault(new ComponentName(info.packageName, info.name),
                PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);
        boolean enabled = state == PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                || (state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && info.enabled);
        if ((flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0
                && (!pkg.applicationInfo.enabled || !enabled)) return false;
        long bootFlags = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        // ComputerEngine.updateFlags supplies the defaults server-side.
        // This profile's user is unlocked, so implicit queries see both.
        if (bootFlags == 0) bootFlags = PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        return (bootFlags & (info.directBootAware ? PackageManager.MATCH_DIRECT_BOOT_AWARE
                : PackageManager.MATCH_DIRECT_BOOT_UNAWARE)) != 0;
    }

    private static void installedComponentInfo(ComponentInfo result, long flags) {
        result.applicationInfo = new ApplicationInfo(installed.applicationInfo);
        boolean metadata = (flags & PackageManager.GET_META_DATA) != 0;
        result.metaData = metadata && result.metaData != null ? new Bundle(result.metaData) : null;
        result.applicationInfo.metaData = metadata && installed.applicationInfo.metaData != null
                ? new Bundle(installed.applicationInfo.metaData) : null;
    }

    private static PackageInfo packageInfo(String name, long flags, int userId) {
        if (installed == null || !installed.packageName.equals(name)
                || userId != installed.applicationInfo.uid / 100000) return null;
        // PackageInfo has no copy constructor. Parcel it as the framework's
        // Binder service does, so callers cannot change the installed record.
        Parcel parcel = Parcel.obtain();
        try {
            installed.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            PackageInfo result = PackageInfo.CREATOR.createFromParcel(parcel);
            if ((flags & PackageManager.GET_SIGNATURES) == 0) result.signatures = null;
            if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) == 0) result.signingInfo = null;
            return result;
        } finally {
            parcel.recycle();
        }
    }

    private static ServiceInfo serviceInfo(ComponentName component, long flags, int userId) {
        PackageInfo pkg = installed;
        if (pkg == null || !pkg.packageName.equals(component.getPackageName()) || pkg.services == null)
            return null;
        for (ServiceInfo service : pkg.services) {
            if (!service.name.equals(component.getClassName())) continue;
            if (!componentMatches(service, flags, userId)) return null;
            ServiceInfo result = new ServiceInfo(service);
            installedComponentInfo(result, flags);
            return result;
        }
        return null;
    }

    private static ProviderInfo resolveProvider(String authority, long flags, int userId) {
        if (installed == null || installed.providers == null || authority == null) return null;
        for (ProviderInfo provider : installed.providers) {
            if (provider.authority == null
                    || !Arrays.asList(provider.authority.split(";")).contains(authority)) continue;
            if (!componentMatches(provider, flags, userId)) return null;
            ProviderInfo result = new ProviderInfo(provider);
            installedComponentInfo(result, flags);
            return result;
        }
        return null;
    }

    private static void checkPackageServices(Context context) throws Exception {
        PackageManager manager = context.getPackageManager();
        PackageInfo signing = manager.getPackageInfo(installed.packageName,
                PackageManager.GET_SIGNATURES | PackageManager.GET_SIGNING_CERTIFICATES);
        if (signing.signatures == null || signing.signatures.length == 0 || signing.signingInfo == null
                || !Arrays.equals(signing.signatures, installed.signatures)
                || !Arrays.equals(signing.signingInfo.getApkContentsSigners(),
                        installed.signingInfo.getApkContentsSigners()))
            throw new AssertionError("installed package signing certificates missing");
        PackageInfo plain = manager.getPackageInfo(installed.packageName, 0);
        if (plain.signatures != null || plain.signingInfo != null)
            throw new AssertionError("signing certificates returned without query flags");
        if (manager.getPackageInfo(installed.packageName, PackageManager.GET_SIGNATURES).signingInfo != null
                || manager.getPackageInfo(installed.packageName,
                        PackageManager.GET_SIGNING_CERTIFICATES).signatures != null)
            throw new AssertionError("legacy and current signing query flags conflated");
        signing.signatures[0] = null;
        if (manager.getPackageInfo(installed.packageName, PackageManager.GET_SIGNATURES).signatures[0] == null)
            throw new AssertionError("signing certificate array aliases package state");
        int flags = PackageManager.MATCH_DISABLED_COMPONENTS | PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        int services = 0, metadata = 0;
        if (installed.services != null) for (ServiceInfo parsed : installed.services) {
            ComponentName component = new ComponentName(installed.packageName, parsed.name);
            ServiceInfo result = manager.getServiceInfo(component, flags | PackageManager.GET_META_DATA);
            if (!parsed.name.equals(result.name) || !installed.packageName.equals(result.packageName)
                    || !installed.applicationInfo.sourceDir.equals(result.applicationInfo.sourceDir)
                    || result.applicationInfo.uid != installed.applicationInfo.uid)
                throw new AssertionError("service identity differs from installed manifest: " + component);
            if (parsed.enabled && installed.applicationInfo.enabled) {
                // SDKs such as Firebase use GET_META_DATA without boot flags.
                ServiceInfo implicit = manager.getServiceInfo(component, PackageManager.GET_META_DATA);
                if (!parsed.name.equals(implicit.name)
                        || (parsed.metaData != null && (implicit.metaData == null
                        || !parsed.metaData.keySet().equals(implicit.metaData.keySet()))))
                    throw new AssertionError("implicit service query lost metadata: " + component);
            }
            if (parsed.metaData != null) {
                if (result.metaData == null || !parsed.metaData.keySet().equals(result.metaData.keySet()))
                    throw new AssertionError("missing service metadata: " + component);
                for (String key : parsed.metaData.keySet())
                    if (!java.util.Objects.equals(parsed.metaData.get(key), result.metaData.get(key)))
                        throw new AssertionError("changed service metadata: " + component + " " + key);
                metadata += parsed.metaData.size();
                result.metaData.clear();
                if (manager.getServiceInfo(component, flags | PackageManager.GET_META_DATA)
                        .metaData.size() != parsed.metaData.size())
                    throw new AssertionError("service metadata aliases package state");
            }
            result = manager.getServiceInfo(component, flags);
            if (result.metaData != null || result.applicationInfo.metaData != null)
                throw new AssertionError("service metadata returned without GET_META_DATA");
            services++;
        }
        for (ComponentName missing : new ComponentName[] {
                new ComponentName(installed.packageName, "bifrost.missing.Service"),
                new ComponentName("bifrost.missing.package", "bifrost.missing.Service")}) {
            try {
                manager.getServiceInfo(missing, flags | PackageManager.GET_META_DATA);
                throw new AssertionError("unknown service found: " + missing);
            } catch (PackageManager.NameNotFoundException expected) {
                // The real framework translates a null service reply.
            }
        }
        int providers = 0;
        if (installed.providers != null) for (ProviderInfo parsed : installed.providers) {
            if (parsed.authority == null) continue;
            for (String authority : parsed.authority.split(";")) {
                ProviderInfo result = manager.resolveContentProvider(authority,
                        flags | PackageManager.GET_META_DATA);
                if (result == null || !parsed.name.equals(result.name)
                        || result.applicationInfo.uid != installed.applicationInfo.uid
                        || !installed.applicationInfo.sourceDir.equals(result.applicationInfo.sourceDir))
                    throw new AssertionError("provider differs from installed manifest: " + authority);
                if (parsed.metaData != null && (result.metaData == null
                        || !parsed.metaData.keySet().equals(result.metaData.keySet())))
                    throw new AssertionError("provider metadata missing: " + authority);
                if (result.metaData != null) {
                    result.metaData.clear();
                    if (manager.resolveContentProvider(authority, flags | PackageManager.GET_META_DATA)
                            .metaData.size() != parsed.metaData.size())
                        throw new AssertionError("provider metadata aliases package state");
                }
                if (parsed.enabled && installed.applicationInfo.enabled) {
                    result = manager.resolveContentProvider(authority, 0);
                    if (result == null || result.metaData != null || result.applicationInfo.metaData != null)
                        throw new AssertionError("implicit provider query lost identity or leaked metadata");
                }
                int otherBoot = parsed.directBootAware ? PackageManager.MATCH_DIRECT_BOOT_UNAWARE
                        : PackageManager.MATCH_DIRECT_BOOT_AWARE;
                if (manager.resolveContentProvider(authority,
                        otherBoot | PackageManager.MATCH_DISABLED_COMPONENTS) != null)
                    throw new AssertionError("provider ignored explicit direct-boot filter");
                providers++;
            }
        }
        if (manager.resolveContentProvider("bifrost.missing.authority", 0) != null)
            throw new AssertionError("unknown content provider found");
        InstallSourceInfo source = manager.getInstallSourceInfo(installed.packageName);
        if (manager.getInstallerPackageName(installed.packageName) != null
                || source.getInstallingPackageName() != null || source.getInitiatingPackageName() != null
                || source.getOriginatingPackageName() != null || source.getUpdateOwnerPackageName() != null
                || source.getPackageSource() != android.content.pm.PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED)
            throw new AssertionError("host-installed APK reported an Android installer");
        try {
            manager.getInstallSourceInfo("bifrost.missing.package");
            throw new AssertionError("unknown install source found");
        } catch (PackageManager.NameNotFoundException expected) {
            // The framework translates the absent package's null reply.
        }
        System.out.println("APP_PACKAGE_SERVICES_OK services=" + services + " metadata=" + metadata
                + " providers=" + providers);
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

    private static void checkRuntimeServices(Context context, boolean resumed) throws Exception {
        checkSensorService(context);
        checkInputService(context);
        checkGameServices(context);
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

    // One 160 dpi, 60 Hz display with id 0: 480x800 unless play mode sizes
    // it to the host window before the app first queries it.
    private static Object displayInfo(int width, int height) throws Exception {
        Class<?> type = Class.forName("android.view.DisplayInfo");
        Object info = type.getConstructor().newInstance();
        type.getField("name").set(info, "Bifrost");
        type.getField("uniqueId").set(info, "bifrost:0");
        for (String field : new String[] {"appWidth", "logicalWidth", "largestNominalAppWidth"})
            type.getField(field).setInt(info, width);
        for (String field : new String[] {"appHeight", "logicalHeight", "largestNominalAppHeight"})
            type.getField(field).setInt(info, height);
        type.getField("smallestNominalAppWidth").setInt(info, Math.min(width, height));
        type.getField("smallestNominalAppHeight").setInt(info, Math.min(width, height));
        type.getField("logicalDensityDpi").setInt(info, 160);
        type.getField("physicalXDpi").setFloat(info, 160);
        type.getField("physicalYDpi").setFloat(info, 160);
        type.getField("state").setInt(info, 2);  // Display.STATE_ON
        type.getField("type").setInt(info, 1);   // Display.TYPE_INTERNAL
        Class<?> modeType = Class.forName("android.view.Display$Mode");
        Object mode = modeType.getConstructor(int.class, int.class, int.class, float.class)
                .newInstance(1, width, height, 60f);
        Object modes = Array.newInstance(modeType, 1);
        Array.set(modes, 0, mode);
        type.getField("supportedModes").set(info, modes);
        type.getField("appsSupportedModes").set(info, modes);
        type.getField("modeId").setInt(info, 1);
        type.getField("defaultModeId").setInt(info, 1);
        return info;
    }

    private static void installServices() throws Exception {
        // The runner exposes both credential and device protected storage
        // for user 0, so this single user is running and unlocked.
        service("user", "android.os.IUserManager", (method, a) -> {
            switch (method) {
                case "isUserUnlockingOrUnlocked":
                case "isUserUnlocked": return ((Integer) a[0]) == 0;
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
        service("mount", "android.os.storage.IStorageManager", (method, a) -> {
            if (method.equals("getVolumeList"))
                return ((Integer) a[0]) == 0 ? new StorageVolume[] {volume} : new StorageVolume[0];
            if (method.equals("mkdirs")) {
                appDirectories((String) a[0], (String) a[1]);
                return null;
            }
            throw unsupported("mount", method);
        });
        display = displayInfo(480, 800);
        service("display", "android.hardware.display.IDisplayManager", (method, a) -> {
            switch (method) {
                case "getDisplayInfo": return ((Integer) a[0]) == 0 ? display : null;
                case "getPreferredWideGamutColorSpaceId": return 0;
                // What system_server reports without hardware-composer data.
                case "getOverlaySupport":
                    return Class.forName("android.hardware.OverlayProperties")
                            .getMethod("getDefault").invoke(null);
                default: throw unsupported("display", method);
            }
        });
        // No physical input devices exist here, so the id list holds only
        // the virtual keyboard. Host key/touch events do not pass through
        // this service: the play loop pumps them from the window backend and
        // dispatches them to the Activity directly, bypassing ViewRootImpl's
        // input stages (which are not attached here).
        service("input", "android.hardware.input.IInputManager", (method, a) -> {
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
        service("game", "android.app.IGameManagerService", (method, a) -> {
            switch (method) {
                case "getGameMode": return GameManager.GAME_MODE_STANDARD;
                case "setGameState": return null;
                default: throw unsupported("game", method);
            }
        });
        // This profile has no network: active lookups report absence, and
        // request callbacks never fire because nothing ever changes.
        service("connectivity", "android.net.IConnectivityManager", (method, a) -> {
            switch (method) {
                case "getActiveNetwork":
                case "getActiveNetworkInfo":
                case "getActiveNetworkInfoForUid": return null;
                case "requestNetwork":
                case "listenForNetwork":
                case "pendingRequestForNetwork":
                case "releaseNetworkRequest": return null;
                default: throw unsupported("connectivity", method);
            }
        });
        service("package", "android.content.pm.IPackageManager", (method, a) -> {
            switch (method) {
                case "hasSystemFeature": return false;
                case "isInstantApp": return false;  // This profile installs full APKs only.
                case "notifyPackageUse": return null;  // one-way usage report
                // (name, flags, user): null means "not installed", which the
                // client turns into NameNotFoundException.
                case "getPackageInfo":
                    return packageInfo((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getApplicationInfo":
                    return installed != null && installed.packageName.equals(a[0])
                            ? installed.applicationInfo : null;
                case "getServiceInfo":
                    return serviceInfo((ComponentName) a[0], ((Number) a[1]).longValue(),
                            (Integer) a[2]);
                case "resolveContentProvider":
                    return resolveProvider((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getInstallerPackageName":
                    if (installed == null || !installed.packageName.equals(a[0]))
                        throw new IllegalArgumentException("Unknown package: " + a[0]);
                    return null;  // Host-installed APK, no Android installer package.
                case "getInstallSourceInfo":
                    if (installed == null || !installed.packageName.equals(a[0])
                            || (Integer) a[1] != installed.applicationInfo.uid / 100000) return null;
                    return InstallSourceInfo.class.getConstructor(String.class,
                            android.content.pm.SigningInfo.class, String.class, String.class)
                            .newInstance(null, null, null, null);
                // An app may change its own components (PackageManagerService
                // rejects components of packages that are not installed).
                case "getComponentEnabledSetting":
                    return componentStates.getOrDefault(ownComponent(a[0]),
                            PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);
                case "setComponentEnabledSetting":
                    componentStates.put(ownComponent(a[0]), (Integer) a[1]);
                    return null;
                default: throw unsupported("package", method);
            }
        });
        // A fresh device's volumes: AudioSystem.DEFAULT_STREAM_VOLUME (client
        // side, read from this image) and AudioService.MAX_STREAM_VOLUME.
        int[] volumes = (int[]) Class.forName("android.media.AudioSystem")
                .getField("DEFAULT_STREAM_VOLUME").get(null);
        int[] maxVolumes = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
        if ((Boolean) Class.forName("android.os.SystemProperties")
                .getMethod("getBoolean", String.class, boolean.class)
                .invoke(null, "servicemanager.ready", false)) {
            Class.forName("android.os.ServiceManager").getMethod(
                    "addService", String.class, IBinder.class)
                    .invoke(null, "sensorservice", sensorService);
            sensorServiceRegistered = true;
            Class.forName("android.os.ServiceManager").getMethod(
                    "addService", String.class, IBinder.class)
                    .invoke(null, "SurfaceFlingerAIDL", surfaceComposer);
        }
        if (play) {
            Method add = Class.forName("android.os.ServiceManager").getMethod(
                    "addService", String.class, IBinder.class);
            add.invoke(null, "media.audio_policy", audioPolicy);
            add.invoke(null, "media.audio_flinger", audioFlinger);
        }
        service("audio", "android.media.IAudioService", (method, a) -> {
            switch (method) {
                case "getStreamVolume": return volumes[(Integer) a[0]];
                case "getStreamMaxVolume": return maxVolumes[(Integer) a[0]];
                default: throw unsupported("audio", method);
            }
        });
        // Observers are kept nowhere: no settings provider ever changes here.
        service("content", "android.content.IContentService", (method, a) -> {
            switch (method) {
                case "registerContentObserver":
                case "unregisterContentObserver": return null;
                default: throw unsupported("content", method);
            }
        });
        service("window", "android.view.IWindowManager", (method, a) -> {
            switch (method) {
                case "hasNavigationBar": return false;
                case "getCurrentAnimatorScale": return 1f;
                default: throw unsupported("window", method);
            }
        });
        // The virtual device stays awake while its host window is open. It
        // has no battery saver, idle controller, or low-power standby policy.
        service("power", "android.os.IPowerManager", (method, a) -> {
            switch (method) {
                case "isInteractive": return windowOpen;
                case "isDisplayInteractive": return (Integer) a[0] == 0 && windowOpen;
                case "isPowerSaveMode":
                case "areAutoPowerSaveModesEnabled":
                case "isDeviceIdleMode":
                case "isLightDeviceIdleMode":
                case "isLowPowerStandbySupported":
                case "isLowPowerStandbyEnabled": return false;
                default: throw unsupported("power", method);
            }
        });
        Map<IBinder, Object> thermalEvents = new ConcurrentHashMap<>();
        Map<IBinder, Object> thermalStatuses = new ConcurrentHashMap<>();
        // No virtual thermal HAL: no temperature/cooling samples and no
        // forecast. NONE is the initial status, not a host-temperature claim.
        service("thermalservice", "android.os.IThermalService", (method, a) -> {
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
        Object client = service("bifrost.activity_client",
                "android.app.IActivityClientController", (method, a) -> {
                    switch (method) {
                        case "setTaskDescription": return null;
                        // Record the first completion for a known Activity;
                        // unknown tokens are ignored, like ActivityRecord lookup.
                        case "reportActivityFullyDrawn":
                            if (orientations.containsKey((IBinder) a[0]))
                                fullyDrawn.putIfAbsent((IBinder) a[0], System.nanoTime());
                            return null;
                        // The launcher Activity was not started for a result.
                        case "getCallingPackage": return null;
                        case "finishActivity":
                            // This launcher owns one Activity, with no caller
                            // awaiting a result. Activity.finish() marks it
                            // finishing after this acknowledgement; the UI
                            // pump delivers its teardown on the next turn.
                            return orientations.containsKey((IBinder) a[0]);
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
        service("activity_task", "android.app.IActivityTaskManager", (method, a) -> {
            if (method.equals("getActivityClientController")) return client;
            throw unsupported("activity_task", method);
        });
        // Settings.Global/Secure/System reads go through a content provider
        // call; an absent value makes the framework use its default.
        Object provider = service("bifrost.settings", "android.content.IContentProvider",
                (method, a) -> {
                    if (method.equals("call") && ((String) a[2]).startsWith("GET_")) {
                        Bundle result = new Bundle();
                        result.putString("value", null);
                        return result;
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
        service("activity", "android.app.IActivityManager", (method, a) -> {
            if (method.equals("getContentProvider") && Arrays.asList(a).contains("settings"))
                return holder;
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

    private static void setField(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    // args: --app <package> <code dir> <launcher class> [<split>=<apk>]...
    private static void runApp(Object thread, Context system, String[] args) throws Exception {
        String packageName = args[1], code = args[2], launcher = args[3];
        PackageInfo packageInfo = system.getPackageManager().getPackageArchiveInfo(
                code + "/base.apk", PackageManager.GET_ACTIVITIES | PackageManager.GET_SERVICES
                        | PackageManager.GET_PROVIDERS | PackageManager.GET_META_DATA
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
        installed = packageInfo;
        if (diagnostic) checkPackageServices(system);
        System.out.println("APP_PARSED " + app.className + " " + activity.name);
        int width = 0, height = 0;
        if (play) {
            boolean landscape = Arrays.asList(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
                    ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
                    ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE,
                    ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE)
                    .contains(activity.screenOrientation);
            width = landscape ? 1280 : 720;
            height = landscape ? 720 : 1280;
            display = displayInfo(width, height);
            System.load("/data/local/tmp/libbifrost-activity.so");
            int error = openWindow(width, height);
            if (error != 0) throw new IllegalStateException("host window: " + error);
            windowOpen = true;
            System.out.println("APP_WINDOW_OPEN " + width + "x" + height);
        }

        Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
        Object loaded = call(thread, "getPackageInfoNoCheck",
                new Class<?>[] {ApplicationInfo.class, compatibility}, app,
                compatibility.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
        ClassLoader loader = (ClassLoader) call(loaded, "getClassLoader", new Class<?>[0]);
        Class<?> launcherClass = loader.loadClass(launcher);
        System.out.println("APP_CLASSLOADER_OK " + launcherClass.getName());

        // handleBindApplication names the process before creating the
        // Application; native code (Unity) reads it from /proc/<pid>/cmdline.
        call(Process.class, "setArgV0", new Class<?>[] {String.class},
                app.processName != null ? app.processName : packageName);
        Instrumentation instrumentation =
                (Instrumentation) call(thread, "getInstrumentation", new Class<?>[0]);
        Application application = (Application) call(loaded, "makeApplicationInner",
                new Class<?>[] {boolean.class, Instrumentation.class}, false, null);
        setField(thread, "mInitialApplication", application);
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
        instrumentation.callApplicationOnCreate(application);
        System.out.println("APP_APPLICATION_STARTED");

        Binder token = new Binder();
        orientations.put(token, activity.screenOrientation);
        Context activityContext = (Context) call(Class.forName("android.app.ContextImpl"),
                "createActivityContext", new Class<?>[] {thread.getClass(), loaded.getClass(),
                        ActivityInfo.class, IBinder.class, int.class, Configuration.class},
                thread, loaded, activity, token, 0, null);
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(packageName, launcher);
        Activity instance = instrumentation.newActivity(launcherClass, activityContext, token,
                application, intent, activity, activity.loadLabel(system.getPackageManager()),
                null, null, null);
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
            play(instrumentation, instance, width, height);
            return;
        }
        System.exit(0);
    }

    private static void findSurfaceViews(View view, List<SurfaceView> out) {
        if (view instanceof SurfaceView) out.add((SurfaceView) view);
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
                findSurfaceViews(((ViewGroup) view).getChildAt(i), out);
    }

    // What ViewRootImpl.performTraversals and SurfaceView.updateSurface do
    // for a new or resized window: lay out the decor, then report each
    // SurfaceView's surface through its registered holder callbacks.
    @SuppressWarnings("unchecked")
    private static void layoutAndReportSurfaces(Activity activity, int width, int height,
            boolean created) throws Exception {
        View decor = activity.getWindow().getDecorView();
        decor.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        decor.layout(0, 0, width, height);
        List<SurfaceView> views = new ArrayList<>();
        findSurfaceViews(decor, views);
        for (SurfaceView view : views) {
            Field frame = SurfaceView.class.getDeclaredField("mSurfaceFrame");
            frame.setAccessible(true);
            ((Rect) frame.get(view)).set(0, 0, view.getWidth(), view.getHeight());
            Field field = SurfaceView.class.getDeclaredField("mCallbacks");
            field.setAccessible(true);
            SurfaceHolder holder = view.getHolder();
            Field requestedFormat = SurfaceView.class.getDeclaredField("mRequestedFormat");
            requestedFormat.setAccessible(true);
            int format = requestedFormat.getInt(view);
            if (created) {
                long window = prepareSurface(holder.getSurface(), !PixelFormat.formatHasAlpha(format));
                if (window <= 0) throw new IllegalStateException("surface preparation: " + window);
                surfaceWindows.put(view, window);
            }
            for (SurfaceHolder.Callback callback :
                    new ArrayList<>((List<SurfaceHolder.Callback>) field.get(view))) {
                if (created) callback.surfaceCreated(holder);
                callback.surfaceChanged(holder, format,
                        view.getWidth(), view.getHeight());
            }
            System.out.println("APP_SURFACE " + view.getClass().getName() + " "
                    + view.getWidth() + "x" + view.getHeight() + " format=" + format);
        }
        if (views.isEmpty()) throw new IllegalStateException("Activity has no SurfaceView");
    }

    // Converts one op-3 event record (activity_window.c) into a framework
    // input event and dispatches it like ViewRootImpl's input stages.
    private static void dispatch(Activity activity, long[] e) {
        final long ms = 1000000;
        if (e[0] == 1) {  // AINPUT_EVENT_TYPE_KEY
            activity.dispatchKeyEvent(new KeyEvent(e[11] / ms, e[12] / ms, (int) e[4],
                    (int) e[8], (int) e[10], (int) e[7], (int) e[6], (int) e[9],
                    (int) e[19], (int) e[5]));
        } else if (e[0] == 2) {  // AINPUT_EVENT_TYPE_MOTION, first pointer only
            MotionEvent event = MotionEvent.obtain(e[11] / ms, e[12] / ms, (int) e[4],
                    Float.intBitsToFloat((int) e[14]), Float.intBitsToFloat((int) e[15]),
                    Float.intBitsToFloat((int) e[16]), Float.intBitsToFloat((int) e[17]),
                    0, 1f, 1f, (int) e[6], 0);
            event.setSource((int) e[5]);
            if ((e[5] & InputDevice.SOURCE_CLASS_POINTER) != 0) activity.dispatchTouchEvent(event);
            else activity.dispatchGenericMotionEvent(event);
            event.recycle();
        }
    }

    @SuppressWarnings("unchecked")
    private static void closeActivity(Activity activity) throws Exception {
        windowOpen = false;
        activity.onWindowFocusChanged(false);
        call(activity, "performPause", new Class<?>[0]);
        System.out.println("APP_PAUSED");
        for (SurfaceView view : new ArrayList<>(surfaceWindows.keySet())) {
            Field callbacks = SurfaceView.class.getDeclaredField("mCallbacks");
            callbacks.setAccessible(true);
            for (SurfaceHolder.Callback callback :
                    new ArrayList<>((List<SurfaceHolder.Callback>) callbacks.get(view)))
                callback.surfaceDestroyed(view.getHolder());
        }
        call(activity, "performStop", new Class<?>[] {boolean.class, String.class}, false, "bifrost");
        System.out.println("APP_STOPPED");
        call(Class.forName("android.app.QueuedWork"), "waitToFinish", new Class<?>[0]);
        call(activity, "performDestroy", new Class<?>[0]);
        System.out.println("APP_DESTROYED");
        for (long window : surfaceWindows.values()) releaseSurface(window);
        surfaceWindows.clear();
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
        layoutAndReportSurfaces(activity, width, height, true);
        activity.onWindowFocusChanged(true);
        System.out.println("APP_SURFACE_READY");
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
                            closeActivity(activity);
                            System.exit(0);
                        }
                        if (event[2] != size[0] || event[3] != size[1]) {
                            size[0] = (int) event[2];
                            size[1] = (int) event[3];
                            layoutAndReportSurfaces(activity, size[0], size[1], false);
                        }
                        if (event[0] == 0) break;
                        dispatch(activity, event);
                    }
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
        if (args.length > 0 && args[0].equals("--play")) {
            play = true;
            args = Arrays.copyOfRange(args, 1, args.length);
        }
        Looper.prepareMainLooper();
        if (diagnostic) checkServiceRegistration();
        installServices();
        if (diagnostic && !play) checkFrameworkVsync();
        System.out.println("ACTIVITY_BOOTSTRAP_BEGIN");
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Object thread = threadType.getMethod("systemMain").invoke(null);
        Context context = (Context) threadType.getMethod("getSystemContext").invoke(thread);
        System.out.println("ACTIVITY_CONTEXT_OK " + context);
        if (diagnostic) checkRuntimeServices(context, false);
        // ActivityThread.handleBindApplication's fallback when no serialized
        // font map arrives (lazy Typeface initialization). This probe has
        // no bindApplication, and a TextView needs the default typefaces.
        Class.forName("android.graphics.Typeface")
                .getMethod("loadPreinstalledSystemFontMap").invoke(null);
        System.out.println("ACTIVITY_FONTS_OK");
        if (args.length >= 4 && args[0].equals("--app")) {
            runApp(thread, context, args);
            return;
        }
        if (args.length != 0) throw new IllegalArgumentException("unexpected arguments");

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

    public static void main(String[] args) {
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
            run(args);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
