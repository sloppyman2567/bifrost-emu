package org.bifrost.fixture;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.view.SurfaceView;
import android.widget.Button;

/** A second Activity above a SurfaceView Activity: start for result, window, finish, resume. */
public final class ManagedActivities extends Activity {
    private final Handler main = new Handler();
    private boolean started;
    private int returned;  // results received

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(new SurfaceView(this));
    }

    @Override public void onResume() {
        super.onResume();
        System.out.println("MANAGED_ACTIVITIES_ROOT_RESUMED returned=" + returned);
        if (returned == 1) {
            // Second round: closed with the Back key instead of a button.
            startActivityForResult(new Intent(this, Second.class).putExtra("back", true), 43);
            return;
        }
        if (returned == 2) {
            main.postDelayed(this::finish, 300);
            return;
        }
        if (started) return;
        started = true;
        try {
            // No browser is installed: an implicit view Intent has no handler.
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.invalid/")));
            throw new AssertionError("implicit Intent resolved");
        } catch (ActivityNotFoundException expected) {
            System.out.println("MANAGED_ACTIVITIES_UNRESOLVED_OK");
        }
        startActivityForResult(new Intent(this, Second.class).putExtra("value", 7), 42);
    }

    @Override public void onPause() {
        super.onPause();
        System.out.println("MANAGED_ACTIVITIES_ROOT_PAUSED");
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        System.out.println("MANAGED_ACTIVITIES_RESULT " + request + " " + result + " "
                + (data == null ? "null" : data.getIntExtra("answer", -1)));
        returned++;
    }

    @Override public void onDestroy() {
        super.onDestroy();
        System.out.println("MANAGED_ACTIVITIES_ROOT_DESTROYED");
    }

    public static final class Second extends Activity {
        private final Handler main = new Handler();

        @Override public void onCreate(Bundle state) {
            super.onCreate(state);
            System.out.println("MANAGED_ACTIVITIES_SECOND_CREATED value="
                    + getIntent().getIntExtra("value", -1) + " caller=" + getCallingActivity());
            Button close = new Button(this);
            close.setText("Close second");
            close.setOnClickListener(view -> {
                System.out.println("MANAGED_ACTIVITIES_SECOND_CLICKED");
                setResult(RESULT_OK, new Intent().putExtra("answer", 42));
                finish();
            });
            setContentView(close);
        }

        @Override public void onResume() {
            super.onResume();
            System.out.println("MANAGED_ACTIVITIES_SECOND_RESUMED");
            // Leave time for the launcher to lay out and draw the window,
            // then press the button through the Activity's view tree.
            main.postDelayed(() -> {
                android.view.View decor = getWindow().getDecorView();
                if (!decor.isAttachedToWindow() || decor.getWidth() == 0)
                    throw new IllegalStateException("second Activity window was not laid out");
                System.out.println("MANAGED_ACTIVITIES_SECOND_WINDOW " + decor.getWidth() + "x" + decor.getHeight());
                if (getIntent().getBooleanExtra("back", false)) {
                    // FrameworkWindows delivers host keys to the focused
                    // window's view tree; BACK finishes with RESULT_CANCELED.
                    for (int action : new int[] {android.view.KeyEvent.ACTION_DOWN, android.view.KeyEvent.ACTION_UP})
                        decor.dispatchKeyEvent(new android.view.KeyEvent(action, android.view.KeyEvent.KEYCODE_BACK));
                    // requestFinish posts finishAfterTransition, as in AOSP.
                    System.out.println("MANAGED_ACTIVITIES_SECOND_BACK");
                    return;
                }
                ((android.view.ViewGroup) decor.findViewById(android.R.id.content)).getChildAt(0).performClick();
            }, 1500);
        }

        @Override public void onWindowFocusChanged(boolean focused) {
            super.onWindowFocusChanged(focused);
            System.out.println("MANAGED_ACTIVITIES_SECOND_FOCUS " + focused);
        }

        @Override public void onDestroy() {
            super.onDestroy();
            System.out.println("MANAGED_ACTIVITIES_SECOND_DESTROYED");
        }
    }
}
