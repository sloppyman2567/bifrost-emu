// SurfaceViews in framework windows. SurfaceView.updateSurface() gives up
// without a SurfaceFlinger-backed window surface, so this applies its
// visibility contract instead: a SurfaceView has a host surface while it is
// attached, its window is visible and not stopped, the view is shown and its
// frame is non-empty (SurfaceView.mRequestedVisible and mHaveFrame). The
// surface is created when that becomes true, reported again when its size or
// format changes, and destroyed when it becomes false or its window goes.
// Callbacks run in SurfaceView's order: surfaceCreated, then surfaceChanged.
import android.graphics.PixelFormat;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.Looper;
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
import java.nio.ByteBuffer;

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
        final ImageReader reader;
        int width, height, format;
        int x = Integer.MIN_VALUE, y = Integer.MIN_VALUE;
        boolean closed;
        Entry(SurfaceView view, Object owner, long window, ImageReader reader) {
            this.view = view; this.owner = owner; this.window = window; this.reader = reader;
        }
    }

    private static final Field CALLBACKS = field("mCallbacks");
    private static final Field REQUESTED_FORMAT = field("mRequestedFormat");
    private static final Field SURFACE_FRAME = field("mSurfaceFrame");
    private static final Field DRAW_FINISHED = field("mDrawFinished");
    private static final Method DRAW_FINISHED_METHOD = method("performDrawFinished");

    private final Host host;
    private final Map<SurfaceView, Entry> entries = new IdentityHashMap<>();
    private static native int present(long window, int x, int y, int width, int height, int[] argb);
    private static native int geometry(long window, int x, int y, int width, int height);

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
            int[] location = new int[2]; view.getLocationInWindow(location);
            boolean changed = entry == null || entry.width != width || entry.height != height || entry.format != format;
            if (!changed && entry.x == location[0] && entry.y == location[1])
                continue;
            ((android.graphics.Rect) SURFACE_FRAME.get(view)).set(0, 0, width, height);
            boolean created = entry == null;
            if (created) {
                // Keep the pinned framework's real BufferQueue, GraphicBuffer
                // ownership and acquire/release fences. A host token cannot
                // stand in for android::Surface in MediaCodec or the NDK.
                ImageReader reader = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 3);
                Surface target = view.getHolder().getSurface();
                long window;
                try {
                    Surface.class.getMethod("transferFrom", Surface.class).invoke(target, reader.getSurface());
                    window = host.prepare(target, !PixelFormat.formatHasAlpha(format));
                    if (window <= 0) throw new IllegalStateException("surface preparation: " + window);
                } catch (Exception error) {
                    reader.close(); target.release(); throw error;
                }
                entry = new Entry(view, owner, window, reader);
                entries.put(view, entry);
                final Entry consumer = entry;
                reader.setOnImageAvailableListener(available -> consume(consumer), new Handler(Looper.getMainLooper()));
            }
            entry.width = width;
            entry.height = height;
            entry.format = format;
            entry.x = location[0]; entry.y = location[1];
            int positioned = geometry(entry.window, entry.x, entry.y, width, height);
            if (positioned != 0) throw new IllegalStateException("Surface geometry: " + positioned);
            SurfaceHolder holder = view.getHolder();
            for (SurfaceHolder.Callback callback : callbacks(view)) {
                if (created) callback.surfaceCreated(holder);
                if (changed) callback.surfaceChanged(holder, format, width, height);
            }
            // The first frame is drawn: the window now punches the
            // SurfaceView's hole (SurfaceView.performDrawFinished).
            if (created && !DRAW_FINISHED.getBoolean(view)) DRAW_FINISHED_METHOD.invoke(view);
            System.out.println("APP_SURFACE " + view.getClass().getName() + " " + width + "x" + height
                    + " at " + location[0] + "," + location[1] + " format=" + format);
        }
    }

    private static int clamp(double value) { return Math.max(0, Math.min(255, (int)Math.round(value))); }

    // CpuConsumer has already waited for the producer's acquire fence before
    // exposing planes. Closing the Image returns its buffer even on failure.
    private static int[] pixels(Image image, int width, int height) {
        Rect crop = image.getCropRect();
        if (crop.width() <= 0 || crop.height() <= 0 || width <= 0 || height <= 0 || width > 4096 || height > 4096)
            throw new IllegalArgumentException("Surface image dimensions");
        Image.Plane[] planes = image.getPlanes();
        if (image.getFormat() != ImageFormat.YUV_420_888 || planes.length != 3)
            throw new UnsupportedOperationException("Surface image format: " + image.getFormat());
        int dataspace;
        try { dataspace = (Integer)Image.class.getMethod("getDataSpace").invoke(image); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
        int standard = (dataspace >> 16) & 63, transfer = (dataspace >> 22) & 31;
        if (transfer == 6 || transfer == 7) throw new UnsupportedOperationException("HDR Surface output");
        double kr = standard == 1 ? .2126 : standard == 6 ? .2627 : .299;
        double kb = standard == 1 ? .0722 : standard == 6 ? .0593 : .114;
        if (standard != 0 && standard != 1 && standard != 2 && standard != 4 && standard != 6)
            throw new UnsupportedOperationException("Surface color standard: " + standard);
        boolean full = ((dataspace >> 27) & 7) == 1;
        ByteBuffer[] bytes = {planes[0].getBuffer(), planes[1].getBuffer(), planes[2].getBuffer()};
        int[] out = new int[width * height];
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
            int sx = crop.left + (int)((long)x * crop.width() / width);
            int sy = crop.top + (int)((long)y * crop.height() / height);
            int yy = bytes[0].get(bytes[0].position() + sy * planes[0].getRowStride() + sx * planes[0].getPixelStride()) & 255;
            int u = bytes[1].get(bytes[1].position() + (sy / 2) * planes[1].getRowStride() + (sx / 2) * planes[1].getPixelStride()) & 255;
            int v = bytes[2].get(bytes[2].position() + (sy / 2) * planes[2].getRowStride() + (sx / 2) * planes[2].getPixelStride()) & 255;
            double luminance = full ? yy : (yy - 16) * (255.0 / 219);
            double cb = (u - 128) * (full ? 1 : 255.0 / 224), cr = (v - 128) * (full ? 1 : 255.0 / 224);
            int r = clamp(luminance + 2 * (1 - kr) * cr);
            int b = clamp(luminance + 2 * (1 - kb) * cb);
            int g = clamp(luminance - 2 * kb * (1 - kb) / (1 - kr - kb) * cb
                    - 2 * kr * (1 - kr) / (1 - kr - kb) * cr);
            out[y * width + x] = 0xff000000 | (r << 16) | (g << 8) | b;
        }
        return out;
    }

    private static void consume(Entry entry) {
        if (entry.closed) return;
        try (Image image = entry.reader.acquireLatestImage()) {
            if (image == null) return;
            int[] location = new int[2];
            entry.view.getLocationInWindow(location);
            int result = present(entry.window, location[0], location[1], entry.width, entry.height,
                                 pixels(image, entry.width, entry.height));
            if (result != 0 && result != -11) throw new IllegalStateException("Surface presentation: " + result);
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
        try {
            for (SurfaceHolder.Callback callback : callbacks(entry.view)) {
                callback.surfaceDestroyed(entry.view.getHolder());
            }
        } finally {
            entry.closed = true;
            entry.reader.setOnImageAvailableListener(null, null);
            entry.reader.close();
            host.release(entry.window);
            entry.view.getHolder().getSurface().release();
        }
        System.out.println("APP_SURFACE_DESTROYED " + entry.view.getClass().getName());
    }
}
