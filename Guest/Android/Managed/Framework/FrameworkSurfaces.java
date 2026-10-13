// SurfaceViews in framework windows. SurfaceView.updateSurface() gives up
// without a SurfaceFlinger-backed window surface, so this applies its
// visibility contract instead: a SurfaceView has a host surface while it is
// attached, its window is visible and not stopped, the view is shown and its
// frame is non-empty (SurfaceView.mRequestedVisible and mHaveFrame). The
// surface is created when that becomes true, reported again when its size or
// format changes, and destroyed when it becomes false or its window goes.
// Callbacks run in SurfaceView's order: surfaceCreated, then surfaceChanged.
import android.graphics.PixelFormat;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;

final class FrameworkSurfaces {
    // Host surfaces for Java Surfaces (ManagedLauncher's activity_window.c bridge).
    interface Host {
        long prepare(Surface surface, boolean opaque);
        void release(long window);
    }

    private static final class Entry {
        final SurfaceView view;
        final Object owner;  // the framework window holding the view
        final long window;
        int width, height, format;
        Entry(SurfaceView view, Object owner, long window) {
            this.view = view; this.owner = owner; this.window = window;
        }
    }

    private static final Field CALLBACKS = field("mCallbacks");
    private static final Field REQUESTED_FORMAT = field("mRequestedFormat");
    private static final Field SURFACE_FRAME = field("mSurfaceFrame");
    private static final Field DRAW_FINISHED = field("mDrawFinished");
    private static final Method DRAW_FINISHED_METHOD = method("performDrawFinished");

    private final Host host;
    private final Map<SurfaceView, Entry> entries = new IdentityHashMap<>();

    FrameworkSurfaces(Host host) {
        this.host = host;
    }

    private static Field field(String name) {
        try {
            Field field = SurfaceView.class.getDeclaredField(name);
            field.setAccessible(true);
            return field;
        } catch (NoSuchFieldException error) {
            throw new AssertionError(error);
        }
    }

    private static Method method(String name) {
        try {
            Method method = SurfaceView.class.getDeclaredMethod(name);
            method.setAccessible(true);
            return method;
        } catch (NoSuchMethodException error) {
            throw new AssertionError(error);
        }
    }

    // Brings the surfaces of one window up to date with its laid-out view
    // tree. `shown` is false while the window is hidden or being removed.
    void update(Object owner, View root, boolean shown) throws Exception {
        List<SurfaceView> visible = new ArrayList<>();
        if (shown && root != null) collect(root, visible);
        for (Entry entry : new ArrayList<>(entries.values()))
            if (entry.owner == owner && !visible.contains(entry.view)) destroy(entry);
        for (SurfaceView view : visible) {
            Entry entry = entries.get(view);
            int format = REQUESTED_FORMAT.getInt(view);
            int width = view.getWidth(), height = view.getHeight();
            if (entry != null && entry.width == width && entry.height == height && entry.format == format)
                continue;
            ((android.graphics.Rect) SURFACE_FRAME.get(view)).set(0, 0, width, height);
            boolean created = entry == null;
            if (created) {
                long window = host.prepare(view.getHolder().getSurface(), !PixelFormat.formatHasAlpha(format));
                if (window <= 0) throw new IllegalStateException("surface preparation: " + window);
                entry = new Entry(view, owner, window);
                entries.put(view, entry);
            }
            entry.width = width;
            entry.height = height;
            entry.format = format;
            SurfaceHolder holder = view.getHolder();
            for (SurfaceHolder.Callback callback : callbacks(view)) {
                if (created) callback.surfaceCreated(holder);
                callback.surfaceChanged(holder, format, width, height);
            }
            // The first frame is drawn: the window now punches the
            // SurfaceView's hole (SurfaceView.performDrawFinished).
            if (created && !DRAW_FINISHED.getBoolean(view)) DRAW_FINISHED_METHOD.invoke(view);
            int[] location = new int[2];
            view.getLocationInWindow(location);
            System.out.println("APP_SURFACE " + view.getClass().getName() + " " + width + "x" + height
                    + " at " + location[0] + "," + location[1] + " format=" + format);
        }
    }

    // A window's SurfaceViews in drawing order, as SurfaceView decides
    // mRequestedVisible: attached and shown (view and ancestors visible).
    private static void collect(View view, List<SurfaceView> out) {
        if (view.getVisibility() != View.VISIBLE) return;
        if (view instanceof SurfaceView && view.isAttachedToWindow()
                && view.getWidth() > 0 && view.getHeight() > 0)
            out.add((SurfaceView) view);
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
                collect(((ViewGroup) view).getChildAt(i), out);
    }

    // Destroys every surface of one window, or of all windows when owner is null.
    void destroy(Object owner) throws Exception {
        for (Entry entry : new ArrayList<>(entries.values()))
            if (owner == null || entry.owner == owner) destroy(entry);
    }

    @SuppressWarnings("unchecked")
    private static List<SurfaceHolder.Callback> callbacks(SurfaceView view) throws Exception {
        return new ArrayList<>((List<SurfaceHolder.Callback>) CALLBACKS.get(view));
    }

    private void destroy(Entry entry) throws Exception {
        entries.remove(entry.view);
        DRAW_FINISHED.setBoolean(entry.view, false);
        for (SurfaceHolder.Callback callback : callbacks(entry.view)) {
            try {
                callback.surfaceDestroyed(entry.view.getHolder());
            } catch (IllegalArgumentException e) {
                // Play-mode Java Surfaces have no native libgui Surface, so
                // Surface.isValid() is false and PixelCopy rejects the final
                // snapshot some apps (Unity) take here. Finish teardown;
                // every other callback failure still propagates.
                StackTraceElement[] trace = e.getStackTrace();
                if (trace.length == 0 || !"android.view.PixelCopy".equals(trace[0].getClassName()))
                    throw e;
                System.out.println("APP_SURFACE_SNAPSHOT_SKIPPED " + callback.getClass().getName());
            }
        }
        host.release(entry.window);
        System.out.println("APP_SURFACE_DESTROYED " + entry.view.getClass().getName());
    }
}
