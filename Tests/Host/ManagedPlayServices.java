package org.bifrost.fixture;

import android.app.Activity;
import android.content.ComponentName;
import android.content.ContentProviderClient;
import android.content.Intent;
import android.content.ServiceConnection;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import com.google.android.gms.common.GoogleApiAvailabilityLight;
import com.google.android.gms.common.GooglePlayServicesUtilLight;

/**
 * Google's own Play services availability check (play-services-basement)
 * against microG, then a Play services binding and provider query that
 * microG answers from its own process.
 */
public final class ManagedPlayServices extends Activity {
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            try {
                System.out.println("MANAGED_PLAY_SERVICES_CONNECTED " + name.flattenToShortString()
                        + " " + binder.getInterfaceDescriptor() + " alive=" + binder.pingBinder());
            } catch (RemoteException error) {
                throw new IllegalStateException("Play services binder failed", error);
            }
            queryGServices();
            resolveChimeraService();
            unbindService(this);
            finish();
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            System.out.println("MANAGED_PLAY_SERVICES_DISCONNECTED " + name.flattenToShortString());
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        int status = GoogleApiAvailabilityLight.getInstance().isGooglePlayServicesAvailable(this);
        System.out.println("MANAGED_PLAY_SERVICES status=" + status
                + " apk=" + GooglePlayServicesUtilLight.getApkVersion(this)
                + " client=" + GooglePlayServicesUtilLight.GOOGLE_PLAY_SERVICES_VERSION_CODE);
        // microG's ad request stub is not bound, so Google Mobile Ads falls
        // back to building its requests itself.
        ServiceConnection ads = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                throw new IllegalStateException("microG ad request stub was bound");
            }
            @Override public void onServiceDisconnected(ComponentName name) {}
        };
        boolean adsBound = bindService(new Intent("com.google.android.gms.ads.service.START")
                .setPackage("com.google.android.gms"), ads, BIND_AUTO_CREATE);
        unbindService(ads);
        System.out.println("MANAGED_PLAY_SERVICES_ADS_BIND " + adsBound);
        // Firebase Analytics binds this action when Play services is available.
        Intent intent = new Intent("com.google.android.gms.measurement.START").setPackage("com.google.android.gms");
        boolean bound = bindService(intent, connection, BIND_AUTO_CREATE);
        System.out.println("MANAGED_PLAY_SERVICES_BIND " + bound);
        if (!bound) finish();
    }

    private void queryGServices() {
        Uri uri = Uri.parse("content://com.google.android.gsf.gservices");
        try (ContentProviderClient client = getContentResolver().acquireContentProviderClient(uri)) {
            if (client == null) throw new IllegalStateException("GServices provider unavailable");
            // A fresh microG database has no values: the provider answers null.
            try (Cursor cursor = client.query(uri, null, null, new String[] {"bifrost.missing.key"}, null)) {
                System.out.println("MANAGED_PLAY_SERVICES_GSERVICES rows=" + (cursor == null ? 0 : cursor.getCount())
                        + " uid=" + Process.myUid());
            }
        } catch (RemoteException error) {
            throw new IllegalStateException("GServices query failed", error);
        }
    }

    // play-services-basement's dynamic service lookup (GmsClientSupervisor)
    // asks this provider for the Intent of a Play services action.
    private void resolveChimeraService() {
        Uri uri = Uri.parse("content://com.google.android.gms.chimera");
        try (ContentProviderClient client = getContentResolver().acquireUnstableContentProviderClient(uri)) {
            if (client == null) throw new IllegalStateException("Chimera provider unavailable");
            Bundle extras = new Bundle();
            extras.putString("serviceActionBundleKey", "com.google.android.gms.measurement.START");
            Bundle result = client.call("serviceIntentCall", null, extras);
            Intent intent = result == null ? null : result.getParcelable("serviceResponseIntentKey");
            System.out.println("MANAGED_PLAY_SERVICES_CHIMERA "
                    + (intent == null || intent.getComponent() == null ? null : intent.getComponent().flattenToShortString()));
        } catch (RemoteException error) {
            throw new IllegalStateException("Chimera service lookup failed", error);
        }
    }
}
