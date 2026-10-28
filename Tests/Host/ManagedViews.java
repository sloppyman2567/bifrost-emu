package org.bifrost.fixture;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;

/**
 * An ordinary View Activity; a SurfaceView added, hidden, shown and removed later.
 * The button fades in with a legacy view animation, which only ends if the
 * window's draw time advances (a fragment's enter animation is one).
 */
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
        android.view.animation.AlphaAnimation fade = new android.view.animation.AlphaAnimation(0f, 1f);
        fade.setDuration(300);
        fade.setAnimationListener(new android.view.animation.Animation.AnimationListener() {
            public void onAnimationStart(android.view.animation.Animation animation) { }
            public void onAnimationRepeat(android.view.animation.Animation animation) { }
            public void onAnimationEnd(android.view.animation.Animation animation) {
                System.out.println("MANAGED_VIEWS_ANIMATION_END");
            }
        });
        button.startAnimation(fade);
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
        // Scrolled 100 px above the window: only its lower 200 rows show (the host layer is clipped to them).
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(400, 300);
        params.topMargin = -100;
        content.addView(surface, 0, params);
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
