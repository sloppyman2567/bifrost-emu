package org.bifrost.fixture;

import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.app.Activity;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;

/**
 * Activities of microG's Play services started from an app: they open in a
 * window of the Play services system app and return their result here.
 * AuthSignInActivity without its configuration answers RESULT_OK with
 * DEVELOPER_ERROR at once, so no Google account or network is needed.
 *
 * Senders created in another process of the app (Play services' sign-in UI
 * arrives in a game as one) start through the same window when they name a
 * Play services Activity, and are refused otherwise.
 */
public final class ManagedGoogleActivity extends Activity {
    private static final String GMS = "com.google.android.gms";
    private static final String SIGN_IN = "org.microg.gms.auth.signin.AuthSignInActivity";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        startActivityForResult(new Intent().setClassName(GMS, SIGN_IN), 7);
        System.out.println("MANAGED_GOOGLE_STARTED");
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        // 10 is CommonStatusCodes.DEVELOPER_ERROR, the Activity's answer to a request without configuration.
        System.out.println("MANAGED_GOOGLE_RESULT " + request + " " + result + " error="
                + (data == null ? "null" : data.getIntExtra("errorCode", -1)));
        if (request == 7) {
            senders();
        } else if (request == 10) {
            refusedSender();
        } else if (request == 8) {
            askPermission();
        }
    }

    // PendingIntents made in the app's ":sender" process: one for a Play
    // services Activity, one for the app's own.
    private void senders() {
        bindService(new Intent(this, Sender.class), new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder service) {
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    service.transact(IBinder.FIRST_CALL_TRANSACTION, data, reply, 0);
                    reply.readException();
                    gmsSender = PendingIntent.readPendingIntentOrNullFromParcel(reply);
                    ownSender = PendingIntent.readPendingIntentOrNullFromParcel(reply);
                    System.out.println("MANAGED_GOOGLE_SENDER_PROCESS " + (gmsSender != null && ownSender != null));
                    startIntentSenderForResult(gmsSender.getIntentSender(), 10, null, 0, 0, 0);
                } catch (Exception failure) {
                    System.out.println("MANAGED_GOOGLE_SENDER_FAILED " + failure);
                    startActivityForResult(new Intent("com.google.android.gms.auth.GOOGLE_SIGN_IN"), 8);
                } finally {
                    data.recycle(); reply.recycle();
                }
            }
            @Override public void onServiceDisconnected(ComponentName name) {}
        }, Context.BIND_AUTO_CREATE);
    }

    private PendingIntent gmsSender, ownSender;

    // Another process's Activity that is not Play services': the start is
    // refused, as before, and the app carries on.
    private void refusedSender() {
        try {
            startIntentSenderForResult(ownSender.getIntentSender(), 11, null, 0, 0, 0);
            System.out.println("MANAGED_GOOGLE_SENDER_STARTED");
        } catch (IntentSender.SendIntentException refused) {
            System.out.println("MANAGED_GOOGLE_SENDER_REFUSED");
        }
        startActivityForResult(new Intent("com.google.android.gms.auth.GOOGLE_SIGN_IN"), 8);
    }

    /** Runs in ":sender": creates the PendingIntents another process receives. */
    public static final class Sender extends Service {
        @Override public IBinder onBind(Intent intent) {
            return new Binder() {
                @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    if (code != FIRST_CALL_TRANSACTION) return false;
                    reply.writeNoException();
                    Intent gms = new Intent().setClassName(GMS, SIGN_IN);
                    Intent own = new Intent(Sender.this, Other.class);
                    int immutable = PendingIntent.FLAG_IMMUTABLE;
                    PendingIntent.writePendingIntentOrNullToParcel(
                            PendingIntent.getActivity(Sender.this, 0, gms, immutable), reply);
                    PendingIntent.writePendingIntentOrNullToParcel(
                            PendingIntent.getActivity(Sender.this, 1, own, immutable), reply);
                    return true;
                }
            };
        }
    }

    /** Not Play services: the target of the refused sender. */
    public static final class Other extends Activity {}

    // An Activity handed the caller's AccountAuthenticatorResponse that ends
    // without answering it: the caller's response is cancelled.
    private void askPermission() {
        Binder response = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                if (code == INTERFACE_TRANSACTION) { reply.writeString("android.accounts.IAccountAuthenticatorResponse"); return true; }
                data.enforceInterface("android.accounts.IAccountAuthenticatorResponse");
                if (code == FIRST_CALL_TRANSACTION + 2) {
                    int error = data.readInt();
                    System.out.println("MANAGED_GOOGLE_RESPONSE_ERROR " + error + " " + data.readString());
                    new Handler(getMainLooper()).post(ManagedGoogleActivity.this::finish);
                    return true;
                }
                return code == FIRST_CALL_TRANSACTION || code == FIRST_CALL_TRANSACTION + 1;
            }
        };
        Parcel parcel = Parcel.obtain();
        parcel.writeStrongBinder(response);
        parcel.setDataPosition(0);
        AccountAuthenticatorResponse wrapper = AccountAuthenticatorResponse.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        Intent intent = new Intent().setClassName(GMS, "org.microg.gms.auth.AskPermissionActivity");
        intent.putExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE, wrapper);
        startActivityForResult(intent, 9);
    }

    @Override public void onDestroy() {
        super.onDestroy();
        System.out.println("MANAGED_GOOGLE_DONE");
    }
}
