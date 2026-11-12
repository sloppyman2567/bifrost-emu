package org.bifrost.fixture;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.PopupWindow;
import android.widget.Toast;
import android.widget.TextView;

/** Framework popup windows over a SurfaceView: AlertDialog, focus, PopupWindow, engine-thread windows. */
public final class ManagedDialog extends Activity {
    private static ManagedDialog root;
    private final Handler main = new Handler();
    private boolean dialogDone;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        root = this;
        setContentView(new SurfaceView(this));
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        System.out.println("MANAGED_DIALOG_ACTIVITY_FOCUS " + focused);
    }

    @Override public void onResume() {
        super.onResume();
        System.out.println("MANAGED_DIALOG_ROOT_RESUMED");
        if (dialogDone) return;
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Bifrost dialog")
                .setMessage("Framework popup window")
                .setPositiveButton("OK", (d, which) -> System.out.println("MANAGED_DIALOG_POSITIVE"))
                .setNegativeButton("Cancel", null)
                .create();
        dialog.setOnShowListener(d -> System.out.println("MANAGED_DIALOG_SHOWN"));
        dialog.setOnDismissListener(d -> {
            System.out.println("MANAGED_DIALOG_DISMISSED");
            dialogDone = true;
            // Nothing was focusable when the dialog went; the window gets
            // focus when it becomes visible again (an Activity resuming
            // under a closed ad).
            getWindow().getDecorView().setVisibility(View.VISIBLE);
            main.postDelayed(this::showPopupWindow, 300);
        });
        dialog.show();
        // Leave time for the launcher to lay out and draw the window, then
        // tap the button as the host would.
        main.postDelayed(() -> getWindow().getDecorView().setVisibility(View.INVISIBLE), 1000);
        main.postDelayed(() -> {
            if (!dialog.getWindow().getDecorView().isAttachedToWindow()
                    || dialog.getWindow().getDecorView().getWidth() == 0)
                throw new IllegalStateException("dialog window was not laid out");
            tap(this, dialog.getButton(DialogInterface.BUTTON_POSITIVE));
        }, 1500);
    }

    // A host tap at the middle of a view, through the launcher's own entry point (ManagedLauncher.dispatch, with the
    // records a window backend reports) on the play loop's thread: it reaches the view's window through that window's
    // input channel, and the click runs on the window's own thread.
    static void tap(Activity activity, View view) {
        int[] at = new int[2];
        view.getLocationOnScreen(at);
        float x = at[0] + view.getWidth() / 2f, y = at[1] + view.getHeight() / 2f;
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                java.lang.reflect.Method dispatch = ClassLoader.getSystemClassLoader().loadClass("ManagedLauncher")
                        .getDeclaredMethod("dispatch", Activity.class, long[].class);
                dispatch.setAccessible(true);
                for (int action : new int[] {0, 1}) {  // ACTION_DOWN, ACTION_UP
                    long[] e = new long[55];
                    long now = SystemClock.uptimeMillis() * 1000000L;
                    e[0] = 2; e[4] = action; e[5] = InputDevice.SOURCE_TOUCHSCREEN; e[6] = 1; e[13] = 1;
                    e[14] = Float.floatToIntBits(x) & 0xffffffffL; e[15] = Float.floatToIntBits(y) & 0xffffffffL;
                    e[16] = Float.floatToIntBits(1f) & 0xffffffffL;
                    e[11] = now; e[12] = now;
                    dispatch.invoke(null, activity, e);
                }
            } catch (ReflectiveOperationException error) {
                throw new IllegalStateException(error);
            }
        });
    }

    private void showPopupWindow() {
        TextView text = new TextView(this);
        text.setText("Popup window");
        text.setBackgroundColor(0xff336699);
        PopupWindow popup = new PopupWindow(text, 240, 80);
        popup.setOnDismissListener(() -> {
            System.out.println("MANAGED_DIALOG_POPUP_DISMISSED");
            // A text toast is a TYPE_TOAST window of the framework layout; it
            // is up for LENGTH_LONG (3.5 s).
            Toast.makeText(this, "Bifrost toast", Toast.LENGTH_LONG).show();
            System.out.println("MANAGED_DIALOG_TOAST_REQUESTED");
            // While the toast is up, windows added after it still stack below it (a dialog, and the next Activity's
            // window), and a PopupWindow of the Activity's window below the dialogs added after that window.
            main.postDelayed(this::showStackedWindows, 1000);
            main.postDelayed(() -> {
                System.out.println("MANAGED_DIALOG_ENGINE_START");
                startActivity(new Intent(this, Engine.class));
            }, 2500);
        });
        popup.showAtLocation(getWindow().getDecorView(), Gravity.TOP | Gravity.START, 40, 60);
        System.out.println("MANAGED_DIALOG_POPUP_SHOWN");
        main.postDelayed(popup::dismiss, 1000);
    }

    private void showStackedWindows() {
        System.out.println("MANAGED_DIALOG_STACK_BEGIN");
        Dialog stacked = new Dialog(this);
        TextView text = new TextView(this);
        text.setText("Stacked dialog");
        stacked.setContentView(text);
        stacked.show();
        main.postDelayed(() -> {
            TextView panel = new TextView(this);
            panel.setText("Stacked popup");
            PopupWindow popup = new PopupWindow(panel, 200, 60);
            popup.showAtLocation(getWindow().getDecorView(), Gravity.BOTTOM | Gravity.START, 0, 0);
            main.postDelayed(() -> {
                popup.dismiss();
                stacked.dismiss();
                System.out.println("MANAGED_DIALOG_STACK_END");
            }, 300);
        }, 300);
    }

    @Override public void onDestroy() { super.onDestroy(); System.out.println("MANAGED_DIALOG_DESTROYED"); }

    /**
     * Unity's layout: an Activity without hardware acceleration whose engine
     * thread, with its own Looper (UnityMain), shows dialogs. Their views
     * belong to that thread, and as software windows their invalidations
     * reach ViewRootImpl.checkThread.
     */
    public static final class Engine extends Activity {
        private Handler engine;

        @Override public void onCreate(Bundle state) {
            super.onCreate(state);
            HandlerThread thread = new HandlerThread("FixtureEngine");
            thread.start();
            engine = new Handler(thread.getLooper());
            engine.post(() -> {
                AlertDialog dialog = new AlertDialog.Builder(this)
                        .setTitle("Engine dialog")
                        .setMessage("Owned by an engine thread")
                        .setPositiveButton("OK", (d, which) -> System.out.println(
                                "MANAGED_DIALOG_ENGINE_POSITIVE " + Thread.currentThread().getName()))
                        .create();
                dialog.setOnDismissListener(d -> {
                    System.out.println("MANAGED_DIALOG_ENGINE_DISMISSED");
                    engine.postDelayed(() -> showVideo(this, "removed"), 300);
                });
                dialog.show();
                // Resizing a drawn button moves its ripple background's
                // bounds at the next draw, which invalidates it from that draw.
                engine.postDelayed(() -> dialog.getButton(DialogInterface.BUTTON_POSITIVE)
                        .setText("Continue with the engine"), 800);
                engine.postDelayed(() -> tap(this, dialog.getButton(DialogInterface.BUTTON_POSITIVE)), 1800);
            });
        }

        // A SurfaceView in an engine-thread window: its SurfaceHolder
        // callbacks run on that thread, and surfaceDestroyed still sees a
        // valid Surface. The "removed" window is dismissed; the "stopped"
        // one, on the root Activity, is still up when the app closes, which
        // stops it on its own Looper before the root closes its windows.
        private void showVideo(Activity context, String label) {
            SurfaceView video = new SurfaceView(context);
            Dialog dialog = new Dialog(context);
            video.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder holder) {
                    System.out.println("MANAGED_DIALOG_ENGINE_SURFACE_CREATED " + label + " "
                            + Thread.currentThread().getName());
                    // The task ends with its last Activity: the Engine on top
                    // finished first (a stopped root shows no window), then the root.
                    if (context == root) root.main.postDelayed(root::finish, 500);
                    else engine.postDelayed(dialog::dismiss, 300);
                }
                @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                    System.out.println("MANAGED_DIALOG_ENGINE_SURFACE_CHANGED " + label + " "
                            + Thread.currentThread().getName() + " " + width + "x" + height);
                }
                @Override public void surfaceDestroyed(SurfaceHolder holder) {
                    System.out.println("MANAGED_DIALOG_ENGINE_SURFACE_DESTROYED " + label + " "
                            + Thread.currentThread().getName() + " valid=" + holder.getSurface().isValid());
                }
            });
            // The root is stopped under this Activity: its dialog is added to a stopped Activity and gets
            // its surface once this one has finished and the root is visible again.
            if (context != root) dialog.setOnDismissListener(d -> {
                engine.postDelayed(() -> showVideo(root, "stopped"), 300);
                root.main.postDelayed(this::finish, 1000);
            });
            dialog.setContentView(video, new ViewGroup.LayoutParams(320, 180));
            dialog.show();
        }
    }
}
