// Activities the app starts above its launcher Activity (full-screen ads,
// sign-in and web pages). IActivityTaskManager.startActivity arrives here;
// the launch follows ActivityThread.performLaunchActivity and
// handleResumeActivity on the main thread. The new Activity's window is added
// through WindowManager, so FrameworkWindows lays it out, draws it in software
// and routes input to it. The Activity beneath is paused; when the new one
// occludes it (its theme is not translucent or floating), it is also stopped
// and its window becomes invisible, so its layer and SurfaceViews leave the
// host, as on Android. Finishing restarts and resumes the caller and delivers
// any requested result.
import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.View;
import android.view.WindowManager;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

final class FrameworkActivities {
    // ActivityManager start results (hidden constants, Android 15 values).
    private static final int START_SUCCESS = 0;
    private static final int START_INTENT_NOT_RESOLVED = -91;
    private static final int START_CLASS_NOT_FOUND = -92;
    // PackageManager permission-request @SystemApi strings (Android 15).
    private static final String ACTION_REQUEST_PERMISSIONS = "android.content.pm.action.REQUEST_PERMISSIONS";
    private static final String EXTRA_REQUEST_PERMISSIONS_NAMES =
            "android.content.pm.extra.REQUEST_PERMISSIONS_NAMES";
    private static final String EXTRA_REQUEST_PERMISSIONS_RESULTS =
            "android.content.pm.extra.REQUEST_PERMISSIONS_RESULTS";
    // Activity.REQUEST_PERMISSIONS_WHO_PREFIX alone: the Activity's own request.
    private static final String ACTIVITY_PERMISSIONS_WHO = "@android:requestPermissions:";

    private static final class Record {
        final Activity activity;
        final IBinder token;
        final Record caller;      // receives the result, or null
        final String resultWho;
        final int requestCode;
        boolean occludes = true;  // ActivityRecord.mOccludesParent
        int resultCode = Activity.RESULT_CANCELED;
        Intent resultData;
        Record(Activity activity, IBinder token, Record caller, String resultWho, int requestCode) {
            this.activity = activity; this.token = token; this.caller = caller;
            this.resultWho = resultWho; this.requestCode = requestCode;
        }
    }

    private final Map<IBinder, Integer> orientations;
    private final FrameworkDisplay displays;
    private final FrameworkPackages packages;
    private final List<Record> stack = new ArrayList<>();  // root first; main thread
    private Handler main;
    private Object thread, loaded;
    private Instrumentation instrumentation;
    private Application application;

    FrameworkActivities(Map<IBinder, Integer> orientations, FrameworkDisplay displays,
            FrameworkPackages packages) {
        this.orientations = orientations;
        this.displays = displays;
        this.packages = packages;
    }

    // Activity.attach receives the ActivityThread from performLaunchActivity;
    // Instrumentation.newActivity passes none. Activity.startActivity and
    // other client paths use mMainThread.getApplicationThread().
    static void attachThread(Activity activity, Object thread) throws Exception {
        Field field = Activity.class.getDeclaredField("mMainThread");
        field.setAccessible(true);
        field.set(activity, thread);
    }

    // The launcher Activity, registered before its onCreate: launches it
    // requests are posted and run once it has been resumed.
    synchronized void attach(Object thread, Object loaded, Instrumentation instrumentation,
            Application application, Activity root, IBinder token) {
        this.thread = thread;
        this.loaded = loaded;
        this.instrumentation = instrumentation;
        this.application = application;
        main = new Handler(Looper.getMainLooper());
        stack.add(new Record(root, token, null, null, -1));
    }

    // IActivityTaskManager.startActivity(caller, callingPackage, callingFeatureId,
    // intent, resolvedType, resultTo, resultWho, requestCode, flags,
    // profilerInfo, options). Returns a start result; the launch is posted.
    int start(Object[] a) throws Exception {
        Intent intent = new Intent((Intent) a[3]);
        IBinder resultTo = (IBinder) a[5];
        String resultWho = (String) a[6];
        int requestCode = (Integer) a[7];
        synchronized (this) {
            if (main == null) throw new UnsupportedOperationException("activity_task.startActivity");
        }
        ComponentName component = intent.getComponent();
        if (component == null && ACTION_REQUEST_PERMISSIONS.equals(intent.getAction())
                && FrameworkPackages.PERMISSION_CONTROLLER.equals(intent.getPackage())) {
            String[] requested = intent.getStringArrayExtra(EXTRA_REQUEST_PERMISSIONS_NAMES);
            String[] names = requested == null ? new String[0] : requested;
            postGrantResult(names, resultTo, resultWho, requestCode, 0);
            return START_SUCCESS;
        }
        if (component == null) {
            // Intent-filter resolution and other packages (browser, store)
            // are not provisioned: like a device without a handler.
            System.out.println("APP_ACTIVITY_UNRESOLVED " + intent.toUri(0));
            return START_INTENT_NOT_RESOLVED;
        }
        ActivityInfo info;
        try {
            info = application.getPackageManager().getActivityInfo(component, 0);
        } catch (PackageManager.NameNotFoundException missing) {
            System.out.println("APP_ACTIVITY_UNRESOLVED " + component.flattenToShortString());
            return START_CLASS_NOT_FOUND;
        }
        main.post(() -> {
            try {
                launch(info, intent, resultTo, resultWho, requestCode);
            } catch (Exception error) {
                // An exception from the app's Activity creation is fatal, as
                // in ActivityThread.
                throw new RuntimeException("Unable to start activity " + component, error);
            }
        });
        return START_SUCCESS;
    }

    // GrantPermissionsActivity: RESULT_OK with each requested name and its
    // grant. This profile has no permission UI, so a permission not granted
    // at install time stays denied, as when the user declines. Like the
    // controller's translucent Activity, the caller pauses, receives the
    // result, then resumes.
    // Activity.requestPermissions records its request (mHasCurrentPermissionsRequest)
    // only after startActivityForResult returns; on Android the result
    // arrives later from another process. A request made off the main thread
    // (Unity's) may still be returning, and an earlier result would leave the
    // flag set, cancelling every later request. Wait for the record, at most
    // about a second; fragment requests (a longer who) do not use it.
    private void postGrantResult(String[] names, IBinder resultTo, String resultWho, int requestCode,
            int attempt) {
        main.postDelayed(() -> {
            try {
                Record caller = resultTo != null && requestCode >= 0 ? find(resultTo) : null;
                if (caller != null && ACTIVITY_PERMISSIONS_WHO.equals(resultWho) && attempt < 1000) {
                    Field pending = Activity.class.getDeclaredField("mHasCurrentPermissionsRequest");
                    pending.setAccessible(true);
                    if (!pending.getBoolean(caller.activity)) {
                        postGrantResult(names, resultTo, resultWho, requestCode, attempt + 1);
                        return;
                    }
                }
                grantResult(names, caller, resultWho, requestCode);
            } catch (Exception error) {
                throw new RuntimeException("Unable to deliver permission result", error);
            }
        }, attempt == 0 ? 0 : 1);
    }

    private void grantResult(String[] names, Record caller, String resultWho, int requestCode)
            throws Exception {
        if (caller == null) return;
        int[] grants = new int[names.length];
        for (int i = 0; i < names.length; i++)
            grants[i] = packages.checkPermission(names[i], application.getApplicationInfo().uid,
                    Context.DEVICE_ID_DEFAULT);
        Intent data = new Intent(ACTION_REQUEST_PERMISSIONS)
                .putExtra(EXTRA_REQUEST_PERMISSIONS_NAMES, names)
                .putExtra(EXTRA_REQUEST_PERMISSIONS_RESULTS, grants);
        boolean resumed = caller == top() && resumedState(caller.activity);
        if (resumed) pause(caller.activity);
        method(Activity.class, "dispatchActivityResult", String.class, int.class, int.class,
                Intent.class, String.class).invoke(caller.activity, resultWho, requestCode,
                Activity.RESULT_OK, data, "bifrost");
        if (resumed) {
            resume(caller.activity);
            System.out.println("APP_ACTIVITY_RESUMED " + caller.activity.getClass().getName());
        }
    }

    private void launch(ActivityInfo info, Intent intent, IBinder resultTo, String resultWho,
            int requestCode) throws Exception {
        Record caller = resultTo != null && requestCode >= 0 ? find(resultTo) : null;
        Record below = top();
        // The caller pauses before the new Activity is created.
        if (below != null) pause(below.activity);
        Binder token = new Binder();
        orientations.put(token, info.screenOrientation);
        Context context = (Context) method(Class.forName("android.app.ContextImpl"), "createActivityContext",
                thread.getClass(), loaded.getClass(), ActivityInfo.class, IBinder.class, int.class,
                Configuration.class).invoke(null, thread, loaded, info, token, 0, displays.activityOverride());
        ClassLoader loader = (ClassLoader) method(loaded.getClass(), "getClassLoader").invoke(loaded);
        intent.setExtrasClassLoader(loader);
        Activity activity = instrumentation.newActivity(loader.loadClass(info.name), context, token,
                application, intent, info, info.loadLabel(application.getPackageManager()),
                null, null, null);
        attachThread(activity, thread);
        method(context.getClass(), "setOuterContext", Context.class).invoke(context, activity);
        if (info.getThemeResource() != 0) activity.setTheme(info.getThemeResource());
        Record record = new Record(activity, token, caller, resultWho, requestCode);
        record.occludes = occludesParent(info);
        synchronized (this) { stack.add(record); }
        System.out.println("APP_ACTIVITY_STARTING " + info.name);
        instrumentation.callActivityOnCreate(activity, null);
        if (!activity.isFinishing()) {
            method(Activity.class, "performStart", String.class).invoke(activity, "bifrost");
            instrumentation.callActivityOnPostCreate(activity, null);
        }
        if (!activity.isFinishing()) {
            resume(activity);
            show(activity);
            System.out.println("APP_ACTIVITY_RESUMED " + info.name);
            // Once the new Activity is visible, the ones it covers stop.
            for (Record revealed : ensureVisibility()) setVisible(revealed.activity, true);
        }
        // An Activity finishing during creation is torn down by update().
    }

    // ActivityRecord: an Activity occludes those beneath it unless its theme is
    // translucent or floating; one showing the wallpaper always does. Without
    // a manifest theme, the default theme is opaque.
    private boolean occludesParent(ActivityInfo info) {
        int themeId = info.getThemeResource();
        if (themeId == 0) return true;
        int[] attrs = {android.R.attr.windowIsTranslucent, android.R.attr.windowIsFloating,
                       android.R.attr.windowShowWallpaper};
        int[] sorted = attrs.clone();
        java.util.Arrays.sort(sorted);  // obtainStyledAttributes requires ascending ids
        android.content.res.Resources.Theme theme = application.getResources().newTheme();
        theme.applyStyle(themeId, true);
        android.content.res.TypedArray values = theme.obtainStyledAttributes(sorted);
        try {
            boolean translucent = values.getBoolean(java.util.Arrays.binarySearch(sorted, attrs[0]), false);
            boolean floating = values.getBoolean(java.util.Arrays.binarySearch(sorted, attrs[1]), false);
            boolean wallpaper = values.getBoolean(java.util.Arrays.binarySearch(sorted, attrs[2]), false);
            return !(translucent || floating) || wallpaper;
        } finally {
            values.recycle();
        }
    }

    // EnsureActivitiesVisibleHelper, top down: Activities stay visible until
    // one occludes its parent; everything beneath it stops and its window
    // becomes invisible. A stopped Activity uncovered again restarts; the
    // caller makes the returned ones visible after any resume, as
    // handleResumeActivity does.
    private List<Record> ensureVisibility() throws Exception {
        List<Record> records;
        synchronized (this) { records = new ArrayList<>(stack); }
        List<Record> revealed = new ArrayList<>();
        boolean covered = false;
        for (int i = records.size() - 1; i >= 0; i--) {
            Record record = records.get(i);
            if (record.activity.isFinishing()) continue;
            if (covered) {
                stop(record.activity);
            } else if (stoppedState(record.activity)) {
                method(Activity.class, "performRestart", boolean.class).invoke(record.activity, true);
                System.out.println("APP_ACTIVITY_RESTARTED " + record.activity.getClass().getName());
                revealed.add(record);
            }
            covered |= record.occludes;
        }
        return revealed;
    }

    // ActivityThread.handleStopActivity: performStop, then updateVisibility(false).
    private static void stop(Activity activity) throws Exception {
        if (stoppedState(activity)) return;
        if (resumedState(activity)) pause(activity);
        method(Activity.class, "performStop", boolean.class, String.class).invoke(activity, false, "bifrost");
        setVisible(activity, false);
        System.out.println("APP_ACTIVITY_STOPPED " + activity.getClass().getName());
    }

    // ActivityThread.updateVisibility: the server's visibility of the decor.
    private static void setVisible(Activity activity, boolean show) throws Exception {
        Field decorField = Activity.class.getDeclaredField("mDecor");
        decorField.setAccessible(true);
        View decor = (View) decorField.get(activity);
        if (decor == null) return;
        Field server = Activity.class.getDeclaredField("mVisibleFromServer");
        server.setAccessible(true);
        if (show == server.getBoolean(activity)) return;
        server.setBoolean(activity, show);
        if (!show) {
            decor.setVisibility(View.INVISIBLE);
        } else {
            Field client = Activity.class.getDeclaredField("mVisibleFromClient");
            client.setAccessible(true);
            if (client.getBoolean(activity)) method(Activity.class, "makeVisible").invoke(activity);
        }
    }

    // handleResumeActivity: the decor becomes a TYPE_BASE_APPLICATION window.
    // The launcher Activity uses the same path.
    static void show(Activity activity) throws Exception {
        if (activity.getWindow() == null) return;
        View decor = activity.getWindow().getDecorView();
        WindowManager.LayoutParams attrs = activity.getWindow().getAttributes();
        attrs.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
        Field field = Activity.class.getDeclaredField("mDecor");
        field.setAccessible(true);
        field.set(activity, decor);
        Field server = Activity.class.getDeclaredField("mVisibleFromServer");
        server.setAccessible(true);
        server.setBoolean(activity, true);
        method(Activity.class, "makeVisible").invoke(activity);
    }

    // IActivityClientController.finishActivity(token, resultCode, resultData,
    // finishTask): records the result; teardown happens on the UI loop.
    synchronized boolean finishing(IBinder token, int resultCode, Intent data) {
        Record record = find(token);
        if (record == null) return false;
        record.resultCode = resultCode;
        record.resultData = data;
        return true;
    }

    // The launcher Activity (task root); before attach, every known token is.
    synchronized boolean isRoot(IBinder token) {
        return stack.isEmpty() || stack.get(0).token == token;
    }

    // ActivityRecord.resultTo: the Activity awaiting this one's result.
    synchronized ComponentName callingActivity(IBinder token) {
        Record record = find(token);
        return record == null || record.caller == null ? null : record.caller.activity.getComponentName();
    }

    // Called from the launcher's UI loop: destroys finished Activities above
    // the root, newest first.
    void update() throws Exception {
        while (true) {
            Record done = null;
            synchronized (this) {
                for (int i = stack.size() - 1; i >= 1; i--)
                    if (stack.get(i).activity.isFinishing()) { done = stack.get(i); break; }
            }
            if (done == null) return;
            destroy(done, true);
        }
    }

    // Host window close: destroy every Activity above the root.
    void closeAll() throws Exception {
        while (true) {
            Record top;
            synchronized (this) {
                if (stack.size() <= 1) return;
                top = stack.get(stack.size() - 1);
            }
            destroy(top, false);
        }
    }

    // ActivityThread.performPauseActivity, then the caller's result and
    // resume, then performStopActivity and handleDestroyActivity.
    private void destroy(Record record, boolean resumeBelow) throws Exception {
        Activity activity = record.activity;
        Record below;
        boolean wasTop;
        synchronized (this) {
            wasTop = top() == record;
            stack.remove(record);
            below = top();
        }
        if (resumedState(activity)) pause(activity);
        // Restart what the finished Activity uncovered, deliver the result,
        // resume the new top, then show the uncovered windows: a stopped
        // caller sees onRestart/onStart, onActivityResult, then onResume.
        List<Record> revealed = resumeBelow ? ensureVisibility() : new ArrayList<>();
        if (record.caller != null && stackContains(record.caller)) {
            method(Activity.class, "dispatchActivityResult", String.class, int.class, int.class,
                    Intent.class, String.class).invoke(record.caller.activity, record.resultWho,
                    record.requestCode, record.resultCode, record.resultData, "bifrost");
        }
        if (wasTop && resumeBelow && below != null) {
            resume(below.activity);
            System.out.println("APP_ACTIVITY_RESUMED " + below.activity.getClass().getName());
        }
        for (Record uncovered : revealed) setVisible(uncovered.activity, true);
        if (!stoppedState(activity))
            method(Activity.class, "performStop", boolean.class, String.class).invoke(activity, false, "bifrost");
        method(Activity.class, "performDestroy").invoke(activity);
        removeWindow(activity);
        method(activity.getBaseContext().getClass(), "scheduleFinalCleanup", String.class, String.class)
                .invoke(activity.getBaseContext(), activity.getClass().getName(), "Activity");
        orientations.remove(record.token);
        System.out.println("APP_ACTIVITY_DESTROYED " + activity.getClass().getName());
    }

    // handleDestroyActivity: an added decor window is removed after onDestroy.
    static void removeWindow(Activity activity) throws Exception {
        Field added = Activity.class.getDeclaredField("mWindowAdded");
        added.setAccessible(true);
        if (added.getBoolean(activity) && activity.getWindow() != null)
            activity.getWindowManager().removeViewImmediate(activity.getWindow().getDecorView());
    }

    static boolean resumedState(Activity activity) throws Exception {
        return (Boolean) method(Activity.class, "isResumed").invoke(activity);
    }

    private static boolean stoppedState(Activity activity) throws Exception {
        Field stopped = Activity.class.getDeclaredField("mStopped");
        stopped.setAccessible(true);
        return stopped.getBoolean(activity);
    }

    private synchronized boolean stackContains(Record record) {
        return stack.contains(record);
    }

    private synchronized Record top() {
        return stack.isEmpty() ? null : stack.get(stack.size() - 1);
    }

    synchronized boolean contains(IBinder token) { return find(token) != null; }

    synchronized void result(IBinder token, String who, int requestCode, int code, Intent intent) {
        Record record = find(token);
        if (record == null || record.activity.isFinishing()) return;
        Intent data = intent == null ? null : new Intent(intent);
        main.post(() -> {
            try {
                method(Activity.class, "dispatchActivityResult", String.class, int.class, int.class,
                        Intent.class, String.class).invoke(record.activity, who, requestCode, code, data, "pending-intent");
            } catch (Exception error) { throw new RuntimeException("PendingIntent Activity result", error); }
        });
    }

    private synchronized Record find(IBinder token) {
        for (Record record : stack) if (record.token == token) return record;
        return null;
    }

    private static Method method(Class<?> type, String name, Class<?>... types) throws Exception {
        for (Class<?> c = type; c != null; c = c.getSuperclass()) {
            try {
                Method method = c.getDeclaredMethod(name, types);
                method.setAccessible(true);
                return method;
            } catch (NoSuchMethodException next) {
                // Inherited: continue with the superclass.
            }
        }
        throw new NoSuchMethodException(type.getName() + "." + name);
    }

    private static void pause(Activity activity) throws Exception {
        method(Activity.class, "performPause").invoke(activity);
        System.out.println("APP_ACTIVITY_PAUSED " + activity.getClass().getName());
    }

    private static void resume(Activity activity) throws Exception {
        method(Activity.class, "performResume", boolean.class, String.class).invoke(activity, false, "bifrost");
    }
}
