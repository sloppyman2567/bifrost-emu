import android.content.res.Resources;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructPollfd;
import dalvik.system.DexFile;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.FileDescriptor;
import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.regex.Pattern;
import org.json.JSONObject;

/**
 * A session's Zygote: `bifrost-zygote ... ManagedZygote SOCKET TOKEN` (Guest/Android/Launchers/zygote_launcher.c).
 * It runs as uid 0 without Binder, preloads what carries no process identity, connects to the supervisor's SOCKET
 * (ProcessHost) and forks a managed service child for each request, as Zygote.forkAndSpecialize does: preFork, a fork
 * whose child takes the app's identity (zygoteFork), postForkChild in the child, postForkCommon in both. The child then
 * opens Binder (ZygoteInit.nativeZygoteInit) and runs `ManagedLauncher --service-child`. ManagedLauncher's statics
 * build services that ask for the caller's identity, so they are initialized in the child, never here.
 */
public final class ManagedZygote {
    private static final Pattern KEY = Pattern.compile("[0-9a-f]{32}");
    private static final Pattern CONTROLLER = Pattern.compile("asgard\\.process\\.[A-Za-z0-9_.]{1,80}");
    private static final Pattern NAME = Pattern.compile("[A-Za-z0-9_.:-]{1,63}");
    // Set once this process (and so every child it forks) holds the preinstalled system font map.
    static boolean fontsLoaded;

    // Registered by bifrost-zygote: fork; the child specializes to `uid`, writes to `log`. Returns -errno on failure.
    private static native int zygoteFork(int uid, int log, String name);

    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            System.err.println("ManagedZygote requires <supervisor socket> <token>");
            System.exit(2);
        }
        preload();
        LocalSocket socket = new LocalSocket();
        socket.connect(new LocalSocketAddress(args[0], LocalSocketAddress.Namespace.FILESYSTEM));
        DataInputStream in = new DataInputStream(socket.getInputStream());
        DataOutputStream out = new DataOutputStream(socket.getOutputStream());
        send(out, new JSONObject().put("token", args[1]).put("pid", Os.getpid()));
        StructPollfd[] control = {new StructPollfd()};
        control[0].fd = socket.getFileDescriptor();
        control[0].events = (short) OsConstants.POLLIN;
        for (;;) {
            reap();
            try {
                if (Os.poll(control, 1000) == 0) continue;
            } catch (ErrnoException interrupted) {
                // A signal (a child's SIGCHLD) ends the wait early. Anything else ends this Zygote, and its children
                // with it, so never let a signal do that.
                if (interrupted.errno == OsConstants.EINTR) continue;
                throw interrupted;
            }
            int length;
            try { length = in.readInt(); }
            catch (EOFException closed) { System.exit(0); return; }  // the supervisor has gone
            if (length <= 0 || length > 8192) throw new IOException("Invalid Zygote request size");
            byte[] data = new byte[length];
            in.readFully(data);
            FileDescriptor[] received = socket.getAncillaryFileDescriptors();
            JSONObject reply = new JSONObject();
            int pid = -1;
            String controller = null, key = null;
            try {
                if (received == null || received.length != 1) throw new IllegalArgumentException("A request carries one log");
                JSONObject request = new JSONObject(new String(data, StandardCharsets.UTF_8));
                key = request.getString("key");
                controller = request.getString("controller");
                String name = request.optString("name", "app_process");
                int uid = request.getInt("uid");
                if (!KEY.matcher(key).matches() || !CONTROLLER.matcher(controller).matches()
                        || !NAME.matcher(name).matches() || !((uid >= 10000 && uid <= 19999) || (uid >= 99000 && uid <= 99999)))
                    throw new IllegalArgumentException("Invalid Zygote request");
                pid = fork(uid, received[0], name);
                if (pid < 0) throw new IllegalStateException("fork: errno " + -pid);
                reply.put("pid", pid);
            } catch (IllegalArgumentException | IllegalStateException | org.json.JSONException error) {
                reply.put("error", String.valueOf(error.getMessage()));
            } finally {
                // Both processes close the received log; the child has it as stdout and stderr by now.
                if (received != null) for (FileDescriptor fd : received) Os.close(fd);
            }
            if (pid == 0) {
                child(socket, controller, key);
                return;
            }
            send(out, reply);
        }
    }

    // ZygoteInit.preload's parts that hold no identity: the system fonts and resources, and the launcher's classes,
    // loaded and verified (not initialized) so each child starts with them.
    private static void preload() throws Exception {
        long start = System.nanoTime();
        Class.forName("android.graphics.Typeface").getMethod("loadPreinstalledSystemFontMap").invoke(null);
        fontsLoaded = true;
        Resources.getSystem();
        ClassLoader loader = ManagedZygote.class.getClassLoader();
        int loaded = 0;
        @SuppressWarnings("deprecation")
        Enumeration<String> names = new DexFile(System.getProperty("java.class.path")).entries();
        while (names.hasMoreElements()) {
            try {
                Class.forName(names.nextElement(), false, loader);
                loaded++;
            } catch (ClassNotFoundException | LinkageError skipped) {
                // A class whose dependencies are absent here loads, and fails, in the child that uses it.
            }
        }
        System.out.println("ZYGOTE_PRELOADED " + loaded + " classes in " + (System.nanoTime() - start) / 1000000 + " ms");
    }

    private static int fork(int uid, FileDescriptor log, String name) throws Exception {
        System.out.flush();
        System.err.flush();
        Class<?> hooks = Class.forName("dalvik.system.ZygoteHooks");
        hooks.getMethod("preFork").invoke(null);
        Method descriptor = FileDescriptor.class.getDeclaredMethod("getInt$");
        int pid = zygoteFork(uid, (Integer) descriptor.invoke(log), name);
        if (pid == 0) {
            // A child that cannot finish must never return to the request loop, which shares the supervisor's socket.
            try {  // Zygote.callPostForkChildHooks: no debug flags, not a system server or child Zygote
                hooks.getMethod("postForkChild", int.class, boolean.class, boolean.class, String.class)
                        .invoke(null, 0, false, false, "arm64");
                hooks.getMethod("postForkCommon").invoke(null);
            } catch (Throwable failure) {
                failure.printStackTrace();
                Runtime.getRuntime().halt(1);
            }
            return 0;
        }
        hooks.getMethod("postForkCommon").invoke(null);
        return pid;
    }

    private static void child(LocalSocket supervisor, String controller, String key) throws Exception {
        supervisor.close();  // the Zygote's channel; closing it here never shuts it down for the Zygote
        // ZygoteInit.zygoteInit: Binder's thread pool, now that this process has the app's identity.
        Method binder = Class.forName("com.android.internal.os.ZygoteInit").getDeclaredMethod("nativeZygoteInit");
        binder.setAccessible(true);
        binder.invoke(null);
        ManagedLauncher.main(new String[] {"--service-child", controller, key});
        System.exit(0);
    }

    // Children are this process's: collect the ones that have ended (Os.waitpid and Int32Ref are hidden APIs).
    private static void reap() throws Exception {
        Class<?> reference = Class.forName("android.system.Int32Ref");
        Object status = reference.getConstructor(int.class).newInstance(0);
        Method waitpid = Os.class.getMethod("waitpid", int.class, reference, int.class);
        try {
            while ((Integer) waitpid.invoke(null, -1, status, OsConstants.WNOHANG) > 0) { }
        } catch (java.lang.reflect.InvocationTargetException none) {
            if (!(none.getCause() instanceof ErrnoException)) throw none;  // ECHILD: no children
        }
    }

    private static void send(DataOutputStream out, JSONObject message) throws IOException {
        byte[] data = message.toString().getBytes(StandardCharsets.UTF_8);
        out.writeInt(data.length);
        out.write(data);
        out.flush();
    }
}
