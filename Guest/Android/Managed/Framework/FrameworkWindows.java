// Every WindowManager.addView window: Activity windows, dialogs, popup menus.
// Android composes them in SurfaceFlinger; here IWindowSession lays them out
// like WindowLayout, each view tree is drawn in software when it is dirty, on
// the thread that owns it, and the host shows the pixels as a layer above every surface
// (activity_window.c op 9). A fully transparent window, such as an Activity
// whose SurfaceView punches a hole through all of it, needs no host layer.
// SurfaceViews in these windows get host surfaces from FrameworkSurfaces.
// A window whose ViewRootImpl took its surface (Window.takeSurface: NativeActivity) is not drawn here: its app owns the
// surface, which FrameworkSurfaces.updateTaken provides.
// Input goes to the topmost window the way InputDispatcher would deliver it, through the window's input channel
// (Channel), so ViewRootImpl runs its own input stages, a taken InputQueue included.
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.os.IBinder;
import android.os.IInterface;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.InputEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowManager;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

final class FrameworkWindows {
    private static native int showPopup(int id, int x, int y, int width, int height, int[] argb);
    private static native int removePopup(int id);

    private static final int ADD_FLAG_IN_TOUCH_MODE = 0x1;   // WindowManagerGlobal
    private static final int ADD_FLAG_APP_VISIBLE = 0x2;
    private static final int DIM = 0x40000000;  // host layer id of a window's dim layer
    private static final Class<?> CHANNEL = channelType();
    private static final Method DISPATCH_APP_VISIBILITY = windowMethod("dispatchAppVisibility", boolean.class);

    private static Method windowMethod(String name, Class<?>... types) {
        try { return Class.forName("android.view.IWindow").getMethod(name, types); }
        catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }

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
        Channel channel;  // made on first input (channel())
        final Rect frame = new Rect();
        final Rect shown = new Rect();
        final Rect dimShown = new Rect();
        int dimAlpha;
        int requestedWidth, requestedHeight, visibility;
        Bitmap bitmap;
        int[] pixels;
        boolean focused;
        boolean clientVisible = true;  // what the window's ViewRootImpl was told (dispatchAppVisibility)
        Popup parent;  // a sub-window's window; null otherwise
        long group;  // windows sharing a token stack together; the id of the first of them
        boolean onHost;  // a host layer shows `shown`; blank windows have none
        boolean announced;  // APP_POPUP_SHOWN logged
        // Set by each traversal (ViewTreeObserver pre-draw), when SurfaceView
        // attachment, visibility or layout may have changed.
        volatile boolean traversed = true;
        View observed;
        // Hardware-accelerated windows: the host layer their ViewRootImpl's
        // ThreadedRenderer draws into, instead of `bitmap`.
        FrameworkSurfaces.Layer layer;
        Object renderer;
        // FLAG_DIM_BEHIND for such a window: a layer created just before `layer`, so it stacks directly below it (the
        // software dim layer always stacks above every surface layer, dimming the window itself).
        FrameworkSurfaces.Layer dimLayer;
        int dimLayerAlpha;
        // Windows created on another thread draw on its Looper (drawForeign):
        // `drawing` hands `bitmap` and `pixels` between the threads, and
        // `removed` (guarded by FrameworkWindows) stops host layers from
        // appearing after that thread removed the window.
        boolean removed;
        volatile boolean drawing;
        volatile boolean updating;  // a SurfaceView update is posted (updateSurfaces)
        boolean drawn;  // the owner drew pixels not yet shown
        volatile Exception failure;  // the owner's draw or SurfaceView update failed
        String text;  // visible text at the first draw, for the first-show log line
        Popup(IBinder token, Object window, int id) { this.token = token; this.window = window; this.id = id; }
    }

    private final FrameworkDisplay displays;
    private final FrameworkSurfaces surfaces;
    private final List<Popup> popups = new ArrayList<>();  // bottom to top
    private Object session;
    private int nextId;
    private Popup touchTarget;
    private final Set<IBinder> stoppedApps = new HashSet<>();  // tokens of stopped Activities
    private boolean stopped;
    private List<Long> stacked = new ArrayList<>();  // host surface order last applied

    FrameworkWindows(FrameworkDisplay displays, FrameworkSurfaces surfaces) {
        this.displays = displays;
        this.surfaces = surfaces;
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
            popup.group = popup.id;
            for (Popup other : popups) {
                if (popup.attrs.token != null && other.attrs.token == popup.attrs.token) popup.group = other.group;
                if (isSubWindow(popup.attrs.type) && other.token == popup.attrs.token) popup.parent = other;
            }
            popups.add(popup);
            popups.sort(Z_ORDER);
            popup.clientVisible = activityVisible(popup);
        }
        for (Object out : a) {
            if (CHANNEL.isInstance(out)) {
                // ViewRootImpl's receiver reads the client end; input is published into the server end (Channel).
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
        StringBuilder stack = new StringBuilder();
        synchronized (this) { for (Popup each : popups) stack.append(stack.length() > 0 ? "," : "").append(each.id); }
        System.out.println("APP_POPUP_ADDED " + popup.id + " type=" + popup.attrs.type
                + " title=" + popup.attrs.getTitle() + " stack=" + stack);  // bottom to top
        updateFocus();
        // WindowManagerService: the flag is set unless the window's Activity is stopped; it is dispatched when that changes.
        return (inTouchMode() ? ADD_FLAG_IN_TOUCH_MODE : 0) | (popup.clientVisible ? ADD_FLAG_APP_VISIBLE : 0);
    }

    // Window layers, from WindowManagerPolicy.getWindowLayerFromTypeLw: the types an app can add, application
    // windows in one layer, others above them; unknown types as in the policy.
    private static int typeLayer(int type) {
        if (type >= WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                && type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW) return 2;
        if (isSubWindow(type)) return 2;  // a sub-window whose parent is gone
        switch (type) {
            case WindowManager.LayoutParams.TYPE_PHONE:
            case 2037: return 3;  // TYPE_PRESENTATION
            case WindowManager.LayoutParams.TYPE_SYSTEM_DIALOG: return 6;
            case WindowManager.LayoutParams.TYPE_TOAST: return 7;
            case WindowManager.LayoutParams.TYPE_PRIORITY_PHONE: return 8;
            case WindowManager.LayoutParams.TYPE_SYSTEM_ALERT:
            case WindowManager.LayoutParams.TYPE_SYSTEM_ERROR: return 9;
            case WindowManager.LayoutParams.TYPE_SYSTEM_OVERLAY: return 10;
            case WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY: return 11;
            case WindowManager.LayoutParams.TYPE_INPUT_METHOD: return 13;
            case WindowManager.LayoutParams.TYPE_INPUT_METHOD_DIALOG: return 14;
            default: return 3;
        }
    }

    // WindowManagerPolicy.getSubWindowLayerFromTypeLw: below (negative) or above the parent window.
    private static int subLayer(int type) {
        switch (type) {
            case 1003: return -2;  // TYPE_APPLICATION_MEDIA
            case 1004: return -1;  // TYPE_APPLICATION_MEDIA_OVERLAY
            case 1005: return 2;   // TYPE_APPLICATION_SUB_PANEL
            case 1006: return 3;   // TYPE_APPLICATION_ABOVE_SUB_PANEL
            default: return 1;     // TYPE_APPLICATION_PANEL, TYPE_APPLICATION_ATTACHED_DIALOG
        }
    }

    // Bottom to top. Roots stack by layer, then by token (an Activity's windows stay together, in the order of its
    // first window), then by add order, except that an Activity's base window is below the dialogs sharing its token
    // whatever the add order (a dialog shown from onResume is added first). Sub-windows stack within their parent's
    // subtree by sub-layer; equal sub-layers below the parent stack newest first (WindowState.sWindowSubLayerComparator).
    private static final java.util.Comparator<Popup> Z_ORDER = (a, b) -> {
        if (a == b) return 0;
        List<Popup> up = chain(a), vp = chain(b);
        Popup ra = up.get(0), rb = vp.get(0);
        if (ra != rb) {
            if (typeLayer(ra.attrs.type) != typeLayer(rb.attrs.type))
                return Integer.compare(typeLayer(ra.attrs.type), typeLayer(rb.attrs.type));
            if (ra.group != rb.group) return Long.compare(ra.group, rb.group);
            return Long.compare(member(ra), member(rb));
        }
        int k = 1;
        while (k < up.size() && k < vp.size() && up.get(k) == vp.get(k)) k++;
        if (k == up.size()) return subLayer(vp.get(k).attrs.type) < 0 ? 1 : -1;  // a is the ancestor
        if (k == vp.size()) return subLayer(up.get(k).attrs.type) < 0 ? -1 : 1;
        int sa = subLayer(up.get(k).attrs.type), sb = subLayer(vp.get(k).attrs.type);
        if (sa != sb) return Integer.compare(sa, sb);
        return sa < 0 ? Long.compare(vp.get(k).id, up.get(k).id) : Long.compare(up.get(k).id, vp.get(k).id);
    };

    private static long member(Popup root) {
        return root.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION && root.attrs.token != null
                ? Long.MIN_VALUE : root.id;
    }

    private static List<Popup> chain(Popup popup) {
        List<Popup> up = new ArrayList<>();
        for (Popup each = popup; each != null; each = each.parent) up.add(0, each);
        return up;
    }

    private static boolean isSubWindow(int type) {
        return type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW
                && type <= WindowManager.LayoutParams.LAST_SUB_WINDOW;
    }

    private int relayout(Object[] a) throws Exception {
        Popup popup = find(((IInterface) a[0]).asBinder());
        if (popup == null) throw new IllegalArgumentException("relayout of unknown window");
        Rect display = displays.bounds();
        boolean focusChanged;
        synchronized (this) {
            int before = popup.visibility;
            int flags = popup.attrs.flags;
            if (a[1] != null) popup.attrs.copyFrom((WindowManager.LayoutParams) a[1]);
            popup.requestedWidth = (Integer) a[2];
            popup.requestedHeight = (Integer) a[3];
            popup.visibility = (Integer) a[4];
            layout(popup, display);
            // WindowManagerService recomputes focus whenever a window's
            // visibility or focusability changes: a base window that
            // becomes visible after the window covering it was removed
            // (an Activity resuming under an ad) takes focus back then.
            focusChanged = before != popup.visibility || flags != popup.attrs.flags;
        }
        if (focusChanged) updateFocus();
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
        Channel channel;
        synchronized (this) {
            popup = find(token);
            if (popup == null) return;
            popups.remove(popup);
            popup.removed = true;
            channel = popup.channel;  // none is made once removed is set (channel())
            if (touchTarget == popup) touchTarget = null;
        }
        // ViewRootImpl detaches the tree before removing its window, as
        // SurfaceView.onDetachedFromWindow ends its surface.
        surfaces.destroy(popup);
        releaseLayer(popup);
        popup.observed = null;
        if (popup.onHost) removePopup(popup.id);
        hideDim(popup);
        releaseChannel(channel, popup.server);
        if (popup.focused) focus(popup, false);
        if (popup.bitmap != null) popup.bitmap.recycle();
        System.out.println("APP_POPUP_REMOVED " + popup.id);
        updateFocus();
    }

    // The sender lives on the main Looper; the server end goes after it.
    private static void releaseChannel(Channel channel, Object server) {
        Runnable release = () -> {
            if (channel != null) channel.close();
            if (server == null) return;
            try { CHANNEL.getMethod("dispose").invoke(server); }
            catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
        };
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) release.run();
        else new android.os.Handler(android.os.Looper.getMainLooper()).post(release);
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

    // WindowState.isVisibleRequestedOrAdding: a window is not visible while its
    // Activity is stopped, dialogs and popups included (sub-windows follow
    // their parent), whatever its own view visibility.
    private synchronized boolean visible(Popup popup) {
        return popup.visibility == View.VISIBLE && activityVisible(popup);
    }

    // ActivityRecord.isClientVisible for the window's Activity.
    private synchronized boolean activityVisible(Popup popup) {
        for (Popup each = popup; each != null; each = each.parent)
            if (each.attrs.token != null && stoppedApps.contains(each.attrs.token)) return false;
        return true;
    }

    // ActivityThread.updateVisibility on the server's side: the Activity's
    // windows lose focus, input and their host layers when it stops, and get
    // them back when it is visible again.
    void appVisible(IBinder token, boolean visible) throws Exception {
        List<Popup> changed = new ArrayList<>();
        synchronized (this) {
            if (visible) stoppedApps.remove(token); else stoppedApps.add(token);
            for (Popup popup : popups) {
                boolean now = activityVisible(popup);
                if (popup.clientVisible != now) { popup.clientVisible = now; changed.add(popup); }
            }
        }
        // WindowState.sendAppVisibilityToClients: ViewRootImpl hides the window's views while the app is not visible.
        for (Popup popup : changed)
            DISPATCH_APP_VISIBILITY.invoke(popup.window, popup.clientVisible);
        updateFocus();
    }

    private boolean focusable(Popup popup) {
        return visible(popup)
                && (popup.attrs.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0;
    }

    private Popup topFocusable() {
        for (int i = popups.size() - 1; i >= 0; i--)
            if (focusable(popups.get(i))) return popups.get(i);
        return null;
    }

    // One focused window: the topmost focusable one. A window that never
    // had focus gets no callback, as under WindowManagerService.
    private void updateFocus() throws Exception {
        Popup top;
        List<Popup> others = new ArrayList<>();
        synchronized (this) {
            top = topFocusable();
            for (Popup popup : popups) if (popup != top && popup.focused) others.add(popup);
        }
        for (Popup popup : others) focus(popup, false);
        if (top != null && !top.focused) focus(top, true);
    }

    // The app is stopping: like ViewRootImpl.setWindowStopped, every
    // SurfaceView loses its surface, on its window's thread, and none is
    // created again.
    void stop() throws Exception {
        synchronized (this) { stopped = true; }
        surfaces.stop();
    }

    // A display resize: every window lays out again against the new bounds
    // (WindowManagerService relayout; ViewRootImpl.forceWmRelayout).
    void displayChanged() throws Exception {
        List<Popup> all;
        synchronized (this) { all = new ArrayList<>(popups); }
        for (Popup popup : all) {
            Object viewRoot = viewRoot(popup);
            if (viewRoot == null) continue;
            Method relayout = viewRoot.getClass().getDeclaredMethod("forceWmRelayout");
            relayout.setAccessible(true);
            onOwner(viewRoot, () -> {
                try { relayout.invoke(viewRoot); }
                catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
            });
        }
    }

    // ViewRootImpl.mThread created the window's views and only it may touch
    // them (checkThread): Android runs each window's traversals, drawing and
    // input on that thread's Looper. Engines such as Unity show dialogs from
    // their own Looper thread.
    private static boolean owned(Object viewRoot) throws Exception {
        return hiddenField(viewRoot, "mThread") == Thread.currentThread();
    }

    private static void onOwner(Object viewRoot, Runnable task) throws Exception {
        if (owned(viewRoot)) task.run();
        else ((android.os.Handler) hiddenField(viewRoot, "mHandler")).post(task);
    }

    private static void focus(Popup popup, boolean focused) throws Exception {
        popup.focused = focused;
        Object viewRoot = viewRoot(popup);
        if (viewRoot == null) return;
        Method change = viewRoot.getClass().getDeclaredMethod("windowFocusChanged", boolean.class);
        change.setAccessible(true);
        change.invoke(viewRoot, focused);
    }

    // Called from the launcher's UI loop: update SurfaceViews after each
    // traversal, then redraw dirty or moved windows bottom to top.
    void render() throws Exception {
        List<Popup> stack = new ArrayList<>();
        synchronized (this) {
            if (stopped) return;
            stack.addAll(popups);
        }
        for (int index = 0; index < stack.size(); index++) {
            Popup popup = stack.get(index);
            if (popup.failure != null) throw popup.failure;
            View root = root(popup);
            Object viewRoot = viewRoot(popup);
            int w = popup.frame.width(), h = popup.frame.height();
            boolean showing = visible(popup) && root != null && w > 0 && h > 0
                    && root.getWidth() > 0 && root.getHeight() > 0;
            SurfaceHolder taken = viewRoot == null ? null : (SurfaceHolder) hiddenField(viewRoot, "mSurfaceHolder");
            if (!showing && taken != null) updateTaken(popup, viewRoot, taken, false);
            if (!showing) {
                if (viewRoot == null || owned(viewRoot)) surfaces.update(popup, null, false);
                else if (!popup.updating && surfaces.has(popup)) updateSurfaces(popup, viewRoot, null, false);
                releaseLayer(popup);
                if (popup.onHost) { removePopup(popup.id); popup.onHost = false; }
                popup.shown.setEmpty();
                hideDim(popup);
                continue;
            }
            if (root.isLayoutRequested()) continue;  // ViewRootImpl lays out before the next draw
            if (viewRoot == null) continue;
            if (popup.observed != root) {
                popup.observed = root;
                onOwner(viewRoot, () -> {
                    android.view.ViewTreeObserver observer = root.getViewTreeObserver();
                    observer.addOnPreDrawListener(() -> {
                        popup.traversed = true;
                        return true;
                    });
                    // ViewRootImpl reports the window detaching before its
                    // views detach, and SurfaceView.onDetachedFromWindow
                    // destroys its Surface: deliver surfaceDestroyed first, as
                    // SurfaceView's own updateSurface does.
                    observer.addOnWindowAttachListener(new android.view.ViewTreeObserver.OnWindowAttachListener() {
                        @Override public void onWindowAttached() {}
                        @Override public void onWindowDetached() {
                            try { surfaces.destroy(popup); }
                            catch (Exception error) { popup.failure = error; }
                        }
                    });
                });
                popup.traversed = true;
            }
            if (popup.traversed && !popup.updating) {
                popup.traversed = false;
                updateSurfaces(popup, viewRoot, root, true);
            }
            if (taken != null) {
                // The app owns the surface, we won't draw (ViewRootImpl.draw).
                releaseLayer(popup);
                if (popup.onHost) { removePopup(popup.id); popup.onHost = false; }
                updateTaken(popup, viewRoot, taken, true);
                continue;
            }
            if (!owned(viewRoot)) {
                synchronized (this) {
                    if (popup.removed) continue;
                    if (dim(popup)) restackAbove(stack, index);
                    if (drawForeign(popup, root, viewRoot, w, h) && show(popup, stack, index, w, h))
                        popup.drawn = false;
                }
                continue;
            }
            Object attachInfo = hiddenField(viewRoot, "mAttachInfo");
            Object renderer = attachInfo == null ? null : hiddenField(attachInfo, "mThreadedRenderer");
            // A GPU window keeps its dim in its own layer (drawHardware); the software dim layer is for software windows.
            if (renderer == null) { if (dim(popup)) restackAbove(stack, index); }
            else hideDim(popup);
            // ViewRootImpl accumulates invalidations in mDirty and clears it
            // only when it draws itself, which it never does without a valid
            // Surface: redraw that region of the retained bitmap, as its
            // software path does (a full window when moved or resized).
            Rect dirty = takeDirty(popup);
            // Any window whose ViewRootImpl made a ThreadedRenderer (FLAG_HARDWARE_ACCELERATED) is drawn on the GPU, as on
            // Android: a software canvas refuses the hardware bitmaps an image loader hands out (YouTube's popup menus).
            if (renderer != null) {
                drawHardware(popup, root, viewRoot, attachInfo, renderer, w, h, !dirty.isEmpty() || root.isDirty());
                continue;
            }
            releaseLayer(popup);
            if (popup.shown.equals(popup.frame) && !root.isDirty() && dirty.isEmpty()) continue;
            drawSoftware(popup, root, dirty, w, h, !popup.shown.equals(popup.frame));
            show(popup, stack, index, w, h);
        }
        // SurfaceFlinger's window order: each window's SurfaceViews directly
        // below its own layer (SurfaceView's default Z), windows bottom to top.
        // Host layers otherwise stack in creation order.
        List<Long> order = new ArrayList<>();
        for (Popup popup : stack) {
            // A window with a taken surface has no layer of its own: that surface is the window.
            if (popup.layer == null && !surfaces.hasTaken(popup)) continue;
            order.addAll(surfaces.windows(popup));
            if (popup.layer != null) order.add(popup.layer.window);
        }
        if (!order.equals(stacked)) {
            surfaces.restack(order);
            stacked = order;
        }
    }

    // ViewRootImpl.draw sets AttachInfo.mDrawingTime before every draw, and
    // the views' legacy animations (a fragment's enter fade) are timed by it:
    // left at zero they never advance, and the animated screen stays at its
    // first frame, invisible. The animation clock is uptimeMillis.
    private static void advanceDrawingTime(Object attachInfo) throws Exception {
        if (attachInfo == null) return;
        Field time = attachInfo.getClass().getDeclaredField("mDrawingTime");
        time.setAccessible(true);
        time.setLong(attachInfo, android.os.SystemClock.uptimeMillis());
    }

    // ViewRootImpl's software draw into the retained bitmap: redraws `dirty`
    // (all of it when `full`, empty or resized) and copies it to `pixels`.
    private static void drawSoftware(Popup popup, View root, Rect dirty, int w, int h, boolean full) throws Exception {
        if (popup.bitmap == null || popup.bitmap.getWidth() != w || popup.bitmap.getHeight() != h) {
            if (popup.bitmap != null) popup.bitmap.recycle();
            popup.bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            popup.pixels = new int[w * h];
            full = true;
        }
        if (full || dirty.isEmpty() || !dirty.intersect(0, 0, w, h)) dirty.set(0, 0, w, h);
        Canvas canvas = new Canvas(popup.bitmap);
        canvas.clipRect(dirty);
        canvas.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR);
        advanceDrawingTime(hiddenField(root, "mAttachInfo"));
        root.draw(canvas);
        popup.bitmap.getPixels(popup.pixels, dirty.top * w + dirty.left, w,
                dirty.left, dirty.top, dirty.width(), dirty.height());
        if (!popup.announced) {
            StringBuilder text = new StringBuilder();
            collectText(root, text);
            popup.text = text.toString();
        }
    }

    // A window owned by another thread: its Looper draws the bitmap while
    // this compositor skips the window; returns true when pixels of the
    // current size are ready to show. A moved window shows retained pixels.
    private boolean drawForeign(Popup popup, View root, Object viewRoot, int w, int h) throws Exception {
        if (popup.drawing) return false;
        boolean sized = popup.bitmap != null && popup.bitmap.getWidth() == w && popup.bitmap.getHeight() == h;
        if (sized && (popup.drawn || !popup.shown.equals(popup.frame))) return true;
        if (sized && !root.isDirty() && ((Rect) hiddenField(viewRoot, "mDirty")).isEmpty()) return false;
        popup.drawn = false;
        popup.drawing = true;
        ((android.os.Handler) hiddenField(viewRoot, "mHandler")).post(() -> {
            try {
                synchronized (this) { if (popup.removed) return; }  // remove() ran on this thread first
                drawSoftware(popup, root, takeDirty(popup), w, h, false);
                popup.drawn = true;
            } catch (Exception error) {
                popup.failure = error;
            } finally {
                popup.drawing = false;
            }
        });
        return false;
    }

    // SurfaceViews follow each traversal on the window's own thread, where
    // SurfaceHolder callbacks run; one update per window is in flight.
    private void updateSurfaces(Popup popup, Object viewRoot, View root, boolean shown) throws Exception {
        if (owned(viewRoot)) {
            surfaces.update(popup, root, shown);
            return;
        }
        popup.updating = true;
        ((android.os.Handler) hiddenField(viewRoot, "mHandler")).post(() -> {
            try {
                synchronized (this) { if (popup.removed) return; }  // remove() ran on this thread first
                surfaces.update(popup, root, shown);
            } catch (Exception error) {
                popup.failure = error;
            } finally {
                popup.updating = false;
            }
        });
    }

    // A taken surface follows the window on its own thread, where Android delivers SurfaceHolder callbacks; one
    // update per window is in flight, as for SurfaceViews.
    private void updateTaken(Popup popup, Object viewRoot, SurfaceHolder holder, boolean shown) throws Exception {
        android.os.Handler handler = (android.os.Handler) hiddenField(viewRoot, "mHandler");
        Rect frame = new Rect(popup.frame);
        int format = popup.attrs.format;
        if (owned(viewRoot)) {
            surfaces.updateTaken(popup, holder, handler, frame.left, frame.top, frame.width(), frame.height(), format, shown);
            return;
        }
        if (popup.updating) return;
        popup.updating = true;
        handler.post(() -> {
            try {
                synchronized (this) { if (popup.removed) return; }  // remove() ran on this thread first
                surfaces.updateTaken(popup, holder, handler, frame.left, frame.top, frame.width(), frame.height(),
                        format, shown);
            } catch (Exception error) {
                popup.failure = error;
            } finally {
                popup.updating = false;
            }
        });
    }

    // Shows a window's software pixels in its host layer; blank windows have
    // none. Returns false when the host is busy (EAGAIN) and it must retry.
    private boolean show(Popup popup, List<Popup> stack, int index, int w, int h) {
        if (!anyVisible(popup.pixels)) {
            if (popup.onHost) {
                removePopup(popup.id);
                popup.onHost = false;
                System.out.println("APP_POPUP_HIDDEN " + popup.id + " transparent");
            }
            popup.shown.set(popup.frame);
            return true;
        }
        int result = showPopup(popup.id, popup.frame.left, popup.frame.top, w, h, popup.pixels);
        if (result == -11) return false;  // EAGAIN: host buffers busy, retry next frame
        if (result != 0) throw new IllegalStateException("popup window " + popup.id + ": host error " + result);
        if (!popup.onHost) {
            if (!popup.announced) {
                String text = popup.text != null ? popup.text : "";
                System.out.println("APP_POPUP_SHOWN " + popup.id + " " + popup.frame.toShortString()
                        + " visible=" + visible(popup.pixels, w * h) + (text.length() > 0 ? " text=" + text : ""));
                popup.announced = true;
            }
            popup.onHost = true;
            restackAbove(stack, index);
        }
        popup.shown.set(popup.frame);
        return true;
    }

    // An Activity window whose ViewRootImpl created a ThreadedRenderer
    // (FLAG_HARDWARE_ACCELERATED) draws on the GPU into its own host layer,
    // created after the window's SurfaceViews so it stacks above them.
    // ViewRootImpl's own Surface stays invalid (no SurfaceFlinger), so it
    // never initializes or draws with the renderer itself; this compositor
    // makes ViewRootImpl's hardware draw call (ThreadedRenderer.draw) for it.
    private void drawHardware(Popup popup, View root, Object viewRoot, Object attachInfo, Object renderer,
            int w, int h, boolean dirty) throws Exception {
        if (popup.onHost) { removePopup(popup.id); popup.onHost = false; }
        boolean created = false;
        boolean dimmed = (popup.attrs.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0;
        int alpha = Math.round(Math.max(0f, Math.min(1f, popup.attrs.dimAmount)) * 255);
        // The dim changed (or went away): rebuild, so the dim stays directly below the window.
        if (popup.layer != null && (dimmed != (popup.dimLayer != null) || (dimmed && alpha != popup.dimLayerAlpha))) releaseLayer(popup);
        if (popup.layer == null || popup.layer.width != w || popup.layer.height != h || popup.renderer != renderer) {
            releaseLayer(popup);
            WindowManager.LayoutParams attrs = popup.attrs;
            if (dimmed) {
                Rect display = displays.bounds();
                popup.dimLayer = surfaces.createLayer(display.width(), display.height(), false);
                surfaces.positionLayer(popup.dimLayer, 0, 0);
                surfaces.fillLayer(popup.dimLayer, alpha << 24);
                popup.dimLayerAlpha = alpha;
            }
            Rect insets = (Rect) hiddenField(attrs, "surfaceInsets");
            boolean opaque = attrs.format == android.graphics.PixelFormat.OPAQUE
                    && insets.left == 0 && insets.top == 0 && insets.right == 0 && insets.bottom == 0;
            popup.layer = surfaces.createLayer(w, h, opaque);
            popup.renderer = renderer;
            RENDERER_INITIALIZE.invoke(renderer, popup.layer.surface);
            RENDERER_SETUP.invoke(renderer, w, h, attachInfo, insets);
            created = true;
        }
        surfaces.positionLayer(popup.layer, popup.frame.left, popup.frame.top);
        if (!dirty && !created) return;
        advanceDrawingTime(attachInfo);
        RENDERER_DRAW.invoke(renderer, root, attachInfo, viewRoot);
        popup.shown.set(popup.frame);
        if (!popup.announced) {
            StringBuilder text = new StringBuilder();
            collectText(root, text);
            System.out.println("APP_WINDOW_GPU " + popup.id + " " + popup.frame.toShortString()
                    + " opaque=" + popup.layer.opaque + (popup.dimLayer != null ? " dim=" + popup.dimLayerAlpha : "")
                    + (text.length() > 0 ? " text=" + text : ""));
            popup.announced = true;
        }
    }

    private void releaseLayer(Popup popup) {
        if (popup.dimLayer != null) { surfaces.releaseLayer(popup.dimLayer); popup.dimLayer = null; }
        if (popup.layer == null) return;
        surfaces.releaseLayer(popup.layer);
        popup.layer = null;
        popup.renderer = null;
    }

    private static Object hiddenField(Object target, String name) throws Exception {
        for (Class<?> type = target.getClass(); type != null; type = type.getSuperclass()) {
            try {
                Field field = type.getDeclaredField(name);
                field.setAccessible(true);
                return field.get(target);
            } catch (NoSuchFieldException next) {
                // Declared by a superclass.
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static final Method RENDERER_INITIALIZE, RENDERER_SETUP, RENDERER_DRAW;
    static {
        try {
            Class<?> renderer = Class.forName("android.view.ThreadedRenderer");
            Class<?> attachInfo = Class.forName("android.view.View$AttachInfo");
            RENDERER_INITIALIZE = renderer.getDeclaredMethod("initialize", android.view.Surface.class);
            RENDERER_SETUP = renderer.getDeclaredMethod("setup", int.class, int.class, attachInfo, Rect.class);
            RENDERER_DRAW = renderer.getDeclaredMethod("draw", View.class, attachInfo,
                    Class.forName("android.view.ThreadedRenderer$DrawCallbacks"));
            for (Method method : new Method[] {RENDERER_INITIALIZE, RENDERER_SETUP, RENDERER_DRAW})
                method.setAccessible(true);
        } catch (ReflectiveOperationException error) {
            throw new ExceptionInInitializerError(error);
        }
    }

    // Whether any pixel has nonzero alpha: a blank window needs no host layer.
    private static boolean anyVisible(int[] argb) {
        for (int pixel : argb) if ((pixel >>> 24) != 0) return true;
        return false;
    }

    // ViewRootImpl.mDirty, taken and cleared like its draw() does.
    private static Rect takeDirty(Popup popup) throws Exception {
        Field ancestor = popup.window.getClass().getDeclaredField("mViewAncestor");
        ancestor.setAccessible(true);
        Object viewRoot = ((WeakReference<?>) ancestor.get(popup.window)).get();
        Rect result = new Rect();
        if (viewRoot == null) return result;
        Field field = viewRoot.getClass().getDeclaredField("mDirty");
        field.setAccessible(true);
        Rect dirty = (Rect) field.get(viewRoot);
        result.set(dirty);
        dirty.setEmpty();
        return result;
    }

    // Pixels with nonzero alpha, for the first-show log line.
    private static int visible(int[] argb, int count) {
        int visible = 0;
        for (int i = 0; i < count; i++) if ((argb[i] >>> 24) != 0) visible++;
        return visible;
    }

    // Host layers stack in creation order. When a layer appears below
    // existing ones, recreate those above it in window order; this pass
    // continues upward and shows them again.
    private void restackAbove(List<Popup> stack, int index) {
        for (int i = index + 1; i < stack.size(); i++) {
            Popup above = stack.get(i);
            hideDim(above);
            if (above.onHost) { removePopup(above.id); above.onHost = false; }
            above.shown.setEmpty();
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
    // spanning the display directly below the window. Returns true when the
    // dim layer is new, so the layers above it must be recreated.
    private boolean dim(Popup popup) throws Exception {
        if ((popup.attrs.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) == 0) {
            hideDim(popup);
            return false;
        }
        Rect display = displays.bounds();
        int alpha = Math.round(Math.max(0f, Math.min(1f, popup.attrs.dimAmount)) * 255);
        if (display.equals(popup.dimShown) && alpha == popup.dimAlpha) return false;
        int[] pixels = new int[display.width() * display.height()];
        java.util.Arrays.fill(pixels, alpha << 24);
        int result = showPopup(DIM | popup.id, 0, 0, display.width(), display.height(), pixels);
        if (result == -11) return false;  // EAGAIN: retry next frame
        if (result != 0) throw new IllegalStateException("dim layer " + popup.id + ": host error " + result);
        boolean created = popup.dimShown.isEmpty();
        if (created && popup.onHost) {
            // Layers stack in creation order: recreate the window above its dim.
            removePopup(popup.id);
            popup.onHost = false;
            popup.shown.setEmpty();
        }
        popup.dimShown.set(display);
        popup.dimAlpha = alpha;
        return created;
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
            return top != null && deliver(top, event);
        }
        if (action == MotionEvent.ACTION_DOWN) {
            touchTarget = null;
            List<Popup> stack;
            synchronized (this) { stack = new ArrayList<>(popups); }
            for (int i = stack.size() - 1; i >= 0; i--) {
                Popup popup = stack.get(i);
                if (!visible(popup)) continue;
                int flags = popup.attrs.flags;
                if ((flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) continue;
                boolean inside = popup.frame.contains((int) event.getX(), (int) event.getY());
                boolean modal = (flags & WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL) == 0;
                if (inside || modal) { touchTarget = popup; break; }
                if ((flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0) {
                    MotionEvent outside = MotionEvent.obtain(event);
                    outside.setAction(MotionEvent.ACTION_OUTSIDE);
                    deliver(popup, outside);
                    outside.recycle();
                }
            }
        }
        Popup target = touchTarget;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) touchTarget = null;
        if (target == null) return false;
        return deliver(target, event);
    }

    private Popup hit(MotionEvent event) {
        synchronized (this) {
            for (int i = popups.size() - 1; i >= 0; i--) {
                Popup popup = popups.get(i);
                if (visible(popup)
                        && popup.frame.contains((int) event.getX(), (int) event.getY())) return popup;
            }
        }
        return null;
    }

    // Touch, hover and ACTION_OUTSIDE for one window: a copy in its own coordinates goes into its input channel.
    private boolean deliver(Popup popup, MotionEvent event) throws Exception {
        Channel channel = channel(popup);
        if (channel == null) return true;  // no input channel, so no input (as under InputDispatcher)
        MotionEvent local = MotionEvent.obtain(event);
        local.offsetLocation(-popup.frame.left, -popup.frame.top);
        channel.send(local, null);
        return true;
    }

    // Keys go to the focused window; `finished` learns whether it handled them. Returns false when no window has focus:
    // the Activity gets the key.
    boolean dispatchKey(KeyEvent event, java.util.function.Consumer<Boolean> finished) throws Exception {
        return dispatchFocused(event, finished);
    }

    // Joystick motion (a gamepad's axes) goes to the focused window like keys, as InputDispatcher sends non-pointer
    // motion. Returns false when no window has focus.
    boolean dispatchMotion(MotionEvent event) throws Exception {
        MotionEvent copy = MotionEvent.obtain(event);
        if (dispatchFocused(copy, null)) return true;
        copy.recycle();
        return false;
    }

    private boolean dispatchFocused(InputEvent event, java.util.function.Consumer<Boolean> finished) throws Exception {
        Popup top;
        synchronized (this) { top = topFocusable(); }
        if (top == null) return false;
        Channel channel = channel(top);
        if (channel != null) {
            channel.send(event, finished);
        } else {
            if (event instanceof MotionEvent) ((MotionEvent) event).recycle();
            if (finished != null) finished.accept(false);
        }
        return true;
    }

    // Made on the play loop's thread (the main Looper), where every event is sent and finished.
    private Channel channel(Popup popup) {
        synchronized (this) {
            if (popup.removed || popup.server == null) return null;
            if (popup.channel == null) popup.channel = new Channel(popup);
            return popup.channel;
        }
    }

    // InputDispatcher's end of one window's input channel. Events are published into the channel that ViewRootImpl's
    // receiver reads (InputEventSender), so ViewRootImpl runs its own input stages on the window's thread: a taken
    // InputQueue (NativeActivity) before and after the IME, the views, then the synthetic stage that turns unhandled
    // joystick motion into d-pad keys. Its finished signal says whether the event was handled. As in InputDispatcher,
    // a key waits until every earlier event of the window has finished (one may move focus: the wait happens where
    // InputDispatcher picks the focused window, findFocusedWindowTargetsLocked), while what a finished callback sends
    // (a key's fallback, already targeted at this window) goes ahead of the waiting events and is sent at once.
    private static final class Channel extends android.view.InputEventSender {
        private static final class Pending {
            final InputEvent event;
            final java.util.function.Consumer<Boolean> finished;
            final boolean targeted;  // a fallback: published without waiting
            Pending(InputEvent event, java.util.function.Consumer<Boolean> finished, boolean targeted) {
                this.event = event; this.finished = finished; this.targeted = targeted;
            }
        }

        private final Popup popup;
        private final List<Pending> outbound = new ArrayList<>();
        private final java.util.Map<Integer, Pending> waiting = new java.util.HashMap<>();
        private int sequence;
        private int front = -1;  // while a finished callback runs: where what it sends is queued
        private boolean closed;

        Channel(Popup popup) {
            super((android.view.InputChannel) popup.server, android.os.Looper.getMainLooper());
            this.popup = popup;
        }

        // Takes the event: a MotionEvent is recycled once it has finished.
        void send(InputEvent event, java.util.function.Consumer<Boolean> finished) {
            Pending pending = new Pending(event, finished, front >= 0);
            if (closed) { done(pending, false); return; }
            if (front >= 0) outbound.add(front++, pending);
            else outbound.add(pending);
            pump();
        }

        private void pump() {
            while (!closed && !outbound.isEmpty()) {
                Pending head = outbound.get(0);
                if (head.event instanceof KeyEvent && !head.targeted && !waiting.isEmpty()) return;
                if (++sequence == 0) ++sequence;
                if (!sendInputEvent(sequence, head.event)) {
                    if (!waiting.isEmpty()) return;  // the channel is full: the next finished signal makes room
                    outbound.remove(0);  // nothing is in flight, so the window's end is gone
                    done(head, false);
                    continue;
                }
                outbound.remove(0);
                waiting.put(sequence, head);
            }
        }

        @Override
        public void onInputEventFinished(int seq, boolean handled) {
            Pending pending = waiting.remove(seq);
            if (pending == null) return;
            try {
                front = 0;
                try { done(pending, handled); } finally { front = -1; }
                pump();
            } catch (RuntimeException error) {
                popup.failure = error;  // thrown by the play loop's next render
            }
        }

        private static void done(Pending pending, boolean handled) {
            if (pending.event instanceof MotionEvent) ((MotionEvent) pending.event).recycle();
            if (pending.finished != null) pending.finished.accept(handled);
        }

        // The window is gone: what it never finished is dropped, as InputDispatcher drops a removed window's queue.
        void close() {
            if (closed) return;
            closed = true;
            dispose();
            for (Pending pending : outbound) if (pending.event instanceof MotionEvent) ((MotionEvent) pending.event).recycle();
            for (Pending pending : waiting.values())
                if (pending.event instanceof MotionEvent) ((MotionEvent) pending.event).recycle();
            outbound.clear();
            waiting.clear();
        }
    }

    // WindowManagerService's touch mode for the one display. IWindowManager.setInTouchMode comes from the window that
    // left or entered it (ViewRootImpl.ensureTouchMode, which has switched itself already); InputDispatcher then tells
    // every window (ViewRootImpl.touchModeChanged, which posts to the window's own thread).
    private boolean inTouchMode = true;
    private static final Method TOUCH_MODE_CHANGED = viewRootMethod("touchModeChanged", boolean.class);

    private static Method viewRootMethod(String name, Class<?>... types) {
        try { return Class.forName("android.view.ViewRootImpl").getMethod(name, types); }
        catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }

    void setInTouchMode(boolean touch) throws Exception {
        List<Popup> stack;
        synchronized (this) {
            if (inTouchMode == touch) return;
            inTouchMode = touch;
            stack = new ArrayList<>(popups);
        }
        for (Popup popup : stack) {
            Object viewRoot = viewRoot(popup);
            if (viewRoot != null) TOUCH_MODE_CHANGED.invoke(viewRoot, touch);
        }
    }

    synchronized boolean inTouchMode() {
        return inTouchMode;
    }

    synchronized boolean hasPopups() {
        return !popups.isEmpty();
    }
}
