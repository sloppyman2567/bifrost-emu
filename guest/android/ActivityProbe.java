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
import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfo;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.Arrays;
import java.util.Map;

public final class ActivityProbe {
    private static boolean created;

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
            if (method.equals("hasSystemFeature")) return false;
            throw unsupported("package", method);
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
                    if (method.equals("setTaskDescription")) return null;
                    throw unsupported("activity_client", method);
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
            throw unsupported("activity", method);
        });
    }

    private static void run() throws Exception {
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

        ActivityInfo info = new ActivityInfo();
        info.applicationInfo = context.getApplicationInfo();
        info.packageName = "org.bifrost.activity";
        info.name = ProbeActivity.class.getName();
        Intent intent = new Intent().setClassName(info.packageName, info.name);
        Instrumentation instrumentation = new Instrumentation();
        Activity activity = instrumentation.newActivity(ProbeActivity.class, context,
                new Binder(), new Application(), intent, info, "Bifrost Activity",
                null, null, null);
        instrumentation.callActivityOnCreate(activity, null);
        if (!created || activity.getWindow() == null ||
                activity.getWindow().peekDecorView() == null)
            throw new IllegalStateException("onCreate did not complete with a decor view");
        System.out.println("FRAMEWORK_OK");
        System.exit(0);
    }

    public static void main(String[] args) {
        try {
            run();
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
