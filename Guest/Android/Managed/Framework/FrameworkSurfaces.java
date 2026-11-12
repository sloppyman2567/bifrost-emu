// SurfaceViews in framework windows. SurfaceView.updateSurface() gives up
// without a SurfaceFlinger-backed window surface, so this applies its
// visibility contract instead: a SurfaceView has a host surface while it is
// attached, its window is visible and not stopped, the view is shown and its
// frame is non-empty (SurfaceView.mRequestedVisible and mHaveFrame). The
// surface is created when that becomes true, reported again when its size or
// format changes, and destroyed when it becomes false or its window goes.
// Callbacks run in SurfaceView's order: surfaceCreated, then surfaceChanged.
// A window whose ViewRootImpl took its surface (Window.takeSurface: NativeActivity) gets one the same way, sized to the
// window, with ViewRootImpl's callbacks: surfaceCreated, surfaceChanged with the window's format, then
// surfaceRedrawNeeded (updateTaken).
//
// Each window's surfaces are updated and destroyed on the thread that owns
// the window (ViewRootImpl.mThread), where Android delivers SurfaceHolder
// callbacks; frames are consumed on the main Looper. The lock on this object
// guards the entries and every host call on an entry's window, never the
// app's callbacks, so a released host window is never presented again.
import android.graphics.PixelFormat;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.hardware.HardwareBuffer;
import android.hardware.SyncFence;
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
import java.io.IOException;
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
        final SurfaceView view;  // null for a window's taken surface
        final SurfaceHolder holder;  // the view's holder, or ViewRootImpl's TakenSurfaceHolder
        final Object owner;  // the framework window holding the view
        final Handler thread;  // the window's ViewRootImpl handler, kept past detach
        final long window;
        ImageReader reader;
        int width, height, format;  // guarded by FrameworkSurfaces
        int x = Integer.MIN_VALUE, y = Integer.MIN_VALUE;
        // The part of the view that shows, in view pixels (getGlobalVisibleRect: clipped by every ancestor and the
        // window); empty when scrolled out. Frames convert and present only this part.
        int clipLeft, clipTop, clipRight, clipBottom;
        boolean cpu;  // frames arrive through the ImageReader (a video), not a native window
        boolean gpu;  // producer's buffers are not CPU-lockable: nothing can be shown
        int frameFormat;  // the HAL format of the last frame shown, to log a change once
        boolean closed;
        Entry(SurfaceView view, SurfaceHolder holder, Handler thread, Object owner, long window, ImageReader reader) {
            this.view = view; this.holder = holder; this.thread = thread; this.owner = owner; this.window = window;
            this.reader = reader;
        }
        Object key() { return view != null ? view : holder; }
    }

    private static final Field CALLBACKS = field("mCallbacks");
    private static final Field REQUESTED_FORMAT = field("mRequestedFormat");
    private static final Field SURFACE_FRAME = field("mSurfaceFrame");
    private static final Field DRAW_FINISHED = field("mDrawFinished");
    private static final Method DRAW_FINISHED_METHOD = method("performDrawFinished");
    private static final Field HAVE_FRAME = field("mHaveFrame");
    private static final Field DRAW_LISTENER = field("mDrawListener");

    private final Host host;
    private final Map<Object, Entry> entries = new IdentityHashMap<>();  // keyed by SurfaceView or taken holder
    private boolean stopped;  // ViewRootImpl.setWindowStopped: no surface is created again
    private static native int present(long window, int x, int y, int width, int height, int[] argb);
    // Locks an acquired Image's HardwareBuffer for CPU reads and describes its planes in `planes` (surface_buffers.c):
    // format, width, height, plane count, then per plane the address, row stride, pixel stride and readable bytes. RGBA is
    // one plane, YUV 4:2:0 is Y, U and V. 0, or a negative errno: -ENOTSUP (-95) for any other format. Unlock it after use.
    static native int lockBuffer(HardwareBuffer buffer, long[] planes);
    static native void unlockBuffer(HardwareBuffer buffer);
    static final int PLANE_VALUES = 16;
    // Ops 26 and 25: the visible region of a width x height layer from the locked planes, converted by the host.
    private static native int presentPlanes(long window, int x, int y, int width, int height, int layerWidth, int layerHeight,
            int left, int top, int cropLeft, int cropTop, int cropWidth, int cropHeight, long[] planes, int matrix, int fullRange);
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

    // A layer of one translucent colour (a dim layer), presented as pixels.
    void fillLayer(Layer layer, int argb) {
        int[] pixels = new int[layer.width * layer.height];
        java.util.Arrays.fill(pixels, argb);
        int result = present(layer.window, 0, 0, layer.width, layer.height, pixels);
        if (result != 0 && result != -11) throw new IllegalStateException("window layer fill: " + result);
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
                if (entry.owner == owner && entry.view != null && !visible.contains(entry.view)) gone.add(entry);
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
                // The queue's reader takes any format (see newReader): the
                // producer is not known before it connects, and a reader
                // configured for one format throws on another.
                ImageReader reader = newReader(width, height);
                Surface target = view.getHolder().getSurface();
                long window;
                try {
                    Surface.class.getMethod("transferFrom", Surface.class).invoke(target, reader.getSurface());
                    window = host.prepare(target, !PixelFormat.formatHasAlpha(format));
                    if (window <= 0) throw new IllegalStateException("surface preparation: " + window);
                } catch (Exception error) {
                    reader.close(); target.release(); throw error;
                }
                entry = new Entry(view, view.getHolder(), view.getHandler(), owner, window, reader);
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

    // A window's taken surface (ViewRootImpl.mSurfaceHolder), on the thread that owns the window: while it is shown, a
    // host surface sized to the window at its position, delivered as ViewRootImpl.performTraversals delivers it
    // (holder.mSurface, setSurfaceFrameSize, surfaceCreated, then surfaceChanged with the window's format), followed
    // by surfaceRedrawNeeded as performDraw asks for the first frame and after every change. Destroyed when the window
    // stops showing. Returns whether the window has a surface now.
    boolean updateTaken(Object owner, SurfaceHolder holder, Handler thread, int x, int y, int width, int height,
            int format, boolean shown) throws Exception {
        Entry entry;
        synchronized (this) {
            entry = entries.get(holder);
            if (stopped) shown = false;
        }
        if (!shown || width <= 0 || height <= 0) {
            if (entry != null) destroy(entry, true);
            return false;
        }
        boolean created = entry == null;
        boolean changed;
        synchronized (this) {
            changed = created || entry.width != width || entry.height != height || entry.format != format;
            if (!changed && entry.x == x && entry.y == y) return true;
        }
        if (created) {
            ImageReader reader = newReader(width, height);
            Surface target = emptySurface();
            long window;
            try {
                Surface.class.getMethod("transferFrom", Surface.class).invoke(target, reader.getSurface());
                window = host.prepare(target, !PixelFormat.formatHasAlpha(format));
                if (window <= 0) throw new IllegalStateException("taken surface preparation: " + window);
            } catch (Exception error) {
                reader.close(); target.release(); throw error;
            }
            entry = new Entry(null, holder, thread, owner, window, reader);
            synchronized (this) {
                if (stopped) {
                    reader.close(); host.release(window); target.release();
                    return false;
                }
                entries.put(holder, entry);
            }
            final Entry consumer = entry;
            reader.setOnImageAvailableListener(available -> consume(consumer), frames());
            setTakenSurface(holder, target);
        }
        synchronized (this) {
            entry.width = width; entry.height = height; entry.format = format;
            entry.x = x; entry.y = y;
            entry.clipLeft = 0; entry.clipTop = 0; entry.clipRight = width; entry.clipBottom = height;
            int positioned = entry.cpu ? placeVisible(entry) : geometry(entry.window, x, y, width, height);
            if (positioned != 0) throw new IllegalStateException("taken surface geometry: " + positioned);
        }
        if (!changed) return true;
        TAKEN_FRAME_SIZE.invoke(holder, width, height);
        SurfaceHolder.Callback[] callbacks = takenCallbacks(holder);
        if (callbacks != null) {
            if (created) for (SurfaceHolder.Callback callback : callbacks) callback.surfaceCreated(holder);
            for (SurfaceHolder.Callback callback : callbacks) callback.surfaceChanged(holder, format, width, height);
            for (SurfaceHolder.Callback callback : callbacks)
                if (callback instanceof SurfaceHolder.Callback2)
                    ((SurfaceHolder.Callback2) callback).surfaceRedrawNeeded(holder);
        }
        System.out.println("APP_SURFACE_TAKEN " + width + "x" + height + " at " + x + "," + y + " format=" + format);
        return true;
    }

    // BaseSurfaceHolder (hidden): ViewRootImpl sets mSurface under mSurfaceLock and reads the callbacks after
    // ungetCallbacks, so a callback added since is seen.
    private static final Method TAKEN_FRAME_SIZE, TAKEN_UNGET, TAKEN_CALLBACKS;
    private static final Field TAKEN_SURFACE, TAKEN_LOCK;
    static {
        try {
            Class<?> type = Class.forName("com.android.internal.view.BaseSurfaceHolder");
            TAKEN_FRAME_SIZE = type.getMethod("setSurfaceFrameSize", int.class, int.class);
            TAKEN_UNGET = type.getMethod("ungetCallbacks");
            TAKEN_CALLBACKS = type.getMethod("getCallbacks");
            TAKEN_SURFACE = type.getField("mSurface");
            TAKEN_LOCK = type.getField("mSurfaceLock");
        } catch (ReflectiveOperationException error) {
            throw new ExceptionInInitializerError(error);
        }
    }

    // new Surface(): the empty Surface ViewRootImpl uses (its constructor is hidden).
    private static Surface emptySurface() throws Exception {
        return Surface.class.getConstructor().newInstance();
    }

    private static SurfaceHolder.Callback[] takenCallbacks(SurfaceHolder holder) throws Exception {
        TAKEN_UNGET.invoke(holder);
        return (SurfaceHolder.Callback[]) TAKEN_CALLBACKS.invoke(holder);
    }

    private static void setTakenSurface(SurfaceHolder holder, Surface surface) throws Exception {
        java.util.concurrent.locks.Lock lock = (java.util.concurrent.locks.Lock) TAKEN_LOCK.get(holder);
        lock.lock();
        try { TAKEN_SURFACE.set(holder, surface); } finally { lock.unlock(); }
    }

    synchronized boolean hasTaken(Object owner) {
        for (Entry entry : entries.values()) if (entry.owner == owner && entry.view == null && !entry.closed) return true;
        return false;
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


    // The reader behind every SurfaceView's Surface. It is opaque (PRIVATE), so it checks nothing about a buffer's format and
    // never abandons its queue over one: a codec's YUV, a GPU or CPU producer's RGBA, and a producer that switches between
    // them all reach it. (A reader configured for one format throws on the other, and replacing it abandoned the queue a
    // codec was connected to.) CPU reads are requested so that producers allocate buffers that lockBuffer can map.
    static ImageReader newReader(int width, int height) {
        return ImageReader.newInstance(width, height, ImageFormat.PRIVATE, 3, HardwareBuffer.USAGE_CPU_READ_OFTEN);
    }

    // Presents the visible region [left,right) x [top,bottom) of the width x height layer from the image: the host converts
    // only that region, so a video far larger than the window costs what shows. An opaque Image has no planes; its
    // HardwareBuffer is locked for the call and read as RGBA or YUV 4:2:0. The lock does not wait for the producer, so its
    // acquire fence is waited for first (a non-opaque reader's lock did that itself). Closing the Image returns its buffer
    // even on failure. The crop an acquired Image reports is its whole buffer, which is what is presented.
    private int present(Entry entry, int x, int y, Image image, int width, int height, int left, int top, int right, int bottom) {
        int regionWidth = right - left, regionHeight = bottom - top;
        if (width <= 0 || height <= 0 || regionWidth <= 0 || regionHeight <= 0 || regionWidth > 4096 || regionHeight > 4096)
            throw new IllegalArgumentException("Surface image dimensions");
        // Wait only on a valid fence: an empty one (a producer that had nothing to wait for) answers false to awaitForever()
        // in android-15.0.0_r1, whatever its documentation says (SyncFence.await is "mNativePtr != 0 && nWait(...)").
        try (SyncFence fence = image.getFence()) {
            if (fence.isValid() && !fence.awaitForever()) throw new IllegalStateException("Surface image acquire fence");
        } catch (IOException error) {
            throw new IllegalStateException("Surface image acquire fence", error);
        }
        long[] planes = new long[PLANE_VALUES];
        try (HardwareBuffer buffer = image.getHardwareBuffer()) {
            int locked = lockBuffer(buffer, planes);
            if (locked == -95)
                throw new UnsupportedOperationException("Surface image format: 0x" + Integer.toHexString(buffer.getFormat()));
            if (locked != 0) throw new IllegalStateException("Surface image lock: " + locked);
            try {
                if (planes[0] != entry.frameFormat) {  // a producer that changes format shows up here once per change
                    entry.frameFormat = (int) planes[0];
                    System.out.println("APP_SURFACE_FRAME_FORMAT 0x" + Integer.toHexString(entry.frameFormat) + " "
                            + planes[1] + "x" + planes[2]);
                }
                int matrix = 0;
                boolean full = false;
                if (planes[3] == 3) {
                    int dataspace;
                    try { dataspace = (Integer)Image.class.getMethod("getDataSpace").invoke(image); }
                    catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
                    int standard = (dataspace >> 16) & 63, transfer = (dataspace >> 22) & 31;
                    if (transfer == 6 || transfer == 7) throw new UnsupportedOperationException("HDR Surface output");
                    if (standard != 0 && standard != 1 && standard != 2 && standard != 4 && standard != 6)
                        throw new UnsupportedOperationException("Surface color standard: " + standard);
                    // BT.709 (1) and BT.2020 (6) have their own weights; everything else the SDK reports is BT.601.
                    matrix = standard == 1 ? 1 : standard == 6 ? 2 : 0;
                    full = ((dataspace >> 27) & 7) == 1;
                }
                return presentPlanes(entry.window, x, y, regionWidth, regionHeight, width, height, left, top, 0, 0,
                        (int) planes[1], (int) planes[2], planes, matrix, full ? 1 : 0);
            } finally {
                unlockBuffer(buffer);
            }
        }
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
            int result = present(entry, entry.x + entry.clipLeft, entry.y + entry.clipTop, image, entry.width,
                    entry.height, entry.clipLeft, entry.clipTop, entry.clipRight, entry.clipBottom);
            long presented = System.nanoTime();
            if (result != 0 && result != -11) throw new IllegalStateException("Surface presentation: " + result);
            note(acquired - start, presented - acquired, (long) w * h);
        } catch (UnsupportedOperationException error) {
            // A frame this path cannot show (a format that is neither RGBA nor YUV 4:2:0, HDR, or an unknown colour
            // standard). An uncaught exception on the frame thread previously killed the process: log once and stop
            // consuming rather than take the app down.
            synchronized (this) {
                if (entry.closed) return;
                entry.gpu = true;
                entry.reader.setOnImageAvailableListener(null, frames());
            }
            System.out.println("APP_SURFACE_UNREADABLE " + error.getMessage());
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
            if (entries.remove(entry.key()) != entry) return;  // destroyed by another path
        }
        if (callbacks && entry.view != null) DRAW_FINISHED.setBoolean(entry.view, false);
        try {
            if (callbacks && entry.view != null) {
                for (SurfaceHolder.Callback callback : callbacks(entry.view)) callback.surfaceDestroyed(entry.holder);
            } else if (callbacks) {  // ViewRootImpl.notifyHolderSurfaceDestroyed
                SurfaceHolder.Callback[] all = takenCallbacks(entry.holder);
                if (all != null) for (SurfaceHolder.Callback callback : all) callback.surfaceDestroyed(entry.holder);
            }
        } finally {
            synchronized (this) {
                entry.closed = true;
                entry.reader.setOnImageAvailableListener(null, null);
                entry.reader.close();
                host.release(entry.window);
            }
            entry.holder.getSurface().release();
            if (entry.view == null) setTakenSurface(entry.holder, emptySurface());  // as ViewRootImpl leaves it
        }
        System.out.println("APP_SURFACE_DESTROYED " + (entry.view != null ? entry.view.getClass().getName() : "taken"));
    }
}
