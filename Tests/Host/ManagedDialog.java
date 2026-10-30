package org.bifrost.fixture;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
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
        // press the button through the dialog's own view tree.
        main.postDelayed(() -> getWindow().getDecorView().setVisibility(View.INVISIBLE), 1000);
        main.postDelayed(() -> {
            if (!dialog.getWindow().getDecorView().isAttachedToWindow()
                    || dialog.getWindow().getDecorView().getWidth() == 0)
                throw new IllegalStateException("dialog window was not laid out");
            dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick();
        }, 1500);
    }

    private void showPopupWindow() {
        TextView text = new TextView(this);
        text.setText("Popup window");
        text.setBackgroundColor(0xff336699);
        PopupWindow popup = new PopupWindow(text, 240, 80);
        popup.setOnDismissListener(() -> {
            System.out.println("MANAGED_DIALOG_POPUP_DISMISSED");
            // A text toast is a TYPE_TOAST window of the framework layout; it
            // is gone after LENGTH_SHORT (2 s) and the next step waits for it.
            Toast.makeText(this, "Bifrost toast", Toast.LENGTH_SHORT).show();
            System.out.println("MANAGED_DIALOG_TOAST_REQUESTED");
            main.postDelayed(() -> {
                System.out.println("MANAGED_DIALOG_TOAST_WAITED");
                startActivity(new Intent(this, Engine.class));
            }, 3000);
        });
        popup.showAtLocation(getWindow().getDecorView(), Gravity.TOP | Gravity.START, 40, 60);
        System.out.println("MANAGED_DIALOG_POPUP_SHOWN");
        main.postDelayed(popup::dismiss, 1000);
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
                engine.postDelayed(() -> dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick(), 1800);
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
                    // The task ends with its last Activity: finish the Engine on top
                    // first, then the root.
                    if (context == root) root.main.postDelayed(() -> {
                        finish();
                        root.main.postDelayed(root::finish, 500);
                    }, 500);
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
            if (context != root) dialog.setOnDismissListener(d -> engine.postDelayed(() -> showVideo(root, "stopped"), 300));
            dialog.setContentView(video, new ViewGroup.LayoutParams(320, 180));
            dialog.show();
        }
    }
}
