// ActivityProbe — creates a real Android Activity through onCreate under
// Bifrost, with the framework's own ActivityThread and Instrumentation.
//
// Run with the Binder registry: app_process64 /system/bin ActivityProbe
// (scripts/test_android_framework.py --services --main-class ActivityProbe).
// Bifrost runs no system_server, so the system services the framework calls
// are in-process Java stubs placed in ServiceManager's cache. Each stub
// answers only the calls Activity creation makes and prints them as
// LOCAL_SERVICE lines; any other call throws, so new framework needs fail
// loudly instead of receiving invented defaults. Prints "FRAMEWORK_OK" once
// onCreate has run and the window's decor view exists. Compile against the
// API-35 android.jar; hidden framework APIs are reached by reflection.
//
// App mode loads an installed package instead of the built-in Activity:
//   ActivityProbe --app <package> <code dir> <launcher class> [<split>=<apk>]...
// It follows ActivityThread.handleBindApplication and performLaunchActivity:
// parse the base APK, build the LoadedApk (class loader and resources over
// the base and split APKs), create and start the Application, then create
// the launcher Activity and run its onCreate. APP_* markers record each step.
import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.Configuration;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Process;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Arrays;
import java.util.concurrent.ConcurrentHashMap;
import java.util.Map;

public final class ActivityProbe {
    private static boolean created;
    // The package installed by app mode; the only package that exists here.
    private static volatile PackageInfo installed;
    // ActivityRecord's requested orientation is initialized from its manifest.
    private static final Map<IBinder, Integer> orientations = new ConcurrentHashMap<>();

    public static final class ProbeActivity extends Activity {
        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            created = true;
            System.out.println("ACTIVITY_CREATED");
        }
    }

    interface Call {
        Object invoke(String method, Object[] arguments) throws Exception;
    }

    // Registers a local implementation of `iface` under `name`.
    // ServiceManager.getService consults sCache before any Binder lookup.
    @SuppressWarnings("unchecked")
    private static Object service(String name, String iface, Call call) throws Exception {
        Class<?> type = Class.forName(iface);
        Binder binder = new Binder();
        Object proxy = Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type},
                (self, method, arguments) -> {
                    if (method.getName().equals("asBinder")) return binder;
                    if (method.getName().equals("toString")) return "Bifrost:" + name;
                    System.out.println("LOCAL_SERVICE " + name + "." + method.getName());
                    return call.invoke(method.getName(), arguments);
                });
        binder.attachInterface((IInterface) proxy, iface);
        Field cache = Class.forName("android.os.ServiceManager").getDeclaredField("sCache");
        cache.setAccessible(true);
        ((Map<String, IBinder>) cache.get(null)).put(name, binder);
        return proxy;
    }

    private static UnsupportedOperationException unsupported(String service, String method) {
        return new UnsupportedOperationException(service + "." + method);
    }

    // One 480x800, 160 dpi, 60 Hz display with id 0.
    private static Object displayInfo() throws Exception {
        Class<?> type = Class.forName("android.view.DisplayInfo");
        Object info = type.getConstructor().newInstance();
        type.getField("name").set(info, "Bifrost");
        type.getField("uniqueId").set(info, "bifrost:0");
        for (String field : new String[] {"appWidth", "logicalWidth",
                "smallestNominalAppWidth", "largestNominalAppWidth"})
            type.getField(field).setInt(info, 480);
        for (String field : new String[] {"appHeight", "logicalHeight",
                "smallestNominalAppHeight", "largestNominalAppHeight"})
            type.getField(field).setInt(info, 800);
        type.getField("logicalDensityDpi").setInt(info, 160);
        type.getField("physicalXDpi").setFloat(info, 160);
        type.getField("physicalYDpi").setFloat(info, 160);
        type.getField("state").setInt(info, 2);  // Display.STATE_ON
        type.getField("type").setInt(info, 1);   // Display.TYPE_INTERNAL
        Class<?> modeType = Class.forName("android.view.Display$Mode");
        Object mode = modeType.getConstructor(int.class, int.class, int.class, float.class)
                .newInstance(1, 480, 800, 60f);
        Object modes = Array.newInstance(modeType, 1);
        Array.set(modes, 0, mode);
        type.getField("supportedModes").set(info, modes);
        type.getField("appsSupportedModes").set(info, modes);
        type.getField("modeId").setInt(info, 1);
        type.getField("defaultModeId").setInt(info, 1);
        return info;
    }

    private static void installServices() throws Exception {
        Object display = displayInfo();
        service("display", "android.hardware.display.IDisplayManager", (method, a) -> {
            switch (method) {
                case "getDisplayInfo": return ((Integer) a[0]) == 0 ? display : null;
                case "getPreferredWideGamutColorSpaceId": return 0;
                // What system_server reports without hardware-composer data.
                case "getOverlaySupport":
                    return Class.forName("android.hardware.OverlayProperties")
                            .getMethod("getDefault").invoke(null);
                default: throw unsupported("display", method);
            }
        });
        service("package", "android.content.pm.IPackageManager", (method, a) -> {
            switch (method) {
                case "hasSystemFeature": return false;
                case "notifyPackageUse": return null;  // one-way usage report
                // (name, flags, user): null means "not installed", which the
                // client turns into NameNotFoundException.
                case "getPackageInfo":
                    return installed != null && installed.packageName.equals(a[0])
                            ? installed : null;
                case "getApplicationInfo":
                    return installed != null && installed.packageName.equals(a[0])
                            ? installed.applicationInfo : null;
                default: throw unsupported("package", method);
            }
        });
        service("window", "android.view.IWindowManager", (method, a) -> {
            switch (method) {
                case "hasNavigationBar": return false;
                case "getCurrentAnimatorScale": return 1f;
                default: throw unsupported("window", method);
            }
        });
        Object client = service("bifrost.activity_client",
                "android.app.IActivityClientController", (method, a) -> {
                    switch (method) {
                        case "setTaskDescription": return null;
                        case "getRequestedOrientation":
                            return orientations.getOrDefault((IBinder) a[0],
                                    ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
                        case "setRequestedOrientation":
                            if (orientations.containsKey((IBinder) a[0]))
                                orientations.put((IBinder) a[0], (Integer) a[1]);
                            return null;
                        default: throw unsupported("activity_client", method);
                    }
                });
        service("activity_task", "android.app.IActivityTaskManager", (method, a) -> {
            if (method.equals("getActivityClientController")) return client;
            throw unsupported("activity_task", method);
        });
        // Settings.Global/Secure/System reads go through a content provider
        // call; an absent value makes the framework use its default.
        Object provider = service("bifrost.settings", "android.content.IContentProvider",
                (method, a) -> {
                    if (method.equals("call") && ((String) a[2]).startsWith("GET_")) {
                        Bundle result = new Bundle();
                        result.putString("value", null);
                        return result;
                    }
                    throw unsupported("settings", method);
                });
        ProviderInfo providerInfo = new ProviderInfo();
        providerInfo.authority = "settings";
        providerInfo.name = "BifrostSettings";
        providerInfo.packageName = "android";
        providerInfo.applicationInfo = new ApplicationInfo();
        providerInfo.applicationInfo.packageName = "android";
        Class<?> holderType = Class.forName("android.app.ContentProviderHolder");
        Object holder = holderType.getConstructor(ProviderInfo.class).newInstance(providerInfo);
        holderType.getField("provider").set(holder, provider);
        holderType.getField("noReleaseNeeded").setBoolean(holder, true);
        service("activity", "android.app.IActivityManager", (method, a) -> {
            if (method.equals("getContentProvider") && Arrays.asList(a).contains("settings"))
                return holder;
            // Log.wtf report; false means "do not kill this process".
            if (method.equals("handleApplicationWtf")) return false;
            throw unsupported("activity", method);
        });
    }

    private static Object call(Object target, String name, Class<?>[] types, Object... arguments)
            throws Exception {
        Class<?> type = target instanceof Class ? (Class<?>) target : target.getClass();
        Method method = null;
        for (Class<?> c = type; method == null && c != null; c = c.getSuperclass()) {
            try {
                method = c.getDeclaredMethod(name, types);
            } catch (NoSuchMethodException next) {
                // Keep searching superclasses.
            }
        }
        if (method == null) throw new NoSuchMethodException(type.getName() + "." + name);
        method.setAccessible(true);
        return method.invoke(target instanceof Class ? null : target, arguments);
    }

    private static void setField(Object target, String name, Object value) throws Exception {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    // args: --app <package> <code dir> <launcher class> [<split>=<apk>]...
    private static void runApp(Object thread, Context system, String[] args) throws Exception {
        String packageName = args[1], code = args[2], launcher = args[3];
        PackageInfo packageInfo = system.getPackageManager().getPackageArchiveInfo(
                code + "/base.apk", PackageManager.GET_ACTIVITIES | PackageManager.GET_META_DATA);
        if (packageInfo == null || !packageName.equals(packageInfo.packageName))
            throw new IllegalStateException("Cannot parse " + code + "/base.apk");
        ApplicationInfo app = packageInfo.applicationInfo;
        ActivityInfo activity = null;
        for (ActivityInfo candidate : packageInfo.activities)
            if (candidate.name.equals(launcher)) activity = candidate;
        if (activity == null) throw new IllegalStateException("No activity " + launcher);
        String[] splitNames = new String[args.length - 4];
        String[] splitPaths = new String[args.length - 4];
        for (int i = 4; i < args.length; i++) {
            String[] split = args[i].split("=", 2);
            splitNames[i - 4] = split[0];
            splitPaths[i - 4] = code + "/" + split[1];
        }
        // What PackageManagerService records for an installed arm64 package.
        app.sourceDir = app.publicSourceDir = code + "/base.apk";
        app.splitNames = splitNames;
        app.splitSourceDirs = app.splitPublicSourceDirs = splitPaths;
        app.nativeLibraryDir = code + "/lib/arm64";
        setField(app, "nativeLibraryRootDir", code + "/lib");
        setField(app, "primaryCpuAbi", "arm64-v8a");
        app.dataDir = "/data/user/0/" + packageName;
        setField(app, "credentialProtectedDataDir", app.dataDir);
        setField(app, "deviceProtectedDataDir", "/data/user_de/0/" + packageName);
        app.uid = Process.myUid();
        // The installed ApplicationInfo marks a base APK with classes.dex as
        // code; the archive-parsed one here does not, and LoadedApk then builds
        // a class loader without the APK's dex files.
        app.flags |= ApplicationInfo.FLAG_HAS_CODE;
        installed = packageInfo;
        System.out.println("APP_PARSED " + app.className + " " + activity.name);

        Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
        Object loaded = call(thread, "getPackageInfoNoCheck",
                new Class<?>[] {ApplicationInfo.class, compatibility}, app,
                compatibility.getField("DEFAULT_COMPATIBILITY_INFO").get(null));
        ClassLoader loader = (ClassLoader) call(loaded, "getClassLoader", new Class<?>[0]);
        Class<?> launcherClass = loader.loadClass(launcher);
        System.out.println("APP_CLASSLOADER_OK " + launcherClass.getName());

        Instrumentation instrumentation =
                (Instrumentation) call(thread, "getInstrumentation", new Class<?>[0]);
        Application application = (Application) call(loaded, "makeApplicationInner",
                new Class<?>[] {boolean.class, Instrumentation.class}, false, null);
        setField(thread, "mInitialApplication", application);
        System.out.println("APP_APPLICATION_CREATED " + application.getClass().getName());
        instrumentation.callApplicationOnCreate(application);
        System.out.println("APP_APPLICATION_STARTED");

        Binder token = new Binder();
        orientations.put(token, activity.screenOrientation);
        Context activityContext = (Context) call(Class.forName("android.app.ContextImpl"),
                "createActivityContext", new Class<?>[] {thread.getClass(), loaded.getClass(),
                        ActivityInfo.class, IBinder.class, int.class, Configuration.class},
                thread, loaded, activity, token, 0, null);
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setClassName(packageName, launcher);
        Activity instance = instrumentation.newActivity(launcherClass, activityContext, token,
                application, intent, activity, activity.loadLabel(system.getPackageManager()),
                null, null, null);
        call(activityContext, "setOuterContext", new Class<?>[] {Context.class}, instance);
        if (activity.getThemeResource() != 0) instance.setTheme(activity.getThemeResource());
        if (instance.getRequestedOrientation() != activity.screenOrientation)
            throw new IllegalStateException("Activity orientation differs from its manifest");
        System.out.println("APP_ACTIVITY_ATTACHED");
        instrumentation.callActivityOnCreate(instance, null);
        System.out.println("APP_ACTIVITY_CREATED");
        System.out.println("FRAMEWORK_OK");
        System.exit(0);
    }

    private static void run(String[] args) throws Exception {
        Looper.prepareMainLooper();
        installServices();
        System.out.println("ACTIVITY_BOOTSTRAP_BEGIN");
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        Object thread = threadType.getMethod("systemMain").invoke(null);
        Context context = (Context) threadType.getMethod("getSystemContext").invoke(thread);
        System.out.println("ACTIVITY_CONTEXT_OK " + context);
        // ActivityThread.handleBindApplication's fallback when no serialized
        // font map arrives (lazy Typeface initialization). This probe has
        // no bindApplication, and a TextView needs the default typefaces.
        Class.forName("android.graphics.Typeface")
                .getMethod("loadPreinstalledSystemFontMap").invoke(null);
        System.out.println("ACTIVITY_FONTS_OK");
        if (args.length >= 4 && args[0].equals("--app")) {
            runApp(thread, context, args);
            return;
        }
        if (args.length != 0) throw new IllegalArgumentException("unexpected arguments");

        ActivityInfo info = new ActivityInfo();
        info.applicationInfo = context.getApplicationInfo();
        info.packageName = "org.bifrost.activity";
        info.name = ProbeActivity.class.getName();
        Intent intent = new Intent().setClassName(info.packageName, info.name);
        Instrumentation instrumentation = new Instrumentation();
        Binder token = new Binder();
        orientations.put(token, info.screenOrientation);
        Activity activity = instrumentation.newActivity(ProbeActivity.class, context,
                token, new Application(), intent, info, "Bifrost Activity",
                null, null, null);
        if (activity.getRequestedOrientation() != info.screenOrientation)
            throw new IllegalStateException("initial Activity orientation differs from its manifest");
        activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        if (activity.getRequestedOrientation() != ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)
            throw new IllegalStateException("requested Activity orientation was not retained");
        activity.setRequestedOrientation(info.screenOrientation);
        instrumentation.callActivityOnCreate(activity, null);
        if (!created || activity.getWindow() == null ||
                activity.getWindow().peekDecorView() == null)
            throw new IllegalStateException("onCreate did not complete with a decor view");
        System.out.println("FRAMEWORK_OK");
        System.exit(0);
    }

    public static void main(String[] args) {
        try {
            run(args);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
