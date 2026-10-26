// IAudioFlingerService.createTrack and the IAudioTrack it returns, for native
// AudioTrack clients (Java AudioTrack, SoundPool, MediaPlayer). The track's shared
// control block and sample ring are a memfd that the client maps; the native side
// (Guest/Android/Adapters/audio_track.c) reads it and plays through the host audio
// engine. Wire formats follow android-15.0.0_r1: IAudioFlingerService.aidl,
// IAudioTrack.aidl, CreateTrackRequest.aidl, CreateTrackResponse.aidl and the
// android.media.audio.common types.
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import java.io.FileDescriptor;
import java.lang.ref.WeakReference;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

final class FrameworkAudioTracks {
    static native int createRegion(long size);
    static native long create(int fd, long regionSize, int frameCount, int channels, int format, int sampleRate);
    static native int start(long track);
    static native void stop(long track);
    static native void pause(long track);
    static native void flush(long track);
    static native void signal(long track);
    static native int position(long track, long[] out);
    static native void release(long track);

    static final int CREATE_TRACK = 1;
    // status_t values AudioTrack understands.
    private static final int BAD_VALUE = -22, INVALID_OPERATION = -38, NO_INIT = -19, NO_MEMORY = -12;
    private static final String TRACK_DESCRIPTOR = "android.media.IAudioTrack";
    private static final int FMT_U8 = 0, FMT_I16 = 1, FMT_I32 = 2, FMT_Q8_24 = 3, FMT_F32 = 4, FMT_I24 = 5;
    private static final int CBLK_SIZE = 232; // sizeof(audio_track_cblk_t)
    // AudioChannelLayout union tags.
    private static final int LAYOUT_INDEX_MASK = 2, LAYOUT_LAYOUT_MASK = 3;
    private static final int CHANNEL_HAPTIC = (1 << 29) | (1 << 30);
    private static final int OUTPUT_ID = 1; // the one host output (see AudioFlinger.onTransact)

    // A refusal AudioTrack understands: binder::Status::fromServiceSpecificError(status_t).
    // ServiceSpecificException is hidden API, so the exception is written by hand.
    static final class Failure extends RuntimeException {
        final int status;
        Failure(int status, String message) { super(message); this.status = status; }
    }

    static void writeFailure(Parcel reply, Failure failure) {
        reply.writeInt(-8);                 // EX_SERVICE_SPECIFIC
        reply.writeString(failure.getMessage());
        reply.writeInt(0);                  // no remote stack trace
        reply.writeInt(failure.status);
    }

    private final AtomicInteger uniqueIds = new AtomicInteger(1);
    private static final Map<Integer, WeakReference<Track>> LIVE = new ConcurrentHashMap<>();

    // The native server's counters for the live track of `sessionId` (frames consumed, time,
    // underruns, peak levels of channels 0 and 1 in millionths), or null. For tests.
    static long[] snapshot(int sessionId) {
        WeakReference<Track> reference = LIVE.get(sessionId);
        Track track = reference == null ? null : reference.get();
        if (track == null) return null;
        long[] out = new long[5];
        return track.snapshot(out) == 0 ? out : null;
    }

    int newUniqueId() { return uniqueIds.getAndIncrement(); }

    // AIDL (cpp backend) writes every parcelable or union field behind a non-null marker.
    // A parcelable is then a size, its fields, and possibly fields a newer writer added.
    private static int begin(Parcel in) {
        if (in.readInt() == 0) throw new Failure(BAD_VALUE, "null parcelable");
        int start = in.dataPosition();
        int size = in.readInt();
        if (size < 4) throw new Failure(BAD_VALUE, "bad parcelable size " + size);
        return start + size;
    }

    // A nullable parcelable: the end of its body, or -1 when it is null.
    private static int beginNullable(Parcel in) {
        int marker = in.dataPosition();
        if (in.readInt() == 0) return -1;
        in.setDataPosition(marker);
        return begin(in);
    }

    private static int channelCount(int tag, int value) {
        switch (tag) {
            case LAYOUT_INDEX_MASK: return Integer.bitCount(value);
            case LAYOUT_LAYOUT_MASK:
                if ((value & CHANNEL_HAPTIC) != 0) return 0; // haptic channels are not played here
                return Integer.bitCount(value);
            default: return 0;
        }
    }

    // calculateMinFrameCount: buffer depth that covers the output latency, at least two
    // output quanta, scaled to the track's rate. A third quantum is slack for the emulator.
    private static long minFrameCount(long afFrameCount, int afSampleRate, int sampleRate, float speed) {
        return Math.max((long) Math.ceil(afFrameCount * 3.0 * sampleRate * speed / afSampleRate), 1);
    }

    // Parses CreateTrackRequest from `data` (positioned after the interface token) and
    // writes CreateTrackResponse to `reply`, or a service-specific status_t when it refuses.
    //
    // The host stream is opened through the process's `libaaudio.so`. When that is Android's own
    // library instead of the Bifrost adapter, it plays through AudioTrack and so back into
    // createTrack on the same thread, without end until the stack overflows. A nested request on
    // the thread is refused, which fails the outer stream open and the app's AudioTrack cleanly.
    private static final ThreadLocal<Boolean> creating = new ThreadLocal<>();

    void createTrack(Parcel data, Parcel reply, long[] hostConfig) throws RemoteException {
        try {
            if (creating.get() != null)
                throw new Failure(NO_INIT, "createTrack re-entered from the host stream: libaaudio is not the Bifrost adapter");
            creating.set(Boolean.TRUE);
            try { createTrackUnchecked(data, reply, hostConfig); }
            finally { creating.remove(); }
        } catch (Failure failure) {
            System.out.println("AUDIOTRACK_REFUSED " + failure.status + " " + failure.getMessage());
            writeFailure(reply, failure);
        }
    }

    private void createTrackUnchecked(Parcel data, Parcel reply, long[] hostConfig) throws RemoteException {
        final int requestEnd = begin(data);
        int end = begin(data); // AudioAttributes
        data.setDataPosition(end);
        end = begin(data); // AudioConfig
        int baseEnd = begin(data); // AudioConfigBase
        final int sampleRate = data.readInt();
        if (data.readInt() == 0) throw new Failure(BAD_VALUE, "null channel mask");
        final int layoutTag = data.readInt(), layoutValue = data.readInt();
        int formatEnd = begin(data); // AudioFormatDescription
        final int formatType = data.readInt(), pcm = data.readInt();
        final String encoding = data.readString();
        data.setDataPosition(formatEnd);
        data.setDataPosition(baseEnd);
        data.setDataPosition(begin(data)); // AudioOffloadInfo
        data.readLong(); // AudioConfig.frameCount: the request's own is used
        data.setDataPosition(end);
        data.setDataPosition(begin(data)); // AudioClient
        int sharedEnd = beginNullable(data);
        final boolean sharedBuffer = sharedEnd >= 0;
        if (sharedBuffer) data.setDataPosition(sharedEnd);
        final int notificationsPerBuffer = data.readInt();
        final float speed = data.readFloat();
        data.readStrongBinder(); // IAudioTrackCallback
        data.readString(); // opPackageName
        final int flags = data.readInt();
        final long requestedFrames = data.readLong();
        final long requestedNotification = data.readLong();
        final int selectedDevice = data.readInt();
        int sessionId = data.readInt();
        data.setDataPosition(requestEnd);

        if (sharedBuffer) throw new Failure(INVALID_OPERATION, "static AudioTrack buffers are not supported");
        if (formatType != 1 || encoding != null && encoding.length() > 0 || pcm < FMT_U8 || pcm > FMT_I24)
            throw new Failure(BAD_VALUE, "only linear PCM tracks are supported");
        final int channels = channelCount(layoutTag, layoutValue);
        if (channels < 1 || channels > 8 || sampleRate < 1) throw new Failure(BAD_VALUE, "unsupported channel mask or rate");
        final long afFrameCount = hostConfig[1];
        final int afSampleRate = (int) hostConfig[0];
        if (afSampleRate <= 0 || afFrameCount <= 0) throw new Failure(NO_INIT, "host audio unavailable");
        final int bytesPerSample = pcm == FMT_U8 ? 1 : pcm == FMT_I16 ? 2 : pcm == FMT_I24 ? 3 : 4;

        long frameCount = Math.max(requestedFrames, minFrameCount(afFrameCount, afSampleRate, sampleRate, speed > 0 ? speed : 1f));
        // Notify at least twice (three times for a triple-buffered period) per buffer.
        long maxNotification = frameCount / 2;
        long notification = requestedNotification == 0 || requestedNotification > maxNotification ? maxNotification : requestedNotification;
        if (frameCount > Integer.MAX_VALUE / 64 / Math.max(channels * bytesPerSample, 1)) throw new Failure(NO_MEMORY, "track too large");
        if (sessionId <= 0) sessionId = newUniqueId();

        // The ring is indexed modulo the next power of two (TrackBase allocates roundup(frameCount)).
        final long ringFrames = Long.highestOneBit(frameCount) == frameCount ? frameCount : Long.highestOneBit(frameCount) << 1;
        final long region = CBLK_SIZE + ringFrames * channels * bytesPerSample;
        int fd = createRegion(region);
        if (fd < 0) throw new Failure(NO_MEMORY, "memfd " + fd);
        long handle = create(fd, region, (int) frameCount, channels, pcm, sampleRate);
        if (handle <= 0) {
            ParcelFileDescriptor.adoptFd(fd); // closed by garbage collection of the unused descriptor
            throw new Failure(handle == -38 ? INVALID_OPERATION : NO_INIT, "native track " + handle);
        }
        ParcelFileDescriptor descriptor = ParcelFileDescriptor.adoptFd(fd);
        Track track = new Track(handle, descriptor, region, sessionId);
        LIVE.put(sessionId, new WeakReference<>(track));
        final int latencyMs = (int) ((afFrameCount * 1000 + afSampleRate - 1) / afSampleRate);

        reply.writeNoException();
        reply.writeInt(1); // non-null CreateTrackResponse
        int start = reply.dataPosition();
        reply.writeInt(0); // size, patched below
        reply.writeInt(flags & ~0x4);                        // flags: fast tracks are not granted
        reply.writeLong(frameCount);                         // frameCount
        reply.writeLong(notification);                       // notificationFrameCount
        reply.writeInt(selectedDevice);                      // selectedDeviceId
        reply.writeInt(sessionId);                           // sessionId
        reply.writeInt(sampleRate);                          // sampleRate
        reply.writeInt(3);                                   // streamType: MUSIC
        reply.writeLong(afFrameCount);                       // afFrameCount
        reply.writeInt(afSampleRate);                        // afSampleRate
        reply.writeInt(1);                                   // afChannelMask: non-null union
        reply.writeInt(LAYOUT_LAYOUT_MASK);                  //   tag layoutMask
        reply.writeInt(3);                                   //   LAYOUT_STEREO
        reply.writeInt(1);                                   // afFormat: non-null AudioFormatDescription
        int formatStart = reply.dataPosition();
        reply.writeInt(0);
        reply.writeInt(1);                                   //   type PCM
        reply.writeInt(FMT_I16);                             //   pcm INT_16_BIT
        reply.writeString("");                               //   encoding
        patchSize(reply, formatStart);
        reply.writeInt(latencyMs);                           // afLatencyMs
        reply.writeInt(0);                                   // afTrackFlags
        reply.writeInt(OUTPUT_ID);                           // outputId
        reply.writeInt(newUniqueId());                       // portId
        reply.writeStrongBinder(track);                      // audioTrack
        patchSize(reply, start);
    }

    private static void patchSize(Parcel p, int start) {
        int end = p.dataPosition();
        p.setDataPosition(start);
        p.writeInt(end - start);
        p.setDataPosition(end);
    }

    // IAudioTrack. Transaction codes are the method order in IAudioTrack.aidl.
    static final class Track extends Binder {
        private static final int GET_CBLK = 1, START = 2, STOP = 3, FLUSH = 4, PAUSE = 5, ATTACH_AUX_EFFECT = 6,
                SET_PARAMETERS = 7, SELECT_PRESENTATION = 8, GET_TIMESTAMP = 9, SIGNAL = 10, APPLY_VOLUME_SHAPER = 11,
                GET_VOLUME_SHAPER_STATE = 12, GET_DUAL_MONO_MODE = 13, SET_DUAL_MONO_MODE = 14,
                GET_AD_MIX_LEVEL = 15, SET_AD_MIX_LEVEL = 16, GET_PLAYBACK_RATE = 17, SET_PLAYBACK_RATE = 18;
        private final long handle;
        private final ParcelFileDescriptor region;
        private final long regionSize;
        private final int sessionId;
        private boolean released;

        Track(long handle, ParcelFileDescriptor region, long regionSize, int sessionId) {
            this.handle = handle; this.region = region; this.regionSize = regionSize; this.sessionId = sessionId;
        }

        synchronized int snapshot(long[] out) { return released ? -1 : position(handle, out); }

        @Override protected synchronized boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(TRACK_DESCRIPTOR); return true; }
            if (code < GET_CBLK || code > SET_PLAYBACK_RATE) return FrameworkServices.unknownTransaction(TRACK_DESCRIPTOR, code, super.onTransact(code, data, reply, flags));
            data.enforceInterface(TRACK_DESCRIPTOR);
            if (released) { writeFailure(reply, new Failure(NO_INIT, "track released")); return true; }
            switch (code) {
                case GET_CBLK:
                    reply.writeNoException();
                    reply.writeInt(1);               // non-null SharedFileRegion
                    int start = reply.dataPosition();
                    reply.writeInt(0);
                    reply.writeInt(1);               // non-null ParcelFileDescriptor
                    reply.writeInt(0);               //   no comm channel
                    reply.writeFileDescriptor(region.getFileDescriptor());
                    reply.writeLong(0);              // offset
                    reply.writeLong(regionSize);     // size
                    reply.writeInt(1);               // writeable: the client writes PCM and its positions
                    patchSize(reply, start);
                    break;
                case START: reply.writeNoException(); reply.writeInt(FrameworkAudioTracks.start(handle)); break;
                case STOP: FrameworkAudioTracks.stop(handle); reply.writeNoException(); break;
                case FLUSH: FrameworkAudioTracks.flush(handle); reply.writeNoException(); break;
                case PAUSE: FrameworkAudioTracks.pause(handle); reply.writeNoException(); break;
                case SIGNAL: FrameworkAudioTracks.signal(handle); reply.writeNoException(); break;
                case ATTACH_AUX_EFFECT: reply.writeNoException(); reply.writeInt(0); break;   // no effects exist
                case SET_PARAMETERS: reply.writeNoException(); reply.writeInt(0); break;
                case SELECT_PRESENTATION: reply.writeNoException(); reply.writeInt(INVALID_OPERATION); break;
                case GET_TIMESTAMP:
                    // WOULD_BLOCK: the host engine reports no presentation time. The out
                    // parameter (AudioTimestampInternal) follows the status regardless.
                    reply.writeNoException();
                    reply.writeInt(-11);
                    reply.writeInt(1);
                    int stampStart = reply.dataPosition();
                    reply.writeInt(0);
                    reply.writeLong(0);              // position
                    reply.writeLong(0);              // sec
                    reply.writeInt(0);               // nsec
                    patchSize(reply, stampStart);
                    break;
                default:
                    // Volume shapers, dual mono, description mix level and playback rate need
                    // mixer features this output does not have.
                    reply.writeException(new UnsupportedOperationException("IAudioTrack." + code));
                    break;
            }
            return true;
        }

        synchronized void close() {
            if (released) return;
            released = true;
            LIVE.remove(sessionId);
            FrameworkAudioTracks.release(handle);
            try { region.close(); } catch (java.io.IOException ignored) { }
        }

        @Override protected void finalize() throws Throwable {
            try { close(); } finally { super.finalize(); }
        }
    }
}
