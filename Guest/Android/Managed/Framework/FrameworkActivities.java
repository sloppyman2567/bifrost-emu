// Activities the app starts above its launcher Activity (full-screen ads,
// sign-in and web pages). IActivityTaskManager.startActivity arrives here;
// the launch follows ActivityThread.performLaunchActivity and
// handleResumeActivity on the main thread. The new Activity's window is added
// through WindowManager, so FrameworkWindows lays it out, draws it in software
// and routes input to it; the Activity beneath is paused, as under a
// translucent Activity, and keeps its surface. Finishing resumes the caller
// and delivers any requested result.
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

    private static final class Record {
        final Activity activity;
        final IBinder token;
        final Record caller;      // receives the result, or null
        final String resultWho;
        final int requestCode;
        int resultCode = Activity.RESULT_CANCELED;
        Intent resultData;
        Record(Activity activity, IBinder token, Record caller, String resultWho, int requestCode) {
            this.activity = activity; this.token = token; this.caller = caller;
            this.resultWho = resultWho; this.requestCode = requestCode;
        }
    }

    private final Map<IBinder, Integer> orientations;
    private final FrameworkDisplay displays;
    private final List<Record> stack = new ArrayList<>();  // root first; main thread
    private Handler main;
    private Object thread, loaded;
    private Instrumentation instrumentation;
    private Application application;

    FrameworkActivities(Map<IBinder, Integer> orientations, FrameworkDisplay displays) {
        this.orientations = orientations;
        this.displays = displays;
    }

    // Activity.attach receives the ActivityThread from performLaunchActivity;
    // Instrumentation.newActivity passes none. Activity.startActivity and
    // other client paths use mMainThread.getApplicationThread().
    static void attachThread(Activity activity, Object thread) throws Exception {
        Field field = Activity.class.getDeclaredField("mMainThread");
        field.setAccessible(true);
        field.set(activity, thread);
    }

    // The launcher Activity, already resumed with its SurfaceView content.
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
        }
        // An Activity finishing during creation is torn down by update().
    }

    // handleResumeActivity: the decor becomes a TYPE_BASE_APPLICATION window.
    private static void show(Activity activity) throws Exception {
        if (activity.getWindow() == null) return;
        View decor = activity.getWindow().getDecorView();
        WindowManager.LayoutParams attrs = activity.getWindow().getAttributes();
        attrs.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
        Field field = Activity.class.getDeclaredField("mDecor");
        field.setAccessible(true);
        field.set(activity, decor);
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
        if (record.caller != null && stackContains(record.caller)) {
            method(Activity.class, "dispatchActivityResult", String.class, int.class, int.class,
                    Intent.class, String.class).invoke(record.caller.activity, record.resultWho,
                    record.requestCode, record.resultCode, record.resultData, "bifrost");
        }
        if (wasTop && resumeBelow && below != null) {
            resume(below.activity);
            System.out.println("APP_ACTIVITY_RESUMED " + below.activity.getClass().getName());
        }
        if (!stoppedState(activity))
            method(Activity.class, "performStop", boolean.class, String.class).invoke(activity, false, "bifrost");
        method(Activity.class, "performDestroy").invoke(activity);
        Field added = Activity.class.getDeclaredField("mWindowAdded");
        added.setAccessible(true);
        if (added.getBoolean(activity) && activity.getWindow() != null)
            activity.getWindowManager().removeViewImmediate(activity.getWindow().getDecorView());
        method(activity.getBaseContext().getClass(), "scheduleFinalCleanup", String.class, String.class)
                .invoke(activity.getBaseContext(), activity.getClass().getName(), "Activity");
        orientations.remove(record.token);
        System.out.println("APP_ACTIVITY_DESTROYED " + activity.getClass().getName());
    }

    private static boolean resumedState(Activity activity) throws Exception {
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
