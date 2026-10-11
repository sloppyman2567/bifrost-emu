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
    private static void run() throws Exception {
        Looper.prepareMainLooper();
        main = new Handler(Looper.getMainLooper());
        Method install = ManagedLauncher.class.getDeclaredMethod("installServices");
        install.setAccessible(true);
        install.invoke(null);
        Field field = ManagedLauncher.class.getDeclaredField("displays");
        field.setAccessible(true);
        FrameworkDisplay service = (FrameworkDisplay) field.get(null);
        Object thread = Class.forName("android.app.ActivityThread").getMethod("systemMain").invoke(null);
        Context context = (Context) thread.getClass().getMethod("getSystemContext").invoke(thread);
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
