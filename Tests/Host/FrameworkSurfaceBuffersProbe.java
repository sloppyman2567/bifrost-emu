// Runs inside the real Android 15 framework through test_android_package.py (built with the launcher).
//
// A SurfaceView's queue is an opaque ImageReader (FrameworkSurfaces.newReader) read through the Image's HardwareBuffer
// (FrameworkSurfaces.lockBuffer). This writes known frames into such a reader with the producers an app has and checks the
// pixels that come back: a YUV 4:2:0 producer (what a codec writes), an RGBA producer (a GPU or CPU producer), one reader
// whose producer switches from one to the other and back (YouTube's first video on a view did, and the queue it was
// connected to was abandoned), and a producer that names no format (Surface.lockCanvas), which gets the queue's default.
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.hardware.SyncFence;
import android.media.Image;
import android.media.ImageReader;
import android.media.ImageWriter;
import android.view.Surface;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

public final class FrameworkSurfaceBuffersProbe {
    static { System.load("/data/local/tmp/libbifrost-activity.so"); }

    private static final int WIDTH = 64, HEIGHT = 32;
    private static int checks;

    private static void check(boolean result, String message) {
        if (!result) throw new AssertionError(message);
        checks++;
    }

    // libcore.io.Memory reads native memory, which is where lockBuffer's planes are.
    private static final Method PEEK;
    static {
        try { PEEK = Class.forName("libcore.io.Memory").getMethod("peekByteArray", long.class, byte[].class, int.class, int.class); }
        catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }

    // The plane's samples as rows x columns, taking pixelStride into account.
    private static int[][] peek(long address, long rowStride, long pixelStride, int rows, int columns) throws Exception {
        int[][] samples = new int[rows][columns];
        byte[] row = new byte[(int) (pixelStride * (columns - 1) + 1)];
        for (int y = 0; y < rows; y++) {
            PEEK.invoke(null, address + y * rowStride, row, 0, row.length);
            for (int x = 0; x < columns; x++) samples[y][x] = row[(int) (x * pixelStride)] & 0xff;
        }
        return samples;
    }

    private static int yuvY(int x, int y) { return (x * 3 + y * 5 + 7) & 0xff; }
    private static int yuvU(int x, int y) { return (40 + x * 2 + y) & 0xff; }
    private static int yuvV(int x, int y) { return (200 - x - y * 3) & 0xff; }
    private static int red(int x) { return (x * 4) & 0xff; }
    private static int green(int y) { return (y * 8) & 0xff; }

    private static void fill(Image.Plane plane, int rows, int columns, java.util.function.IntBinaryOperator value) {
        ByteBuffer buffer = plane.getBuffer();
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < columns; x++)
                buffer.put(y * plane.getRowStride() + x * plane.getPixelStride(), (byte) value.applyAsInt(x, y));
    }

    // One YUV_420_888 frame from an ImageWriter, which connects to the reader's queue as a producer of that format.
    private static void writeYuv(ImageReader reader) {
        try (ImageWriter writer = ImageWriter.newInstance(reader.getSurface(), 2, ImageFormat.YUV_420_888)) {
            Image image = writer.dequeueInputImage();
            Image.Plane[] planes = image.getPlanes();
            fill(planes[0], HEIGHT, WIDTH, FrameworkSurfaceBuffersProbe::yuvY);
            fill(planes[1], HEIGHT / 2, WIDTH / 2, FrameworkSurfaceBuffersProbe::yuvU);
            fill(planes[2], HEIGHT / 2, WIDTH / 2, FrameworkSurfaceBuffersProbe::yuvV);
            image.setTimestamp(1);
            writer.queueInputImage(image);
        }
    }

    private static void writeRgba(ImageReader reader) {
        try (ImageWriter writer = ImageWriter.newInstance(reader.getSurface(), 2, android.graphics.PixelFormat.RGBA_8888)) {
            Image image = writer.dequeueInputImage();
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buffer = plane.getBuffer();
            for (int y = 0; y < HEIGHT; y++)
                for (int x = 0; x < WIDTH; x++) {
                    int at = y * plane.getRowStride() + x * plane.getPixelStride();
                    buffer.put(at, (byte) red(x)); buffer.put(at + 1, (byte) green(y));
                    buffer.put(at + 2, (byte) 0x55); buffer.put(at + 3, (byte) 0xff);
                }
            image.setTimestamp(2);
            writer.queueInputImage(image);
        }
    }

    // The next frame, locked as FrameworkSurfaces does it: the acquire fence first, then the HardwareBuffer.
    private interface Frame { void read(long[] planes) throws Exception; }
    private static void locked(ImageReader reader, Frame frame) throws Exception {
        try (Image image = reader.acquireNextImage()) {
            check(image != null, "no frame reached the reader");
            check(image.getFormat() == ImageFormat.PRIVATE, "an opaque reader's image has format " + image.getFormat());
            try (SyncFence fence = image.getFence()) { check(!fence.isValid() || fence.awaitForever(), "acquire fence did not signal"); }
            long[] planes = new long[FrameworkSurfaces.PLANE_VALUES];
            try (HardwareBuffer buffer = image.getHardwareBuffer()) {
                int result = FrameworkSurfaces.lockBuffer(buffer, planes);
                check(result == 0, "lockBuffer returned " + result + " for format 0x" + Integer.toHexString(buffer.getFormat()));
                try { frame.read(planes); } finally { FrameworkSurfaces.unlockBuffer(buffer); }
            }
        }
    }

    private static void readYuv(long[] planes) throws Exception {
        check(planes[0] == 0x23 && planes[3] == 3, "YUV frame format 0x" + Long.toHexString(planes[0]) + " planes " + planes[3]);
        check(planes[1] == WIDTH && planes[2] == HEIGHT, "YUV frame size " + planes[1] + "x" + planes[2]);
        int[][] y = peek(planes[4], planes[5], planes[6], HEIGHT, WIDTH);
        int[][] u = peek(planes[8], planes[9], planes[10], HEIGHT / 2, WIDTH / 2);
        int[][] v = peek(planes[12], planes[13], planes[14], HEIGHT / 2, WIDTH / 2);
        for (int row = 0; row < HEIGHT; row++)
            for (int x = 0; x < WIDTH; x++)
                check(y[row][x] == yuvY(x, row), "Y(" + x + "," + row + ") = " + y[row][x] + ", wanted " + yuvY(x, row));
        for (int row = 0; row < HEIGHT / 2; row++)
            for (int x = 0; x < WIDTH / 2; x++) {
                check(u[row][x] == yuvU(x, row), "U(" + x + "," + row + ") = " + u[row][x] + ", wanted " + yuvU(x, row));
                check(v[row][x] == yuvV(x, row), "V(" + x + "," + row + ") = " + v[row][x] + ", wanted " + yuvV(x, row));
            }
        // The host reads sample (x, y) at row * rowStride + x * pixelStride, so each plane must hold that much.
        for (int p = 0; p < 3; p++) {
            long rows = p == 0 ? HEIGHT : HEIGHT / 2, columns = p == 0 ? WIDTH : WIDTH / 2;
            long need = planes[5 + p * 4] * (rows - 1) + planes[6 + p * 4] * (columns - 1) + 1;
            check(planes[7 + p * 4] >= need, "plane " + p + " size " + planes[7 + p * 4] + " is under " + need);
        }
    }

    private static void readRgba(long[] planes, int formatA, int formatB) throws Exception {
        check(planes[3] == 1 && (planes[0] == formatA || planes[0] == formatB),
                "RGBA frame format 0x" + Long.toHexString(planes[0]) + " planes " + planes[3]);
        check(planes[1] == WIDTH && planes[2] == HEIGHT && planes[6] == 4, "RGBA frame " + planes[1] + "x" + planes[2]
                + " pixel stride " + planes[6]);
        byte[] row = new byte[(int) (planes[5] * (HEIGHT - 1) + WIDTH * 4)];
        PEEK.invoke(null, planes[4], row, 0, row.length);
        check(planes[7] == row.length, "RGBA plane size " + planes[7] + ", wanted " + row.length);
        for (int y = 0; y < HEIGHT; y++)
            for (int x = 0; x < WIDTH; x++) {
                int at = (int) (y * planes[5] + x * 4);
                check((row[at] & 0xff) == red(x) && (row[at + 1] & 0xff) == green(y) && (row[at + 2] & 0xff) == 0x55
                        && (row[at + 3] & 0xff) == 0xff, "RGBA(" + x + "," + y + ") = " + (row[at] & 0xff) + ","
                        + (row[at + 1] & 0xff) + "," + (row[at + 2] & 0xff) + "," + (row[at + 3] & 0xff));
            }
    }

    private static void run() throws Exception {
        ImageReader reader = FrameworkSurfaces.newReader(WIDTH, HEIGHT);
        check(reader.getImageFormat() == ImageFormat.PRIVATE, "reader is not opaque");
        // YUV, then RGBA, then YUV again on the same reader: the producer changes and nothing is abandoned or replaced.
        writeYuv(reader);
        locked(reader, FrameworkSurfaceBuffersProbe::readYuv);
        writeRgba(reader);
        locked(reader, planes -> readRgba(planes, 1, 1));
        writeYuv(reader);
        locked(reader, FrameworkSurfaceBuffersProbe::readYuv);
        writeRgba(reader);
        locked(reader, planes -> readRgba(planes, 1, 1));
        // A producer that names no format gets the queue's default (IMPLEMENTATION_DEFINED, which is linear RGBA here).
        Surface surface = reader.getSurface();
        Canvas canvas = surface.lockCanvas(null);
        check(canvas != null && canvas.getWidth() == WIDTH && canvas.getHeight() == HEIGHT, "lockCanvas gave no canvas");
        canvas.drawColor(0xff336699);
        surface.unlockCanvasAndPost(canvas);
        locked(reader, planes -> {
            check(planes[3] == 1 && (planes[0] == 1 || planes[0] == 0x22), "canvas frame format 0x" + Long.toHexString(planes[0]));
            int[][] red = peek(planes[4], planes[5], 4, HEIGHT, WIDTH);
            check(red[0][0] == 0x33 && red[HEIGHT - 1][WIDTH - 1] == 0x33, "canvas frame is not 0x336699: red=" + red[0][0]);
        });
        reader.close();
        // A buffer that is not RGBA or YUV 4:2:0 is refused without locking it.
        try (ImageReader other = ImageReader.newInstance(WIDTH, HEIGHT, ImageFormat.PRIVATE, 2, HardwareBuffer.USAGE_CPU_READ_OFTEN);
                ImageWriter writer = ImageWriter.newInstance(other.getSurface(), 2, android.graphics.PixelFormat.RGB_565)) {
            Image image = writer.dequeueInputImage();
            writer.queueInputImage(image);
            try (Image acquired = other.acquireNextImage(); HardwareBuffer buffer = acquired.getHardwareBuffer()) {
                check(FrameworkSurfaces.lockBuffer(buffer, new long[FrameworkSurfaces.PLANE_VALUES]) == -95,
                        "an RGB_565 buffer was not refused with ENOTSUP");
            }
        }
        System.out.println("FRAMEWORK_SURFACE_BUFFERS_OK checks=" + checks);
        System.out.println("FRAMEWORK_OK");
    }

    public static void main(String[] args) {
        try { run(); System.exit(0); }
        catch (Throwable error) { error.printStackTrace(); System.exit(1); }
    }
}
