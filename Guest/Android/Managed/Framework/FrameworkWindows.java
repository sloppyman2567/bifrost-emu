// Framework windows other than the Activity's SurfaceView content: dialogs,
// popup menus and other WindowManager.addView windows. Android composes them
// in SurfaceFlinger; here IWindowSession lays them out like WindowLayout, the
// launcher draws each view tree in software when it is dirty, and the host
// shows the pixels as a layer above every surface (activity_window.c op 9).
// Input goes to the topmost popup the way InputDispatcher would deliver it.
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.os.IBinder;
import android.os.IInterface;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;

final class FrameworkWindows {
    private static native int showPopup(int id, int x, int y, int width, int height, int[] argb);
    private static native int removePopup(int id);

    private static final int ADD_FLAG_IN_TOUCH_MODE = 0x1;   // WindowManagerGlobal
    private static final int ADD_FLAG_APP_VISIBLE = 0x2;
    private static final int DIM = 0x40000000;  // host layer id of a window's dim layer
    private static final Class<?> CHANNEL = channelType();

    private static Class<?> channelType() {
        try { return Class.forName("android.view.InputChannel"); }
        catch (ClassNotFoundException error) { throw new AssertionError(error); }
    }

    private static final class Popup {
        final IBinder token;
        final Object window;  // ViewRootImpl.W
        final int id;
        WindowManager.LayoutParams attrs;
        Object server;  // android.view.InputChannel (hidden)
        final Rect frame = new Rect();
        final Rect shown = new Rect();
        final Rect dimShown = new Rect();
        int dimAlpha;
        int requestedWidth, requestedHeight, visibility;
        Bitmap bitmap;
        int[] pixels;
        boolean focused;
        Popup(IBinder token, Object window, int id) { this.token = token; this.window = window; this.id = id; }
    }

    private final FrameworkDisplay displays;
    private final List<Popup> popups = new ArrayList<>();
    private Object session;
    private int nextId;
    private Consumer<Boolean> activityFocus = focused -> {};
    private boolean activityFocused = true;
    private Popup touchTarget;

    FrameworkWindows(FrameworkDisplay displays) {
        this.displays = displays;
    }

    // Activity window focus follows the topmost focusable popup; the callback
    // receives the current state once installed.
    void setActivityFocus(Consumer<Boolean> callback) {
        boolean focused;
        synchronized (this) {
            activityFocus = callback;
            focused = activityFocused;
        }
        callback.accept(focused);
    }

    synchronized Object openSession(FrameworkServices services) throws Exception {
        if (session == null) session = services.local("window_session", "android.view.IWindowSession", this::call);
        return session;
    }

    private Object call(String method, Object[] a) throws Exception {
        switch (method) {
            case "addToDisplay":
            case "addToDisplayAsUser":
            case "addToDisplayWithoutInputChannel":
                return add(a);
            case "relayout":
            case "relayoutLegacy":
                return relayout(a);
            case "relayoutAsync":
                relayout(a);
                return null;
            case "remove":
                remove((IBinder) a[0]);
                return null;
            // Drawing, insets and gesture reports need no compositor state here.
            case "finishDrawing":
            case "setInsets":
            case "clearTouchableRegion":
            case "pokeDrawLock":
            case "updateRequestedVisibleTypes":
            case "updateTapExcludeRegion":
            case "reportSystemGestureExclusionChanged":
            case "reportDecorViewGestureInterceptionChanged":
            case "reportKeepClearAreasChanged":
            case "onRectangleOnScreenRequested":
            case "setOnBackInvokedCallbackInfo":
            case "performHapticFeedbackAsync":
                return null;
            case "cancelDraw":
            case "outOfMemory":
            case "performHapticFeedback":
                return false;
            case "getWindowId":
                return null;
            default:
                throw new UnsupportedOperationException("window_session." + method);
        }
    }

    private int add(Object[] a) throws Exception {
        IInterface window = (IInterface) a[0];
        Popup popup;
        synchronized (this) {
            popup = new Popup(window.asBinder(), window, ++nextId);
            popup.attrs = new WindowManager.LayoutParams();
            popup.attrs.copyFrom((WindowManager.LayoutParams) a[1]);
            popup.visibility = (Integer) a[2];
            popups.add(popup);
        }
        for (Object out : a) {
            if (CHANNEL.isInstance(out)) {
                // ViewRootImpl's input receiver requires an initialized channel.
                // Events are dispatched to the view tree directly (dispatch*).
                Object[] pair = (Object[]) CHANNEL.getMethod("openInputChannelPair", String.class)
                        .invoke(null, "bifrost-popup-" + popup.id);
                popup.server = pair[0];
                CHANNEL.getMethod("copyTo", CHANNEL).invoke(pair[1], out);
                CHANNEL.getMethod("dispose").invoke(pair[1]);
            } else if (out instanceof float[] && ((float[]) out).length > 0) {
                ((float[]) out)[0] = 1f;  // size compatibility scale
            } else if (out != null && out.getClass().getName().equals("android.view.InsetsState")) {
                displays.windowInsets(0, out);
            } else if (out instanceof Rect && isSubWindow(popup.attrs.type)) {
                ((Rect) out).set(displays.bounds());  // parent: the full-screen activity
            }
        }
        System.out.println("APP_POPUP_ADDED " + popup.id + " type=" + popup.attrs.type
                + " title=" + popup.attrs.getTitle());
        updateFocus();
        return ADD_FLAG_IN_TOUCH_MODE | ADD_FLAG_APP_VISIBLE;
    }

    private static boolean isSubWindow(int type) {
        return type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW
                && type <= WindowManager.LayoutParams.LAST_SUB_WINDOW;
    }

    private int relayout(Object[] a) throws Exception {
        Popup popup = find(((IInterface) a[0]).asBinder());
        if (popup == null) throw new IllegalArgumentException("relayout of unknown window");
        Rect display = displays.bounds();
        synchronized (this) {
            if (a[1] != null) popup.attrs.copyFrom((WindowManager.LayoutParams) a[1]);
            popup.requestedWidth = (Integer) a[2];
            popup.requestedHeight = (Integer) a[3];
            popup.visibility = (Integer) a[4];
            layout(popup, display);
        }
        View root = root(popup);
        Configuration config = root != null ? root.getResources().getConfiguration() : null;
        for (int i = 5; i < a.length; i++) fill(a[i], popup.frame, display, config);
        return 0;
    }

    // WindowLayout.computeFrames for a window without insets or cutouts.
    private static void layout(Popup popup, Rect display) {
        WindowManager.LayoutParams attrs = popup.attrs;
        int dw = display.width(), dh = display.height();
        int w = attrs.width >= 0 ? attrs.width
                : attrs.width == WindowManager.LayoutParams.MATCH_PARENT ? dw : popup.requestedWidth;
        int h = attrs.height >= 0 ? attrs.height
                : attrs.height == WindowManager.LayoutParams.MATCH_PARENT ? dh : popup.requestedHeight;
        w = Math.min(Math.max(w, 0), dw);
        h = Math.min(Math.max(h, 0), dh);
        Gravity.apply(attrs.gravity, w, h, display,
                (int) (attrs.x + attrs.horizontalMargin * dw),
                (int) (attrs.y + attrs.verticalMargin * dh), popup.frame);
        if ((attrs.flags & WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS) == 0)
            Gravity.applyDisplay(attrs.gravity, display, popup.frame);
    }

    // Fills relayout outputs: ClientWindowFrames, MergedConfiguration,
    // InsetsState, or a WindowRelayoutResult holding them.
    private void fill(Object out, Rect frame, Rect display, Configuration config) throws Exception {
        if (out == null) return;
        String type = out.getClass().getSimpleName();
        switch (type) {
            case "WindowRelayoutResult":
                for (String name : new String[] {"frames", "mergedConfiguration", "insetsState"})
                    fill(field(out, name), frame, display, config);
                break;
            case "ClientWindowFrames":
                ((Rect) field(out, "frame")).set(frame);
                ((Rect) field(out, "displayFrame")).set(display);
                ((Rect) field(out, "parentFrame")).set(display);
                out.getClass().getField("compatScale").setFloat(out, 1f);
                break;
            case "MergedConfiguration":
                // Report the configuration the window already has: dialogs
                // inherit the Activity's, and no change has been requested.
                if (config != null)
                    out.getClass().getMethod("setConfiguration", Configuration.class, Configuration.class)
                            .invoke(out, config, config);
                break;
            case "InsetsState":
                displays.windowInsets(0, out);
                break;
            default:
                break;
        }
    }

    private static Object field(Object target, String name) throws Exception {
        return target.getClass().getField(name).get(target);
    }

    private void remove(IBinder token) throws Exception {
        Popup popup;
        synchronized (this) {
            popup = find(token);
            if (popup == null) return;
            popups.remove(popup);
            if (touchTarget == popup) touchTarget = null;
        }
        removePopup(popup.id);
        hideDim(popup);
        if (popup.server != null) CHANNEL.getMethod("dispose").invoke(popup.server);
        if (popup.focused) focus(popup, false);
        if (popup.bitmap != null) popup.bitmap.recycle();
        System.out.println("APP_POPUP_REMOVED " + popup.id);
        updateFocus();
    }

    private synchronized Popup find(IBinder token) {
        for (Popup popup : popups) if (popup.token == token) return popup;
        return null;
    }

    private static View root(Popup popup) throws Exception {
        Field ancestor = popup.window.getClass().getDeclaredField("mViewAncestor");
        ancestor.setAccessible(true);
        Object viewRoot = ((WeakReference<?>) ancestor.get(popup.window)).get();
        if (viewRoot == null) return null;
        Field view = viewRoot.getClass().getDeclaredField("mView");
        view.setAccessible(true);
        return (View) view.get(viewRoot);
    }

    private static Object viewRoot(Popup popup) throws Exception {
        Field ancestor = popup.window.getClass().getDeclaredField("mViewAncestor");
        ancestor.setAccessible(true);
        return ((WeakReference<?>) ancestor.get(popup.window)).get();
    }

    private static boolean focusable(Popup popup) {
        return popup.visibility == View.VISIBLE
                && (popup.attrs.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0;
    }

    private Popup topFocusable() {
        for (int i = popups.size() - 1; i >= 0; i--)
            if (focusable(popups.get(i))) return popups.get(i);
        return null;
    }

    // One focused window: the topmost focusable popup, else the Activity.
    private void updateFocus() throws Exception {
        Popup top;
        List<Popup> others = new ArrayList<>();
        synchronized (this) {
            top = topFocusable();
            for (Popup popup : popups) if (popup != top && popup.focused) others.add(popup);
        }
        for (Popup popup : others) focus(popup, false);
        if (top != null && !top.focused) focus(top, true);
        if (activityFocused != (top == null)) {
            activityFocused = top == null;
            activityFocus.accept(activityFocused);
        }
    }

    private static void focus(Popup popup, boolean focused) throws Exception {
        popup.focused = focused;
        Object viewRoot = viewRoot(popup);
        if (viewRoot == null) return;
        Method change = viewRoot.getClass().getDeclaredMethod("windowFocusChanged", boolean.class);
        change.setAccessible(true);
        change.invoke(viewRoot, focused);
    }

    // Called from the launcher's UI loop: redraw dirty or moved popups.
    void render() throws Exception {
        List<Popup> visible = new ArrayList<>();
        synchronized (this) { visible.addAll(popups); }
        for (Popup popup : visible) {
            View root = root(popup);
            int w = popup.frame.width(), h = popup.frame.height();
            boolean showing = popup.visibility == View.VISIBLE && root != null && w > 0 && h > 0
                    && root.getWidth() > 0 && root.getHeight() > 0;
            if (!showing) {
                if (!popup.shown.isEmpty()) { removePopup(popup.id); popup.shown.setEmpty(); }
                hideDim(popup);
                continue;
            }
            if (root.isLayoutRequested()) continue;  // ViewRootImpl lays out before the next draw
            dim(popup);
            if (popup.shown.equals(popup.frame) && !root.isDirty()) continue;
            if (popup.bitmap == null || popup.bitmap.getWidth() != w || popup.bitmap.getHeight() != h) {
                if (popup.bitmap != null) popup.bitmap.recycle();
                popup.bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                popup.pixels = new int[w * h];
            }
            popup.bitmap.eraseColor(0);
            Canvas canvas = new Canvas(popup.bitmap);
            canvas.clipRect(0, 0, w, h);
            root.draw(canvas);
            popup.bitmap.getPixels(popup.pixels, 0, w, 0, 0, w, h);
            int result = showPopup(popup.id, popup.frame.left, popup.frame.top, w, h, popup.pixels);
            if (result == 0) {
                if (popup.shown.isEmpty()) {
                    StringBuilder text = new StringBuilder();
                    collectText(root, text);
                    System.out.println("APP_POPUP_SHOWN " + popup.id + " " + popup.frame.toShortString()
                            + (text.length() > 0 ? " text=" + text : ""));
                }
                popup.shown.set(popup.frame);
            }
            else if (result != -11) {  // EAGAIN: host buffers busy, retry next frame
                throw new IllegalStateException("popup window " + popup.id + ": host error " + result);
            }
        }
    }

    // Visible text of a popup (dialog title, message, buttons) for the log:
    // apps often explain a stall only in a dialog.
    private static void collectText(View view, StringBuilder out) {
        if (view.getVisibility() != View.VISIBLE || out.length() > 512) return;
        if (view instanceof android.widget.TextView) {
            CharSequence text = ((android.widget.TextView) view).getText();
            if (text != null && text.length() > 0)
                out.append(out.length() > 0 ? " | " : "").append(text.toString().replace('\n', ' '));
        }
        if (view instanceof android.view.ViewGroup) {
            android.view.ViewGroup group = (android.view.ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) collectText(group.getChildAt(i), out);
        }
    }

    // FLAG_DIM_BEHIND: SurfaceFlinger's dim layer, a translucent black layer
    // spanning the display directly below the window.
    private void dim(Popup popup) throws Exception {
        if ((popup.attrs.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) == 0) {
            hideDim(popup);
            return;
        }
        Rect display = displays.bounds();
        int alpha = Math.round(Math.max(0f, Math.min(1f, popup.attrs.dimAmount)) * 255);
        if (display.equals(popup.dimShown) && alpha == popup.dimAlpha) return;
        int[] pixels = new int[display.width() * display.height()];
        java.util.Arrays.fill(pixels, alpha << 24);
        int result = showPopup(DIM | popup.id, 0, 0, display.width(), display.height(), pixels);
        if (result == -11) return;  // EAGAIN: retry next frame
        if (result != 0) throw new IllegalStateException("dim layer " + popup.id + ": host error " + result);
        if (popup.dimShown.isEmpty() && !popup.shown.isEmpty()) {
            // Layers stack in creation order: recreate the window above its dim.
            removePopup(popup.id);
            popup.shown.setEmpty();
        }
        popup.dimShown.set(display);
        popup.dimAlpha = alpha;
    }

    private static void hideDim(Popup popup) {
        if (popup.dimShown.isEmpty()) return;
        removePopup(DIM | popup.id);
        popup.dimShown.setEmpty();
    }

    // Touch delivery. Returns false when the Activity should receive it.
    boolean dispatchPointer(MotionEvent event) throws Exception {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_HOVER_MOVE || action == MotionEvent.ACTION_HOVER_ENTER
                || action == MotionEvent.ACTION_HOVER_EXIT) {
            Popup top = hit(event);
            return top != null && deliver(top, event, true);
        }
        if (action == MotionEvent.ACTION_DOWN) {
            touchTarget = null;
            List<Popup> stack;
            synchronized (this) { stack = new ArrayList<>(popups); }
            for (int i = stack.size() - 1; i >= 0; i--) {
                Popup popup = stack.get(i);
                if (popup.visibility != View.VISIBLE) continue;
                int flags = popup.attrs.flags;
                if ((flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) continue;
                boolean inside = popup.frame.contains((int) event.getX(), (int) event.getY());
                boolean modal = (flags & WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL) == 0;
                if (inside || modal) { touchTarget = popup; break; }
                if ((flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0) {
                    MotionEvent outside = MotionEvent.obtain(event);
                    outside.setAction(MotionEvent.ACTION_OUTSIDE);
                    deliver(popup, outside, false);
                    outside.recycle();
                }
            }
        }
        Popup target = touchTarget;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) touchTarget = null;
        if (target == null) return false;
        return deliver(target, event, false);
    }

    private Popup hit(MotionEvent event) {
        synchronized (this) {
            for (int i = popups.size() - 1; i >= 0; i--) {
                Popup popup = popups.get(i);
                if (popup.visibility == View.VISIBLE
                        && popup.frame.contains((int) event.getX(), (int) event.getY())) return popup;
            }
        }
        return null;
    }

    private static boolean deliver(Popup popup, MotionEvent event, boolean generic) throws Exception {
        View root = root(popup);
        if (root == null) return true;
        MotionEvent local = MotionEvent.obtain(event);
        local.offsetLocation(-popup.frame.left, -popup.frame.top);
        try {
            if (generic) root.dispatchGenericMotionEvent(local);
            else root.dispatchTouchEvent(local);
        } finally {
            local.recycle();
        }
        return true;
    }

    // Key delivery to the focused popup. Returns false for the Activity.
    boolean dispatchKey(KeyEvent event) throws Exception {
        Popup top;
        synchronized (this) { top = topFocusable(); }
        if (top == null) return false;
        View root = root(top);
        if (root != null) root.dispatchKeyEvent(event);
        return true;
    }

    synchronized boolean hasPopups() {
        return !popups.isEmpty();
    }
}
