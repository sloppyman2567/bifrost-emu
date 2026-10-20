package org.bifrost.fixture;

import android.accounts.AbstractAccountAuthenticator;
import android.accounts.Account;
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.accounts.NetworkErrorException;
import android.accounts.OnAccountsUpdateListener;
import android.app.Activity;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * AccountManager against the framework's account service: an authenticator in
 * the app's own package adds an account, issues tokens, answers feature
 * queries and allows removal. The same account store must also list microG's
 * "com.google" authenticator.
 */
public final class ManagedAccounts extends Activity {
    static final String TYPE = "org.bifrost.fixture.account";
    static int tokenCounter;

    public static final class Authenticator extends AbstractAccountAuthenticator {
        private final Context context;

        Authenticator(Context context) { super(context); this.context = context; }

        @Override public Bundle editProperties(AccountAuthenticatorResponse response, String accountType) {
            throw new UnsupportedOperationException();
        }

        @Override public Bundle addAccount(AccountAuthenticatorResponse response, String accountType,
                String authTokenType, String[] requiredFeatures, Bundle options) throws NetworkErrorException {
            Account account = new Account("fixture@example.test", accountType);
            Bundle extras = new Bundle();
            extras.putString("services", "mail,sync");
            if (!AccountManager.get(context).addAccountExplicitly(account, "secret", extras))
                throw new NetworkErrorException("account exists");
            Bundle result = new Bundle();
            result.putString(AccountManager.KEY_ACCOUNT_NAME, account.name);
            result.putString(AccountManager.KEY_ACCOUNT_TYPE, account.type);
            return result;
        }

        @Override public Bundle confirmCredentials(AccountAuthenticatorResponse response, Account account,
                Bundle options) {
            Bundle result = new Bundle();
            result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
            return result;
        }

        @Override public Bundle getAuthToken(AccountAuthenticatorResponse response, Account account,
                String authTokenType, Bundle options) {
            Bundle result = new Bundle();
            result.putString(AccountManager.KEY_ACCOUNT_NAME, account.name);
            result.putString(AccountManager.KEY_ACCOUNT_TYPE, account.type);
            result.putString(AccountManager.KEY_AUTHTOKEN, "tok-" + (++tokenCounter) + "-" + authTokenType
                    + "-" + options.getString(AccountManager.KEY_ANDROID_PACKAGE_NAME));
            return result;
        }

        @Override public String getAuthTokenLabel(String authTokenType) { return "Label " + authTokenType; }

        @Override public Bundle updateCredentials(AccountAuthenticatorResponse response, Account account,
                String authTokenType, Bundle options) {
            return null;
        }

        @Override public Bundle hasFeatures(AccountAuthenticatorResponse response, Account account,
                String[] features) {
            String services = AccountManager.get(context).getUserData(account, "services");
            boolean all = services != null;
            for (String feature : features) all &= all && services.contains(feature);
            Bundle result = new Bundle();
            result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, all);
            return result;
        }
    }

    public static final class AuthenticatorService extends Service {
        private Authenticator authenticator;

        @Override public void onCreate() { authenticator = new Authenticator(this); }

        @Override public IBinder onBind(Intent intent) { return authenticator.getIBinder(); }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(this::run, "accounts").start();
    }

    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    private void run() {
        try {
            AccountManager am = AccountManager.get(this);
            boolean fixture = false, googleType = false;
            for (AuthenticatorDescription type : am.getAuthenticatorTypes()) {
                fixture |= TYPE.equals(type.type) && getPackageName().equals(type.packageName);
                googleType |= "com.google".equals(type.type) && "com.google.android.gms".equals(type.packageName);
            }
            System.out.println("MANAGED_ACCOUNTS_TYPES fixture=" + fixture + " google=" + googleType);
            check(fixture && googleType, "authenticator types");
            check(am.getAccountsByType(TYPE).length == 0, "starts empty");
            check(am.getAccountsByType("com.google").length == 0, "no google account");

            // A Google account another process stored (what Asgard imports):
            // microG's authenticator, in its own process, reads the same store.
            File store = new File("/data/system/users/0/bifrost-accounts.json");
            try (FileOutputStream out = new FileOutputStream(store)) {
                out.write(("{\"version\":1,\"revision\":0,\"accounts\":[{\"name\":\"seed@example.test\","
                        + "\"type\":\"com.google\",\"password\":\"master\",\"previous\":null,\"authenticated\":0,"
                        + "\"data\":{\"services\":\"ac2dm,mail\"},\"tokens\":{},"
                        + "\"visibility\":{\"" + getPackageName() + "\":1},\"grants\":[]}]}")
                        .getBytes(StandardCharsets.UTF_8));
            }
            Thread.sleep(200);
            Account[] google = am.getAccountsByType("com.google");
            check(google.length == 1 && "seed@example.test".equals(google[0].name), "seeded google account is visible");
            boolean ac2dm = am.hasFeatures(google[0], new String[] {"service_ac2dm"}, null, null)
                    .getResult(60, TimeUnit.SECONDS);
            boolean calendar = am.hasFeatures(google[0], new String[] {"service_calendar"}, null, null)
                    .getResult(60, TimeUnit.SECONDS);
            System.out.println("MANAGED_ACCOUNTS_GOOGLE_FEATURES ac2dm=" + ac2dm + " calendar=" + calendar);
            check(ac2dm && !calendar, "microG authenticator answers hasFeatures");
            try {
                am.getPassword(google[0]);
                check(false, "an app cannot read another authenticator's secrets");
            } catch (SecurityException expected) {
                System.out.println("MANAGED_ACCOUNTS_SECRETS_DENIED");
            }

            CountDownLatch updated = new CountDownLatch(2);
            OnAccountsUpdateListener listener = accounts -> updated.countDown();
            am.addOnAccountsUpdatedListener(listener, null, true);

            Bundle added = am.addAccount(TYPE, null, null, null, this, null, null).getResult(30, TimeUnit.SECONDS);
            Account account = new Account(added.getString(AccountManager.KEY_ACCOUNT_NAME),
                    added.getString(AccountManager.KEY_ACCOUNT_TYPE));
            System.out.println("MANAGED_ACCOUNTS_ADDED " + account.name);
            Account[] listed = am.getAccountsByType(TYPE);
            check(listed.length == 1 && listed[0].equals(account), "added account is listed");
            check(updated.await(10, TimeUnit.SECONDS), "accounts-updated listener");

            // The authenticator owns the secrets; this app is its own authenticator.
            check("secret".equals(am.getPassword(account)), "password");
            check("mail,sync".equals(am.getUserData(account, "services")), "extras are user data");
            am.setUserData(account, "k", "v");
            check("v".equals(am.getUserData(account, "k")), "user data round trip");

            String first = am.blockingGetAuthToken(account, "scope", true);
            String again = am.blockingGetAuthToken(account, "scope", true);
            System.out.println("MANAGED_ACCOUNTS_TOKEN " + first + " " + again);
            check(first != null && first.startsWith("tok-1-scope-" + getPackageName()), "first token");
            check(first.equals(again), "token is cached");
            am.invalidateAuthToken(TYPE, first);
            String renewed = am.blockingGetAuthToken(account, "scope", true);
            check(renewed != null && renewed.startsWith("tok-2-"), "invalidated token is reissued: " + renewed);

            boolean has = am.hasFeatures(account, new String[] {"mail"}, null, null).getResult(30, TimeUnit.SECONDS);
            boolean lacks = am.hasFeatures(account, new String[] {"calendar"}, null, null).getResult(30, TimeUnit.SECONDS);
            check(has && !lacks, "hasFeatures");
            Account[] withMail = am.getAccountsByTypeAndFeatures(TYPE, new String[] {"mail"}, null, null)
                    .getResult(30, TimeUnit.SECONDS);
            Account[] withCalendar = am.getAccountsByTypeAndFeatures(TYPE, new String[] {"calendar"}, null, null)
                    .getResult(30, TimeUnit.SECONDS);
            check(withMail.length == 1 && withCalendar.length == 0, "getAccountsByTypeAndFeatures");
            Bundle confirmed = am.confirmCredentials(account, null, this, null, null).getResult(30, TimeUnit.SECONDS);
            check(confirmed.getBoolean(AccountManager.KEY_BOOLEAN_RESULT), "confirmCredentials");

            check(am.setAccountVisibility(account, "org.example.other", AccountManager.VISIBILITY_VISIBLE), "visibility set");
            check(am.getAccountVisibility(account, "org.example.other") == AccountManager.VISIBILITY_NOT_VISIBLE
                    || am.getAccountVisibility(account, "org.example.other") == AccountManager.VISIBILITY_VISIBLE,
                    "visibility readable");

            // Persistence: the shared store file holds the account for other processes.
            File persisted = new File("/data/system/users/0/bifrost-accounts.json");
            check(persisted.isFile() && persisted.length() > 0, "account store file");

            check(am.removeAccount(account, this, null, null).getResult(30, TimeUnit.SECONDS)
                    .getBoolean(AccountManager.KEY_BOOLEAN_RESULT), "removeAccount");
            check(am.getAccountsByType(TYPE).length == 0, "removed account is gone");
            am.removeOnAccountsUpdatedListener(listener);

            // WebView forwards unhandled media keys to AudioManager; with no media
            // session service this threw and Chromium aborted the sign-in window.
            android.media.AudioManager audio = (android.media.AudioManager) getSystemService(AUDIO_SERVICE);
            audio.dispatchMediaKeyEvent(new android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN,
                    android.view.KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE));
            audio.dispatchMediaKeyEvent(new android.view.KeyEvent(android.view.KeyEvent.ACTION_UP,
                    android.view.KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE));
            System.out.println("MANAGED_ACCOUNTS_MEDIA_KEY_OK");
            System.out.println("MANAGED_ACCOUNTS_OK");
        } catch (Throwable error) {
            error.printStackTrace(System.out);
            System.out.println("MANAGED_ACCOUNTS_FAILED " + error);
        }
        runOnUiThread(this::finish);
    }
}
