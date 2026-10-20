package org.bifrost.fixture;

import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.app.Activity;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.Parcel;

/**
 * Activities of microG's Play services started from an app: they open in a
 * window of the Play services system app and return their result here.
 * AuthSignInActivity without its configuration answers RESULT_OK with
 * DEVELOPER_ERROR at once, so no Google account or network is needed.
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
            startActivityForResult(new Intent("com.google.android.gms.auth.GOOGLE_SIGN_IN"), 8);
        } else if (request == 8) {
            askPermission();
        }
    }

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
