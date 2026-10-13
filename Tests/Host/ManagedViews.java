package org.bifrost.fixture;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;

/** An ordinary View Activity; a SurfaceView added, hidden, shown and removed later. */
public final class ManagedViews extends Activity {
    private final Handler main = new Handler();
    private FrameLayout content;
    private SurfaceView surface;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        content = new FrameLayout(this);
        Button button = new Button(this);
        button.setText("Overlay button");
        content.addView(button, new FrameLayout.LayoutParams(320, 120));
        setContentView(content);
    }

    @Override public void onResume() {
        super.onResume();
        System.out.println("MANAGED_VIEWS_RESUMED");
        main.postDelayed(this::addSurface, 800);
    }

    private void addSurface() {
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder holder) {
                System.out.println("MANAGED_VIEWS_SURFACE_CREATED");
            }
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                System.out.println("MANAGED_VIEWS_SURFACE_CHANGED " + width + "x" + height);
            }
            public void surfaceDestroyed(SurfaceHolder holder) {
                System.out.println("MANAGED_VIEWS_SURFACE_DESTROYED");
            }
        });
        // Below the button in drawing order: the button stays over the surface.
        content.addView(surface, 0, new FrameLayout.LayoutParams(400, 300));
        main.postDelayed(() -> surface.setVisibility(View.INVISIBLE), 800);
        main.postDelayed(() -> surface.setVisibility(View.VISIBLE), 1600);
        main.postDelayed(() -> content.removeView(surface), 2400);
        main.postDelayed(this::finish, 3200);
    }

    @Override public void onDestroy() {
        super.onDestroy();
        System.out.println("MANAGED_VIEWS_DESTROYED");
    }
}
