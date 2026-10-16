package org.bifrost.fixture;

import android.app.Activity;
import android.os.Bundle;
import com.google.android.gms.common.GoogleApiAvailabilityLight;
import com.google.android.gms.common.GooglePlayServicesUtilLight;

/** Google's own Play services availability check (play-services-basement) against microG. */
public final class ManagedPlayServices extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        int status = GoogleApiAvailabilityLight.getInstance().isGooglePlayServicesAvailable(this);
        System.out.println("MANAGED_PLAY_SERVICES status=" + status
                + " apk=" + GooglePlayServicesUtilLight.getApkVersion(this)
                + " client=" + GooglePlayServicesUtilLight.GOOGLE_PLAY_SERVICES_VERSION_CODE);
        finish();
    }
}
