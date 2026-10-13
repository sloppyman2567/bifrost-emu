// AccountManagerService ("account") for a device with no accounts and no
// account authenticators (no Google Play services). AccountManager is built
// from ServiceManager.getServiceOrThrow("account"), so without it
// getSystemService(ACCOUNT_SERVICE) is null. Answers follow Android 15's
// AccountManagerService for account types no authenticator provides.
import android.accounts.Account;
import android.accounts.AuthenticatorDescription;
import android.os.Bundle;
import android.os.Process;

final class FrameworkAccounts implements FrameworkServices.Call {
    private static final int ERROR_CODE_REMOTE_EXCEPTION = 1;  // AccountManager
    private static final int USER_CURRENT_OR_SELF = -3, USER_CURRENT = -2;

    private enum Contract {
        getAccountsAsUser,
        getAuthenticatorTypes,
        getAccountsByFeatures,
        getAccountByTypeAndFeatures,
        hasFeatures,
        getPassword,
        getUserData,
        addAccountExplicitly,
        registerAccountListener,
        unregisterAccountListener
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkAccounts(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("account", "android.accounts.IAccountManager", this);
    }

    // AppOpsManager.checkPackage(callingUid, opPackageName).
    private void ownPackage(Object pkg) {
        if (pkg == null || packages.packageUid((String) pkg) != Process.myUid())
            throw new SecurityException("Package " + pkg + " does not belong to " + Process.myUid());
    }

    private void user(int userId) {
        if (userId != 0 && userId != USER_CURRENT && userId != USER_CURRENT_OR_SELF
                && !packages.granted("android.permission.INTERACT_ACROSS_USERS_FULL"))
            throw new SecurityException("User 0 trying to access user " + userId);
    }

    private static void respond(Object response, String method, Object... args) throws Exception {
        for (java.lang.reflect.Method candidate : response.getClass().getMethods())
            if (candidate.getName().equals(method) && candidate.getParameterCount() == args.length) {
                candidate.invoke(response, args);
                return;
            }
        throw new IllegalStateException("IAccountManagerResponse." + method + " missing");
    }

    // Secrets and explicit additions need the authenticator of the account's
    // type to share the caller's uid; no authenticator exists here.
    private static SecurityException notAuthenticator(Account account) {
        return new SecurityException("uid " + Process.myUid() + " cannot explicitly add accounts of type: "
                + account.type);
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("account." + method); }
        switch (contract) {
            case getAccountsAsUser: user((Integer) a[1]); ownPackage(a[2]); return new Account[0];
            case getAuthenticatorTypes: user((Integer) a[0]); return new AuthenticatorDescription[0];
            case getAccountsByFeatures: {
                ownPackage(a[3]);
                if (a[0] == null) throw new IllegalArgumentException("response is null");
                if (a[1] == null) throw new IllegalArgumentException("accountType is null");
                // A type without an authenticator is not visible to the caller.
                Bundle result = new Bundle();
                result.putParcelableArray("accounts", new Account[0]);
                respond(a[0], "onResult", result);
                return null;
            }
            case getAccountByTypeAndFeatures: {
                ownPackage(a[3]);
                if (a[0] == null) throw new IllegalArgumentException("response is null");
                if (a[1] == null) throw new IllegalArgumentException("accountType is null");
                String[] features = (String[]) a[2];
                // handleGetAccountsResult: no qualified account, an empty Bundle.
                // With features, AOSP's session fails to bind the missing
                // authenticator and swallows the error, so no reply is sent.
                if (features == null || features.length == 0) respond(a[0], "onResult", new Bundle());
                return null;
            }
            case hasFeatures: {
                ownPackage(a[4]);
                if (a[0] == null) throw new IllegalArgumentException("response is null");
                if (a[1] == null) throw new IllegalArgumentException("account is null");
                if (a[2] == null) throw new IllegalArgumentException("features is null");
                user((Integer) a[3]);
                respond(a[0], "onError", ERROR_CODE_REMOTE_EXCEPTION, "bind failure");
                return null;
            }
            case getPassword:
            case getUserData: {
                if (a[0] == null) throw new IllegalArgumentException("account is null");
                Account account = (Account) a[0];
                throw new SecurityException("uid " + Process.myUid() + " cannot get secrets for accounts of type: " + account.type);
            }
            case addAccountExplicitly: {
                if (a[0] == null) throw new IllegalArgumentException("account is null");
                throw notAuthenticator((Account) a[0]);
            }
            // Account sets never change, so listeners are never notified.
            case registerAccountListener:
            case unregisterAccountListener: ownPackage(a[1]); return null;
            default: throw new UnsupportedOperationException("account." + method);
        }
    }
}
