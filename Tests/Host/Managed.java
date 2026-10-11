package org.bifrost.fixture;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.content.SharedPreferences;
import android.provider.Settings;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/** Real ART/framework fixture: persistent data, Surface callbacks and finish. */
public final class Managed extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        SharedPreferences prefs = getSharedPreferences("fixture", MODE_PRIVATE);
        String id = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
        if (id == null || !id.matches("[0-9a-f]{16}")
                || !id.equals(prefs.getString("android_id", id)))
            throw new IllegalStateException("Android ID missing or changed across launches");
        if (!prefs.edit().putString("android_id", id).commit())
            throw new IllegalStateException("Android ID preference commit failed");
        if (Settings.Global.getString(getContentResolver(), "bifrost.missing") != null
                || Settings.Secure.getInt(getContentResolver(), "bifrost.missing", 37) != 37)
            throw new IllegalStateException("Settings missing/default semantics incorrect");
        System.out.println("MANAGED_FIXTURE_SETTINGS_OK " + id);
        int starts = prefs.getInt("starts", 0) + 1;
        if (!prefs.edit().putInt("starts", starts).commit())
            throw new IllegalStateException("preference commit failed");
        System.out.println("MANAGED_FIXTURE_START " + starts);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder holder) {
                System.out.println("MANAGED_FIXTURE_SURFACE_CREATED");
            }
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
            public void surfaceDestroyed(SurfaceHolder holder) {
                System.out.println("MANAGED_FIXTURE_SURFACE_DESTROYED");
            }
        });
        setContentView(view);
    }
    @Override public void onResume() {
        super.onResume();
        new Handler().postDelayed(() -> finish(), 1000);
    }
    @Override public void onPause() { super.onPause(); System.out.println("MANAGED_FIXTURE_PAUSED"); }
    @Override public void onStop() { super.onStop(); System.out.println("MANAGED_FIXTURE_STOPPED"); }
    @Override public void onDestroy() { super.onDestroy(); System.out.println("MANAGED_FIXTURE_DESTROYED"); }
}
