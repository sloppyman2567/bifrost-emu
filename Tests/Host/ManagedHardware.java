package org.bifrost.fixture;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Handler;
import android.view.FrameMetrics;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.Window;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * HWUI's GPU renderer on a SurfaceView's host-backed surface
 * (Surface.lockHardwareCanvas), and the Activity window itself drawn by its
 * ThreadedRenderer: frame metrics count frames HWUI rendered and swapped.
 */
public final class ManagedHardware extends Activity {
    private final Handler main = new Handler();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private SurfaceHolder holder;
    private TextView label;
    private int frames, windowFrames;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        FrameLayout content = new FrameLayout(this);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder created) { holder = created; }
            public void surfaceChanged(SurfaceHolder changed, int format, int width, int height) {
                System.out.println("MANAGED_HARDWARE_SURFACE " + width + "x" + height);
                main.post(ManagedHardware.this::draw);
            }
            public void surfaceDestroyed(SurfaceHolder destroyed) { holder = null; }
        });
        content.addView(view);
        label = new TextView(this);
        label.setTextSize(32);
        label.setTextColor(Color.WHITE);
        label.setBackgroundColor(Color.argb(160, 0, 0, 0));
        content.addView(label, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.START));
        setContentView(content);
        getWindow().addOnFrameMetricsAvailableListener(new Window.OnFrameMetricsAvailableListener() {
            public void onFrameMetricsAvailable(Window window, FrameMetrics metrics, int dropped) {
                if (metrics.getMetric(FrameMetrics.SWAP_BUFFERS_DURATION) >= 0) windowFrames++;
            }
        }, main);
    }

    private void draw() {
        if (holder == null) return;
        Canvas canvas = holder.getSurface().lockHardwareCanvas();
        if (!canvas.isHardwareAccelerated()) throw new AssertionError("software canvas");
        canvas.drawColor(frames % 2 == 0 ? Color.rgb(0, 96, 160) : Color.rgb(160, 96, 0));
        paint.setColor(Color.WHITE);
        paint.setTextSize(48);
        canvas.drawText("HWUI frame " + frames, 40, 120, paint);
        holder.getSurface().unlockCanvasAndPost(canvas);
        label.setText("Window frame " + frames);
        if (++frames < 30) { main.postDelayed(this::draw, 16); return; }
        System.out.println("MANAGED_HARDWARE_FRAMES " + frames);
        // Let the window's last frames report their metrics.
        main.postDelayed(() -> {
            System.out.println("MANAGED_HARDWARE_WINDOW accelerated=" + label.isHardwareAccelerated()
                    + " frames=" + windowFrames);
            showTexture();
        }, 500);
    }

    // A TextureView samples its producer's gralloc buffers through
    // EGL_NATIVE_BUFFER_ANDROID images (HWUI's DeferredLayerUpdater). First a
    // CPU canvas frame (RGBA), then an ImageWriter YUV_420_888 frame.
    private android.view.TextureView texture;
    private int textureStep;

    private void showTexture() {
        texture = new android.view.TextureView(this);
        texture.setSurfaceTextureListener(new android.view.TextureView.SurfaceTextureListener() {
            public void onSurfaceTextureAvailable(android.graphics.SurfaceTexture st, int width, int height) {
                android.view.Surface surface = new android.view.Surface(st);
                Canvas canvas = surface.lockCanvas(null);
                canvas.drawColor(Color.rgb(200, 40, 90));
                surface.unlockCanvasAndPost(canvas);
                surface.release();
            }
            public void onSurfaceTextureSizeChanged(android.graphics.SurfaceTexture st, int width, int height) {}
            public boolean onSurfaceTextureDestroyed(android.graphics.SurfaceTexture st) { return true; }
            public void onSurfaceTextureUpdated(android.graphics.SurfaceTexture st) {
                main.post(() -> textureUpdated(st));
            }
        });
        setContentView(texture, new android.view.ViewGroup.LayoutParams(64, 64));
    }

    private void textureUpdated(android.graphics.SurfaceTexture st) {
        android.graphics.Bitmap bitmap = texture.getBitmap(64, 64);
        int pixel = bitmap == null ? 0 : bitmap.getPixel(32, 32);
        String rgb = Color.red(pixel) + "," + Color.green(pixel) + "," + Color.blue(pixel);
        if (textureStep++ == 0) {
            System.out.println("MANAGED_HARDWARE_TEXTURE_RGBA " + rgb);
            // ImageWriter tags YUV_420_888 as JFIF (libui PublicFormat.cpp):
            // full-range BT.601 Y=81 U=90 V=240 is (238, 14, 14).
            android.media.ImageWriter writer = android.media.ImageWriter.newInstance(
                    new android.view.Surface(st), 1, android.graphics.ImageFormat.YUV_420_888);
            android.media.Image image = writer.dequeueInputImage();
            int[] values = {81, 90, 240};
            for (int p = 0; p < 3; p++) {
                java.nio.ByteBuffer plane = image.getPlanes()[p].getBuffer();
                while (plane.hasRemaining()) plane.put((byte) values[p]);
            }
            writer.queueInputImage(image);
            return;
        }
        if (textureStep == 2) {
            System.out.println("MANAGED_HARDWARE_TEXTURE_YUV " + rgb);
            // A HARDWARE bitmap (an AHardwareBuffer, as image loaders such as Glide produce) read back through HWUI's
            // GL import: YouTube's thumbnails are such bitmaps, and they showed as empty boxes.
            android.graphics.Bitmap source = android.graphics.Bitmap.createBitmap(64, 64, android.graphics.Bitmap.Config.ARGB_8888);
            source.eraseColor(Color.rgb(30, 160, 220));
            android.graphics.Bitmap hardware = source.copy(android.graphics.Bitmap.Config.HARDWARE, false);
            System.out.println("MANAGED_HARDWARE_BITMAP created=" + (hardware != null)
                    + (hardware == null ? "" : " config=" + hardware.getConfig()));
            android.graphics.Bitmap back = hardware == null ? null : hardware.copy(android.graphics.Bitmap.Config.ARGB_8888, false);
            int value = back == null ? 0 : back.getPixel(32, 32);
            System.out.println("MANAGED_HARDWARE_TEXTURE_HWBITMAP " + Color.red(value) + "," + Color.green(value) + "," + Color.blue(value));
            finish();
            return;
        }

    }
}
