package org.bifrost.fixture;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.os.Handler;
import android.view.Gravity;
import android.view.SurfaceView;
import android.widget.PopupWindow;
import android.widget.TextView;

/** Framework popup windows over a SurfaceView: AlertDialog, focus, PopupWindow. */
public final class ManagedDialog extends Activity {
    private final Handler main = new Handler();
    private boolean dialogDone;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
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
            main.postDelayed(this::showPopupWindow, 300);
        });
        dialog.show();
        // Leave time for the launcher to lay out and draw the window, then
        // press the button through the dialog's own view tree.
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
            main.postDelayed(this::finish, 300);
        });
        popup.showAtLocation(getWindow().getDecorView(), Gravity.TOP | Gravity.START, 40, 60);
        System.out.println("MANAGED_DIALOG_POPUP_SHOWN");
        main.postDelayed(popup::dismiss, 1000);
    }

    @Override public void onDestroy() { super.onDestroy(); System.out.println("MANAGED_DIALOG_DESTROYED"); }
}
