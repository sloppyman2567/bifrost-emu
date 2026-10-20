// Names the code behind a main-thread stall. The launcher's UI loop calls
// `beat()` every few milliseconds; a daemon thread that sees no beat for
// STALL_MS prints the main thread's Java stack once per second until it runs
// again. Reading one thread's stack does not suspend the process (a SIGQUIT
// dump does, for seconds), so a stall report is cheap and always on. Output is
// bounded: at most MAX_REPORTS stack lines per session.
final class FrameworkStallWatch {
    private static final long STALL_MS = 1500, REPORT_MS = 1000;
    private static final int MAX_REPORTS = 60, FRAMES = 14;
    private final Thread main;
    private volatile long beat = android.os.SystemClock.uptimeMillis();
    private int reports;

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
        boolean stalled = false;
        while (true) {
            try { Thread.sleep(250); } catch (InterruptedException stop) { return; }
            long now = android.os.SystemClock.uptimeMillis(), silent = now - beat;
            if (silent < STALL_MS) {
                if (stalled) System.out.println("APP_MAIN_STALL_END after about " + silent + "ms since the last beat");
                stalled = false;
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
        }
    }
}
