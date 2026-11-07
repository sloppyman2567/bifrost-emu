package org.bifrost.fixture;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.widget.TextView;

/**
 * An Activity that turns the window by itself, once, and reports its lifecycle. Without configChanges Android relaunches
 * it: onSaveInstanceState and the retained instance go to a new instance of it, which gets the saved state, the retained
 * object and the Intent setIntent() replaced. With configChanges for the change it only gets onConfigurationChanged.
 */
public final class ManagedRelaunch extends Activity {
    private static int created;  // instances made in this process
    private final int generation = created++;
    private final Handler main = new Handler();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        // Only the first instance replaces the Intent; a relaunched one must find that replacement, not the manifest's.
        if (getIntent().getStringExtra("mark") == null)
            setIntent(new Intent(getIntent()).putExtra("mark", "kept-by-" + generation));
        TextView text = new TextView(this);
        text.setText("Relaunch " + generation);
        setContentView(text);
        Object retained = getLastNonConfigurationInstance();
        System.out.println("RELAUNCH_CREATE gen=" + generation + " saved=" + (state == null ? "none" : state.getString("note"))
                + " retained=" + retained + " orientation=" + getResources().getConfiguration().orientation);
    }

    @Override protected void onStart() {
        super.onStart();
        System.out.println("RELAUNCH_START gen=" + generation);
    }

    @Override protected void onRestoreInstanceState(Bundle state) {
        super.onRestoreInstanceState(state);
        System.out.println("RELAUNCH_RESTORE gen=" + generation + " saved=" + state.getString("note"));
    }

    @Override protected void onResume() {
        super.onResume();
        System.out.println("RELAUNCH_RESUME gen=" + generation + " orientation=" + getResources().getConfiguration().orientation
                + " intent=" + getIntent().getStringExtra("mark"));
        // The first instance turns the window the other way; the one after a relaunch ends the run.
        main.postDelayed(() -> {
            if (generation == 0 && !turned) {
                turned = true;
                boolean landscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
                System.out.println("RELAUNCH_TURN to " + (landscape ? "portrait" : "landscape"));
                setRequestedOrientation(landscape ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                                                  : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            } else {
                finish();
            }
        }, 1500);
    }
    private static boolean turned;

    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        System.out.println("RELAUNCH_CONFIG_CHANGED gen=" + generation + " orientation=" + config.orientation);
        main.postDelayed(this::finish, 800);  // handled in place: nothing else would end the run
    }

    @Override protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putString("note", "saved-by-" + generation);
        System.out.println("RELAUNCH_SAVE gen=" + generation);
    }

    @Override public Object onRetainNonConfigurationInstance() {
        return "retained-by-" + generation;
    }

    @Override protected void onPause() {
        super.onPause();
        System.out.println("RELAUNCH_PAUSE gen=" + generation);
    }

    @Override protected void onStop() {
        super.onStop();
        System.out.println("RELAUNCH_STOP gen=" + generation + " changing=" + isChangingConfigurations());
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        main.removeCallbacksAndMessages(null);
        System.out.println("RELAUNCH_DESTROY gen=" + generation + " changing=" + isChangingConfigurations());
    }
}
