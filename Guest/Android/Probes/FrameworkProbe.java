// FrameworkProbe — milestone-1 check that app_process64 starts the real
// Android framework under Bifrost.
//
// Run as: app_process64 -Djava.class.path=<dex> /system/bin FrameworkProbe
// Framework classes are reached by reflection so the probe compiles without
// Android SDK stubs. Prints "FRAMEWORK_OK" when every step passes.
import java.lang.reflect.Method;

public final class FrameworkProbe {
    private static int failures = 0;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "OK   " : "FAIL ") + what);
        if (!ok) failures++;
    }

    public static void main(String[] args) throws Exception {
        Class<?> build = Class.forName("android.os.Build$VERSION");
        int sdk = build.getField("SDK_INT").getInt(null);
        check(sdk == 35, "Build.VERSION.SDK_INT == 35 (got " + sdk + ")");

        Class<?> log = Class.forName("android.util.Log");
        Method i = log.getMethod("i", String.class, String.class);
        check(((Integer) i.invoke(null, "FrameworkProbe", "hello from the framework")) > 0,
              "android.util.Log.i writes through liblog");

        // A main Looper and a Handler message: the event loop every
        // Activity runs on.
        Class<?> looper = Class.forName("android.os.Looper");
        looper.getMethod("prepareMainLooper").invoke(null);
        Object main = looper.getMethod("getMainLooper").invoke(null);
        Class<?> handlerClass = Class.forName("android.os.Handler");
        Object handler = handlerClass.getConstructor(looper).newInstance(main);
        Class<?> process = Class.forName("android.os.Process");
        int pid = (Integer) process.getMethod("myPid").invoke(null);
        check(pid > 0, "android.os.Process.myPid() > 0 (got " + pid + ")");

        Runnable task = () -> {
            check(true, "Handler message ran on the main Looper");
            // Android's main MessageQueue cannot quit. Finish the process
            // from its dispatched callback instead of invoking Looper.quit().
            System.out.println(failures == 0 ? "FRAMEWORK_OK" : "FRAMEWORK_FAIL " + failures);
            System.exit(failures == 0 ? 0 : 1);
        };
        check((Boolean) handlerClass.getMethod("post", Runnable.class).invoke(handler, task),
              "Handler.post accepted the message");
        looper.getMethod("loop").invoke(null);
        throw new IllegalStateException("Main Looper returned before the probe completed");
    }
}
