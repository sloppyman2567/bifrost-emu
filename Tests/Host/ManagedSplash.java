package org.bifrost.fixture;

import android.app.Activity;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;

/**
 * A splash screen: the launcher Activity starts the main one and finishes at
 * once. The app keeps running in the started Activity, and destroying the
 * splash skips final cleanup of its wrapped (non-ContextImpl) base context.
 */
public final class ManagedSplash extends Activity {
    @Override protected void attachBaseContext(Context base) {
        super.attachBaseContext(new ContextWrapper(base));
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        startActivity(new Intent(this, Main.class));
        finish();
        System.out.println("MANAGED_SPLASH_FINISHING");
    }

    @Override public void onDestroy() {
        super.onDestroy();
        System.out.println("MANAGED_SPLASH_DESTROYED");
    }

    public static final class Main extends Activity {
        @Override public void onResume() {
            super.onResume();
            System.out.println("MANAGED_SPLASH_MAIN_RESUMED");
            new Handler().postDelayed(this::finish, 1000);
        }

        @Override public void onDestroy() {
            super.onDestroy();
            System.out.println("MANAGED_SPLASH_MAIN_DESTROYED");
        }
    }
}
