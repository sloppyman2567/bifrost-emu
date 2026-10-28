// SurfaceViews in framework windows. SurfaceView.updateSurface() gives up
// without a SurfaceFlinger-backed window surface, so this applies its
// visibility contract instead: a SurfaceView has a host surface while it is
// attached, its window is visible and not stopped, the view is shown and its
// frame is non-empty (SurfaceView.mRequestedVisible and mHaveFrame). The
// surface is created when that becomes true, reported again when its size or
// format changes, and destroyed when it becomes false or its window goes.
// Callbacks run in SurfaceView's order: surfaceCreated, then surfaceChanged.
//
// Each window's surfaces are updated and destroyed on the thread that owns
// the window (ViewRootImpl.mThread), where Android delivers SurfaceHolder
// callbacks; frames are consumed on the main Looper. The lock on this object
// guards the entries and every host call on an entry's window, never the
// app's callbacks, so a released host window is never presented again.
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
import android.view.ViewTreeObserver;
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
        final Handler thread;  // the window's ViewRootImpl handler, kept past detach
        final long window;
        final ImageReader reader;
        int width, height, format;  // guarded by FrameworkSurfaces
        int x = Integer.MIN_VALUE, y = Integer.MIN_VALUE;
        // The part of the view that shows, in view pixels (getGlobalVisibleRect: clipped by every ancestor and the
        // window); empty when scrolled out. Frames convert and present only this part.
        int clipLeft, clipTop, clipRight, clipBottom;
        boolean cpu;  // frames arrive through the ImageReader (a video), not a native window
        boolean closed;
        Entry(SurfaceView view, Object owner, long window, ImageReader reader) {
            this.view = view; this.owner = owner; this.window = window; this.reader = reader;
            this.thread = view.getHandler();
        }
    }

    private static final Field CALLBACKS = field("mCallbacks");
    private static final Field REQUESTED_FORMAT = field("mRequestedFormat");
    private static final Field SURFACE_FRAME = field("mSurfaceFrame");
    private static final Field DRAW_FINISHED = field("mDrawFinished");
    private static final Method DRAW_FINISHED_METHOD = method("performDrawFinished");
    private static final Field HAVE_FRAME = field("mHaveFrame");
    private static final Field DRAW_LISTENER = field("mDrawListener");

    private final Host host;
    private final Map<SurfaceView, Entry> entries = new IdentityHashMap<>();
    private boolean stopped;  // ViewRootImpl.setWindowStopped: no surface is created again
    private static native int present(long window, int x, int y, int width, int height, int[] argb);
    // Op 25: the visible region of a width x height layer from a YUV_420_888 image's planes, converted by the host.
    private static native int presentYuv(long window, int x, int y, int width, int height, int layerWidth, int layerHeight,
            int left, int top, int cropLeft, int cropTop, int cropWidth, int cropHeight, ByteBuffer yPlane, ByteBuffer uPlane,
            ByteBuffer vPlane, int yRow, int uRow, int vRow, int yPixel, int uPixel, int vPixel, int matrix, int fullRange);
    private static native int geometry(long window, int x, int y, int width, int height);

    FrameworkSurfaces(Host host) {
        this.host = host;
    }

    private Handler frameHandler;
    private synchronized Handler frames() {
        if (frameHandler == null) {
            android.os.HandlerThread thread = new android.os.HandlerThread("SurfaceFrames");
            thread.start();
            frameHandler = new Handler(thread.getLooper());
        }
        return frameHandler;
    }

    // A framework window's own host layer: HWUI's ThreadedRenderer draws the
    // window through EGL straight into it. The ImageReader keeps the
    // framework's real BufferQueue behind the Surface, as for SurfaceViews.
    static final class Layer {
        final ImageReader reader;
        final Surface surface;
        final long window;
        final int width, height;
        final boolean opaque;
        int x = Integer.MIN_VALUE, y = Integer.MIN_VALUE;
        Layer(ImageReader reader, long window, int width, int height, boolean opaque) {
            this.reader = reader; this.surface = reader.getSurface(); this.window = window;
            this.width = width; this.height = height; this.opaque = opaque;
        }
    }

    Layer createLayer(int width, int height, boolean opaque) {
        ImageReader reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
        long window = host.prepare(reader.getSurface(), opaque);
        if (window <= 0) {
            reader.close();
            throw new IllegalStateException("window layer preparation: " + window);
        }
        return new Layer(reader, window, width, height, opaque);
    }

    void positionLayer(Layer layer, int x, int y) {
        if (layer.x == x && layer.y == y) return;
        int result = geometry(layer.window, x, y, layer.width, layer.height);
        if (result != 0) throw new IllegalStateException("window layer geometry: " + result);
        layer.x = x; layer.y = y;
    }

    private static native int placeBelow(long window, long sibling);

    // The host surfaces of one framework window's SurfaceViews.
    synchronized List<Long> windows(Object owner) {
        List<Long> result = new ArrayList<>();
        for (Entry entry : entries.values()) if (entry.owner == owner && !entry.closed) result.add(entry.window);
        return result;
    }

    synchronized boolean has(Object owner) {
        for (Entry entry : entries.values()) if (entry.owner == owner) return true;
        return false;
    }

    // Stack host surfaces bottom to top in `order`.
    synchronized void restack(List<Long> order) {
        for (int i = order.size() - 2; i >= 0; i--) {
            int result = placeBelow(order.get(i), order.get(i + 1));
            if (result != 0) throw new IllegalStateException("surface order: " + result);
        }
    }

    void releaseLayer(Layer layer) {
        host.release(layer.window);
        layer.reader.close();
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
    // Runs on the thread that owns the window.
    void update(Object owner, View root, boolean shown) throws Exception {
        List<SurfaceView> visible = new ArrayList<>();
        if (shown && root != null) collect(root, visible);
        List<Entry> gone = new ArrayList<>();
        synchronized (this) {
            if (stopped) visible.clear();
            for (Entry entry : entries.values())
                if (entry.owner == owner && !visible.contains(entry.view)) gone.add(entry);
        }
        for (Entry entry : gone) destroy(entry, true);
        for (SurfaceView view : visible) {
            // This class is the SurfaceView's compositor. Its own updateSurface()
            // treats our host-drawn window (no valid ViewRootImpl surface) as
            // lost: it would call surfaceDestroyed and destroy the real Surface
            // installed below. Only its pre-draw listener sets mHaveFrame, and
            // every updateSurface() path returns while that is false. A
            // re-attach adds the listener again, so detach it on every pass.
            view.getViewTreeObserver().removeOnPreDrawListener(
                    (ViewTreeObserver.OnPreDrawListener) DRAW_LISTENER.get(view));
            HAVE_FRAME.setBoolean(view, false);
            int format = REQUESTED_FORMAT.getInt(view);
            int width = view.getWidth(), height = view.getHeight();
            int[] location = new int[2]; view.getLocationInWindow(location);
            int[] clip = visibleClip(view, location, width, height);
            Entry entry;
            boolean changed;
            synchronized (this) {
                entry = entries.get(view);
                changed = entry == null || entry.width != width || entry.height != height || entry.format != format;
                if (!changed && entry.x == location[0] && entry.y == location[1] && entry.clipLeft == clip[0]
                        && entry.clipTop == clip[1] && entry.clipRight == clip[2] && entry.clipBottom == clip[3])
                    continue;
            }
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
                synchronized (this) {
                    if (stopped) {  // stopped while this one was prepared
                        reader.close(); host.release(window); target.release();
                        continue;
                    }
                    entries.put(view, entry);
                }
                final Entry consumer = entry;
                // Video frames are handled off the main Looper: even a large frame must not hold up the app's UI.
                reader.setOnImageAvailableListener(available -> consume(consumer), frames());
            }
            synchronized (this) {
                entry.width = width;
                entry.height = height;
                entry.format = format;
                entry.x = location[0]; entry.y = location[1];
                entry.clipLeft = clip[0]; entry.clipTop = clip[1]; entry.clipRight = clip[2]; entry.clipBottom = clip[3];
                int positioned = entry.cpu ? placeVisible(entry) : geometry(entry.window, entry.x, entry.y, width, height);
                if (positioned != 0) throw new IllegalStateException("Surface geometry: " + positioned);
            }
            SurfaceHolder holder = view.getHolder();
            for (SurfaceHolder.Callback callback : callbacks(view)) {
                if (created) callback.surfaceCreated(holder);
                if (changed) callback.surfaceChanged(holder, format, width, height);
            }
            // The first frame is drawn: the window now punches the
            // SurfaceView's hole (SurfaceView.performDrawFinished).
            if (created && !DRAW_FINISHED.getBoolean(view)) DRAW_FINISHED_METHOD.invoke(view);
            System.out.println("APP_SURFACE " + view.getClass().getName() + " " + width + "x" + height
                    + " at " + location[0] + "," + location[1] + " format=" + format
                    + (clip[0] == 0 && clip[1] == 0 && clip[2] == width && clip[3] == height
                            ? "" : " clip=" + clip[0] + "," + clip[1] + "," + clip[2] + "," + clip[3]));
        }
    }

    // The visible part of a SurfaceView in its own pixels: all of it when nothing clips it, none when it is scrolled out
    // (SurfaceView hands SurfaceFlinger the same clip as the window crop).
    private static int[] visibleClip(View view, int[] location, int width, int height) {
        Rect visible = new Rect();
        if (!view.getGlobalVisibleRect(visible)) return new int[4];
        int left = Math.max(0, visible.left - location[0]), top = Math.max(0, visible.top - location[1]);
        int right = Math.min(width, visible.right - location[0]), bottom = Math.min(height, visible.bottom - location[1]);
        return right <= left || bottom <= top ? new int[4] : new int[] {left, top, right, bottom};
    }

    // A video layer holds the last visible part it was given, at its place; nothing visible shows a transparent pixel.
    private int placeVisible(Entry entry) {
        int w = entry.clipRight - entry.clipLeft, h = entry.clipBottom - entry.clipTop;
        if (w <= 0 || h <= 0) return present(entry.window, 0, 0, 1, 1, new int[1]);
        return geometry(entry.window, entry.x + entry.clipLeft, entry.y + entry.clipTop, w, h);
    }


    // CpuConsumer has already waited for the producer's acquire fence before
    // exposing planes. Closing the Image returns its buffer even on failure.
    // Presents the visible region [left,right) x [top,bottom) of the width x height layer from the image: the host
    // converts only that region, so a video far larger than the window costs what shows.
    private static int present(long window, int x, int y, Image image, int width, int height, int left, int top, int right,
            int bottom) {
        Rect crop = image.getCropRect();
        int regionWidth = right - left, regionHeight = bottom - top;
        if (crop.width() <= 0 || crop.height() <= 0 || width <= 0 || height <= 0 || regionWidth <= 0 || regionHeight <= 0
                || regionWidth > 4096 || regionHeight > 4096)
            throw new IllegalArgumentException("Surface image dimensions");
        Image.Plane[] planes = image.getPlanes();
        if (image.getFormat() != ImageFormat.YUV_420_888 || planes.length != 3)
            throw new UnsupportedOperationException("Surface image format: " + image.getFormat());
        int dataspace;
        try { dataspace = (Integer)Image.class.getMethod("getDataSpace").invoke(image); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
        int standard = (dataspace >> 16) & 63, transfer = (dataspace >> 22) & 31;
        if (transfer == 6 || transfer == 7) throw new UnsupportedOperationException("HDR Surface output");
        if (standard != 0 && standard != 1 && standard != 2 && standard != 4 && standard != 6)
            throw new UnsupportedOperationException("Surface color standard: " + standard);
        // BT.709 (1) and BT.2020 (6) have their own weights; everything else the SDK reports is BT.601.
        int matrix = standard == 1 ? 1 : standard == 6 ? 2 : 0;
        boolean full = ((dataspace >> 27) & 7) == 1;
        ByteBuffer[] bytes = {planes[0].getBuffer(), planes[1].getBuffer(), planes[2].getBuffer()};
        for (ByteBuffer buffer : bytes)
            if (!buffer.isDirect() || buffer.position() != 0) throw new UnsupportedOperationException("Surface image planes are not direct buffers");
        return presentYuv(window, x, y, regionWidth, regionHeight, width, height, left, top, crop.left, crop.top,
                crop.width(), crop.height(), bytes[0], bytes[1], bytes[2], planes[0].getRowStride(), planes[1].getRowStride(),
                planes[2].getRowStride(), planes[0].getPixelStride(), planes[1].getPixelStride(), planes[2].getPixelStride(),
                matrix, full ? 1 : 0);
    }

    // The frame thread. The view's position comes from its last update, made on
    // the window's own thread.
    private synchronized void consume(Entry entry) {
        if (entry.closed) return;
        long start = System.nanoTime();
        try (Image image = entry.reader.acquireLatestImage()) {
            if (image == null) return;
            entry.cpu = true;
            int w = entry.clipRight - entry.clipLeft, h = entry.clipBottom - entry.clipTop;
            if (w <= 0 || h <= 0) return;  // scrolled out: the layer shows nothing
            long acquired = System.nanoTime();
            int result = present(entry.window, entry.x + entry.clipLeft, entry.y + entry.clipTop, image, entry.width,
                    entry.height, entry.clipLeft, entry.clipTop, entry.clipRight, entry.clipBottom);
            long presented = System.nanoTime();
            if (result != 0 && result != -11) throw new IllegalStateException("Surface presentation: " + result);
            note(acquired - start, presented - acquired, (long) w * h);
        }
    }

    // One APP_SURFACE_FRAMES line per 10 s while video frames are shown: how many, how long each stage took on average
    // (acquire the image, then convert on the host and present) and the pixels per frame. Main-thread time spent
    // here is time the app does not draw.
    private long statsSince, statsFrames, statsAcquire, statsPresent, statsPixels;
    private void note(long acquire, long present, long pixels) {
        long now = System.nanoTime();
        if (statsSince == 0) statsSince = now;
        statsFrames++; statsAcquire += acquire; statsPresent += present; statsPixels += pixels;
        if (now - statsSince < 10_000_000_000L) return;
        double seconds = (now - statsSince) / 1e9;
        System.out.println(String.format(java.util.Locale.ROOT,
                "APP_SURFACE_FRAMES frames=%d fps=%.1f acquire_ms=%.2f present_ms=%.2f pixels=%d busy=%.0f%%",
                statsFrames, statsFrames / seconds, statsAcquire / 1e6 / statsFrames,
                statsPresent / 1e6 / statsFrames, statsPixels / statsFrames,
                100.0 * (statsAcquire + statsPresent) / (now - statsSince)));
        statsSince = now; statsFrames = statsAcquire = statsPresent = statsPixels = 0;
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

    // Destroys every surface of one window, on the thread that owns it.
    void destroy(Object owner) throws Exception {
        List<Entry> gone = new ArrayList<>();
        synchronized (this) {
            for (Entry entry : entries.values()) if (entry.owner == owner) gone.add(entry);
        }
        for (Entry entry : gone) destroy(entry, true);
    }

    // The app is stopping (ViewRootImpl.setWindowStopped): every surface is
    // destroyed and none is created again. Like WindowManagerGlobal's
    // setStoppedState, windows owned by other threads stop synchronously on
    // their Looper (Handler.runWithScissors, no timeout). A window whose
    // thread has quit its Looper gets no callbacks; its host surfaces go.
    void stop() throws Exception {
        List<Entry> all;
        synchronized (this) {
            stopped = true;
            all = new ArrayList<>(entries.values());
        }
        for (Entry entry : all) {
            // Through the owner's queue even while its window dies there.
            Handler owner = entry.thread;
            if (owner == null || owner.getLooper() == Looper.myLooper()) {
                destroy(entry, true);
                continue;
            }
            Exception[] failure = new Exception[1];
            Runnable task = () -> {
                try { destroy(entry, true); }
                catch (Exception error) { failure[0] = error; }
            };
            if (!(Boolean) RUN_WITH_SCISSORS.invoke(owner, task, 0L)) destroy(entry, false);
            if (failure[0] != null) throw failure[0];
        }
    }

    private static final Method RUN_WITH_SCISSORS;
    static {
        try {
            RUN_WITH_SCISSORS = Handler.class.getDeclaredMethod("runWithScissors", Runnable.class, long.class);
            RUN_WITH_SCISSORS.setAccessible(true);
        } catch (NoSuchMethodException error) {
            throw new ExceptionInInitializerError(error);
        }
    }

    @SuppressWarnings("unchecked")
    private static List<SurfaceHolder.Callback> callbacks(SurfaceView view) throws Exception {
        return new ArrayList<>((List<SurfaceHolder.Callback>) CALLBACKS.get(view));
    }

    // surfaceDestroyed runs while the host surface is still valid, as the
    // app may read it back (Unity copies its last frame).
    private void destroy(Entry entry, boolean callbacks) throws Exception {
        synchronized (this) {
            if (entries.remove(entry.view) != entry) return;  // destroyed by another path
        }
        if (callbacks) DRAW_FINISHED.setBoolean(entry.view, false);
        try {
            if (callbacks)
                for (SurfaceHolder.Callback callback : callbacks(entry.view))
                    callback.surfaceDestroyed(entry.view.getHolder());
        } finally {
            synchronized (this) {
                entry.closed = true;
                entry.reader.setOnImageAvailableListener(null, null);
                entry.reader.close();
                host.release(entry.window);
            }
            entry.view.getHolder().getSurface().release();
        }
        System.out.println("APP_SURFACE_DESTROYED " + entry.view.getClass().getName());
    }
}
