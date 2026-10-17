// Every WindowManager.addView window: Activity windows, dialogs, popup menus.
// Android composes them in SurfaceFlinger; here IWindowSession lays them out
// like WindowLayout, each view tree is drawn in software when it is dirty, on
// the thread that owns it, and the host shows the pixels as a layer above every surface
// (activity_window.c op 9). A fully transparent window, such as an Activity
// whose SurfaceView punches a hole through all of it, needs no host layer.
// SurfaceViews in these windows get host surfaces from FrameworkSurfaces.
// Input goes to the topmost window the way InputDispatcher would deliver it.
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
        // Windows created on another thread draw on its Looper (drawForeign):
        // `drawing` hands `bitmap` and `pixels` between the threads, and
        // `removed` (guarded by FrameworkWindows) stops host layers from
        // appearing after that thread removed the window.
        boolean removed;
        volatile boolean drawing;
        boolean drawn;  // the owner drew pixels not yet shown
        Exception failure;  // the owner's draw failed
        String text;  // visible text at the first draw, for the first-show log line
        Popup(IBinder token, Object window, int id) { this.token = token; this.window = window; this.id = id; }
    }

    private final FrameworkDisplay displays;
    private final FrameworkSurfaces surfaces;
    private final List<Popup> popups = new ArrayList<>();  // bottom to top
    private Object session;
    private int nextId;
    private Popup touchTarget;
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
            // WindowManager stacks an Activity's base window below the
            // dialogs and panels sharing its token, whatever the add order
            // (a dialog shown from onResume is added first).
            int index = popups.size();
            if (popup.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION
                    && popup.attrs.token != null)
                for (int i = 0; i < popups.size(); i++)
                    if (popups.get(i).attrs.token == popup.attrs.token) { index = i; break; }
            popups.add(index, popup);
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
            popup.removed = true;
            if (touchTarget == popup) touchTarget = null;
        }
        // ViewRootImpl detaches the tree before removing its window, as
        // SurfaceView.onDetachedFromWindow ends its surface.
        surfaces.destroy(popup);
        releaseLayer(popup);
        popup.observed = null;
        if (popup.onHost) removePopup(popup.id);
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
    // SurfaceView loses its surface and none is created again.
    void stop() throws Exception {
        synchronized (this) { stopped = true; }
        surfaces.destroy(null);
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
            View root = root(popup);
            int w = popup.frame.width(), h = popup.frame.height();
            boolean showing = popup.visibility == View.VISIBLE && root != null && w > 0 && h > 0
                    && root.getWidth() > 0 && root.getHeight() > 0;
            if (!showing) {
                surfaces.update(popup, null, false);
                releaseLayer(popup);
                if (popup.onHost) { removePopup(popup.id); popup.onHost = false; }
                popup.shown.setEmpty();
                hideDim(popup);
                continue;
            }
            if (root.isLayoutRequested()) continue;  // ViewRootImpl lays out before the next draw
            Object viewRoot = viewRoot(popup);
            if (viewRoot == null) continue;
            if (!owned(viewRoot)) {
                // Their SurfaceViews would need FrameworkSurfaces on that
                // thread too; none get host surfaces.
                synchronized (this) {
                    if (popup.removed) continue;
                    if (dim(popup)) restackAbove(stack, index);
                    if (drawForeign(popup, root, viewRoot, w, h) && show(popup, stack, index, w, h))
                        popup.drawn = false;
                }
                continue;
            }
            if (popup.observed != root) {
                popup.observed = root;
                root.getViewTreeObserver().addOnPreDrawListener(() -> {
                    popup.traversed = true;
                    return true;
                });
                popup.traversed = true;
            }
            if (popup.traversed) {
                popup.traversed = false;
                surfaces.update(popup, root, true);
            }
            if (dim(popup)) restackAbove(stack, index);
            // ViewRootImpl accumulates invalidations in mDirty and clears it
            // only when it draws itself, which it never does without a valid
            // Surface: redraw that region of the retained bitmap, as its
            // software path does (a full window when moved or resized).
            Rect dirty = takeDirty(popup);
            Object attachInfo = hiddenField(viewRoot, "mAttachInfo");
            Object renderer = attachInfo == null ? null : hiddenField(attachInfo, "mThreadedRenderer");
            if (renderer != null && popup.attrs.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION) {
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
            if (popup.layer == null) continue;
            order.addAll(surfaces.windows(popup));
            order.add(popup.layer.window);
        }
        if (!order.equals(stacked)) {
            surfaces.restack(order);
            stacked = order;
        }
    }

    // ViewRootImpl's software draw into the retained bitmap: redraws `dirty`
    // (all of it when `full`, empty or resized) and copies it to `pixels`.
    private static void drawSoftware(Popup popup, View root, Rect dirty, int w, int h, boolean full) {
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
        if (popup.failure != null) throw popup.failure;
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
        if (popup.layer == null || popup.layer.width != w || popup.layer.height != h || popup.renderer != renderer) {
            releaseLayer(popup);
            WindowManager.LayoutParams attrs = popup.attrs;
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
        RENDERER_DRAW.invoke(renderer, root, attachInfo, viewRoot);
        popup.shown.set(popup.frame);
        if (!popup.announced) {
            StringBuilder text = new StringBuilder();
            collectText(root, text);
            System.out.println("APP_WINDOW_GPU " + popup.id + " " + popup.frame.toShortString()
                    + " opaque=" + popup.layer.opaque + (text.length() > 0 ? " text=" + text : ""));
            popup.announced = true;
        }
    }

    private void releaseLayer(Popup popup) {
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
        Object viewRoot = viewRoot(popup);
        View root = root(popup);
        if (viewRoot == null || root == null) return true;
        MotionEvent local = MotionEvent.obtain(event);
        local.offsetLocation(-popup.frame.left, -popup.frame.top);
        onOwner(viewRoot, () -> {
            try {
                if (generic) root.dispatchGenericMotionEvent(local);
                else root.dispatchTouchEvent(local);
            } finally {
                local.recycle();
            }
        });
        return true;
    }

    // Key delivery to the focused popup. Returns false for the Activity.
    boolean dispatchKey(KeyEvent event) throws Exception {
        Popup top;
        synchronized (this) { top = topFocusable(); }
        if (top == null) return false;
        Object viewRoot = viewRoot(top);
        View root = root(top);
        if (viewRoot != null && root != null) onOwner(viewRoot, () -> root.dispatchKeyEvent(event));
        return true;
    }

    synchronized boolean hasPopups() {
        return !popups.isEmpty();
    }
}
