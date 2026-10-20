// Android 15 input-method service for Asgard's single-user, no-installed-IME profile.
// Host key events are independent of IME sessions. The desktop's text input
// (committed text and composition) reaches the focused editor through its
// RemoteInputConnection, as an installed IME's would: an editor with a live
// InputConnection is bound to a host session, any other start stays NO_IME.
import android.content.Context;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Process;
import android.os.Parcel;
import android.os.ResultReceiver;
import android.os.SystemClock;
import android.text.InputType;
import android.util.SparseArray;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;
import java.util.function.BiPredicate;

final class FrameworkInputMethod {
    private static final String INTERNAL = "com.android.internal.inputmethod.";
    private static final String HOST_IME = "org.bifrost.asgard/.HostTextInput";

    private final Map<IBinder, Client> clients = new HashMap<>();
    private final Map<IBinder, Long> requests = new HashMap<>();
    private final BiPredicate<String, Integer> ownsPackage;
    private Handler callbacks;
    private Class<?> bindResult, sessionType, connectionType, headerType;
    private FrameworkServices services;
    private Object tracker;
    private Client focused;  // the client whose editor receives host text
    private int binds;

    private final class Client implements IBinder.DeathRecipient {
        final IInterface endpoint;
        final Object fallbackConnection;
        final int uid = Binder.getCallingUid(), pid = Binder.getCallingPid(), display;
        IBinder window;
        EditorInfo editor;
        Object connection, session;  // IRemoteInputConnection and its IInputMethodSession
        int sessionId;               // the connection's current InputConnectionCommandHeader id
        Client(IInterface endpoint, Object fallbackConnection, int display) {
            this.endpoint = endpoint;
            this.fallbackConnection = fallbackConnection;
            this.display = display;
        }
        @Override public void binderDied() {
            synchronized (FrameworkInputMethod.this) {
                clients.remove(endpoint.asBinder());
                endpoint.asBinder().unlinkToDeath(this, 0);
                release(this);
            }
        }
    }

    FrameworkInputMethod(BiPredicate<String, Integer> ownsPackage) {
        this.ownsPackage = ownsPackage;
    }

    void install(FrameworkServices services) throws Exception {
        this.services = services;
        bindResult = Class.forName(INTERNAL + "InputBindResult");
        sessionType = Class.forName(INTERNAL + "IInputMethodSession");
        connectionType = Class.forName(INTERNAL + "IRemoteInputConnection");
        headerType = Class.forName(INTERNAL + "InputConnectionCommandHeader");
        tracker =services.local("input_method.tracker", INTERNAL + "IImeTracker", this::track);
        services.register("input_method", "com.android.internal.view.IInputMethodManager", this::invoke);
    }

    private synchronized Handler callbacks() {
        if (callbacks == null) {
            HandlerThread thread = new HandlerThread("AsgardInputMethod");
            thread.start();
            callbacks = new Handler(thread.getLooper());
        }
        return callbacks;
    }

    private static void user(int userId) {
        if (userId != 0) throw new SecurityException("Asgard input methods belong to user 0");
    }

    private synchronized Client client(Object value) {
        if (!(value instanceof IInterface)) throw new IllegalArgumentException("Missing IME client");
        Client client = clients.get(((IInterface) value).asBinder());
        if (client == null) throw new IllegalArgumentException("Unknown IME client");
        if (client.uid != Binder.getCallingUid() || client.pid != Binder.getCallingPid())
            throw new SecurityException("IME client belongs to another process");
        return client;
    }

    private Object result(String name) throws Exception { return bindResult.getField(name).get(null); }

    // Clears `client`'s editor focus (and the host's text input if it held it).
    private void release(Client client) {
        client.connection = null;
        if (focused == client) focused = null;
    }

    private synchronized Object start(Object[] a) throws Exception {
        user((Integer) a[10]);
        if (a[2] == null) return result("NULL");
        Client client = client(a[1]);
        if (client.display != 0) return result("INVALID_DISPLAY_ID");
        EditorInfo editor = (EditorInfo) a[6];
        if (editor != null && !ownsPackage.test(editor.packageName, client.uid))
            return result("INVALID_PACKAGE_NAME");
        // There is one display. The launcher supplies the window token here;
        // full WindowManager focus arbitration is not available in this profile.
        client.window = (IBinder) a[2];
        client.editor = null;
        if (editor != null) {
            Parcel parcel = Parcel.obtain();
            try {
                editor.writeToParcel(parcel, 0);
                parcel.setDataPosition(0);
                client.editor = EditorInfo.CREATOR.createFromParcel(parcel);
            } finally { parcel.recycle(); }
        }
        // An editor with a live InputConnection gets the host session; a focus
        // change to anything else ends it.
        Object connection = a[7];
        if (editor != null && connection != null && editor.inputType != InputType.TYPE_NULL) {
            client.connection = connection;
            client.sessionId = 0;  // RemoteInputConnectionImpl starts at 0
            focused = client;
            if (client.session == null)
                client.session = services.local("input_method.session", INTERNAL + "IInputMethodSession",
                        (method, arguments) -> session(client, method, arguments));
            // No input channel: key events keep dispatching straight to the window.
            return bindResult.getConstructor(int.class, sessionType, SparseArray.class,
                    Class.forName("android.view.InputChannel"), String.class, int.class, boolean.class)
                    .newInstance(0 /* SUCCESS_WITH_IME_SESSION */, client.session, null, null,
                            HOST_IME, ++binds, false);
        }
        release(client);
        return result(editor == null ? "NULL_EDITOR_INFO" : "NO_IME");
    }

    // The IInputMethodSession the client talks to (oneway). Only the calls that
    // change which connection and session id the host may use matter here.
    private Object session(Client client, String method, Object[] a) {
        synchronized (this) {
            switch (method) {
                case "invalidateInput":  // the app changed its text: older commands are stale
                    if (a[1] != null && client.connection != null) {
                        client.connection = a[1];
                        client.sessionId = (Integer) a[2];
                    }
                    break;
                case "finishInput":
                case "finishSession":
                    release(client);
                    break;
                case "updateSelection": case "viewClicked": case "updateCursor":
                case "updateCursorAnchorInfo": case "updateExtractedText":
                case "displayCompletions": case "appPrivateCommand": case "removeImeSurface":
                    break;
                default: throw new UnsupportedOperationException("input_method.session." + method);
            }
        }
        return null;
    }

    // True while an editor with a live InputConnection owns the host text input.
    synchronized boolean textActive() { return focused != null; }

    // Committed text from the desktop's input method replaces any composition.
    boolean commit(String text) { return deliver("commitText", text); }

    // The composition in progress (empty clears it).
    boolean compose(String text) { return deliver("setComposingText", text); }

    private boolean deliver(String method, String text) {
        Object connection;
        int sessionId;
        synchronized (this) {
            if (focused == null || focused.connection == null) return false;
            connection = focused.connection;
            sessionId = focused.sessionId;
        }
        try {
            Object header = headerType.getConstructor(int.class).newInstance(sessionId);
            connectionType.getMethod(method, headerType, CharSequence.class, int.class)
                    .invoke(connection, header, text, 1);  // 1: cursor after the text
            return true;
        } catch (ReflectiveOperationException failure) {
            android.util.Log.w("AsgardInputMethod", "Text delivery failed", failure);
            return false;
        }
    }

    private Object invoke(String method, Object[] a) throws Exception {
        switch (method) {
            case "addClient": {
                IInterface endpoint = (IInterface) a[0];
                if (endpoint == null) throw new IllegalArgumentException("Missing IME client");
                synchronized (this) {
                    if (clients.containsKey(endpoint.asBinder()))
                        throw new SecurityException("IME client already registered");
                    Client client = new Client(endpoint, a[1], (Integer) a[2]);
                    for (Client registered : clients.values()) {
                        if (registered.uid == client.uid && registered.pid == client.pid
                                && registered.display == client.display)
                            throw new SecurityException("IME process/display already registered");
                    }
                    // AOSP accepts untrusted display IDs at registration so
                    // getSystemService remains usable. Validate at start-input.
                    endpoint.asBinder().linkToDeath(client, 0);
                    clients.put(endpoint.asBinder(), client);
                }
                return null;
            }
            case "getInputMethodList":
            case "getEnabledInputMethodList":
                user((Integer) a[0]);
                return Class.forName(INTERNAL + "InputMethodInfoSafeList").getMethod("empty").invoke(null);
            case "getInputMethodListLegacy":
            case "getEnabledInputMethodListLegacy":
                user((Integer) a[0]);
                return new ArrayList<>();
            case "getEnabledInputMethodSubtypeList":
                user((Integer) a[2]);
                return new ArrayList<>();
            case "getCurrentInputMethodInfoAsUser":
            case "getCurrentInputMethodSubtype":
            case "getLastInputMethodSubtype":
                user((Integer) a[0]);
                return null;
            case "startInputOrWindowGainedFocus": return start(a);
            case "startInputOrWindowGainedFocusAsync": {
                Object reply = start(a);
                IInterface endpoint = (IInterface) a[1];
                int sequence = (Integer) a[12];
                callbacks().post(() -> {
                    try {
                        synchronized (this) {
                            if (!clients.containsKey(endpoint.asBinder())) return;
                        }
                        Class.forName(INTERNAL + "IInputMethodClient")
                                .getMethod("onStartInputResult", bindResult, int.class)
                                .invoke(endpoint, reply, sequence);
                    } catch (ReflectiveOperationException failure) {
                        android.util.Log.w("AsgardInputMethod", "Start-input callback failed", failure);
                    }
                });
                return null;
            }
            case "showSoftInput":
            case "hideSoftInput": {
                client(a[0]);
                // No selected IME can consume the request. Report the actual
                // unchanged visibility to ResultReceiver, never claim it showed.
                ResultReceiver receiver = (ResultReceiver) a[method.equals("showSoftInput") ? 5 : 4];
                if (receiver != null) receiver.send(InputMethodManager.RESULT_UNCHANGED_HIDDEN, null);
                finish(a[2]);
                return false;
            }
            case "getInputMethodWindowVisibleHeight": client(a[0]); return 0;
            case "showInputMethodPickerFromClient": client(a[0]); return null; // empty picker
            case "reportPerceptibleAsync":
            case "removeImeSurfaceFromWindowAsync": return null; // no IME surface exists
            case "isStylusHandwritingAvailableAsUser": user((Integer) a[0]); return false;
            case "startStylusHandwriting": client(a[0]); return null; // no stylus/IME
            case "isImeTraceEnabled": return false;
            case "startProtoDump": return null; // tracing is disabled
            case "getImeTrackerService": return tracker;
            case "showInputMethodPickerFromSystem":
            case "hideSoftInputFromServerForTest":
            case "isInputMethodPickerShownForTest":
            case "setAdditionalInputMethodSubtypes":
            case "setExplicitlyEnabledInputMethodSubtypes":
            case "removeImeSurface":
            case "startImeTrace":
            case "stopImeTrace":
            case "addVirtualStylusIdForTestSession":
            case "setStylusWindowIdleTimeoutForTest":
                throw new SecurityException("Privileged input-method operation: " + method);
            default: throw new UnsupportedOperationException("input_method." + method);
        }
    }

    private synchronized void finish(Object token) throws Exception {
        if (token != null) requests.remove((IBinder) token.getClass().getMethod("getBinder").invoke(token));
    }

    private synchronized Object track(String method, Object[] a) throws Exception {
        // Tracking is bounded and expired opportunistically; it never retains
        // clients or editors, and cannot grow on repeated failed show requests.
        long now = SystemClock.uptimeMillis();
        Iterator<Long> ages = requests.values().iterator();
        while (ages.hasNext()) if (now - ages.next() > 10000) ages.remove();
        switch (method) {
            case "onStart": {
                if ((Integer) a[1] != Process.myUid()) throw new SecurityException("IME tracker UID");
                IBinder binder = new Binder();
                if (requests.size() >= 1024) requests.clear();
                requests.put(binder, now);
                return Class.forName("android.view.inputmethod.ImeTracker$Token")
                        .getConstructor(IBinder.class, String.class).newInstance(binder, a[0]);
            }
            case "onProgress": return null;
            case "onFailed":
            case "onCancelled":
            case "onShown":
            case "onHidden":
            case "onDispatched": finish(a[0]); return null;
            case "hasPendingImeVisibilityRequests":
            case "finishTrackingPendingImeVisibilityRequests":
                throw new SecurityException("TEST_INPUT_METHOD permission required");
            default: throw new UnsupportedOperationException("input_method.tracker." + method);
        }
    }

    void check(Context context) {
        InputMethodManager manager = context.getSystemService(InputMethodManager.class);
        if (manager == null || !manager.getInputMethodList().isEmpty()
                || !manager.getEnabledInputMethodList().isEmpty()
                || manager.getCurrentInputMethodSubtype() != null)
            throw new AssertionError("No-installed-IME profile mismatch");
        System.out.println("ACTIVITY_INPUT_METHOD_OK noInstalledIme=true");
    }
}
