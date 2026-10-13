// Opt-in APK boundary calls and stall diagnostics. ART owns Java/JNI method tracing.
import android.content.ComponentName;
import android.content.Intent;
import android.os.Debug;
import android.os.Process;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import org.json.JSONArray;
import org.json.JSONObject;

final class FrameworkTrace {
    private static final AtomicLong sequence = new AtomicLong();
    private static final Map<Long, Pending> pending = new ConcurrentHashMap<>();
    private static final long LIMIT = 64L * 1024 * 1024;
    private static FileOutputStream output;
    private static volatile boolean enabled;
    private static boolean activityOnly;
    private static final Set<String> catalogs = ConcurrentHashMap.newKeySet();
    private static boolean methods;
    private static long bytes;
    private static File directory;

    private static final class Pending {
        final String operation;
        final String signature;
        final long started = System.nanoTime();
        final int tid = Process.myTid();
        Pending(String operation, String signature) { this.operation = operation; this.signature = signature; }
    }

    private static JSONObject event(String kind) throws Exception {
        return new JSONObject().put("event", kind).put("mono_ns", System.nanoTime())
                .put("pid", Process.myPid()).put("tid", Process.myTid())
                .put("thread", Thread.currentThread().getName());
    }

    private static synchronized void write(JSONObject event) {
        if (!enabled) return;
        try {
            byte[] line = (event.toString() + "\n").getBytes(StandardCharsets.UTF_8);
            if (bytes + line.length > LIMIT) {
                line = "{\"event\":\"trace.truncated\",\"limit_bytes\":67108864}\n"
                        .getBytes(StandardCharsets.UTF_8);
                output.write(line);
                enabled = false;
                return;
            }
            output.write(line); // unbuffered: completed records survive SIGKILL/ART abort
            bytes += line.length;
        } catch (IOException error) {
            enabled = false;
            System.err.println("APK_TRACE_WRITE_FAILED " + error.getClass().getName());
        }
    }

    // Never call arbitrary app toString(), inspect Bundles or invoke a Binder from logging.
    private static String value(Object object) {
        if (object == null) return "null";
        if (object instanceof String) {
            String text = (String) object;
            return text.length() > 256 ? text.substring(0, 256) + "…" : text;
        }
        if (object instanceof Integer || object instanceof Long || object instanceof Short
                || object instanceof Byte || object instanceof Double || object instanceof Float
                || object instanceof Boolean || object instanceof Character)
            return String.valueOf(object);
        if (object instanceof ComponentName) return ((ComponentName) object).flattenToShortString();
        if (object instanceof Intent) {
            Intent intent = (Intent) object;
            return "Intent{action=" + value(intent.getAction()) + ",component=" + value(intent.getComponent())
                    + ",package=" + value(intent.getPackage()) + ",flags=0x"
                    + Integer.toHexString(intent.getFlags()) + "}";
        }
        if (object.getClass().isArray())
            return object.getClass().getName() + "[length=" + java.lang.reflect.Array.getLength(object) + "]";
        return object.getClass().getName() + "@" + Integer.toHexString(System.identityHashCode(object));
    }

    private static JSONArray stack(StackTraceElement[] frames, int limit) {
        JSONArray result = new JSONArray();
        for (int i = 0; i < Math.min(frames.length, limit); i++) result.put(frames[i].toString());
        return result;
    }

    private static boolean activity(String service) {
        return service.equals("activity") || service.equals("activity_task") || service.equals("bifrost.activity_client");
    }

    static void catalog(String service, Class<?> type, String[] handled) {
        if (!enabled || !activityOnly || !activity(service) || !catalogs.add(service + ":" + type.getName())) return;
        try {
            java.lang.reflect.Method[] declared = type.getMethods();
            java.util.Arrays.sort(declared, java.util.Comparator.comparing(java.lang.reflect.Method::toGenericString));
            JSONArray methods = new JSONArray();
            for (java.lang.reflect.Method method : declared) {
                if (method.getName().equals("asBinder") || method.getDeclaringClass() == Object.class) continue;
                methods.put(new JSONObject().put("method", method.getName()).put("signature", method.toGenericString())
                        .put("declared_coverage", handled == null ? "unclassified"
                                : java.util.Arrays.asList(handled).contains(method.getName()) ? "implemented" : "unsupported"));
            }
            write(event("api.catalog").put("service", service).put("interface", type.getName()).put("methods", methods));
        } catch (Exception error) { failed("catalog", error); }
    }

    static long begin(String service, java.lang.reflect.Method method, Object[] arguments) {
        if (!enabled || (activityOnly && !activity(service))) return 0;
        return begin(service, method.getName(), activityOnly ? method.toGenericString() : null, arguments);
    }

    static long begin(String service, String method, Object[] arguments) {
        return begin(service, method, null, arguments);
    }

    private static long begin(String service, String method, String signature, Object[] arguments) {
        if (!enabled || (activityOnly && !activity(service))) return 0;
        long id = sequence.incrementAndGet();
        Pending call = new Pending(service + "." + method, signature);
        pending.put(id, call);
        try {
            JSONArray args = new JSONArray();
            if (arguments != null) for (Object argument : arguments) args.put(value(argument));
            write(event("call").put("id", id).put("operation", call.operation).put("signature", signature).put("args", args)
                    .put("caller", stack(Thread.currentThread().getStackTrace(), 12)));
        } catch (Exception error) { failed("call", error); }
        return id;
    }

    static void end(long id, Object result, Throwable error) {
        if (id == 0) return;
        Pending call = pending.remove(id);
        if (call == null || !enabled) return;
        try {
            JSONObject record = event(error == null ? "return" : "throw").put("id", id)
                    .put("operation", call.operation).put("signature", call.signature)
                    .put("duration_ns", System.nanoTime() - call.started);
            if (error == null) record.put("result", value(result));
            else record.put("exception", error.getClass().getName()).put("message", value(error.getMessage()))
                    .put("stack", stack(error.getStackTrace(), 24));
            write(record);
        } catch (Exception failure) { failed("return", failure); }
    }

    private static void failed(String phase, Throwable error) {
        try { write(event("trace.error").put("phase", phase).put("exception", error.getClass().getName())); }
        catch (Exception ignored) { System.err.println("APK_TRACE_ERROR " + phase); }
    }

    private static void snapshot(Set<String> libraries) throws Exception {
        long now = System.nanoTime();
        JSONArray active = new JSONArray();
        for (Map.Entry<Long, Pending> entry : pending.entrySet()) {
            Pending call = entry.getValue();
            active.put(new JSONObject().put("id", entry.getKey()).put("operation", call.operation)
                    .put("tid", call.tid).put("elapsed_ns", now - call.started));
        }
        Runtime runtime = Runtime.getRuntime();
        write(event("heartbeat").put("pending", active).put("java_used_bytes", runtime.totalMemory() - runtime.freeMemory())
                .put("java_total_bytes", runtime.totalMemory()).put("java_max_bytes", runtime.maxMemory()));
        if (activityOnly) return;
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/self/maps"))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] fields = line.trim().split("\\s+", 6);
                if (fields.length == 6 && fields[5].contains(".so") && libraries.add(fields[5]))
                    write(event("library.mapping").put("path", fields[5]).put("range", fields[0]));
            }
        }
        // A separate marker makes an ART suspension stuck inside the stack dump visible.
        write(event("threads.begin"));
        for (Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
            Thread thread = entry.getKey();
            write(event("thread.stack").put("java_thread_id", thread.getId()).put("name", thread.getName())
                    .put("state", thread.getState().name()).put("stack", stack(entry.getValue(), 32)));
        }
        write(event("threads.end"));
    }

    private static synchronized void startMethods(String mode) throws Exception {
        if (!enabled) return;
        String path = new File(directory, "methods.trace").getPath();
        if (mode.equals("methods")) Debug.startMethodTracing(path, 16 * 1024 * 1024, 0);
        else Debug.startMethodTracingSampling(path, 16 * 1024 * 1024, 10000);
        methods = true;
        write(event("methods.start").put("mode", mode).put("path", path).put("buffer_bytes", 16 * 1024 * 1024));
    }

    private static synchronized void stopMethods() {
        if (!methods) return;
        try {
            Debug.stopMethodTracing();
            methods = false;
            write(event("methods.stop").put("bytes", new File(directory, "methods.trace").length()));
        } catch (Exception error) { failed("methods.stop", error); }
    }

    static void start() throws Exception {
        String path = System.getProperty("bifrost.apk.trace.dir");
        if (path == null) return;
        directory = new File(path);
        if (!directory.isDirectory()) throw new IOException("APK trace directory is missing: " + path);
        output = new FileOutputStream(new File(directory, "calls.jsonl"));
        enabled = true;
        String mode = System.getProperty("bifrost.apk.trace.mode", "calls");
        activityOnly = mode.equals("activity");
        long after = Long.parseLong(System.getProperty("bifrost.apk.trace.after", "0")) * 1000;
        long duration = Long.parseLong(System.getProperty("bifrost.apk.trace.duration", "15")) * 1000;
        write(event("trace.start").put("mode", mode).put("calls_limit_bytes", LIMIT)
                .put("scope", activityOnly ? "activity Binder APIs; declared signatures and observed calls"
                        : "local framework boundaries; periodic Java stacks and library mappings"));
        Runtime.getRuntime().addShutdownHook(new Thread(FrameworkTrace::stop, "Bifrost trace shutdown"));
        Thread watcher = new Thread(() -> {
            Set<String> libraries = new HashSet<>();
            while (enabled) {
                try { snapshot(libraries); Thread.sleep(10000); }
                catch (Exception error) { failed("snapshot", error); break; }
            }
        }, "Bifrost APK trace");
        watcher.setDaemon(true);
        watcher.start();
        if (mode.equals("methods") || mode.equals("sample")) {
            Thread tracer = new Thread(() -> {
                try { Thread.sleep(after); startMethods(mode); Thread.sleep(duration); stopMethods(); }
                catch (Exception error) { failed("methods", error); }
            }, "Bifrost method trace");
            tracer.setDaemon(true);
            tracer.start();
        }
        System.out.println("APK_TRACE_READY " + path);
    }

    private static synchronized void stop() {
        stopMethods();
        if (output == null) return;
        try {
            write(event("trace.stop").put("pending_count", pending.size()));
            enabled = false;
            output.close();
        } catch (Exception error) { failed("stop", error); }
    }
}
