// MediaSessionService ("media_session") with no media sessions. AudioManager and
// WebView forward media keys here (Chromium hands unhandled keys to
// AudioManager.dispatchMediaKeyEvent); a missing service made that call throw
// a NullPointerException on Chromium's key path and abort the process. With
// no session or media-button receiver, a media key reaches nobody, as in
// MediaSessionService.dispatchMediaKeyEvent. Other methods are unsupported.
import android.os.Process;
import android.view.KeyEvent;

final class FrameworkMediaSessions implements FrameworkServices.Call {
    private enum Contract { dispatchMediaKeyEvent }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkMediaSessions(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("media_session", "android.media.session.ISessionManager", this);
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        try { Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("media_session." + method); }
        // dispatchMediaKeyEvent(packageName, asSystemService, keyEvent, needWakeLock)
        if (a[0] == null || packages.packageUid((String) a[0]) != Process.myUid())
            throw new SecurityException("Package " + a[0] + " does not belong to " + Process.myUid());
        // A null or non-media key is dropped (with a warning in AOSP); a media key
        // finds no session, user-set media-button receiver or media key listener.
        KeyEvent event = (KeyEvent) a[2];
        if (event == null || !KeyEvent.isMediaSessionKey(event.getKeyCode()))
            android.util.Log.w("MediaSessionService", "Attempted to dispatch null or non-media key event.");
        return null;
    }
}
