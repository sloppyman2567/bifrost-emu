// MediaSessionService ("media_session"): the sessions an app creates for itself, as MediaSessionRecord keeps them
// (android-15.0.0_r1). An app (YouTube's MediaSessionCompat) creates a MediaSession for its playback; the session
// stores what the app publishes (metadata, playback state, queue, extras, volume, flags) and tells the controllers
// registered on it, and a controller's transport commands reach the app's ISessionCallback. Nothing else controls
// a session here: no system UI, no notification listener and no media key routing. Media keys that WebView hands
// to AudioManager.dispatchMediaKeyEvent reach nobody, as they do with no active session; sessions of other
// packages, session listeners and the custom key dispatchers are not modelled.
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.MediaMetadata;
import android.media.session.PlaybackState;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.SystemClock;
import android.view.KeyEvent;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class FrameworkMediaSessions implements FrameworkServices.Call {
    private enum Contract { dispatchMediaKeyEvent, createSession }

    // ISession and ISessionController, the interfaces handed to the app.
    private enum SessionContract {
        sendEvent, getController, setFlags, setActive, setMediaButtonReceiver, setMediaButtonBroadcastReceiver,
        setLaunchPendingIntent, destroySession, setMetadata, setPlaybackState, resetQueue, getBinderForSetQueue,
        setQueueTitle, setExtras, setRatingType, setPlaybackToLocal, setPlaybackToRemote, setCurrentVolume
    }
    private enum ControllerContract {
        sendCommand, sendMediaButton, registerCallback, unregisterCallback, getPackageName, getTag, getSessionInfo,
        getLaunchPendingIntent, getFlags, getVolumeAttributes, adjustVolume, setVolumeTo, prepare, prepareFromMediaId,
        prepareFromSearch, prepareFromUri, play, playFromMediaId, playFromSearch, playFromUri, skipToQueueItem, pause,
        stop, next, previous, fastForward, rewind, seekTo, rate, setPlaybackSpeed, sendCustomAction, getMetadata,
        getPlaybackState, getQueue, getQueueTitle, getExtras, getRatingType
    }

    @Override public String[] methods() { return names(Contract.values()); }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    // MediaSessionService.SESSION_CREATION_LIMIT_PER_UID (skipped for a caller with MEDIA_CONTENT_CONTROL, which an
    // app lacks). Every package in this process runs as one uid.
    private static final int SESSION_CREATION_LIMIT_PER_UID = 100;
    // MediaController.PlaybackInfo.PLAYBACK_TYPE_*, VolumeProvider.VOLUME_CONTROL_ABSOLUTE.
    private static final int PLAYBACK_TYPE_LOCAL = 1, PLAYBACK_TYPE_REMOTE = 2, VOLUME_CONTROL_ABSOLUTE = 2;

    private final FrameworkPackages packages;
    private FrameworkServices services;
    private int sessionCount;
    // Controller callbacks are oneway: deliver them in order on one thread, as MediaSessionRecord's handler does.
    private final ExecutorService deliveries = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "media-session-callbacks");
        thread.setDaemon(true);
        return thread;
    });

    FrameworkMediaSessions(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        this.services = services;
        services.register("media_session", "android.media.session.ISessionManager", this);
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("media_session." + method); }
        if (contract == Contract.createSession) return createSession((String) a[0], a[1], (String) a[2], (Bundle) a[3], (Integer) a[4]);
        // dispatchMediaKeyEvent(packageName, asSystemService, keyEvent, needWakeLock)
        enforcePackage((String) a[0]);
        // A null or non-media key is dropped (with a warning in AOSP); a media key
        // finds no session, user-set media-button receiver or media key listener.
        KeyEvent event = (KeyEvent) a[2];
        if (event == null || !KeyEvent.isMediaSessionKey(event.getKeyCode()))
            android.util.Log.w("MediaSessionService", "Attempted to dispatch null or non-media key event.");
        return null;
    }

    private void enforcePackage(String packageName) {
        if (packageName == null || packages.packageUid(packageName) != Process.myUid())
            throw new SecurityException("Package " + packageName + " does not belong to " + Process.myUid());
    }

    // createSession(packageName, ISessionCallback, tag, sessionInfo, userId) -> ISession
    private synchronized Object createSession(String packageName, Object callback, String tag, Bundle info, int userId)
            throws Exception {
        enforcePackage(packageName);
        if (userId != Process.myUid() / 100000) throw new SecurityException("Foreign user " + userId);
        if (callback == null) throw new IllegalArgumentException("Controller callback cannot be null");
        if (sessionCount >= SESSION_CREATION_LIMIT_PER_UID)
            throw new RuntimeException("Created too many sessions. count=" + sessionCount + ")");
        sessionCount++;
        return new Session(packageName, callback, tag, info == null ? null : new Bundle(info)).session;
    }

    private synchronized void released() {
        sessionCount--;
    }

    // One MediaSessionRecord: the state, and the ISession and ISessionController views of it.
    private final class Session {
        final String packageName, tag;
        final Bundle info;
        final Object sessionCallback;  // the app's ISessionCallback
        final Object session, controller;
        private final List<Object> controllerCallbacks = new ArrayList<>();
        private boolean destroyed, active;
        private long flags, duration = -1;
        private PendingIntent launchIntent, buttonReceiver;
        private ComponentName buttonBroadcastReceiver;
        private MediaMetadata metadata;
        private PlaybackState playbackState;
        private List<Object> queue;
        private CharSequence queueTitle;
        private Bundle extras;
        private int ratingType;
        private int volumeType = PLAYBACK_TYPE_LOCAL, volumeControl, maxVolume, currentVolume;
        private String volumeControlId;
        private AudioAttributes attributes = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();

        Session(String packageName, Object sessionCallback, String tag, Bundle info) throws Exception {
            this.packageName = packageName; this.tag = tag; this.info = info; this.sessionCallback = sessionCallback;
            controller = services.local("media_session.controller", "android.media.session.ISessionController",
                    new ControllerCall());
            session = services.local("media_session.session", "android.media.session.ISession", new SessionCall());
        }

        // ISessionControllerCallback is oneway: queue each call for every registered callback.
        void tell(String method, Object... arguments) {
            List<Object> targets;
            synchronized (this) { targets = new ArrayList<>(controllerCallbacks); }
            for (Object target : targets) deliveries.execute(() -> call(target, "android.media.session.ISessionControllerCallback", method, arguments));
        }

        // The app's ISessionCallback, called as MediaSessionRecord.SessionCallbackLink does, in order: every
        // method takes (packageName of the controller, pid, uid) and then the command's own arguments.
        void command(String method, String controllerPackage, Object... rest) {
            Object[] all = new Object[rest.length + 3];
            all[0] = controllerPackage; all[1] = Process.myPid(); all[2] = Process.myUid();
            System.arraycopy(rest, 0, all, 3, rest.length);
            deliveries.execute(() -> call(sessionCallback, "android.media.session.ISessionCallback", method, all));
        }

        // The state a controller reads: a playing state's position moves on with the clock
        // (MediaSessionRecord.getStateWithUpdatedPosition).
        synchronized PlaybackState stateWithUpdatedPosition() {
            PlaybackState state = playbackState;
            if (state == null) return null;
            int kind = state.getState();
            if (kind != PlaybackState.STATE_PLAYING && kind != PlaybackState.STATE_FAST_FORWARDING
                    && kind != PlaybackState.STATE_REWINDING) return state;
            long updated = state.getLastPositionUpdateTime(), now = SystemClock.elapsedRealtime();
            if (updated <= 0) return state;
            long position = (long) (state.getPlaybackSpeed() * (now - updated)) + state.getPosition();
            if (duration >= 0 && position > duration) position = duration;
            else if (position < 0) position = 0;
            PlaybackState.Builder builder = new PlaybackState.Builder(state);
            builder.setState(kind, position, state.getPlaybackSpeed(), now);
            return builder.build();
        }

        Object playbackInfo() throws Exception {
            int type, control, max, current;
            String controlId;
            AudioAttributes used;
            synchronized (this) {
                type = volumeType; control = volumeControl; max = maxVolume; current = currentVolume;
                controlId = volumeControlId; used = attributes;
            }
            if (type == PLAYBACK_TYPE_LOCAL) {
                android.content.Context context = (android.content.Context) Class.forName("android.app.ActivityThread")
                        .getMethod("currentApplication").invoke(null);
                android.media.AudioManager audio = context.getSystemService(android.media.AudioManager.class);
                control = VOLUME_CONTROL_ABSOLUTE;
                max = audio.getStreamMaxVolume(android.media.AudioManager.STREAM_MUSIC);
                current = audio.getStreamVolume(android.media.AudioManager.STREAM_MUSIC);
                controlId = null;
            }
            Class<?> info = Class.forName("android.media.session.MediaController$PlaybackInfo");
            Constructor<?> constructor = info.getDeclaredConstructor(int.class, int.class, int.class, int.class,
                    AudioAttributes.class, String.class);
            constructor.setAccessible(true);
            return constructor.newInstance(type, control, max, current, used, controlId);
        }

        // ISession.
        final class SessionCall implements FrameworkServices.Call {
            @Override public String[] methods() { return names(SessionContract.values()); }

            @Override public Object invoke(String method, Object[] a) throws Exception {
                SessionContract contract;
                try { contract = SessionContract.valueOf(method); }
                catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("media_session.session." + method); }
                switch (contract) {
                    case sendEvent: {
                        Bundle data = a[1] == null ? null : new Bundle((Bundle) a[1]);
                        tell("onEvent", a[0], data);
                        return null;
                    }
                    case getController: return controller;
                    case setFlags:
                        // FLAG_EXCLUSIVE_GLOBAL_PRIORITY (1 << 16) needs MODIFY_PHONE_STATE, which an app lacks.
                        if (((Integer) a[0] & (1 << 16)) != 0)
                            throw new SecurityException("Must be phone or system to set the global priority flag");
                        synchronized (Session.this) { flags = (Integer) a[0]; }
                        return null;
                    case setActive: synchronized (Session.this) { active = (Boolean) a[0]; } return null;
                    case setMediaButtonReceiver:
                        // MediaSessionRecord ignores a receiver that starts an activity (and logs a warning).
                        if (a[0] != null && ((PendingIntent) a[0]).isActivity()) {
                            android.util.Log.w("MediaSessionRecord", "Ignoring invalid media button receiver targeting an activity.");
                            return null;
                        }
                        synchronized (Session.this) { buttonReceiver = (PendingIntent) a[0]; }
                        return null;
                    case setMediaButtonBroadcastReceiver:
                        if (a[0] != null && !packageName.equals(((ComponentName) a[0]).getPackageName()))
                            throw new IllegalArgumentException("receiver does not belong to package name=" + packageName);
                        synchronized (Session.this) { buttonBroadcastReceiver = (ComponentName) a[0]; }
                        return null;
                    case setLaunchPendingIntent: synchronized (Session.this) { launchIntent = (PendingIntent) a[0]; } return null;
                    case destroySession: destroy(); return null;
                    case setMetadata: {
                        MediaMetadata copy = a[0] == null ? null : new MediaMetadata.Builder((MediaMetadata) a[0]).build();
                        synchronized (Session.this) { metadata = copy; duration = (Long) a[1]; }
                        tell("onMetadataChanged", copy);
                        return null;
                    }
                    case setPlaybackState:
                        synchronized (Session.this) { playbackState = (PlaybackState) a[0]; }
                        tell("onPlaybackStateChanged", a[0]);
                        return null;
                    case resetQueue:
                        synchronized (Session.this) { queue = null; }
                        tell("onQueueChanged", (Object) null);
                        return null;
                    case getBinderForSetQueue: return queueBinder();
                    case setQueueTitle:
                        synchronized (Session.this) { queueTitle = (CharSequence) a[0]; }
                        tell("onQueueTitleChanged", a[0]);
                        return null;
                    case setExtras: {
                        Bundle copy = a[0] == null ? null : new Bundle((Bundle) a[0]);
                        synchronized (Session.this) { extras = copy; }
                        tell("onExtrasChanged", copy);
                        return null;
                    }
                    case setRatingType: synchronized (Session.this) { ratingType = (Integer) a[0]; } return null;
                    case setPlaybackToLocal: {
                        boolean changed;
                        synchronized (Session.this) {
                            changed = volumeType != PLAYBACK_TYPE_LOCAL;
                            volumeType = PLAYBACK_TYPE_LOCAL; volumeControlId = null;
                            if (a[0] != null) attributes = (AudioAttributes) a[0];
                            else android.util.Log.d("MediaSessionRecord", "Received null audio attributes, using existing attributes");
                        }
                        if (changed) tell("onVolumeInfoChanged", playbackInfo());
                        return null;
                    }
                    case setPlaybackToRemote: {
                        boolean changed;
                        synchronized (Session.this) {
                            changed = volumeType != PLAYBACK_TYPE_REMOTE;
                            volumeType = PLAYBACK_TYPE_REMOTE;
                            volumeControl = (Integer) a[0]; maxVolume = (Integer) a[1]; volumeControlId = (String) a[2];
                        }
                        if (changed) tell("onVolumeInfoChanged", playbackInfo());
                        return null;
                    }
                    case setCurrentVolume:
                        synchronized (Session.this) { currentVolume = (Integer) a[0]; }
                        tell("onVolumeInfoChanged", playbackInfo());
                        return null;
                    default: throw new UnsupportedOperationException("media_session.session." + method);
                }
            }
        }

        // ISessionController.
        final class ControllerCall implements FrameworkServices.Call {
            @Override public String[] methods() { return names(ControllerContract.values()); }

            @Override public Object invoke(String method, Object[] a) throws Exception {
                ControllerContract contract;
                try { contract = ControllerContract.valueOf(method); }
                catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("media_session.controller." + method); }
                switch (contract) {
                    case registerCallback: {
                        Object callback = a[1];
                        boolean gone;
                        synchronized (Session.this) {
                            gone = destroyed;
                            if (!gone && !controllerCallbacks.contains(callback)) controllerCallbacks.add(callback);
                        }
                        // A destroyed session tells the new controller at once and keeps no reference to it.
                        if (gone) deliveries.execute(() -> call(callback, "android.media.session.ISessionControllerCallback", "onSessionDestroyed"));
                        return null;
                    }
                    case unregisterCallback: synchronized (Session.this) { controllerCallbacks.remove(a[0]); } return null;
                    case getPackageName: return packageName;
                    case getTag: return tag;
                    case getSessionInfo: return info == null ? null : new Bundle(info);
                    case getLaunchPendingIntent: synchronized (Session.this) { return launchIntent; }
                    case getFlags: synchronized (Session.this) { return flags; }
                    case getVolumeAttributes: return playbackInfo();
                    case getMetadata: synchronized (Session.this) { return metadata; }
                    case getPlaybackState: return stateWithUpdatedPosition();
                    case getQueue: synchronized (Session.this) { return queue == null ? null : slice(queue); }
                    case getQueueTitle: synchronized (Session.this) { return queueTitle; }
                    case getExtras: synchronized (Session.this) { return extras; }
                    case getRatingType: synchronized (Session.this) { return ratingType; }
                    case sendMediaButton: {
                        // sendMediaButton(packageName, keyEvent): the key reaches the session's callback as a media button intent.
                        Intent button = new Intent(Intent.ACTION_MEDIA_BUTTON).putExtra(Intent.EXTRA_KEY_EVENT, (KeyEvent) a[1]);
                        command("onMediaButtonFromController", (String) a[0], button);
                        return true;
                    }
                    case sendCommand: command("onCommand", (String) a[0], a[1], a[2], a[3]); return null;
                    case adjustVolume:
                    case setVolumeTo:
                        // Remote playback is the app's to adjust; local playback follows the system stream volume, which
                        // nothing here changes on a controller's behalf.
                        synchronized (Session.this) {
                            if (volumeType != PLAYBACK_TYPE_REMOTE) return null;
                        }
                        command(contract == ControllerContract.adjustVolume ? "onAdjustVolume" : "onSetVolumeTo", (String) a[0], a[2]);
                        return null;
                    default: {
                        // The transport commands: play(packageName) is onPlay(packageName, pid, uid), and so on.
                        String target = TRANSPORT.get(method);
                        if (target == null) throw new UnsupportedOperationException("media_session.controller." + method);
                        command(target, (String) a[0], java.util.Arrays.copyOfRange(a, 1, a.length));
                        return null;
                    }
                }
            }
        }

        void destroy() {
            List<Object> targets;
            synchronized (this) {
                if (destroyed) return;
                destroyed = true;
                targets = new ArrayList<>(controllerCallbacks);
                controllerCallbacks.clear();
            }
            for (Object target : targets)
                deliveries.execute(() -> call(target, "android.media.session.ISessionControllerCallback", "onSessionDestroyed"));
            released();
        }

        // ParcelableListBinder: the app sends its queue (a list of QueueItem) through this binder.
        private IBinder queueBinder() throws Exception {
            Constructor<?> constructor = Class.forName("android.media.session.ParcelableListBinder")
                    .getConstructor(Class.class, java.util.function.Consumer.class);
            java.util.function.Consumer<List<Object>> consumer = list -> {
                synchronized (Session.this) { queue = new ArrayList<>(list); }
                try { tell("onQueueChanged", slice(list)); } catch (Exception error) { throw new IllegalStateException(error); }
            };
            return (IBinder) constructor.newInstance(android.media.session.MediaSession.QueueItem.class, consumer);
        }
    }

    // MediaController transport command -> ISessionCallback method.
    private static final Map<String, String> TRANSPORT = new HashMap<>();
    static {
        for (String name : new String[] {"prepare", "prepareFromMediaId", "prepareFromSearch", "prepareFromUri", "play",
                "playFromMediaId", "playFromSearch", "playFromUri", "pause", "stop", "next", "previous", "fastForward",
                "rewind", "seekTo", "rate", "setPlaybackSpeed"})
            TRANSPORT.put(name, "on" + Character.toUpperCase(name.charAt(0)) + name.substring(1));
        TRANSPORT.put("skipToQueueItem", "onSkipToTrack");
        TRANSPORT.put("sendCustomAction", "onCustomAction");
    }

    private static Object slice(List<?> list) throws Exception {
        return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class).newInstance(list);
    }

    // Calls an AIDL interface method by name; the callbacks are oneway, so a failure is the callee's and only logged.
    private static void call(Object target, String iface, String method, Object... arguments) {
        try {
            for (Method candidate : Class.forName(iface).getMethods()) {
                if (candidate.getName().equals(method) && candidate.getParameterCount() == arguments.length) {
                    candidate.invoke(target, arguments);
                    return;
                }
            }
            throw new NoSuchMethodException(iface + "." + method);
        } catch (InvocationTargetException error) {
            android.util.Log.w("MediaSessionRecord", "Remote failure in " + method, error.getCause());
        } catch (ReflectiveOperationException error) {
            android.util.Log.w("MediaSessionRecord", "Cannot call " + method, error);
        }
    }
}
