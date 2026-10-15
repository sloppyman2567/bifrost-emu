// Real DisplayManager/DisplayManagerGlobal clients; resize drives the production callback path.
import android.content.Context;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

public final class FrameworkDisplayProbe {
    private static int checks;
    private static Handler main;
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }
    private static void await(BooleanSupplier condition, Runnable next) {
        long deadline = SystemClock.uptimeMillis() + 15000;
        main.post(new Runnable() {
            public void run() {
                if (condition.getAsBoolean()) { next.run(); return; }
                if (SystemClock.uptimeMillis() >= deadline) throw new AssertionError("display callback timed out");
                main.postDelayed(this, 20);
            }
        });
    }
    private static void wifiAndRoutes(Context context, FrameworkPackages packages, Object display, Class<?> iface) throws Exception {
        Object status = iface.getMethod("getWifiDisplayStatus").invoke(display);
        Class<?> type = status.getClass();
        check((Integer) type.getMethod("getFeatureState").invoke(status) == 0,
                "wireless display incorrectly advertised as available");
        check((Integer) type.getMethod("getScanState").invoke(status) == 0
                && (Integer) type.getMethod("getActiveDisplayState").invoke(status) == 0,
                "wireless display scan/connection active without adapter");
        check(type.getMethod("getActiveDisplay").invoke(status) == null
                && java.lang.reflect.Array.getLength(type.getMethod("getDisplays").invoke(status)) == 0,
                "wireless display devices fabricated");
        check(iface.getMethod("getWifiDisplayStatus").invoke(display) != status,
                "wireless display reply aliases prior snapshot");
        iface.getMethod("disconnectWifiDisplay").invoke(display); // intentionally unprivileged
        for (String method : new String[] {"startWifiDisplayScan", "stopWifiDisplayScan",
                "pauseWifiDisplay", "resumeWifiDisplay", "connectWifiDisplay", "renameWifiDisplay",
                "forgetWifiDisplay"}) {
            Class<?>[] parameters = method.equals("renameWifiDisplay") ? new Class<?>[] {String.class, String.class}
                    : method.equals("connectWifiDisplay") || method.equals("forgetWifiDisplay")
                    ? new Class<?>[] {String.class} : new Class<?>[0];
            Object[] arguments = parameters.length == 2 ? new Object[] {"00:11:22:33:44:55", "alias"}
                    : parameters.length == 1 ? new Object[] {"00:11:22:33:44:55"} : new Object[0];
            try {
                iface.getMethod(method, parameters).invoke(display, arguments);
                throw new AssertionError("unprivileged wifi display control accepted: " + method);
            } catch (java.lang.reflect.InvocationTargetException expected) {
                check(expected.getCause() instanceof SecurityException, "wifi control permission incorrect: " + method);
            }
        }
        try {
            iface.getMethod("connectWifiDisplay", String.class).invoke(display, new Object[] {null});
            throw new AssertionError("null wireless display address accepted");
        } catch (java.lang.reflect.InvocationTargetException expected) {
            check(expected.getCause() instanceof IllegalArgumentException, "wifi address validation order incorrect");
        }
        // This reproduces Unity's complete constructor path, including audio
        // route observation and registration with the legacy router service.
        android.media.MediaRouter router = (android.media.MediaRouter) context.getSystemService(Context.MEDIA_ROUTER_SERVICE);
        check(router != null && router.getRouteCount() == 1, "local media router failed to initialize");
        check(router.getDefaultRoute() == router.getSelectedRoute(android.media.MediaRouter.ROUTE_TYPE_LIVE_AUDIO),
                "media router failed to select local audio");
        check(router.getDefaultRoute().getPlaybackType() == android.media.MediaRouter.RouteInfo.PLAYBACK_TYPE_LOCAL,
                "default route advertised as remote");
        android.media.MediaRouter.Callback callback = new android.media.MediaRouter.SimpleCallback() {};
        router.addCallback(android.media.MediaRouter.ROUTE_TYPE_LIVE_AUDIO, callback);
        router.removeCallback(callback);

        FrameworkServices services = new FrameworkServices();
        Object client = services.local("router-client", "android.media.IMediaRouterClient", (method, args) -> null);
        Object trustedClient = services.local("router-trusted-client", "android.media.IMediaRouterClient", (method, args) -> null);
        FrameworkMediaRouter service = new FrameworkMediaRouter(packages);
        check(service.invoke("getState", new Object[] {client}) == null, "unregistered router client has state");
        try {
            service.invoke("registerClientAsUser", new Object[] {client, "foreign.package", 0});
            throw new AssertionError("foreign router package registered");
        } catch (SecurityException expected) { checks++; }
        try {
            service.invoke("registerClientAsUser", new Object[] {client, context.getPackageName(), 1});
            throw new AssertionError("foreign router user registered");
        } catch (SecurityException expected) { checks++; }
        service.invoke("registerClientAsUser", new Object[] {client, context.getPackageName(), 0});
        check(service.invoke("getState", new Object[] {client}) == null, "untrusted client can enumerate remote routes");
        service.invoke("setDiscoveryRequest", new Object[] {client, 7, true});
        service.invoke("setSelectedRoute", new Object[] {client, "DEFAULT_ROUTE", false});
        try {
            service.invoke("requestSetVolume", new Object[] {client, null, 1});
            throw new AssertionError("null remote route accepted");
        } catch (NullPointerException expected) { checks++; }
        try {
            service.invoke("getSystemSessionInfo", new Object[0]);
            throw new AssertionError("unsupported Router2 API silently accepted");
        } catch (UnsupportedOperationException expected) { checks++; }
        // Grant the platform permission in the fixture only, to cover the
        // system client's no-adapter branch as well as the normal app denial.
        Field grants = FrameworkPackages.class.getDeclaredField("grantedPermissions");
        grants.setAccessible(true);
        @SuppressWarnings("unchecked") java.util.Set<String> permissions = (java.util.Set<String>) grants.get(packages);
        permissions.add("android.permission.CONFIGURE_WIFI_DISPLAY");
        try {
            for (String method : new String[] {"startWifiDisplayScan", "startWifiDisplayScan",
                    "stopWifiDisplayScan", "stopWifiDisplayScan", "pauseWifiDisplay", "resumeWifiDisplay"})
                iface.getMethod(method).invoke(display);
            Object noAdapter = iface.getMethod("getWifiDisplayStatus").invoke(display);
            check((Integer) type.getMethod("getScanState").invoke(noAdapter) == 0,
                    "scan fabricated wireless hardware");
            iface.getMethod("connectWifiDisplay", String.class).invoke(display, "00:11:22:33:44:55");
            iface.getMethod("renameWifiDisplay", String.class, String.class).invoke(display, "00:11:22:33:44:55", null);
            iface.getMethod("forgetWifiDisplay", String.class).invoke(display, "00:11:22:33:44:55");
            service.invoke("registerClientAsUser", new Object[] {trustedClient, context.getPackageName(), -2});
            Object state = service.invoke("getState", new Object[] {trustedClient});
            @SuppressWarnings("unchecked") java.util.List<Object> routes =
                    (java.util.List<Object>) state.getClass().getField("routes").get(state);
            check(routes.isEmpty(), "remote provider routes fabricated");
            routes.add(null);
            Object fresh = service.invoke("getState", new Object[] {trustedClient});
            check(((java.util.List<?>) fresh.getClass().getField("routes").get(fresh)).isEmpty(),
                    "router reply aliases client state");
            service.invoke("registerClientAsUser", new Object[] {client, context.getPackageName(), 0});
            check(service.invoke("getState", new Object[] {client}) == null, "duplicate registration changed client trust");
        } finally { permissions.remove("android.permission.CONFIGURE_WIFI_DISPLAY"); }
        service.invoke("unregisterClient", new Object[] {trustedClient});
        service.invoke("unregisterClient", new Object[] {trustedClient});
        check(service.invoke("getState", new Object[] {trustedClient}) == null, "unregistered client retained state");
        service.invoke("unregisterClient", new Object[] {client});
    }
    private static void run() throws Exception {
        Looper.prepareMainLooper();
        main = new Handler(Looper.getMainLooper());
        Method install = ManagedLauncher.class.getDeclaredMethod("installServices", boolean.class);
        install.setAccessible(true);
        install.invoke(null, false);  // the session process, not a service child
        Field field = ManagedLauncher.class.getDeclaredField("displays");
        field.setAccessible(true);
        FrameworkDisplay service = (FrameworkDisplay) field.get(null);
        Object thread = Class.forName("android.app.ActivityThread").getMethod("systemMain").invoke(null);
        Context context = (Context) thread.getClass().getMethod("getSystemContext").invoke(thread);
        Field packageField = ManagedLauncher.class.getDeclaredField("packages");
        packageField.setAccessible(true);
        FrameworkPackages packages = (FrameworkPackages) packageField.get(null);
        android.content.pm.PackageInfo pkg = new android.content.pm.PackageInfo();
        pkg.packageName = context.getPackageName();
        pkg.applicationInfo = new android.content.pm.ApplicationInfo();
        pkg.applicationInfo.packageName = pkg.packageName;
        pkg.applicationInfo.uid = android.os.Process.myUid();
        pkg.applicationInfo.enabled = true;
        pkg.applicationInfo.targetSdkVersion = 35;
        packages.install(context, pkg);
        service.attachApplicationThread(thread);
        DisplayManager manager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        Display[] displays = manager.getDisplays();
        check(displays.length == 1 && displays[0].getDisplayId() == 0, "default display enumeration incorrect");
        check(manager.getDisplay(1234) == null, "unknown display exists");
        check(manager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION).length == 0,
                "internal display advertised as presentation display");
        check(displays[0].getRefreshRate() == 60f && displays[0].getState() == Display.STATE_ON,
                "display mode/state incorrect");
        Point size = new Point();
        displays[0].getRealSize(size);
        check(size.x == 480 && size.y == 800, "initial size incorrect");
        Context windowContext = context.createWindowContext(displays[0],
                android.view.WindowManager.LayoutParams.TYPE_APPLICATION, null);
        check(windowContext.getResources().getConfiguration().screenWidthDp == 480,
                "window context initial configuration incorrect");
        android.view.WindowManager windows = windowContext.getSystemService(android.view.WindowManager.class);
        android.view.WindowMetrics metrics = windows.getCurrentWindowMetrics();
        check(metrics.getBounds().equals(new android.graphics.Rect(0, 0, 480, 800)),
                "window metrics bounds incorrect");
        check(metrics.getWindowInsets().getInsets(android.view.WindowInsets.Type.systemBars()
                | android.view.WindowInsets.Type.ime()).equals(android.graphics.Insets.NONE),
                "virtual window has unexpected system-bar or IME insets");
        Object insets = Class.forName("android.view.InsetsState").getConstructor().newInstance();
        try { service.windowInsets(1234, insets); throw new AssertionError("unknown display has insets"); }
        catch (android.view.WindowManager.InvalidDisplayException expected) { checks++; }
        check(service.attachWindowContext(new Object(), new android.os.Binder(), 1, 0) == null,
                "foreign process attached window context");
        Method appThread = thread.getClass().getDeclaredMethod("getApplicationThread");
        appThread.setAccessible(true);
        check(service.attachWindowContext(appThread.invoke(thread), new android.os.Binder(), 1, 1234) == null,
                "window context attached to unknown display");
        Class<?> iface = Class.forName("android.hardware.display.IDisplayManager");
        Object remote = Class.forName("android.hardware.display.IDisplayManager$Stub")
                .getMethod("asInterface", android.os.IBinder.class).invoke(null,
                        Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "display"));
        wifiAndRoutes(context, packages, remote, iface);
        Object info = iface.getMethod("getDisplayInfo", int.class).invoke(remote, 0);
        info.getClass().getField("logicalWidth").setInt(info, 1);
        Object fresh = iface.getMethod("getDisplayInfo", int.class).invoke(remote, 0);
        check(fresh.getClass().getField("logicalWidth").getInt(fresh) == 480, "display reply aliases service state");
        int[] ids = (int[]) iface.getMethod("getDisplayIds", boolean.class).invoke(remote, false);
        ids[0] = 99;
        check(((int[]) iface.getMethod("getDisplayIds", boolean.class).invoke(remote, true))[0] == 0,
                "display id reply aliases service state");
        try {
            iface.getMethod("registerCallback", Class.forName("android.hardware.display.IDisplayManagerCallback"))
                    .invoke(remote, new Object[] {null});
            throw new AssertionError("null callback accepted");
        } catch (java.lang.reflect.InvocationTargetException expected) {
            check(expected.getCause() instanceof IllegalArgumentException, "null callback exception incorrect");
        }
        HandlerThread worker = new HandlerThread("display-client");
        worker.start();
        Handler delivery = new Handler(worker.getLooper());
        AtomicInteger changed = new AtomicInteger();
        DisplayManager.DisplayListener listener = new DisplayManager.DisplayListener() {
            public void onDisplayAdded(int id) { throw new AssertionError("spurious added event"); }
            public void onDisplayRemoved(int id) { throw new AssertionError("spurious removed event"); }
            public void onDisplayChanged(int id) {
                if (id != 0 || Looper.myLooper() != worker.getLooper())
                    throw new AssertionError("display callback ID/thread incorrect");
                changed.incrementAndGet();
            }
        };
        Method register = DisplayManager.class.getMethod("registerDisplayListener",
                DisplayManager.DisplayListener.class, Handler.class, long.class);
        register.invoke(manager, listener, delivery, 1L); // only ADDED
        service.setSize(640, 480);
        main.postDelayed(() -> {
            try {
                check(changed.get() == 0, "changed event ignored listener mask");
                check(windowContext.getResources().getConfiguration().screenWidthDp == 640,
                        "window context resize incorrectly depends on display listener mask");
                manager.unregisterDisplayListener(listener);
                manager.registerDisplayListener(listener, delivery);
                service.setSize(800, 600);
                await(() -> changed.get() == 1, () -> {
                    displays[0].getRealSize(size);
                    check(size.x == 800 && size.y == 600, "callback observed stale display dimensions");
                    check(windowContext.getResources().getConfiguration().screenWidthDp == 800
                            && windowContext.getResources().getConfiguration().screenHeightDp == 600,
                            "window context configuration stale after resize");
                    check(windows.getCurrentWindowMetrics().getBounds().equals(new android.graphics.Rect(0, 0, 800, 600)),
                            "window metrics stale after resize");
                    manager.unregisterDisplayListener(listener);
                    try { windowContext.getClass().getMethod("release").invoke(windowContext); }
                    catch (Exception error) { throw new RuntimeException(error); }
                    try { service.setSize(900, 600); }
                    catch (Exception error) { throw new RuntimeException(error); }
                    main.postDelayed(() -> {
                        check(changed.get() == 1, "callback delivered after unregister");
                        check(windowContext.getResources().getConfiguration().screenWidthDp == 800,
                                "released window context still receives configuration updates");
                        worker.quitSafely();
                        System.out.println("FRAMEWORK_DISPLAY_CONTRACT_OK checks=" + checks);
                        System.out.println("FRAMEWORK_OK");
                        System.exit(0);
                    }, 150);
                });
            } catch (Throwable error) { error.printStackTrace(); System.exit(1); }
        }, 150);
        Looper.loop();
    }
    public static void main(String[] args) {
        try { run(); }
        catch (Throwable error) { error.printStackTrace(); System.exit(1); }
    }
}
