package org.bifrost.fixture;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.view.SurfaceView;
import android.widget.Button;
import java.util.Arrays;
import java.util.List;

/**
 * A second Activity above a SurfaceView Activity: start for result, window, finish, resume.
 * First a provider intent query and a permission request round trip. The opaque second
 * Activity stops the root and hides its window; the translucent one only pauses it.
 */
public final class ManagedActivities extends Activity {
    private final Handler main = new Handler();
    private boolean requested, started;
    private int permissionResults;
    private int returned;  // results received

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(new SurfaceView(this));
    }

    @Override public void onResume() {
        super.onResume();
        System.out.println("MANAGED_ACTIVITIES_ROOT_RESUMED returned=" + returned);
        if (returned == 1) {
            // Second round: translucent, closed with the Back key instead of a button.
            startActivityForResult(new Intent(this, Translucent.class).putExtra("back", true), 43);
            return;
        }
        if (returned == 2) {
            main.postDelayed(this::finish, 300);
            return;
        }
        if (started) return;
        if (!requested) {
            requested = true;
            try {
                // No browser is installed: an implicit view Intent has no handler.
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.invalid/")));
                throw new AssertionError("implicit Intent resolved");
            } catch (ActivityNotFoundException expected) {
                System.out.println("MANAGED_ACTIVITIES_UNRESOLVED_OK");
            }
            // Provider intent filters come from the installed split cluster's manifest.
            List<ResolveInfo> files = getPackageManager().queryIntentContentProviders(
                    new Intent("org.bifrost.fixture.FILES"), 0);
            if (files.size() != 1 || !files.get(0).providerInfo.name.equals(Files.class.getName())
                    || !getPackageManager().queryIntentContentProviders(
                            new Intent("org.bifrost.fixture.NONE"), 0).isEmpty())
                throw new AssertionError("provider intent query incorrect: " + files);
            System.out.println("MANAGED_ACTIVITIES_PROVIDER_QUERY_OK");
            // Off the main thread, as Unity requests permissions.
            new Thread(() -> requestPermissions(new String[] {android.Manifest.permission.INTERNET,
                    android.Manifest.permission.CAMERA}, 7)).start();
            return;
        }
        if (permissionResults < 2) return;
        started = true;
        startActivityForResult(new Intent(this, Second.class).putExtra("value", 7), 42);
    }

    @Override public void onRequestPermissionsResult(int request, String[] names, int[] grants) {
        System.out.println("MANAGED_ACTIVITIES_PERMISSIONS " + request + " " + Arrays.toString(names)
                + " " + Arrays.toString(grants));
        // A finished request allows the next one; a stale pending request
        // would cancel it with empty arrays.
        if (++permissionResults == 1)
            requestPermissions(new String[] {android.Manifest.permission.CAMERA}, 8);
    }

    /** Declared with an intent filter; queries resolve it without instantiating it. */
    public static final class Files {}

    @Override public void onPause() {
        super.onPause();
        System.out.println("MANAGED_ACTIVITIES_ROOT_PAUSED");
    }

    @Override public void onStop() {
        super.onStop();
        System.out.println("MANAGED_ACTIVITIES_ROOT_STOPPED");
    }

    @Override public void onRestart() {
        super.onRestart();
        System.out.println("MANAGED_ACTIVITIES_ROOT_RESTARTED");
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

    /** Theme.Translucent in the manifest: the root stays visible beneath it. */
    public static final class Translucent extends Second {}

    public static class Second extends Activity {
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
