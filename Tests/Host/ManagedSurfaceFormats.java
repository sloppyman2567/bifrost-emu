package org.bifrost.fixture;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.ImageFormat;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageWriter;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.nio.ByteBuffer;

/**
 * A SurfaceView whose producer changes: a real VP9 decoder (what YouTube connects first), then YUV 4:2:0 frames from an
 * ImageWriter, RGBA (a GPU or CPU producer), YUV and RGBA again, then a producer that names no format
 * (Surface.lockCanvas). The view's queue must take every one without
 * being replaced: YouTube's first video on a view used to abandon the queue its codec was connected to, and the codec
 * failed with "BufferQueue has been abandoned". Frames go through the framework's own consumer to the host window.
 */
public final class ManagedSurfaceFormats extends Activity {
    private static final int FRAMES = 4;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                System.out.println("SURFACE_FORMATS_CREATED");
                Thread producer = new Thread(() -> produce(holder.getSurface()), "formats-producer");
                producer.start();
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
            @Override public void surfaceDestroyed(SurfaceHolder holder) {}
        });
        setContentView(view);
    }

    private void produce(Surface surface) {
        try {
            codec(surface);
            for (int round = 0; round < 2; round++) {
                yuv(surface, round);
                rgba(surface, round);
            }
            canvas(surface);
            System.out.println("SURFACE_FORMATS_DONE");
        } catch (Throwable error) {
            error.printStackTrace();
            System.out.println("SURFACE_FORMATS_FAILED " + error);
        }
        runOnUiThread(this::finish);
    }

    // One 64x64 black VP9 key frame (the host decoder's own probe stream, Y=16 and U=V=128), decoded FRAMES times to the
    // view's Surface and rendered: the decoder's YUV output is the first thing the queue receives, as in a video app.
    private static final int[] VP9_FRAME = {0x82, 0x49, 0x83, 0x42, 0x00, 0x03, 0xf0, 0x03, 0xf6, 0x00, 0x38, 0x24, 0x1c, 0x18,
            0x4a, 0x00, 0x00, 0x30, 0x60, 0x00, 0x00, 0x67, 0x3f, 0xff, 0xff, 0x56, 0x45, 0x64, 0xee, 0xcc, 0x00};

    private void codec(Surface surface) throws Exception {
        byte[] frame = new byte[VP9_FRAME.length];
        for (int i = 0; i < frame.length; i++) frame[i] = (byte) VP9_FRAME[i];
        MediaCodec decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_VP9);
        int fed = 0, shown = 0;
        try {
            decoder.configure(MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_VP9, 64, 64), surface, null, 0);
            decoder.start();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long deadline = System.nanoTime() + 20_000_000_000L;
            while (shown < FRAMES && System.nanoTime() < deadline) {
                if (fed < FRAMES) {
                    int input = decoder.dequeueInputBuffer(10_000);
                    if (input >= 0) {
                        ByteBuffer buffer = decoder.getInputBuffer(input);
                        buffer.clear();
                        buffer.put(frame);
                        decoder.queueInputBuffer(input, 0, frame.length, fed * 33_000L, 0);
                        fed++;
                    }
                }
                int output = decoder.dequeueOutputBuffer(info, 10_000);
                if (output >= 0) {
                    decoder.releaseOutputBuffer(output, true);
                    shown++;
                    Thread.sleep(60);
                }
            }
        } finally {
            decoder.stop();
            decoder.release();
        }
        System.out.println("SURFACE_FORMATS_CODEC frames=" + shown);
    }

    private static void fill(Image.Plane plane, int rows, int columns, int seed) {
        ByteBuffer buffer = plane.getBuffer();
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < columns; x++)
                buffer.put(y * plane.getRowStride() + x * plane.getPixelStride(), (byte) (seed + x + y));
    }

    // FRAMES frames of one YUV_420_888 writer, which connects to the view's queue and leaves it when closed.
    private void yuv(Surface surface, int round) throws InterruptedException {
        try (ImageWriter writer = ImageWriter.newInstance(surface, 3, ImageFormat.YUV_420_888)) {
            for (int frame = 0; frame < FRAMES; frame++) {
                Image image = writer.dequeueInputImage();
                int width = image.getWidth(), height = image.getHeight();
                Image.Plane[] planes = image.getPlanes();
                fill(planes[0], height, width, 16 + frame * 8);
                fill(planes[1], height / 2, width / 2, 90);
                fill(planes[2], height / 2, width / 2, 160);
                image.setTimestamp(System.nanoTime());
                writer.queueInputImage(image);
                Thread.sleep(60);
            }
        }
        System.out.println("SURFACE_FORMATS_PRODUCED yuv round=" + round);
    }

    private void rgba(Surface surface, int round) throws InterruptedException {
        try (ImageWriter writer = ImageWriter.newInstance(surface, 3, PixelFormat.RGBA_8888)) {
            for (int frame = 0; frame < FRAMES; frame++) {
                Image image = writer.dequeueInputImage();
                Image.Plane plane = image.getPlanes()[0];
                ByteBuffer buffer = plane.getBuffer();
                for (int y = 0; y < image.getHeight(); y++)
                    for (int x = 0; x < image.getWidth(); x++) {
                        int at = y * plane.getRowStride() + x * plane.getPixelStride();
                        buffer.put(at, (byte) (x + frame * 20)); buffer.put(at + 1, (byte) y);
                        buffer.put(at + 2, (byte) 0x40); buffer.put(at + 3, (byte) 0xff);
                    }
                image.setTimestamp(System.nanoTime());
                writer.queueInputImage(image);
                Thread.sleep(60);
            }
        }
        System.out.println("SURFACE_FORMATS_PRODUCED rgba round=" + round);
    }

    private void canvas(Surface surface) throws InterruptedException {
        for (int frame = 0; frame < FRAMES; frame++) {
            Canvas canvas = surface.lockCanvas(null);
            canvas.drawColor(0xff203040 + frame * 0x101010);
            surface.unlockCanvasAndPost(canvas);
            Thread.sleep(60);
        }
        System.out.println("SURFACE_FORMATS_PRODUCED canvas");
    }
}
