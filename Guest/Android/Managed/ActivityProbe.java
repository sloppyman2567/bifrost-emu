import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.ResultReceiver;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

// Diagnostic compatibility entry point; the managed launcher owns the runtime.
public final class ActivityProbe {
    public static void main(String[] args) {
        ManagedLauncher.probe(args);
    }

    // A native eventfd callback must run when JAVA polls its MessageQueue.
    // Polling ALooper explicitly would hide a split Java/native looper bug.
    public static final class NativeLooperProbe {
        private static native long arm();
        private static native int finish(long registration);
        private static native int nativeFrames();
        static void checkNativeFrames() {
            int result = nativeFrames();
            if (result != 1) throw new AssertionError("native Choreographer worker: " + result);
            System.out.println("ANDROID_NATIVE_CHOREOGRAPHER_OK");
        }
        private static void check(android.os.Handler handler, Runnable next) {
            long registration = arm();
            if (registration == 0) throw new AssertionError("native looper registration failed");
            handler.postDelayed(() -> {
                int result = finish(registration);
                if (result != 1) throw new AssertionError("Java/native looper callback: " + result);
                next.run();
            }, 1000);
        }
        public static void main(String[] args) {
            System.load("/data/local/tmp/libbifrost-activity.so");
            Looper.prepareMainLooper();
            android.os.Handler main = new android.os.Handler(Looper.getMainLooper());
            check(main, () -> {
                android.os.HandlerThread worker = new android.os.HandlerThread("NativeLooperOwner");
                worker.start();
                android.os.Handler owner = new android.os.Handler(worker.getLooper());
                owner.post(() -> check(owner, () -> {
                    worker.quitSafely();
                    main.post(() -> {
                        System.out.println("ANDROID_JAVA_NATIVE_LOOPER_OK");
                        System.out.println("FRAMEWORK_OK");
                        System.exit(0);
                    });
                }));
            });
            Looper.loop();
        }
    }

    // Uses the launcher's real Java display service with initially unattached
    // pthreads, as a native frame-pacing library does.
    public static final class NativeChoreographerProbe {
        public static void main(String[] args) {
            ManagedLauncher.probe(new String[] {"--native-choreographer"});
        }
    }

    public static final class EglProbe {
        private static native int check();
        public static void main(String[] args) {
            System.load("/data/local/tmp/libbifrost-activity.so");
            int line = check();
            if (line != 0) throw new AssertionError("EGL contract failed at activity_window.c:" + line);
            System.out.println("ANDROID_EGL_CONTRACT_OK");
            System.out.println("FRAMEWORK_OK");
            System.exit(0);
        }
    }

    public static final class GrallocProbe {
        static native int check();
        // A Java Parcel client independently checks the frozen V2 shape.
        // Request packed 1x1 RGB888 pixels plus a 64-byte reserved region.
        private static int[] allocateReserved() throws Exception {
            IBinder allocator = (IBinder) Class.forName("android.os.ServiceManager")
                    .getMethod("checkService", String.class).invoke(null,
                            "android.hardware.graphics.allocator.IAllocator/default");
            android.os.Parcel data = android.os.Parcel.obtain(), reply = android.os.Parcel.obtain();
            try {
                data.writeInterfaceToken("android.hardware.graphics.allocator.IAllocator");
                data.writeInt(1);
                int start = data.dataPosition(); data.writeInt(0);
                data.writeByteArray(new byte[128]);
                data.writeInt(1); data.writeInt(1); data.writeInt(1); data.writeInt(3);
                data.writeLong(0x2433); data.writeLong(64); data.writeInt(0); // CPU plus libui's retained legacy hints
                int end = data.dataPosition(); data.setDataPosition(start);
                data.writeInt(end - start); data.setDataPosition(end); data.writeInt(1);
                if (!allocator.transact(2, data, reply, 0)) throw new AssertionError("allocate2 transaction");
                reply.readException();
                if (reply.readInt() != 1) throw new AssertionError("AllocationResult presence");
                reply.readInt();
                if (reply.readInt() != 1 || reply.readInt() != 1 || reply.readInt() != 1)
                    throw new AssertionError("AllocationResult stride/count/handle");
                reply.readInt();
                if (reply.readInt() != 1 || reply.readInt() != 1) throw new AssertionError("NativeHandle fds");
                try (android.os.ParcelFileDescriptor fd = android.os.ParcelFileDescriptor.CREATOR.createFromParcel(reply)) {
                    int[] info = reply.createIntArray();
                    if (info == null || info.length != 12) throw new AssertionError("NativeHandle ints");
                    int[] transport = new int[13];
                    System.arraycopy(info, 0, transport, 1, 12);
                    transport[0] = fd.detachFd();
                    return transport;
                }
            } finally { data.recycle(); reply.recycle(); }
        }
        static void checkDeclarations() throws Exception {
            Class<?> manager = Class.forName("android.os.ServiceManager");
            String iface = "android.hardware.graphics.allocator.IAllocator";
            if (!(Boolean) manager.getMethod("isDeclared", String.class).invoke(null, iface + "/default")
                    || (Boolean) manager.getMethod("isDeclared", String.class).invoke(null, iface + "/missing"))
                throw new AssertionError("HAL declaration filtering");
            String[] instances = (String[]) manager.getMethod("getDeclaredInstances", String.class).invoke(null, iface);
            String[] missing = (String[]) manager.getMethod("getDeclaredInstances", String.class).invoke(null, "bifrost.missing.IHal");
            if (!Arrays.equals(instances, new String[] {"default"}) || missing.length != 0)
                throw new AssertionError("HAL instance enumeration");
        }
        public static void main(String[] args) {
            ManagedLauncher.probe(args.length == 0 ? new String[] {"--gralloc"}
                    : new String[] {"--gralloc", args[0]});
        }
    }

    // Standalone real-ART contract probe; runs without starting an Activity.
    public static final class InputMethodProbe {
        static Object invoke(Object endpoint, String name, Object... args) throws Exception {
            Class<?> iface = endpoint.getClass().getInterfaces()[0];
            for (Method method : iface.getMethods()) if (method.getName().equals(name)) {
                try { return method.invoke(endpoint, args); }
                catch (InvocationTargetException error) {
                    if (error.getCause() instanceof Exception) throw (Exception) error.getCause();
                    throw error;
                }
            }
            throw new AssertionError(name);
        }
        static int result(Object value) throws Exception {
            return value.getClass().getField("result").getInt(value);
        }
        static void check(boolean value, String message) {
            if (!value) throw new AssertionError(message);
        }
        public static void main(String[] args) throws Exception {
            Looper.prepareMainLooper();
            FrameworkServices services = new FrameworkServices();
            FrameworkInputMethod input = new FrameworkInputMethod((name, uid) ->
                "org.asgard.ime".equals(name) && uid == android.os.Process.myUid());
            input.install(services);
            IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                    .getMethod("getService", String.class).invoke(null, "input_method");
            Object manager = binder.queryLocalInterface("com.android.internal.view.IInputMethodManager");
            check(manager != null, "service publication");
            CountDownLatch callback = new CountDownLatch(1);
            AtomicInteger asyncResult = new AtomicInteger(-1), asyncSequence = new AtomicInteger(-1);
            Object client = services.local("ime-probe",
                    "com.android.internal.inputmethod.IInputMethodClient", (method, a) -> {
                if (!method.equals("onStartInputResult")) throw new AssertionError(method);
                asyncResult.set(result(a[0]));
                asyncSequence.set((Integer) a[1]);
                callback.countDown();
                return null;
            });
            invoke(manager, "addClient", client, null, 0);
            try { invoke(manager, "addClient", client, null, 0); throw new AssertionError("duplicate client accepted"); }
            catch (SecurityException expected) {}
            Object badClient = services.local("ime-bad-client",
                    "com.android.internal.inputmethod.IInputMethodClient", (m, a) -> null);
            try { invoke(manager, "addClient", badClient, null, 0); throw new AssertionError("duplicate process/display accepted"); }
            catch (SecurityException expected) {}
            invoke(manager, "addClient", badClient, null, 1);
            try { invoke(manager, "getEnabledInputMethodList", 1); throw new AssertionError("cross-user query accepted"); }
            catch (SecurityException expected) {}
            List<?> list = (List<?>) invoke(manager, "getInputMethodListLegacy", 0, 0);
            check(list.isEmpty(), "IME inventory");
            list.add(null);
            check(((List<?>)invoke(manager, "getInputMethodListLegacy", 0, 0)).isEmpty(), "copy isolation");
            Object safeList = invoke(manager, "getEnabledInputMethodList", 0);
            check(((List<?>) safeList.getClass().getMethod("extractFrom", safeList.getClass())
                    .invoke(null, safeList)).isEmpty(), "safe-list inventory");
            check(invoke(manager, "getCurrentInputMethodInfoAsUser", 0) == null, "selected IME");
            EditorInfo editor = new EditorInfo(); editor.packageName = "org.asgard.ime"; editor.inputType = 1;
            Object[] start = {1, client, new Binder(), 0, 0, 0, editor, null, null, 35, 0, null};
            check(result(invoke(manager, "startInputOrWindowGainedFocus", start)) == 6, "NO_IME result");
            editor.packageName = "spoofed.package";
            check(result(invoke(manager, "startInputOrWindowGainedFocus", start)) == 7, "invalid package result");
            editor.packageName = "org.asgard.ime";
            start[6] = null;
            check(result(invoke(manager, "startInputOrWindowGainedFocus", start)) == 11, "null editor result");
            start[6] = editor; start[2] = null;
            check(result(invoke(manager, "startInputOrWindowGainedFocus", start)) == 5, "null window result");
            start[2] = new Binder();
            start[1] = badClient;
            check(result(invoke(manager, "startInputOrWindowGainedFocus", start)) == 15,
                    "invalid display result");
            start[1] = client;
            Object[] async = Arrays.copyOf(start, 13); async[12] = 73;
            invoke(manager, "startInputOrWindowGainedFocusAsync", async);
            check(callback.await(2, TimeUnit.SECONDS) && asyncResult.get() == 6
                    && asyncSequence.get() == 73, "async result/sequence");
            start[1] = services.local("ime-unknown-client",
                    "com.android.internal.inputmethod.IInputMethodClient", (m, a) -> null);
            try { invoke(manager, "startInputOrWindowGainedFocus", start); throw new AssertionError("unknown client accepted"); }
            catch (IllegalArgumentException expected) {}
            AtomicInteger visibility = new AtomicInteger(-1);
            ResultReceiver receiver = new ResultReceiver(null) {
                protected void onReceiveResult(int code, Bundle data) { visibility.set(code); }
            };
            Object tracker = invoke(manager, "getImeTrackerService");
            Object token = invoke(tracker, "onStart", "ime-probe", android.os.Process.myUid(), 1, 1, 1, false);
            check(!(Boolean)invoke(manager, "showSoftInput", client, start[2], token, 0, 0, receiver, 0), "show fabricated keyboard");
            check(visibility.get() == InputMethodManager.RESULT_UNCHANGED_HIDDEN, "show visibility result");
            visibility.set(-1);
            check(!(Boolean)invoke(manager, "hideSoftInput", client, start[2], null, 0, receiver, 0), "hide return");
            check(visibility.get() == InputMethodManager.RESULT_UNCHANGED_HIDDEN, "hide visibility result");
            check((Integer)invoke(manager, "getInputMethodWindowVisibleHeight", client) == 0, "IME inset height");
            try { invoke(manager, "removeImeSurface"); throw new AssertionError("privileged method accepted"); }
            catch (SecurityException expected) {}
            System.out.println("INPUT_METHOD_PROTOCOL_OK");
            System.out.println("FRAMEWORK_OK");
            System.exit(0);
        }
    }
}
