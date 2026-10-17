// Android 15 app audio control. Native PCM streams remain owned by the host
// audio backend; focus is cooperative, with loss/gain delivered to clients.
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.os.IBinder;
import android.os.IInterface;
import android.os.PersistableBundle;
import android.os.Process;
import android.os.RemoteException;
import java.lang.reflect.InvocationTargetException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.HashMap;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class FrameworkAudio implements FrameworkServices.Call {
    private enum Contract {
        requestAudioFocus,
        abandonAudioFocus,
        unregisterAudioFocusClient,
        getCurrentAudioFocus,
        getStreamVolume,
        getStreamMaxVolume,
        getStreamMinVolume,
        getMode,
        getRingerModeExternal,
        isVolumeFixed,
        isSpeakerphoneOn,
        isStreamMute,
        areNavigationRepeatSoundEffectsEnabled,
        playSoundEffect,
        playSoundEffectVolume,
        isMusicActive,
        isBluetoothA2dpOn,
        startWatchingRoutes,
        registerLoudnessCodecUpdatesDispatcher,
        unregisterLoudnessCodecUpdatesDispatcher,
        startLoudnessCodecUpdates,
        stopLoudnessCodecUpdates,
        addLoudnessCodecInfo,
        removeLoudnessCodecInfo,
        getLoudnessParams,
        getSpatializerImmersiveAudioLevel,
        isSpatializerEnabled,
        isSpatializerAvailable,
        canBeSpatialized,
        registerSpatializerCallback,
        unregisterSpatializerCallback
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final List<Focus> stack = new ArrayList<>();
    private final ArrayDeque<Change> pending = new ArrayDeque<>();
    private boolean dispatching;
    private final int[] maximum = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
    private final int[] minimum = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    private int[] volumes;
    private final Map<IBinder, IBinder.DeathRecipient> routeObservers = new HashMap<>();

    private final class Focus implements IBinder.DeathRecipient {
        final String id, pkg;
        final IBinder token;
        final Object dispatcher;
        final int gain, flags;
        int loss;
        Focus(String id, String pkg, IBinder token, Object dispatcher, int gain, int flags) {
            this.id = id; this.pkg = pkg; this.token = token;
            this.dispatcher = dispatcher; this.gain = gain; this.flags = flags;
        }
        @Override public void binderDied() { removeToken(token); }
        void close() { token.unlinkToDeath(this, 0); }
    }

    private static final class Change {
        final Object dispatcher;
        final String id;
        final int value;
        Change(Focus focus, int value) {
            this.dispatcher = focus.dispatcher; this.id = focus.id; this.value = value;
        }
    }

    FrameworkAudio(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        volumes = ((int[]) Class.forName("android.media.AudioSystem")
                .getField("DEFAULT_STREAM_VOLUME").get(null)).clone();
        services.register("audio", "android.media.IAudioService", this);
    }

    private void owner(String name) {
        if (packages.packageUid(Objects.requireNonNull(name, "package")) != Process.myUid())
            throw new SecurityException("Audio package does not belong to caller");
    }

    private int stream(Object value) {
        int index = (Integer) value;
        if (index < 0 || index >= minimum.length) throw new IllegalArgumentException("Invalid audio stream");
        return index;
    }

    // Queue while holding the state lock; drain outside it. Concurrent
    // requests preserve callback order without calling clients under a lock.
    private void dispatchPending() {
        synchronized (this) {
            if (dispatching) return;
            dispatching = true;
        }
        try {
            while (true) {
                Change change;
                synchronized (this) {
                    change = pending.poll();
                    if (change == null) { dispatching = false; return; }
                }
                if (change.dispatcher == null) continue;
                try {
                    Class.forName("android.media.IAudioFocusDispatcher")
                            .getMethod("dispatchAudioFocusChange", int.class, String.class)
                            .invoke(change.dispatcher, change.value, change.id);
                } catch (ReflectiveOperationException error) {
                    Throwable cause = error instanceof InvocationTargetException
                            ? ((InvocationTargetException) error).getCause() : error;
                    if (!(cause instanceof RemoteException))
                        throw new IllegalStateException("Audio focus callback failed", cause);
                }
            }
        } catch (RuntimeException error) {
            synchronized (this) { dispatching = false; }
            throw error;
        }
    }

    private Focus top() { return stack.isEmpty() ? null : stack.get(stack.size() - 1); }

    private void restore(List<Change> changes) {
        Focus next = top();
        if (next != null && next.loss != 0) {
            next.loss = 0; changes.add(new Change(next, AudioManager.AUDIOFOCUS_GAIN));
        }
    }

    private void removeToken(IBinder token) {
        List<Change> changes = new ArrayList<>();
        synchronized (this) {
            boolean wasTop = top() != null && top().token.equals(token);
            for (int i = stack.size() - 1; i >= 0; i--)
                if (stack.get(i).token.equals(token)) stack.remove(i).close();
            if (wasTop) restore(changes);
            pending.addAll(changes);
        }
        dispatchPending();
    }

    // MediaFocusControl propagates losses through every retained owner. A
    // weaker request must not weaken an earlier transient/permanent loss.
    private int request(Object[] a) throws Exception {
        Objects.requireNonNull((AudioAttributes) a[0], "attributes");
        int gain = (Integer) a[1], flags = (Integer) a[7];
        IBinder token = (IBinder) Objects.requireNonNull(a[2], "focus token");
        String id = (String) Objects.requireNonNull(a[4], "client id"), pkg = (String) a[5];
        owner(pkg);
        if (gain < AudioManager.AUDIOFOCUS_GAIN || gain > AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE)
            throw new IllegalArgumentException("Invalid audio focus gain");
        if ((flags & ~3) != 0 || a[8] != null)
            throw new SecurityException("Audio focus locking/policy requires system audio policy");
        if (!token.pingBinder()) return AudioManager.AUDIOFOCUS_REQUEST_FAILED;
        List<Change> changes = new ArrayList<>();
        synchronized (this) {
            Focus previous = top();
            if (previous != null && previous.id.equals(id) && previous.pkg.equals(pkg)
                    && previous.gain == gain && previous.flags == flags)
                return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            Focus next = new Focus(id, pkg, token, a[3], gain, flags);
            try { token.linkToDeath(next, 0); }
            catch (RemoteException dead) { return AudioManager.AUDIOFOCUS_REQUEST_FAILED; }
            for (int i = stack.size() - 1; i >= 0; i--)
                if (stack.get(i).id.equals(id) && stack.get(i).pkg.equals(pkg)) stack.remove(i).close();
            if (stack.size() >= 100) { next.close(); return AudioManager.AUDIOFOCUS_REQUEST_FAILED; }
            int loss = gain == AudioManager.AUDIOFOCUS_GAIN ? AudioManager.AUDIOFOCUS_LOSS
                    : gain == AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK
                    ? AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK : AudioManager.AUDIOFOCUS_LOSS_TRANSIENT;
            for (int i = stack.size() - 1; i >= 0; i--) {
                Focus focus = stack.get(i);
                int change = focus.loss == 0 ? loss : Math.max(focus.loss, loss);
                if (change != focus.loss) { focus.loss = change; changes.add(new Change(focus, change)); }
                if (change == AudioManager.AUDIOFOCUS_LOSS) stack.remove(i).close();
            }
            stack.add(next);
            pending.addAll(changes);
        }
        dispatchPending();
        return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
    }

    private void abandon(String id, String pkg) {
        List<Change> changes = new ArrayList<>();
        synchronized (this) {
            Focus previous = top();
            for (int i = stack.size() - 1; i >= 0; i--)
                if (stack.get(i).id.equals(id) && stack.get(i).pkg.equals(pkg)) stack.remove(i).close();
            if (previous != top()) restore(changes);
            pending.addAll(changes);
        }
        dispatchPending();
    }

    // LoudnessCodecHelper: AAC loudness parameters for the media route. The
    // only route is the built-in speaker, CTA-2075 SPL range "small" on a
    // phone or tablet unless audio.loudness.builtin-speaker-spl-range-size
    // names one. No playback configurations are reported and the route never
    // changes, so addLoudnessCodecInfo's initial update is the only one.
    private final Map<IBinder, IInterface> loudnessDispatchers = new HashMap<>();
    private final Map<Integer, Set<Object>> loudnessSessions = new HashMap<>();
    private ExecutorService loudnessUpdates;

    private static PersistableBundle loudnessParams(Object info) throws Exception {
        int metadata = info.getClass().getField("metadataType").getInt(info);
        boolean downmixing = info.getClass().getField("isDownmixing").getBoolean(info);
        String range = (String) Class.forName("android.os.SystemProperties")
                .getMethod("get", String.class, String.class)
                .invoke(null, "audio.loudness.builtin-speaker-spl-range-size", "unknown");
        if (range.equals("unknown")) range = "small"; // phone/tablet default; other names are unknown
        boolean mpeg4 = metadata == 1, mpegD = metadata == 2; // LoudnessCodecInfo.CodecMetadataType
        PersistableBundle params = new PersistableBundle();
        switch (range) {
            case "large":
                params.putInt(android.media.MediaFormat.KEY_AAC_DRC_TARGET_REFERENCE_LEVEL, 124);
                if (mpeg4) params.putInt(android.media.MediaFormat.KEY_AAC_DRC_HEAVY_COMPRESSION, 0);
                else if (mpegD) params.putInt(android.media.MediaFormat.KEY_AAC_DRC_EFFECT_TYPE, 6);
                break;
            case "small":
                params.putInt(android.media.MediaFormat.KEY_AAC_DRC_TARGET_REFERENCE_LEVEL, 64);
                if (mpeg4) params.putInt(android.media.MediaFormat.KEY_AAC_DRC_HEAVY_COMPRESSION, 1);
                else if (mpegD) params.putInt(android.media.MediaFormat.KEY_AAC_DRC_EFFECT_TYPE, 3);
                break;
            default: // medium and unknown
                params.putInt(android.media.MediaFormat.KEY_AAC_DRC_TARGET_REFERENCE_LEVEL, 96);
                if (mpeg4) params.putInt(android.media.MediaFormat.KEY_AAC_DRC_HEAVY_COMPRESSION, downmixing ? 1 : 0);
                else if (mpegD) params.putInt(android.media.MediaFormat.KEY_AAC_DRC_EFFECT_TYPE, 6);
                break;
        }
        return params;
    }

    // ILoudnessCodecUpdatesDispatcher and addLoudnessCodecInfo are one-way:
    // deliver in order on a worker, never inside the caller's own call.
    private void dispatchLoudness(int session, PersistableBundle params) {
        List<IInterface> targets;
        synchronized (loudnessDispatchers) {
            targets = new ArrayList<>(loudnessDispatchers.values());
            if (loudnessUpdates == null) loudnessUpdates = Executors.newSingleThreadExecutor(work -> {
                Thread thread = new Thread(work, "AudioLoudness");
                thread.setDaemon(true);
                return thread;
            });
        }
        loudnessUpdates.execute(() -> {
            for (IInterface target : targets) {
                try {
                    Class.forName("android.media.ILoudnessCodecUpdatesDispatcher")
                            .getMethod("dispatchLoudnessCodecParameterChange", int.class, PersistableBundle.class)
                            .invoke(target, session, params);
                } catch (ReflectiveOperationException error) {
                    Throwable cause = error instanceof InvocationTargetException
                            ? ((InvocationTargetException) error).getCause() : error;
                    if (!(cause instanceof RemoteException))
                        throw new IllegalStateException("Loudness update failed", cause);
                }
            }
        });
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("audio." + method); }
        switch (contract) {
            case requestAudioFocus: return request(a);
            case abandonAudioFocus:
                owner((String) a[3]); abandon((String) a[1], (String) a[3]);
                return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            case unregisterAudioFocusClient: {
                List<String> owners = new ArrayList<>();
                synchronized (this) {
                    for (Focus focus : stack) if (focus.id.equals(a[0])) owners.add(focus.pkg);
                }
                for (String pkg : owners) { owner(pkg); abandon((String) a[0], pkg); }
                return null;
            }
            case getCurrentAudioFocus: synchronized (this) { return top() == null ? 0 : top().gain; }
            case getStreamVolume: return volumes[stream(a[0])];
            case getStreamMaxVolume: return maximum[stream(a[0])];
            case getStreamMinVolume: return minimum[stream(a[0])];
            case getMode: return AudioManager.MODE_NORMAL;
            case getRingerModeExternal: return AudioManager.RINGER_MODE_NORMAL;
            case isVolumeFixed:
            case isSpeakerphoneOn:
            case isStreamMute:
            case areNavigationRepeatSoundEffectsEnabled: return false;
            case playSoundEffect:
            case playSoundEffectVolume: return null;
            case isMusicActive: return !(Boolean) a[0] && ManagedLauncher.hostAudioActive();
            case isBluetoothA2dpOn: return false;
            case startWatchingRoutes: {
                IBinder token = ((IInterface) Objects.requireNonNull(a[0], "observer")).asBinder();
                synchronized (routeObservers) {
                    if (!routeObservers.containsKey(token)) {
                        IBinder.DeathRecipient death = () -> {
                            synchronized (routeObservers) { routeObservers.remove(token); }
                        };
                        try { token.linkToDeath(death, 0); routeObservers.put(token, death); }
                        catch (RemoteException dead) { /* A dead observer receives no changes. */ }
                    }
                }
                // A fixed local speaker route (MAIN_SPEAKER == 0), no Bluetooth.
                // Return a fresh snapshot, as the real Binder reply does.
                return Class.forName("android.media.AudioRoutesInfo").getConstructor().newInstance();
            }
            case registerLoudnessCodecUpdatesDispatcher: {
                IInterface dispatcher = (IInterface) Objects.requireNonNull(a[0], "dispatcher");
                IBinder token = dispatcher.asBinder();
                synchronized (loudnessDispatchers) {
                    if (loudnessDispatchers.containsKey(token)) return null;
                    try {
                        token.linkToDeath(() -> {
                            synchronized (loudnessDispatchers) { loudnessDispatchers.remove(token); }
                        }, 0);
                    } catch (RemoteException dead) { return null; }
                    loudnessDispatchers.put(token, dispatcher);
                }
                return null;
            }
            case unregisterLoudnessCodecUpdatesDispatcher:
                synchronized (loudnessDispatchers) {
                    loudnessDispatchers.remove(((IInterface) Objects.requireNonNull(a[0], "dispatcher")).asBinder());
                }
                return null;
            case startLoudnessCodecUpdates:
                synchronized (loudnessSessions) { loudnessSessions.putIfAbsent((Integer) a[0], new HashSet<>()); }
                return null;
            case stopLoudnessCodecUpdates:
                synchronized (loudnessSessions) { loudnessSessions.remove((Integer) a[0]); }
                return null;
            case addLoudnessCodecInfo: {
                Object info = Objects.requireNonNull(a[2], "codecInfo");
                synchronized (loudnessSessions) {
                    Set<Object> infos = loudnessSessions.get((Integer) a[0]);
                    if (infos == null) return null; // a stopped session gets no updates
                    infos.add(info);
                }
                PersistableBundle update = new PersistableBundle();
                update.putPersistableBundle(Integer.toString((Integer) a[1]), loudnessParams(info));
                dispatchLoudness((Integer) a[0], update);
                return null;
            }
            case removeLoudnessCodecInfo:
                synchronized (loudnessSessions) {
                    Set<Object> infos = loudnessSessions.get((Integer) a[0]);
                    if (infos != null) infos.remove(a[1]);
                }
                return null;
            case getLoudnessParams: return loudnessParams(Objects.requireNonNull(a[0], "codecInfo"));
            // SpatializerHelper without a spatializer effect: STATE_NOT_SUPPORTED,
            // SPATIALIZATION_LEVEL_NONE. That state never changes, so the state
            // callbacks it registers are never called.
            case getSpatializerImmersiveAudioLevel: return 0;
            case isSpatializerEnabled:
            case isSpatializerAvailable: return false;
            case canBeSpatialized:
                Objects.requireNonNull(a[0], "attributes"); Objects.requireNonNull(a[1], "format");
                return false;
            case registerSpatializerCallback:
            case unregisterSpatializerCallback:
                Objects.requireNonNull(a[0], "callback");
                return null;
            default: throw new UnsupportedOperationException("audio." + method);
        }
    }
}
