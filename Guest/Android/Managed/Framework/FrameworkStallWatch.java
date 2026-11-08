// Names the code behind a main-thread stall. The launcher's UI loop calls
// `beat()` every few milliseconds; a daemon thread that sees no beat for
// STALL_MS prints the main thread's Java stack once per second until it runs
// again. A stall that lasts THREADS_MS also reports what the other threads are
// doing once (a main thread waiting for a game's render thread is only
// explained by that thread). Reading one thread's stack does not suspend the
// process (a SIGQUIT dump does, for seconds), so a stall report is cheap and
// always on. Output is bounded: at most MAX_REPORTS stack lines per session and
// MAX_DUMPS thread reports of at most DUMP_THREADS threads.
final class FrameworkStallWatch {
    private static final long STALL_MS = 1500, REPORT_MS = 1000, THREADS_MS = 4000;
    private static final int MAX_REPORTS = 60, FRAMES = 14, MAX_DUMPS = 3, DUMP_THREADS = 40, DUMP_FRAMES = 10;
    private final Thread main;
    private volatile long beat = android.os.SystemClock.uptimeMillis();
    private int reports, dumps;

    private FrameworkStallWatch(Thread main) { this.main = main; }

    // Called from the thread being watched, which keeps beating.
    static FrameworkStallWatch start() {
        FrameworkStallWatch watch = new FrameworkStallWatch(Thread.currentThread());
        Thread thread = new Thread(watch::run, "StallWatch");
        thread.setDaemon(true);
        thread.start();
        return watch;
    }

    void beat() { beat = android.os.SystemClock.uptimeMillis(); }

    private void run() {
        long reported = 0;
        boolean stalled = false, dumped = false;
        while (true) {
            try { Thread.sleep(250); } catch (InterruptedException stop) { return; }
            long now = android.os.SystemClock.uptimeMillis(), silent = now - beat;
            if (silent < STALL_MS) {
                if (stalled) System.out.println("APP_MAIN_STALL_END after about " + silent + "ms since the last beat");
                stalled = false;
                dumped = false;
                continue;
            }
            stalled = true;
            if (now - reported < REPORT_MS || reports >= MAX_REPORTS) continue;
            reported = now;
            reports++;
            StringBuilder text = new StringBuilder("APP_MAIN_STALL ").append(silent).append("ms");
            StackTraceElement[] stack = main.getStackTrace();
            for (int i = 0; i < Math.min(stack.length, FRAMES); i++) text.append(i == 0 ? " at " : " <- ").append(stack[i]);
            System.out.println(text);
            if (silent >= THREADS_MS && !dumped && dumps < MAX_DUMPS) {
                dumped = true;
                dumps++;
                dumpThreads();
            }
        }
    }

    // One APP_STALL_THREAD line per thread that is doing something: an idle Looper or a pool thread waiting for work says
    // nothing. The surface frame thread and a game's own threads are among them.
    private void dumpThreads() {
        int printed = 0;
        for (java.util.Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
            Thread thread = entry.getKey();
            StackTraceElement[] stack = entry.getValue();
            if (thread == main || thread == Thread.currentThread() || stack.length == 0 || idle(stack)) continue;
            if (printed++ >= DUMP_THREADS) break;
            StringBuilder text = new StringBuilder("APP_STALL_THREAD ").append(thread.getName()).append(' ')
                    .append(thread.getState());
            for (int i = 0; i < Math.min(stack.length, DUMP_FRAMES); i++) text.append(i == 0 ? " at " : " <- ").append(stack[i]);
            System.out.println(text);
        }
        System.out.println("APP_STALL_THREADS " + printed);
    }

    private static boolean idle(StackTraceElement[] stack) {
        String top = stack[0].toString();
        if (top.contains("MessageQueue.nativePollOnce")) return true;
        for (StackTraceElement frame : stack) {
            String name = frame.toString();
            if (name.contains("ThreadPoolExecutor.getTask") || name.contains("DelayedWorkQueue.take")
                    || name.contains("ReferenceQueue.remove") || name.contains("Daemons$")) return true;
        }
        return false;
    }
}
