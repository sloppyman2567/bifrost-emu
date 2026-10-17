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
            finish();
        }, 500);
    }
}
