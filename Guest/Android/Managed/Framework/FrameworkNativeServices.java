// In-process Binder services that native (C++) platform clients reach
// directly: IAudioPolicyService, IAudioFlingerService, ISensorServer and
// ISurfaceComposer with its IDisplayEventConnection vsync channel.
// ManagedLauncher owns the instances and registers them with ServiceManager.

import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.FileDescriptor;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

final class FrameworkNativeServices {
    private FrameworkNativeServices() { }

    // Native AudioSystem clients use Binder, not ServiceManager's Java cache.
    // This profile has no Android HAL ports/patches; host audio is a separate backend.
    static final class AudioPolicy extends Binder {
        static final String DESCRIPTOR = "android.media.IAudioPolicyService";
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

    // Android 15 IAudioFlingerService: only the queries used by this profile.
    // Playback uses the managed AAudio adapter and Bifrost's host Audio engine.
    static final class AudioFlinger extends Binder {
        static final String DESCRIPTOR = "android.media.IAudioFlingerService";
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
            if (code != 23 && ManagedLauncher.audioConfig(config) != 0)
                throw new IllegalStateException("host audio configuration unavailable");
            reply.writeNoException();
            if (code == 45) reply.writeInt((int) config[0]);
            if (code == 46) reply.writeLong(config[1]);
            return true;
        }
    }

    // Native libsensor uses the hand-written ISensorServer protocol, not AIDL:
    // replies start with the count/bool itself, with no exception header.
    // This virtual device exposes no physical, dynamic or virtual-device sensors.
    static final class SensorService extends Binder {
        static final String DESCRIPTOR = "android.gui.SensorServer";
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

    // Android 15 ISurfaceComposer / IDisplayEventConnection and BitTube wire
    // formats. This virtual display uses a monotonic 60 Hz clock, as does the
    // NDK adapter. Real libgui/libandroid_runtime own the receiver and deliver
    // callbacks on the client's MessageQueue; no Java Choreographer override.
    static final long DISPLAY_PERIOD_NS = 1000000000L / 60;
    static final long PHYSICAL_DISPLAY_ID = 1;
    static final class DisplayClock implements Runnable {
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
    static final class DisplayEventConnection extends Binder {
        static final String DESCRIPTOR = "android.gui.IDisplayEventConnection";
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
        synchronized void close() {
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
    static final class SurfaceComposer extends Binder {
        static final String DESCRIPTOR = "android.gui.ISurfaceComposer";
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
}
