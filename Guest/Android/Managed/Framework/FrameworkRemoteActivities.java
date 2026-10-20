// Activities of microG's Play services (Google Sign-In, the account chooser,
// consent and add-account screens) that an app starts. On Android they run in
// the Play services ":ui" process; here the session host runs each one as a
// session of the system app in its own host window (android_google.py) and
// returns its result, so the request travels through files in the app's cache
// directory and the host's process socket. This class has two sides: the
// calling app writes the request and delivers the result, and the system app
// session (ManagedLauncher.runApp) reads the request staged in its own root
// and writes the result when its Activity finishes.
import android.accounts.AccountAuthenticatorResponse;
import android.accounts.AccountManager;
import android.app.Application;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import org.json.JSONObject;

final class FrameworkRemoteActivities {
    static final String PACKAGE = "com.google.android.gms";
    // Where android_google.py stages a request in the system app's root.
    private static final String STAGED = "/data/system/users/0/bifrost-activity";
    private static final String CACHE = "google-activity";
    private static final int START_SUCCESS = 0, START_CANCELED = -96;

    interface Delivery {
        void deliver(IBinder resultTo, String who, int requestCode, int code, Intent data) throws Exception;
    }

    private final FrameworkPackages packages;
    private final FrameworkProcessRuntime processes;
    private final Delivery delivery;
    private Application application;
    private Handler main;

    FrameworkRemoteActivities(FrameworkPackages packages, FrameworkProcessRuntime processes, Delivery delivery) {
        this.packages = packages;
        this.processes = processes;
        this.delivery = delivery;
    }

    void attach(Application application, Handler main) {
        this.application = application;
        this.main = main;
    }

    // ---- the calling app ----

    // The Play services Activity an Intent names or resolves to, or null.
    // Implicit resolution parses the whole Play services package, so only
    // its own actions take it.
    ActivityInfo resolve(Intent intent) throws Exception {
        ComponentName component = intent.getComponent();
        boolean ours = PACKAGE.equals(application.getPackageName());
        if (component != null)
            return !ours && PACKAGE.equals(component.getPackageName()) ? packages.systemActivity(component) : null;
        String action = intent.getAction();
        if (ours || !(PACKAGE.equals(intent.getPackage()) || action != null && action.startsWith(PACKAGE + "."))) return null;
        ActivityInfo info = packages.resolveSystemActivity(intent);
        return info != null && PACKAGE.equals(info.packageName) ? info : null;
    }

    // IActivityTaskManager.startActivity for one of those Activities. The
    // caller's AccountAuthenticatorResponse (add-account and token screens)
    // is a Binder of this process: it stays here and the session's own
    // response stands in for it, relayed when the window closes.
    int start(ActivityInfo info, Intent original, ComponentName caller, IBinder resultTo, String who,
            int requestCode) {
        Intent intent = new Intent(original).setComponent(new ComponentName(info.packageName, info.name));
        AccountAuthenticatorResponse response = null;
        try {
            response = intent.getParcelableExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE);
        } catch (RuntimeException unreadable) {
            android.util.Log.w("GoogleActivity", "unreadable authenticator response", unreadable);
        }
        intent.removeExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE);
        File directory = new File(new File(application.getCacheDir(), CACHE), UUID.randomUUID().toString().replace("-", ""));
        try {
            PackageInfo app = packages.installedPackage();
            if (app == null) throw new IllegalStateException("No installed package");
            app.applicationInfo.nonLocalizedLabel = application.getPackageManager()
                    .getApplicationLabel(application.getApplicationInfo());
            if (!directory.mkdirs()) throw new IOException("Cannot create " + directory);
            write(new File(directory, "intent.bin"), marshal(intent));
            write(new File(directory, "caller.bin"), marshal(app));
            write(new File(directory, "request.json"), new JSONObject()
                    .put("component", info.packageName + "/" + info.name)
                    .put("caller", caller.flattenToString())
                    .put("response", response != null).toString().getBytes(StandardCharsets.UTF_8));
            FrameworkProcessRuntime.host(new JSONObject().put("operation", "activity")
                    .put("directory", directory.getAbsolutePath()));
        } catch (Exception failure) {
            android.util.Log.w("GoogleActivity", "cannot start " + info.name, failure);
            System.out.println("APP_ACTIVITY_REMOTE_FAILED " + info.name + " " + failure);
            delete(directory);
            if (response != null) response.onError(AccountManager.ERROR_CODE_CANCELED, "canceled");
            return START_CANCELED;
        }
        System.out.println("APP_ACTIVITY_REMOTE " + info.name);
        AccountAuthenticatorResponse relay = response;
        main.postDelayed(new Runnable() {
            @Override public void run() {
                File result = new File(directory, "result.json");
                if (!result.isFile()) {
                    main.postDelayed(this, 200);
                    return;
                }
                try {
                    finish(directory, relay, resultTo, who, requestCode);
                } catch (Exception failure) {
                    throw new RuntimeException("Unable to deliver " + info.name + " result", failure);
                } finally {
                    delete(directory);
                }
            }
        }, 200);
        return START_SUCCESS;
    }

    // The host wrote the result: refresh Play services when the accounts
    // changed, answer the authenticator's caller, then the Activity's caller.
    private void finish(File directory, AccountAuthenticatorResponse response, IBinder resultTo, String who,
            int requestCode) throws Exception {
        JSONObject result = new JSONObject(new String(read(new File(directory, "result.json")), StandardCharsets.UTF_8));
        int code = result.getInt("resultCode");
        Intent data = result.optBoolean("data") ? intent(read(new File(directory, "result.bin"))) : null;
        System.out.println("APP_ACTIVITY_REMOTE_RESULT " + code + " response=" + result.optString("response", "none")
                + " changed=" + result.optBoolean("changed") + (result.has("error") ? " error=" + result.getString("error") : ""));
        // Play services processes cache their registration and accounts: a
        // new sign-in reaches them through a fresh process.
        if (result.optBoolean("changed")) processes.restart(PACKAGE);
        if (response != null) {
            String kind = result.optString("response", "none");
            if (kind.equals("result")) response.onResult(bundle(read(new File(directory, "response.bin"))));
            else if (kind.equals("error")) response.onError(result.optInt("errorCode"), result.optString("errorMessage"));
            else response.onError(AccountManager.ERROR_CODE_CANCELED, "canceled");
        }
        if (resultTo != null && requestCode >= 0) {
            if (data != null) data.setExtrasClassLoader(application.getClassLoader());
            delivery.deliver(resultTo, who, requestCode, code, data);
        }
    }

    // ---- the system app session ----

    static final class Request {
        final Intent intent;
        final PackageInfo caller;
        final ComponentName callingActivity;
        final Relay relay;
        Request(Intent intent, PackageInfo caller, ComponentName callingActivity, Relay relay) {
            this.intent = intent; this.caller = caller; this.callingActivity = callingActivity; this.relay = relay;
        }
    }

    // The staged request for this session, or null for an ordinary start.
    static Request load(ClassLoader loader) throws Exception {
        File request = new File(STAGED, "request.json");
        if (!request.isFile()) return null;
        JSONObject json = new JSONObject(new String(read(request), StandardCharsets.UTF_8));
        Intent intent = intent(read(new File(STAGED, "intent.bin")));
        intent.setExtrasClassLoader(loader);
        PackageInfo caller = unmarshal(read(new File(STAGED, "caller.bin")), PackageInfo.CREATOR);
        Relay relay = null;
        if (json.getBoolean("response")) {
            relay = new Relay();
            Parcel parcel = Parcel.obtain();
            try {
                parcel.writeStrongBinder(relay);
                parcel.setDataPosition(0);
                intent.putExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE,
                        (Parcelable) AccountAuthenticatorResponse.CREATOR.createFromParcel(parcel));
            } finally { parcel.recycle(); }
        }
        return new Request(intent, caller, ComponentName.unflattenFromString(json.getString("caller")), relay);
    }

    // IAccountAuthenticatorResponse: what the Activity answers, kept for the
    // calling app's own response.
    static final class Relay extends Binder {
        private static final String DESCRIPTOR = "android.accounts.IAccountAuthenticatorResponse";
        byte[] result;
        int errorCode;
        String errorMessage;
        boolean error;

        @Override protected synchronized boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code == INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
            data.enforceInterface(DESCRIPTOR);
            switch (code) {
                case FIRST_CALL_TRANSACTION: {
                    Bundle value = data.readTypedObject(Bundle.CREATOR);
                    result = marshal(value == null ? new Bundle() : value);
                    error = false;
                    return true;
                }
                case FIRST_CALL_TRANSACTION + 1: return true;  // onRequestContinued
                case FIRST_CALL_TRANSACTION + 2:
                    errorCode = data.readInt();
                    errorMessage = data.readString();
                    error = true;
                    result = null;
                    return true;
                default: return false;
            }
        }
    }

    // The Activity finished: what it returned and what it told its response.
    static void writeResult(Request request, int resultCode, Intent data) {
        try {
            JSONObject json = new JSONObject().put("resultCode", resultCode).put("response", "none");
            if (data != null) {
                try {
                    write(new File(STAGED, "result.bin"), marshal(data));
                    json.put("data", true);
                } catch (RuntimeException unmarshallable) {
                    System.out.println("APP_ACTIVITY_REMOTE_RESULT_UNMARSHALLABLE " + unmarshallable);
                }
            }
            Relay relay = request.relay;
            if (relay != null) synchronized (relay) {
                if (relay.result != null) {
                    write(new File(STAGED, "response.bin"), relay.result);
                    json.put("response", "result");
                } else if (relay.error) {
                    json.put("response", "error").put("errorCode", relay.errorCode)
                            .put("errorMessage", relay.errorMessage == null ? "" : relay.errorMessage);
                }
            }
            write(new File(STAGED, "nested.json"), json.toString().getBytes(StandardCharsets.UTF_8));
        } catch (Exception failure) {
            System.out.println("APP_ACTIVITY_REMOTE_RESULT_FAILED " + failure);
        }
    }

    // ---- shared helpers ----

    private static byte[] marshal(Parcelable value) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            return parcel.marshall();
        } finally { parcel.recycle(); }
    }

    private static byte[] marshal(Bundle value) {
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeBundle(value);
            return parcel.marshall();
        } finally { parcel.recycle(); }
    }

    private static <T> T unmarshal(byte[] bytes, Parcelable.Creator<T> creator) {
        Parcel parcel = Parcel.obtain();
        try {
            parcel.unmarshall(bytes, 0, bytes.length);
            parcel.setDataPosition(0);
            return creator.createFromParcel(parcel);
        } finally { parcel.recycle(); }
    }

    private static Intent intent(byte[] bytes) { return unmarshal(bytes, Intent.CREATOR); }

    private static Bundle bundle(byte[] bytes) {
        Parcel parcel = Parcel.obtain();
        try {
            parcel.unmarshall(bytes, 0, bytes.length);
            parcel.setDataPosition(0);
            return parcel.readBundle(FrameworkRemoteActivities.class.getClassLoader());
        } finally { parcel.recycle(); }
    }

    private static byte[] read(File file) throws IOException {
        try (FileInputStream input = new FileInputStream(file)) {
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            for (int count; (count = input.read(chunk)) > 0;) {
                buffer.write(chunk, 0, count);
                if (buffer.size() > 16 * 1024 * 1024) throw new IOException("Oversized activity request: " + file);
            }
            return buffer.toByteArray();
        }
    }

    private static void write(File file, byte[] bytes) throws IOException {
        try (FileOutputStream output = new FileOutputStream(file)) { output.write(bytes); }
    }

    private static void delete(File directory) {
        File[] files = directory.listFiles();
        if (files != null) for (File file : files) file.delete();
        directory.delete();
    }
}
