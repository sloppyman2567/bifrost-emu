// Toast queue of NotificationManagerService: one toast shows at a time, for
// 2 s (LENGTH_SHORT) or 3.5 s (LENGTH_LONG), then the next. There is no
// SystemUI, so a text toast gets a TYPE_TOAST window of the framework's
// transient_notification layout, added by this process as Android did before
// SystemUI rendered them. A custom toast renders itself: the service only
// calls ITransientNotification.show(windowToken) and hide().
import android.content.Context;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

final class FrameworkToasts {
    private static final long SHORT_DELAY_MS = 2000, LONG_DELAY_MS = 3500;
    // NotificationManagerService.MAX_PACKAGE_TOASTS bounds a background
    // package; a foreground one is unlimited. This process is the foreground
    // app, so only a sanity cap applies.
    private static final int MAX_QUEUED = 50;
    private static final String TRANSIENT = "android.app.ITransientNotification",
            CALLBACK = "android.app.ITransientNotificationCallback";

    private static final class Record {
        final IBinder token;
        final Object custom;            // ITransientNotification, or null for a text toast
        CharSequence text;
        final Object callback;          // ITransientNotificationCallback, or null
        int duration;
        View view;                      // a text toast's window, while shown
        final IBinder windowToken = new Binder();

        Record(IBinder token, Object custom, CharSequence text, Object callback, int duration) {
            this.token = token; this.custom = custom; this.text = text; this.callback = callback;
            this.duration = duration;
        }
    }

    private final ScheduledExecutorService clock = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "ToastQueue");
        thread.setDaemon(true);
        return thread;
    });
    private final List<Record> queue = new ArrayList<>();
    private ScheduledFuture<?> timeout;

    synchronized boolean enqueue(IBinder token, Object custom, CharSequence text, Object callback, int duration) {
        Objects.requireNonNull(token, "token");
        for (Record record : queue) if (record.token == token) {
            // NMS updates a queued toast with the same token in place.
            record.duration = duration; record.text = text;
            return true;
        }
        if (queue.size() >= MAX_QUEUED) return false;
        queue.add(new Record(token, custom, text, callback, duration));
        if (queue.size() == 1) show();
        return true;
    }

    synchronized void cancel(IBinder token) {
        for (int i = 0; i < queue.size(); i++) if (queue.get(i).token == token) {
            boolean showing = i == 0;
            if (showing) { timeout.cancel(false); hide(queue.get(0)); }
            queue.remove(i);
            if (showing && !queue.isEmpty()) show();
            return;
        }
    }

    // The head of the queue is on screen from here until its delay expires.
    private void show() {
        Record record = queue.get(0);
        display(record);
        timeout = clock.schedule(() -> {
            synchronized (this) {
                if (queue.isEmpty() || queue.get(0) != record) return;
                hide(record);
                queue.remove(0);
                if (!queue.isEmpty()) show();
            }
        }, record.duration == 1 ? LONG_DELAY_MS : SHORT_DELAY_MS, TimeUnit.MILLISECONDS);
    }

    private void display(Record record) {
        if (record.custom != null) { call(TRANSIENT, record.custom, "show", IBinder.class, record.windowToken); return; }
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                Context context = (Context) Class.forName("android.app.ActivityThread")
                        .getMethod("currentApplication").invoke(null);
                View view = LayoutInflater.from(context).inflate(
                        id(context, "transient_notification", "layout"), null);
                ((TextView) view.findViewById(id(context, "message", "id"))).setText(record.text);
                WindowManager.LayoutParams params = new WindowManager.LayoutParams();
                params.height = params.width = WindowManager.LayoutParams.WRAP_CONTENT;
                params.format = android.graphics.PixelFormat.TRANSLUCENT;
                params.type = WindowManager.LayoutParams.TYPE_TOAST;
                params.setTitle("Toast");
                params.flags = WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                        | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
                params.gravity = context.getResources().getInteger(id(context, "config_toastDefaultGravity", "integer"));
                if ((params.gravity & Gravity.HORIZONTAL_GRAVITY_MASK) == Gravity.FILL_HORIZONTAL)
                    params.horizontalWeight = 1.0f;
                if ((params.gravity & Gravity.VERTICAL_GRAVITY_MASK) == Gravity.FILL_VERTICAL)
                    params.verticalWeight = 1.0f;
                params.y = context.getResources().getDimensionPixelSize(id(context, "toast_y_offset", "dimen"));
                int animation = id(context, "Animation.Toast", "style");
                if (animation != 0) params.windowAnimations = animation;
                context.getSystemService(WindowManager.class).addView(view, params);
                synchronized (this) {
                    // Cancelled or expired before the main thread ran: take it down again.
                    if (queue.isEmpty() || queue.get(0) != record) {
                        context.getSystemService(WindowManager.class).removeViewImmediate(view);
                        return;
                    }
                    record.view = view;
                }
                if (record.callback != null) call(CALLBACK, record.callback, "onToastShown", null, null);
                System.out.println("APP_TOAST_SHOWN " + record.text);
            } catch (Exception error) {
                System.out.println("APP_TOAST_FAILED " + error);
            }
        });
    }

    private void hide(Record record) {
        if (record.custom != null) { call(TRANSIENT, record.custom, "hide", null, null); return; }
        new Handler(Looper.getMainLooper()).post(() -> {
            View view;
            synchronized (this) { view = record.view; record.view = null; }
            if (view == null) return;
            try {
                view.getContext().getSystemService(WindowManager.class).removeViewImmediate(view);
            } catch (RuntimeException error) {
                System.out.println("APP_TOAST_FAILED " + error);
            }
            if (record.callback != null) call(CALLBACK, record.callback, "onToastHidden", null, null);
        });
    }

    private static int id(Context context, String name, String type) {
        int id = context.getResources().getIdentifier(name, type, "android");
        if (id == 0 && !type.equals("style")) throw new IllegalStateException("android:" + type + "/" + name);
        return id;
    }

    // Both Binder interfaces are hidden API, so call them by name.
    private static void call(String iface, Object target, String name, Class<?> type, Object argument) {
        try {
            Class<?> declared = Class.forName(iface);
            if (type == null) declared.getMethod(name).invoke(target);
            else declared.getMethod(name, type).invoke(target, argument);
        } catch (ReflectiveOperationException | RuntimeException error) {
            System.out.println("APP_TOAST_FAILED " + name + " " + error);
        }
    }
}
