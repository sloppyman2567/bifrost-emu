// AccountManagerService ("account") for one user. Follows Android 15's
// com.android.server.accounts.AccountManagerService: authenticators are the
// services declaring android.accounts.AccountAuthenticator, accounts are
// visible to callers by signature, permission and stored visibility, and
// operations that need an authenticator bind it and run a Session.
//
// Every guest process (the session, microG's service processes) hosts its
// own copy of this proxy, so accounts, tokens and visibility live in
// FrameworkAccountStore's shared file. An authenticator is bound by whichever
// process receives the call; its Intents (KEY_INTENT) start Activities through
// FrameworkActivities. Not modelled: other users, shared and restricted
// accounts, device-policy limits, two-step sessions, credential-grant UI,
// sign-in notifications and account-access requests.
import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.res.XmlResourceParser;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.SystemClock;
import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

final class FrameworkAccounts implements FrameworkServices.Call {
    private static final int USER_CURRENT_OR_SELF = -3, USER_CURRENT = -2;
    private static final long TIMEOUT_DELAY_MS = 15 * 60 * 1000;
    // Hidden AccountManager constants (Android 15).
    private static final String ACTION_VISIBLE_ACCOUNTS_CHANGED = "android.accounts.action.VISIBLE_ACCOUNTS_CHANGED",
            KEY_ACCOUNT_ACCESS_ID = "accountAccessId", KEY_NOTIFY_ON_FAILURE = "notifyOnAuthFailure";
    private static final String AUTHENTICATOR_META = "android.accounts.AccountAuthenticator";
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    private static final String RESPONSE_DESCRIPTOR = "android.accounts.IAccountAuthenticatorResponse";
    private static final int SIGNATURE_UID = 2, SIGNATURE_MATCH = 1, SIGNATURE_MISMATCH = 0;
    private static final String GET_ACCOUNTS = "android.permission.GET_ACCOUNTS",
            GET_ACCOUNTS_PRIVILEGED = "android.permission.GET_ACCOUNTS_PRIVILEGED",
            READ_CONTACTS = "android.permission.READ_CONTACTS";
    // Interface methods this service answers; the rest throw (inventory).
    private enum Contract {
        getPassword, getUserData, getAuthenticatorTypes, getAccountsForPackage, getAccountsByTypeForPackage,
        getAccountsAsUser, hasFeatures, getAccountByTypeAndFeatures, getAccountsByFeatures,
        addAccountExplicitly, removeAccountAsUser, removeAccountExplicitly, invalidateAuthToken,
        peekAuthToken, setAuthToken, setPassword, clearPassword, setUserData, updateAppPermission,
        getAuthToken, addAccount, addAccountAsUser, updateCredentials, editProperties,
        confirmCredentialsAsUser, accountAuthenticated, getAuthTokenLabel, renameAccount, getPreviousName,
        someUserHasAccount, isCredentialsUpdateSuggested, getPackagesAndVisibilityForAccount,
        addAccountExplicitlyWithVisibility, setAccountVisibility, getAccountVisibility,
        getAccountsAndVisibilityForPackage, registerAccountListener, unregisterAccountListener,
        hasAccountAccess, onAccountAccessed
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private static final class Authenticator {
        final String type, packageName;
        final ComponentName service;
        final int uid, labelId, iconId, smallIconId, preferencesId;
        final boolean customTokens;
        Authenticator(String type, ServiceInfo info, int labelId, int iconId, int smallIconId, int preferencesId,
                boolean customTokens) {
            this.type = type; packageName = info.packageName; uid = info.applicationInfo.uid;
            service = new ComponentName(info.packageName, info.name);
            this.labelId = labelId; this.iconId = iconId; this.smallIconId = smallIconId;
            this.preferencesId = preferencesId; this.customTokens = customTokens;
        }
        AuthenticatorDescription description() {
            return new AuthenticatorDescription(type, packageName, labelId, iconId, smallIconId, preferencesId,
                    customTokens);
        }
    }

    private final FrameworkPackages packages;
    private final FrameworkAccountStore store = new FrameworkAccountStore(new File(FrameworkAccountStore.FILE));
    private final Set<String> listeners = new HashSet<>();
    private final Map<String, Session> sessions = new HashMap<>();
    private Context context;
    private Handler handler;
    private java.util.function.Consumer<Intent> broadcaster;
    private List<Authenticator> authenticators;
    private long seenRevision = -1;
    private boolean watching;

    FrameworkAccounts(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("account", "android.accounts.IAccountManager", this);
    }

    // The process's Application: authenticators are bound and their metadata
    // read through it. `broadcast` delivers to this process's receivers, as
    // the system sends the protected account broadcasts.
    synchronized void attach(Context application, java.util.function.Consumer<Intent> broadcast) {
        context = application;
        broadcaster = broadcast;
        if (handler == null) {
            HandlerThread thread = new HandlerThread("AccountSessions");
            thread.setDaemon(true);
            thread.start();
            handler = new Handler(thread.getLooper());
        }
    }

    // ---- caller identity ----

    // These services run inside each guest process rather than behind Binder: the
    // caller is the process itself, even while it serves an incoming transaction
    // (an authenticator reading its own account data on a Binder thread).
    private int callingUid() { return Process.myUid(); }

    // AppOpsManager.checkPackage(callingUid, opPackageName).
    private void ownPackage(Object pkg) {
        if (pkg == null || packages.packageUid((String) pkg) != callingUid())
            throw new SecurityException("Package " + pkg + " does not belong to " + callingUid());
    }

    private void user(int userId) {
        if (userId != 0 && userId != USER_CURRENT && userId != USER_CURRENT_OR_SELF
                && !packages.granted("android.permission.INTERACT_ACROSS_USERS_FULL"))
            throw new SecurityException("User 0 trying to access user " + userId);
    }

    // The system process (and no other package: Settings-class UI is not provisioned).
    private static boolean isSystemUid(int uid) { return uid % 100000 == Process.SYSTEM_UID; }

    private boolean isPrivileged(int uid) {
        if (isSystemUid(uid)) return true;
        String name = packages.nameForUid(uid);
        return name != null && packages.isSystemPackage(name);
    }

    // ---- authenticators ----

    private synchronized List<Authenticator> authenticators() throws Exception {
        if (authenticators != null) return authenticators;
        List<Authenticator> found = new ArrayList<>();
        if (context == null) return found;
        PackageManager pm = context.getPackageManager();
        for (ResolveInfo match : packages.queryServices(new Intent(AccountManager.ACTION_AUTHENTICATOR_INTENT), null,
                PackageManager.GET_META_DATA, 0)) {
            ServiceInfo info = match.serviceInfo;
            try (XmlResourceParser parser = info.loadXmlMetaData(pm, AUTHENTICATOR_META)) {
                if (parser == null) continue;
                int event;
                while ((event = parser.next()) != XmlResourceParser.START_TAG && event != XmlResourceParser.END_DOCUMENT) { }
                if (event != XmlResourceParser.START_TAG || !"account-authenticator".equals(parser.getName())) continue;
                String type = parser.getAttributeValue(ANDROID_NS, "accountType");
                if (type == null) continue;
                found.add(new Authenticator(type, info,
                        parser.getAttributeResourceValue(ANDROID_NS, "label", 0),
                        parser.getAttributeResourceValue(ANDROID_NS, "icon", 0),
                        parser.getAttributeResourceValue(ANDROID_NS, "smallIcon", 0),
                        parser.getAttributeResourceValue(ANDROID_NS, "accountPreferences", 0),
                        parser.getAttributeBooleanValue(ANDROID_NS, "customTokens", false)));
            }
        }
        authenticators = found;
        return found;
    }

    private Authenticator authenticator(String type) throws Exception {
        for (Authenticator each : authenticators()) if (each.type.equals(type)) return each;
        return null;
    }

    private int signatureCheck(String accountType, int uid) throws Exception {
        if (accountType == null) return SIGNATURE_MISMATCH;
        Authenticator owner = authenticator(accountType);
        if (owner == null) return SIGNATURE_MISMATCH;
        if (owner.uid == uid) return SIGNATURE_UID;
        String name = packages.nameForUid(uid);
        return name != null && packages.sameSigner(owner.packageName, name) ? SIGNATURE_MATCH : SIGNATURE_MISMATCH;
    }

    // Applications with the authenticator's signature (AccountManagerService.getTypesManagedByCaller).
    private boolean managedByCaller(String accountType, int uid) throws Exception {
        return signatureCheck(accountType, uid) != SIGNATURE_MISMATCH;
    }

    private void requireManaged(String accountType, int uid, String what) throws Exception {
        if (!managedByCaller(accountType, uid))
            throw new SecurityException("uid " + uid + " cannot " + what + " of type: " + accountType);
    }

    // ---- visibility ----

    private int visibilityOf(FrameworkAccountStore.Entry entry, String packageName) {
        Integer value = entry.visibility.get(packageName);
        return value == null ? AccountManager.VISIBILITY_UNDEFINED : value;
    }

    private boolean permitted(String packageName, String... permissions) {
        int uid = packages.packageUid(packageName);
        if (uid < 0) return false;
        for (String permission : permissions)
            if (packages.checkPermission(permission, uid, Context.DEVICE_ID_DEFAULT) == PackageManager.PERMISSION_GRANTED)
                return true;
        return false;
    }

    private boolean accountTypeManagesContacts(String type) throws Exception {
        Authenticator owner = authenticator(type);
        return owner != null && permitted(owner.packageName, READ_CONTACTS);
    }

    // Applications targeting below Android 8 see accounts by GET_ACCOUNTS.
    private boolean preO(String packageName) {
        android.content.pm.PackageInfo pkg = packages.processPackage(packageName);
        return pkg == null || pkg.applicationInfo.targetSdkVersion < android.os.Build.VERSION_CODES.O;
    }

    // AccountManagerService.resolveAccountVisibility: never VISIBILITY_UNDEFINED.
    private int resolveVisibility(FrameworkAccountStore.Entry entry, String packageName) throws Exception {
        int uid = packages.packageUid(packageName);
        if (uid < 0) return AccountManager.VISIBILITY_NOT_VISIBLE;
        if (isSystemUid(uid)) return AccountManager.VISIBILITY_VISIBLE;
        int signature = signatureCheck(entry.type, uid);
        if (signature == SIGNATURE_UID) return AccountManager.VISIBILITY_VISIBLE;
        int visibility = visibilityOf(entry, packageName);
        if (visibility != AccountManager.VISIBILITY_UNDEFINED) return visibility;
        boolean privileged = permitted(packageName, GET_ACCOUNTS_PRIVILEGED);
        if (signature != SIGNATURE_MISMATCH
                || (preO(packageName) && permitted(packageName, GET_ACCOUNTS, GET_ACCOUNTS_PRIVILEGED))
                || (permitted(packageName, READ_CONTACTS) && accountTypeManagesContacts(entry.type))
                || privileged) {
            visibility = visibilityOf(entry, AccountManager.PACKAGE_NAME_KEY_LEGACY_VISIBLE);
            return visibility == AccountManager.VISIBILITY_UNDEFINED
                    ? AccountManager.VISIBILITY_USER_MANAGED_VISIBLE : visibility;
        }
        visibility = visibilityOf(entry, AccountManager.PACKAGE_NAME_KEY_LEGACY_NOT_VISIBLE);
        return visibility == AccountManager.VISIBILITY_UNDEFINED
                ? AccountManager.VISIBILITY_USER_MANAGED_NOT_VISIBLE : visibility;
    }

    private static boolean visible(int visibility) {
        return visibility == AccountManager.VISIBILITY_VISIBLE
                || visibility == AccountManager.VISIBILITY_USER_MANAGED_VISIBLE;
    }

    // getAccountsFromCache + filterAccounts: system sees all; others see what
    // resolves visible to their package (and, when asked, user-managed-hidden).
    private List<Account> visibleAccounts(FrameworkAccountStore.State state, String type, int uid, String packageName,
            boolean includeUserManagedNotVisible) throws Exception {
        List<Account> result = new ArrayList<>();
        for (FrameworkAccountStore.Entry entry : state.ofType(type)) {
            if (isSystemUid(uid)) { result.add(entry.account()); continue; }
            int visibility = resolveVisibility(entry, packageName);
            if (visible(visibility) || (includeUserManagedNotVisible
                    && visibility == AccountManager.VISIBILITY_USER_MANAGED_NOT_VISIBLE))
                result.add(entry.account());
        }
        return result;
    }

    // ---- notifications ----

    private void accountsChanged() {
        sendLocal(new Intent(AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION));
        if (!listeners.isEmpty()) sendLocal(new Intent(ACTION_VISIBLE_ACCOUNTS_CHANGED));
        try {
            AccountManager.class.getMethod("invalidateLocalAccountsDataCaches").invoke(null);
            AccountManager.class.getMethod("invalidateLocalAccountUserDataCaches").invoke(null);
        } catch (ReflectiveOperationException | RuntimeException ignored) { }
        synchronized (this) { seenRevision = store.revision(); }
    }

    private void sendLocal(Intent intent) {
        java.util.function.Consumer<Intent> send;
        synchronized (this) { send = broadcaster; }
        if (send != null) send.accept(intent);
    }

    // A change another process published (microG's sign-in Activity) reaches this
    // process's listeners; the system broadcasts to every process.
    private synchronized void watch() {
        if (watching || handler == null) return;
        watching = true;
        seenRevision = store.revision();
        handler.postDelayed(new Runnable() {
            @Override public void run() {
                long revision = store.revision();
                boolean changed;
                synchronized (FrameworkAccounts.this) { changed = revision != seenRevision; }
                if (changed) accountsChanged();
                handler.postDelayed(this, 1500);
            }
        }, 1500);
    }

    // ---- sessions ----

    private static final int TIMED_OUT = 1;

    private static Bundle copy(Bundle value) {
        if (value == null) return null;
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeBundle(value);
            parcel.setDataPosition(0);
            return parcel.readBundle(FrameworkAccounts.class.getClassLoader());
        } finally { parcel.recycle(); }
    }

    private static Object reflect(Object target, String method, Object... args) throws Exception {
        for (Method candidate : target.getClass().getMethods())
            if (candidate.getName().equals(method) && candidate.getParameterCount() == args.length) {
                try { return candidate.invoke(target, args); }
                catch (InvocationTargetException failure) {
                    Throwable cause = failure.getCause();
                    if (cause instanceof Exception) throw (Exception) cause;
                    throw failure;
                }
            }
        throw new IllegalStateException(target.getClass().getName() + "." + method + " missing");
    }

    // IAccountManagerResponse delivery, as a oneway call.
    private void deliver(Object response, Bundle result, int errorCode, String message) {
        if (response == null) return;
        Runnable task = () -> {
            try {
                if (result != null) reflect(response, "onResult", result);
                else reflect(response, "onError", errorCode, message);
            } catch (Exception failure) {
                android.util.Log.w("AccountManager", "failure while notifying response", failure);
            }
        };
        if (handler != null) handler.post(task); else task.run();
    }

    private void onResult(Object response, Bundle result) { deliver(response, result, 0, null); }
    private void onError(Object response, int code, String message) { deliver(response, null, code, message); }

    // Android 15's AccountManagerService.Session: an IAccountAuthenticatorResponse
    // bound to one authenticator for one operation.
    private abstract class Session extends Binder implements ServiceConnection {
        Object response;  // IAccountManagerResponse
        final String accountType, accountName;
        final boolean expectActivityLaunch, stripAuthTokenFromResult, updateLastAuthenticatedTime;
        final String key = Long.toHexString(System.nanoTime()) + "-" + System.identityHashCode(this);
        Object authenticator;
        Authenticator owner;
        boolean closed;

        Session(Object response, String accountType, boolean expectActivityLaunch, boolean stripAuthTokenFromResult,
                String accountName, boolean updateLastAuthenticatedTime) {
            if (accountType == null) throw new IllegalArgumentException("accountType is null");
            this.response = response;
            this.accountType = accountType;
            this.expectActivityLaunch = expectActivityLaunch;
            this.stripAuthTokenFromResult = stripAuthTokenFromResult;
            this.accountName = accountName;
            this.updateLastAuthenticatedTime = updateLastAuthenticatedTime;
            synchronized (FrameworkAccounts.this) { sessions.put(key, this); }
            handler.postAtTime(this::timedOut, key, SystemClock.uptimeMillis() + TIMEOUT_DELAY_MS);
        }

        // Which IAccountAuthenticator method this session calls.
        abstract void run(Object authenticator, Object responseProxy) throws Exception;

        Object responseAndClose() {
            Object result;
            synchronized (this) { result = response; }
            close();
            return result;
        }

        void close() {
            synchronized (FrameworkAccounts.this) {
                if (sessions.remove(key) == null) return;
            }
            synchronized (this) { response = null; }
            handler.removeCallbacksAndMessages(key);
            boolean unbind;
            synchronized (this) { unbind = authenticator != null; authenticator = null; }
            if (unbind) {
                try { context.unbindService(this); } catch (RuntimeException ignored) { }
            }
        }

        void timedOut() {
            Object response = responseAndClose();
            if (response != null) FrameworkAccounts.this.onError(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "timeout");
        }

        void bind() {
            try {
                Authenticator owner = authenticator(accountType);
                if (owner == null) {
                    android.util.Log.w("AccountManager", "there is no authenticator for " + accountType + ", bailing out");
                    fail("bind failure");
                    return;
                }
                this.owner = owner;
                Intent intent = new Intent(AccountManager.ACTION_AUTHENTICATOR_INTENT).setComponent(owner.service);
                // The system binds authenticators with its own identity.
                FrameworkServiceRuntime.SYSTEM_BIND.set(true);
                boolean bound;
                try { bound = context.bindService(intent, Context.BIND_AUTO_CREATE, handler::post, this); }
                finally { FrameworkServiceRuntime.SYSTEM_BIND.set(false); }
                if (!bound) {
                    android.util.Log.w("AccountManager", "bindService to " + owner.service + " failed");
                    try { context.unbindService(this); } catch (RuntimeException ignored) { }
                    fail("bind failure");
                }
            } catch (Exception failure) {
                android.util.Log.w("AccountManager", "bind attempt failed for " + accountType, failure);
                fail("bind failure");
            }
        }

        private void fail(String message) { onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, message); }

        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            try {
                Object connected = Class.forName("android.accounts.IAccountAuthenticator$Stub")
                        .getMethod("asInterface", IBinder.class).invoke(null, service);
                synchronized (this) {
                    if (closed || !FrameworkAccounts.this.sessions.containsKey(key)) return;
                    authenticator = connected;
                }
                Object responseProxy = Class.forName("android.accounts.IAccountAuthenticatorResponse$Stub")
                        .getMethod("asInterface", IBinder.class).invoke(null, this);
                run(connected, responseProxy);
            } catch (Exception failure) {
                android.util.Log.w("AccountManager", "authenticator call failed", failure);
                onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "remote exception");
            }
        }

        @Override public void onServiceDisconnected(ComponentName name) {
            Object response = responseAndClose();
            if (response != null) FrameworkAccounts.this.onError(response, AccountManager.ERROR_CODE_REMOTE_EXCEPTION,
                    "disconnected");
        }

        // IAccountAuthenticatorResponse.
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(RESPONSE_DESCRIPTOR); return true; }
            data.enforceInterface(RESPONSE_DESCRIPTOR);
            switch (code) {
                case FIRST_CALL_TRANSACTION: onResult(data.readTypedObject(Bundle.CREATOR)); return true;
                case FIRST_CALL_TRANSACTION + 1: return true;  // onRequestContinued
                case FIRST_CALL_TRANSACTION + 2: onError(data.readInt(), data.readString()); return true;
                default: return super.onTransact(code, data, reply, flags);
            }
        }

        // The authenticator's uid is the calling uid of its response; local
        // authenticators answer on the call's own thread.
        void onResult(Bundle result) {
            if (result != null) {
                boolean confirmed = result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
                boolean created = result.containsKey(AccountManager.KEY_ACCOUNT_NAME)
                        && result.containsKey(AccountManager.KEY_ACCOUNT_TYPE);
                if (updateLastAuthenticatedTime && accountName != null && (confirmed || created)) {
                    try { touchAuthenticated(new Account(accountName, accountType)); }
                    catch (Exception ignored) { }
                }
                if (!keyIntentValid(result)) {
                    onError(AccountManager.ERROR_CODE_INVALID_RESPONSE, "invalid intent in bundle returned");
                    return;
                }
            }
            Object target;
            if (expectActivityLaunch && result != null && result.containsKey(AccountManager.KEY_INTENT)) {
                synchronized (this) { target = response; }
            } else {
                target = responseAndClose();
            }
            if (target == null) return;
            if (result == null) {
                FrameworkAccounts.this.onError(target, AccountManager.ERROR_CODE_INVALID_RESPONSE, "null bundle returned");
                return;
            }
            if (stripAuthTokenFromResult) {
                result.remove(AccountManager.KEY_AUTHTOKEN);
                if (!keyIntentValid(result)) {
                    onError(AccountManager.ERROR_CODE_INVALID_RESPONSE, "invalid intent in bundle returned");
                    return;
                }
            }
            if (result.getInt(AccountManager.KEY_ERROR_CODE, -1) > 0)
                FrameworkAccounts.this.onError(target, result.getInt(AccountManager.KEY_ERROR_CODE),
                        result.getString(AccountManager.KEY_ERROR_MESSAGE));
            else FrameworkAccounts.this.onResult(target, result);
        }

        void onError(int code, String message) {
            Object target = responseAndClose();
            if (target != null) FrameworkAccounts.this.onError(target, code, message);
        }

        // KEY_INTENT must open an Activity of the authenticator's own application
        // (or one with its signature): the system never launches an
        // authenticator-chosen Intent into an unrelated package.
        private boolean keyIntentValid(Bundle result) {
            Intent intent;
            try { intent = result.getParcelable(AccountManager.KEY_INTENT, Intent.class); }
            catch (RuntimeException malformed) { return false; }
            if (intent == null) return true;
            if (intent.getClass() != Intent.class) return false;
            int prohibited = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
            if ((intent.getFlags() & prohibited) != 0 || owner == null) return false;
            ComponentName component = intent.getComponent();
            return component != null && (component.getPackageName().equals(owner.packageName)
                    || packages.sameSigner(owner.packageName, component.getPackageName()));
        }
    }

    // A Session that makes one authenticator call; failures answer the response.
    private abstract class Op extends Session {
        Op(Object response, String type, boolean expectActivityLaunch, boolean strip, String name, boolean touch) {
            super(response, type, expectActivityLaunch, strip, name, touch);
        }
        @Override final void run(Object authenticator, Object responseProxy) throws Exception {
            try { call(authenticator, responseProxy); }
            catch (RuntimeException | RemoteException failure) {
                onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "remote exception");
            }
        }
        abstract void call(Object authenticator, Object responseProxy) throws Exception;
    }

    // IAccountAuthenticator.<method>(response, ...), reflectively.
    private static void authenticate(Object authenticator, String method, Object responseProxy, Object... args)
            throws Exception {
        Object[] all = new Object[args.length + 1];
        all[0] = responseProxy;
        System.arraycopy(args, 0, all, 1, args.length);
        reflect(authenticator, method, all);
    }

    // ---- store helpers ----

    private void touchAuthenticated(Account account) throws Exception {
        store.update(state -> {
            FrameworkAccountStore.Entry entry = state.find(account);
            if (entry == null) return false;
            entry.lastAuthenticated = System.currentTimeMillis();
            state.touch();
            return true;
        });
    }

    private boolean exists(Account account) throws Exception { return store.read().find(account) != null; }

    private boolean addInternal(Account account, String password, Bundle extras, Map<?, ?> visibility,
            int uid) throws Exception {
        if (account.name == null || account.name.length() > 200 || account.type == null
                || account.type.length() > 200) return false;
        boolean added = store.update(state -> {
            if (state.find(account) != null || state.entries.size() > 100) return false;
            FrameworkAccountStore.Entry entry = state.add(account);
            entry.password = password;
            if (extras != null) for (String key : extras.keySet()) entry.data.put(key, extras.getString(key));
            if (visibility != null) for (Map.Entry<?, ?> each : visibility.entrySet())
                entry.visibility.put((String) each.getKey(), (Integer) each.getValue());
            return true;
        });
        if (added) accountsChanged();
        return added;
    }

    private boolean removeInternal(Account account) throws Exception {
        boolean removed = store.update(state -> state.remove(account));
        if (removed) accountsChanged();
        return removed;
    }

    private List<Account> featureCandidates(String type, int uid, String packageName) throws Exception {
        return visibleAccounts(store.read(), type, uid, packageName, false);
    }

    // ---- the interface ----

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("account." + method); }
        watch();
        int uid = callingUid();
        switch (contract) {
            case getPassword: {
                Account account = requireAccount(a[0]);
                requireManaged(account.type, uid, "get secrets for accounts");
                FrameworkAccountStore.Entry entry = store.read().find(account);
                return entry == null ? null : entry.password;
            }
            case getUserData: {
                Account account = requireAccount(a[0]);
                if (a[1] == null) throw new NullPointerException("key cannot be null");
                requireManaged(account.type, uid, "get user data for accounts");
                FrameworkAccountStore.Entry entry = store.read().find(account);
                return entry == null ? null : entry.data.get((String) a[1]);
            }
            case getAuthenticatorTypes: {
                user((Integer) a[0]);
                List<Authenticator> all = authenticators();
                AuthenticatorDescription[] result = new AuthenticatorDescription[all.size()];
                for (int i = 0; i < result.length; i++) result[i] = all.get(i).description();
                return result;
            }
            case getAccountsAsUser: {
                user((Integer) a[1]);
                ownPackage(a[2]);
                return accountsOfType((String) a[0], uid, (String) a[2], false);
            }
            case getAccountsForPackage: {
                // Callers other than the system see only what their own uid may.
                if (!isSystemUid(uid) && !isPrivileged(uid)) throw new SecurityException(
                        "uid " + uid + " cannot get accounts for another package");
                return accountsOfType(null, (Integer) a[1], (String) a[0], true);
            }
            case getAccountsByTypeForPackage: {
                String type = (String) a[0], packageName = (String) a[1];
                int packageUid = packages.packageUid(packageName);
                if (packageUid < 0) return new Account[0];
                if (!isSystemUid(uid) && !(type != null && managedByCaller(type, uid)))
                    throw new SecurityException("uid " + uid + " cannot get accounts for package " + packageName);
                return accountsOfType(type, packageUid, packageName, true);
            }
            case hasFeatures: {
                ownPackage(a[4]);
                Object response = a[0];
                if (response == null) throw new IllegalArgumentException("response is null");
                if (a[1] == null) throw new IllegalArgumentException("account is null");
                if (a[2] == null) throw new IllegalArgumentException("features is null");
                user((Integer) a[3]);
                Account account = (Account) a[1];
                String[] features = (String[]) a[2];
                if (!visibleAccounts(store.read(), account.type, uid, (String) a[4], false).contains(account)) {
                    Bundle none = new Bundle();
                    none.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false);
                    onResult(response, none);
                    return null;
                }
                new Op(response, account.type, false, true, account.name, false) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "hasFeatures", proxy, account, features);
                    }
                    @Override void onResult(Bundle result) {
                        Object target = responseAndClose();
                        if (target == null) return;
                        if (result == null) { FrameworkAccounts.this.onError(target,
                                AccountManager.ERROR_CODE_INVALID_RESPONSE, "null bundle"); return; }
                        Bundle reply = new Bundle();
                        reply.putBoolean(AccountManager.KEY_BOOLEAN_RESULT,
                                result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT, false));
                        FrameworkAccounts.this.onResult(target, reply);
                    }
                }.bind();
                return null;
            }
            case getAccountByTypeAndFeatures:
            case getAccountsByFeatures: {
                ownPackage(a[3]);
                Object response = a[0];
                if (response == null) throw new IllegalArgumentException("response is null");
                if (a[1] == null) throw new IllegalArgumentException("accountType is null");
                String type = (String) a[1];
                String[] features = (String[]) a[2];
                boolean single = contract == Contract.getAccountByTypeAndFeatures;
                if (!visibleTypes(uid).contains(type)) {
                    if (single) onResult(response, new Bundle());
                    else { Bundle none = new Bundle(); none.putParcelableArray("accounts", new Account[0]); onResult(response, none); }
                    return null;
                }
                List<Account> candidates = featureCandidates(type, uid, (String) a[3]);
                if (features == null || features.length == 0) {
                    finishFeatures(response, candidates, single);
                    return null;
                }
                new FeatureSearch(response, type, candidates, features, single).bind();
                return null;
            }
            case addAccountExplicitly:
            case addAccountExplicitlyWithVisibility: {
                boolean withVisibility = contract == Contract.addAccountExplicitlyWithVisibility;
                Account account = requireAccount(a[0]);
                String opPackage = (String) a[withVisibility ? 4 : 3];
                requireManaged(account.type, uid, "explicitly add accounts");
                return addInternal(account, (String) a[1], (Bundle) a[2], withVisibility ? (Map<?, ?>) a[3] : null, uid);
            }
            case removeAccountAsUser: {
                Object response = a[0];
                Account account = (Account) a[1];
                if (account == null) throw new IllegalArgumentException("account cannot be null");
                if (response == null) throw new IllegalArgumentException("response cannot be null");
                user((Integer) a[3]);
                if (!managedByCaller(account.type, uid) && !isSystemUid(uid))
                    throw new SecurityException("uid " + uid + " cannot remove accounts of type: " + account.type);
                new Op(response, account.type, (Boolean) a[2], true, account.name, false) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "getAccountRemovalAllowed", proxy, account);
                    }
                    @Override void onResult(Bundle result) {
                        if (result != null && result.containsKey(AccountManager.KEY_BOOLEAN_RESULT)
                                && !result.containsKey(AccountManager.KEY_INTENT)) {
                            try {
                                if (result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT)) removeInternal(account);
                            } catch (Exception failure) {
                                onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "storage failure");
                                return;
                            }
                            Object target = responseAndClose();
                            if (target != null) FrameworkAccounts.this.onResult(target, result);
                            return;
                        }
                        super.onResult(result);
                    }
                }.bind();
                return null;
            }
            case removeAccountExplicitly: {
                if (a[0] == null) return false;
                Account account = (Account) a[0];
                requireManaged(account.type, uid, "explicitly remove accounts");
                return removeInternal(account);
            }
            case invalidateAuthToken: {
                String type = Objects_requireNonNull(a[0], "accountType cannot be null");
                String token = Objects_requireNonNull(a[1], "authToken cannot be null");
                boolean changed = store.update(state -> {
                    boolean any = false;
                    for (FrameworkAccountStore.Entry entry : state.ofType(type))
                        any |= entry.tokens.values().removeIf(token::equals);
                    if (any) state.touch();
                    return any;
                });
                if (changed) accountsChanged();
                return null;
            }
            case peekAuthToken: {
                Account account = requireAccount(a[0]);
                Objects_requireNonNull(a[1], "authTokenType cannot be null");
                requireManaged(account.type, uid, "peek the authtokens associated with accounts");
                FrameworkAccountStore.Entry entry = store.read().find(account);
                return entry == null ? null : entry.tokens.get((String) a[1]);
            }
            case setAuthToken: {
                Account account = requireAccount(a[0]);
                Objects_requireNonNull(a[1], "authTokenType cannot be null");
                requireManaged(account.type, uid, "set auth tokens associated with accounts");
                String type = (String) a[1], token = (String) a[2];
                store.update(state -> {
                    FrameworkAccountStore.Entry entry = state.find(account);
                    if (entry == null) return null;
                    if (token == null) entry.tokens.remove(type); else entry.tokens.put(type, token);
                    state.touch();
                    return null;
                });
                return null;
            }
            case setPassword:
            case clearPassword: {
                Account account = requireAccount(a[0]);
                requireManaged(account.type, uid, contract == Contract.setPassword
                        ? "set secrets for accounts" : "clear passwords for accounts");
                String password = contract == Contract.setPassword ? (String) a[1] : null;
                boolean changed = store.update(state -> {
                    FrameworkAccountStore.Entry entry = state.find(account);
                    if (entry == null) return false;
                    entry.password = password;
                    entry.tokens.clear();
                    state.touch();
                    return true;
                });
                if (changed) accountsChanged();
                return null;
            }
            case setUserData: {
                if (a[1] == null) throw new IllegalArgumentException("key is null");
                Account account = requireAccount(a[0]);
                requireManaged(account.type, uid, "set user data for accounts");
                String key = (String) a[1], value = (String) a[2];
                store.update(state -> {
                    FrameworkAccountStore.Entry entry = state.find(account);
                    if (entry == null) return null;
                    entry.data.put(key, value);
                    state.touch();
                    return null;
                });
                return null;
            }
            case updateAppPermission: {
                if (!isSystemUid(uid)) throw new SecurityException("Only the system can update account permissions");
                Account account = requireAccount(a[0]);
                String grant = a[2] + ":" + a[1];
                boolean value = (Boolean) a[3];
                store.update(state -> {
                    FrameworkAccountStore.Entry entry = state.find(account);
                    if (entry == null) return null;
                    if (value) entry.grants.add(grant); else entry.grants.remove(grant);
                    state.touch();
                    return null;
                });
                return null;
            }
            case getAuthToken: return getAuthToken(a, uid);
            case addAccount:
            case addAccountAsUser: {
                Object response = a[0];
                if (response == null) throw new IllegalArgumentException("response is null");
                if (a[1] == null) throw new IllegalArgumentException("accountType is null");
                if (contract == Contract.addAccountAsUser) user((Integer) a[6]);
                String type = (String) a[1], tokenType = (String) a[2];
                String[] features = (String[]) a[3];
                Bundle options = a[5] == null ? new Bundle() : copy((Bundle) a[5]);
                options.putInt(AccountManager.KEY_CALLER_UID, uid);
                options.putInt(AccountManager.KEY_CALLER_PID, Process.myPid());
                new Op(response, type, (Boolean) a[4], true, null, true) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "addAccount", proxy, type, tokenType, features, options);
                    }
                }.bind();
                return null;
            }
            case updateCredentials: {
                Object response = a[0];
                Account account = (Account) a[1];
                if (response == null) throw new IllegalArgumentException("response is null");
                if (account == null) throw new IllegalArgumentException("account is null");
                String tokenType = (String) a[2];
                Bundle options = copy((Bundle) a[4]);
                new Op(response, account.type, (Boolean) a[3], false, account.name, true) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "updateCredentials", proxy, account, tokenType, options);
                    }
                }.bind();
                return null;
            }
            case editProperties: {
                Object response = a[0];
                if (response == null) throw new IllegalArgumentException("response is null");
                if (a[1] == null) throw new IllegalArgumentException("accountType is null");
                String type = (String) a[1];
                requireManaged(type, uid, "edit properties of accounts");
                new Op(response, type, (Boolean) a[2], false, null, false) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "editProperties", proxy, type);
                    }
                }.bind();
                return null;
            }
            case confirmCredentialsAsUser: {
                Object response = a[0];
                Account account = (Account) a[1];
                if (response == null) throw new IllegalArgumentException("response is null");
                if (account == null) throw new IllegalArgumentException("account is null");
                user((Integer) a[4]);
                Bundle options = copy((Bundle) a[2]);
                new Op(response, account.type, (Boolean) a[3], true, account.name, true) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "confirmCredentials", proxy, account, options);
                    }
                }.bind();
                return null;
            }
            case accountAuthenticated: {
                Account account = requireAccount(a[0]);
                requireManaged(account.type, uid, "notify authentication for accounts");
                return store.update(state -> {
                    FrameworkAccountStore.Entry entry = state.find(account);
                    if (entry == null) return false;
                    entry.lastAuthenticated = System.currentTimeMillis();
                    state.touch();
                    return true;
                });
            }
            case getAuthTokenLabel: {
                if (a[1] == null) throw new IllegalArgumentException("accountType cannot be null");
                if (a[2] == null) throw new IllegalArgumentException("authTokenType cannot be null");
                if (!isSystemUid(uid)) throw new SecurityException("can only call from system");
                String tokenType = (String) a[2];
                new Op(a[0], (String) a[1], false, false, null, false) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "getAuthTokenLabel", proxy, tokenType);
                    }
                    @Override void onResult(Bundle result) {
                        Bundle reply = null;
                        if (result != null) {
                            reply = new Bundle();
                            reply.putString(AccountManager.KEY_AUTH_TOKEN_LABEL,
                                    result.getString(AccountManager.KEY_AUTH_TOKEN_LABEL));
                        }
                        super.onResult(reply);
                    }
                }.bind();
                return null;
            }
            case renameAccount: {
                Object response = a[0];
                Account account = (Account) a[1];
                String newName = (String) a[2];
                if (account == null) throw new IllegalArgumentException("account is null");
                if (newName != null && newName.length() > 200) throw new IllegalArgumentException("account name longer than 200");
                requireManaged(account.type, uid, "rename accounts");
                Account renamed = store.update(state -> {
                    FrameworkAccountStore.Entry entry = state.find(account);
                    if (entry == null || newName == null || state.find(new Account(newName, account.type)) != null)
                        return null;
                    FrameworkAccountStore.Entry copy = new FrameworkAccountStore.Entry(newName, entry.type);
                    copy.password = entry.password;
                    copy.previousName = entry.name;
                    copy.lastAuthenticated = entry.lastAuthenticated;
                    copy.data.putAll(entry.data);
                    copy.tokens.putAll(entry.tokens);
                    copy.visibility.putAll(entry.visibility);
                    copy.grants.addAll(entry.grants);
                    state.entries.set(state.entries.indexOf(entry), copy);
                    state.touch();
                    return copy.account();
                });
                if (renamed == null) {
                    // AccountManagerService leaves the response unanswered on failure;
                    // answer rather than hang the caller.
                    onError(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS, "rename failed");
                    return null;
                }
                accountsChanged();
                Bundle result = new Bundle();
                result.putString(AccountManager.KEY_ACCOUNT_NAME, renamed.name);
                result.putString(AccountManager.KEY_ACCOUNT_TYPE, renamed.type);
                result.putString(KEY_ACCOUNT_ACCESS_ID, accessId(renamed));
                onResult(response, result);
                return null;
            }
            case getPreviousName: {
                FrameworkAccountStore.Entry entry = store.read().find(requireAccount(a[0]));
                return entry == null ? null : entry.previousName;
            }
            case someUserHasAccount: {
                if (!isSystemUid(uid)) throw new SecurityException("Only the system can check accounts of all users");
                return exists(requireAccount(a[0]));
            }
            case isCredentialsUpdateSuggested: {
                Object response = a[0];
                Account account = (Account) a[1];
                String status = (String) a[2];
                if (response == null) throw new IllegalArgumentException("response is null");
                if (account == null) throw new IllegalArgumentException("account is null");
                if (status == null || status.isEmpty()) throw new IllegalArgumentException("status token is empty");
                requireManaged(account.type, uid, "check credentials of accounts");
                new Op(response, account.type, false, false, null, false) {
                    @Override void call(Object authenticator, Object proxy) throws Exception {
                        authenticate(authenticator, "isCredentialsUpdateSuggested", proxy, account, status);
                    }
                    @Override void onResult(Bundle result) {
                        Object target = responseAndClose();
                        if (target == null) return;
                        if (result == null) { FrameworkAccounts.this.onError(target,
                                AccountManager.ERROR_CODE_INVALID_RESPONSE, "null bundle"); return; }
                        Bundle reply = new Bundle();
                        reply.putBoolean(AccountManager.KEY_BOOLEAN_RESULT,
                                result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT, false));
                        FrameworkAccounts.this.onResult(target, reply);
                    }
                }.bind();
                return null;
            }
            case getPackagesAndVisibilityForAccount: {
                Account account = requireAccount(a[0]);
                if (!managedByCaller(account.type, uid) && !isSystemUid(uid))
                    throw new SecurityException("uid " + uid + " cannot get secrets for account " + account);
                FrameworkAccountStore.Entry entry = store.read().find(account);
                return new HashMap<>(entry == null ? new HashMap<String, Integer>() : entry.visibility);
            }
            case getAccountVisibility: {
                Account account = requireAccount(a[0]);
                String packageName = Objects_requireNonNull(a[1], "packageName cannot be null");
                if (!managedByCaller(account.type, uid) && !isSystemUid(uid))
                    throw new SecurityException("uid " + uid + " cannot get secrets for accounts of type: " + account.type);
                FrameworkAccountStore.Entry entry = store.read().find(account);
                if (entry == null) return AccountManager.VISIBILITY_NOT_VISIBLE;
                if (AccountManager.PACKAGE_NAME_KEY_LEGACY_VISIBLE.equals(packageName)) {
                    int value = visibilityOf(entry, packageName);
                    return value != AccountManager.VISIBILITY_UNDEFINED ? value : AccountManager.VISIBILITY_USER_MANAGED_VISIBLE;
                }
                if (AccountManager.PACKAGE_NAME_KEY_LEGACY_NOT_VISIBLE.equals(packageName)) {
                    int value = visibilityOf(entry, packageName);
                    return value != AccountManager.VISIBILITY_UNDEFINED ? value : AccountManager.VISIBILITY_USER_MANAGED_NOT_VISIBLE;
                }
                return packages.packageUid(packageName) < 0 ? AccountManager.VISIBILITY_NOT_VISIBLE
                        : resolveVisibility(entry, packageName);
            }
            case setAccountVisibility: {
                Account account = requireAccount(a[0]);
                String packageName = Objects_requireNonNull(a[1], "packageName cannot be null");
                if (!managedByCaller(account.type, uid) && !isSystemUid(uid))
                    throw new SecurityException("uid " + uid + " cannot get secrets for accounts of type: " + account.type);
                int visibility = (Integer) a[2];
                boolean changed = store.update(state -> {
                    FrameworkAccountStore.Entry entry = state.find(account);
                    if (entry == null) return false;
                    Integer old = entry.visibility.put(packageName, visibility);
                    if (old != null && old == visibility) return false;
                    state.touch();
                    return true;
                });
                if (changed) accountsChanged();
                return changed || exists(account);
            }
            case getAccountsAndVisibilityForPackage: {
                String packageName = (String) a[0], type = (String) a[1];
                boolean system = isSystemUid(uid);
                if (packages.packageUid(packageName) < 0) return new LinkedHashMap<Account, Integer>();
                Map<Account, Integer> result = new LinkedHashMap<>();
                for (FrameworkAccountStore.Entry entry : store.read().ofType(type))
                    if (system || managedByCaller(entry.type, uid))
                        result.put(entry.account(), resolveVisibility(entry, packageName));
                return result;
            }
            // Listeners learn of changes through the visible-accounts broadcast.
            case registerAccountListener:
            case unregisterAccountListener: {
                ownPackage(a[1]);
                synchronized (this) {
                    if (contract == Contract.registerAccountListener) listeners.add((String) a[1]);
                    else listeners.remove((String) a[1]);
                }
                return null;
            }
            case hasAccountAccess: {
                Account account = requireAccount(a[0]);
                String packageName = (String) a[1];
                if (!isSystemUid(uid) && !isPrivileged(uid)) throw new SecurityException("uid " + uid + " cannot check account access");
                FrameworkAccountStore.Entry entry = store.read().find(account);
                return entry != null && packages.packageUid(packageName) >= 0
                        && visible(resolveVisibility(entry, packageName));
            }
            case onAccountAccessed: return null;
            default: throw new UnsupportedOperationException("account." + method);
        }
    }

    private static <T> T Objects_requireNonNull(Object value, String message) {
        if (value == null) throw new NullPointerException(message);
        @SuppressWarnings("unchecked") T result = (T) value;
        return result;
    }

    // Account.getAccessId() is hidden.
    private static String accessId(Account account) {
        try { return (String) Account.class.getMethod("getAccessId").invoke(account); }
        catch (ReflectiveOperationException unavailable) { throw new IllegalStateException(unavailable); }
    }

    private static Account requireAccount(Object value) {
        if (value == null) throw new IllegalArgumentException("account is null");
        return (Account) value;
    }

    private List<String> visibleTypes(int uid) throws Exception {
        List<String> types = new ArrayList<>();
        for (Authenticator each : authenticators()) types.add(each.type);
        return types;
    }

    private Account[] accountsOfType(String type, int uid, String packageName, boolean includeUserManagedNotVisible)
            throws Exception {
        if (type != null && !visibleTypes(uid).contains(type)) return new Account[0];
        List<Account> accounts = visibleAccounts(store.read(), type, uid, packageName, includeUserManagedNotVisible);
        return accounts.toArray(new Account[0]);
    }

    // handleGetAccountsResult for a feature-less request: a Bundle naming the
    // only qualifying account, an empty one for none. Choosing among several
    // is a system Activity this profile does not provision.
    private void finishFeatures(Object response, List<Account> accounts, boolean single) {
        Bundle result = new Bundle();
        if (!single) {
            result.putParcelableArray("accounts", accounts.toArray(new Account[0]));
        } else if (accounts.size() == 1) {
            result.putString(AccountManager.KEY_ACCOUNT_NAME, accounts.get(0).name);
            result.putString(AccountManager.KEY_ACCOUNT_TYPE, accounts.get(0).type);
            result.putString(KEY_ACCOUNT_ACCESS_ID, accessId(accounts.get(0)));
        } else if (accounts.size() > 1) {
            onError(response, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION, "choosing among accounts is not available");
            return;
        }
        onResult(response, result);
    }

    // GetAccountsByTypeAndFeatureSession: ask the authenticator about each candidate in turn.
    private final class FeatureSearch extends Op {
        private final List<Account> candidates, matches = new ArrayList<>();
        private final String[] features;
        private final boolean single;
        private int index;
        private Object proxy, bound;

        FeatureSearch(Object response, String type, List<Account> candidates, String[] features, boolean single) {
            super(response, type, false, true, null, false);
            this.candidates = candidates; this.features = features; this.single = single;
        }

        @Override void call(Object authenticator, Object proxy) throws Exception {
            this.bound = authenticator; this.proxy = proxy;
            next();
        }

        private void next() throws Exception {
            if (index >= candidates.size()) {
                Object target = responseAndClose();
                if (target != null) finishFeatures(target, matches, single);
                return;
            }
            authenticate(bound, "hasFeatures", proxy, candidates.get(index), features);
        }

        @Override void onResult(Bundle result) {
            if (result == null) { onError(AccountManager.ERROR_CODE_INVALID_RESPONSE, "null bundle"); return; }
            if (result.getBoolean(AccountManager.KEY_BOOLEAN_RESULT, false)) matches.add(candidates.get(index));
            index++;
            try { next(); }
            catch (Exception failure) { onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "remote exception"); }
        }
    }

    private Object getAuthToken(Object[] a, int uid) throws Exception {
        Object response = a[0];
        Account account = (Account) a[1];
        String tokenType = (String) a[2];
        boolean expectActivityLaunch = (Boolean) a[4];
        Bundle options = copy((Bundle) a[5]);
        if (response == null) throw new IllegalArgumentException("response cannot be null");
        if (account == null) { onError(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS, "account is null"); return null; }
        if (tokenType == null) { onError(response, AccountManager.ERROR_CODE_BAD_ARGUMENTS, "authTokenType is null"); return null; }
        Authenticator owner = authenticator(account.type);
        boolean customTokens = owner != null && owner.customTokens;
        boolean granted = customTokens || isPrivileged(uid) || managedByCaller(account.type, uid)
                || hasGrant(account, tokenType, uid);
        String callerPackage = options.getString(AccountManager.KEY_ANDROID_PACKAGE_NAME);
        if (callerPackage == null || packages.packageUid(callerPackage) != uid)
            throw new SecurityException("Uid " + uid + " is attempting to illegally masquerade as package " + callerPackage + "!");
        options.putInt(AccountManager.KEY_CALLER_UID, uid);
        options.putInt(AccountManager.KEY_CALLER_PID, Process.myPid());
        if ((Boolean) a[3]) options.putBoolean(KEY_NOTIFY_ON_FAILURE, true);
        if (!customTokens && granted) {
            FrameworkAccountStore.Entry entry = store.read().find(account);
            String token = entry == null ? null : entry.tokens.get(tokenType);
            if (token != null) {
                Bundle result = new Bundle();
                result.putString(AccountManager.KEY_AUTHTOKEN, token);
                result.putString(AccountManager.KEY_ACCOUNT_NAME, account.name);
                result.putString(AccountManager.KEY_ACCOUNT_TYPE, account.type);
                onResult(response, result);
                return null;
            }
        }
        if (!granted) {
            // The credential-grant dialog is a system Activity this profile lacks.
            onError(response, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION,
                    "access to " + account.type + " tokens requires a grant");
            return null;
        }
        new Op(response, account.type, expectActivityLaunch, false, account.name, false) {
            @Override void call(Object authenticator, Object proxy) throws Exception {
                authenticate(authenticator, "getAuthToken", proxy, account, tokenType, options);
            }
            @Override void onResult(Bundle result) {
                if (result != null) {
                    String token = result.getString(AccountManager.KEY_AUTHTOKEN);
                    if (token != null) {
                        String name = result.getString(AccountManager.KEY_ACCOUNT_NAME);
                        String type = result.getString(AccountManager.KEY_ACCOUNT_TYPE);
                        if (name == null || name.isEmpty() || type == null || type.isEmpty()) {
                            onError(AccountManager.ERROR_CODE_INVALID_RESPONSE, "the type and name should not be empty");
                            return;
                        }
                        if (!customTokens) {
                            try {
                                store.update(state -> {
                                    FrameworkAccountStore.Entry entry = state.find(new Account(name, type));
                                    if (entry == null) return null;
                                    entry.tokens.put(tokenType, token);
                                    state.touch();
                                    return null;
                                });
                            } catch (Exception failure) {
                                onError(AccountManager.ERROR_CODE_REMOTE_EXCEPTION, "storage failure");
                                return;
                            }
                        }
                    }
                }
                super.onResult(result);
            }
        }.bind();
        return null;
    }

    private boolean hasGrant(Account account, String tokenType, int uid) throws Exception {
        FrameworkAccountStore.Entry entry = store.read().find(account);
        return entry != null && entry.grants.contains(uid + ":" + tokenType);
    }
}
