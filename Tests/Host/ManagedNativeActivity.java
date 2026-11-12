package org.bifrost.fixture;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.lang.reflect.Method;

// A NativeActivity subclass with Java around it, as Defold (Family Island) and other engines ship their games: it runs
// on the managed path, where the window's taken surface and input queue must reach libnativewindow.so
// (android_native_window_fixture.c). Once the window has focus, it feeds host input through the launcher's own entry
// point (ManagedLauncher.dispatch, with the event records a window backend reports): key A and a touch the native
// code handles, and key B it leaves to the views, which this Activity logs. Then it finishes.
public class ManagedNativeActivity extends android.app.NativeActivity {
    private boolean sent;

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus || sent) return;
        sent = true;
        Handler main = new Handler(Looper.getMainLooper());
        main.postDelayed(() -> {
            try {
                Method dispatch = ClassLoader.getSystemClassLoader().loadClass("ManagedLauncher")
                        .getDeclaredMethod("dispatch", Activity.class, long[].class);
                dispatch.setAccessible(true);
                for (int code : new int[] {KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_B})
                    for (int action : new int[] {KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP})
                        dispatch.invoke(null, this, key(code, action));
                dispatch.invoke(null, this, touch(MotionEvent.ACTION_DOWN, 40, 50));
                dispatch.invoke(null, this, touch(MotionEvent.ACTION_UP, 40, 50));
                System.out.println("NATIVE_JAVA_INPUT_SENT");
            } catch (Exception error) {
                System.out.println("NATIVE_JAVA_INPUT_FAILED " + error);
            }
            main.postDelayed(this::finish, 1500);
        }, 1000);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        System.out.println("NATIVE_JAVA_KEY " + event.getKeyCode() + " " + event.getAction());
        return super.dispatchKeyEvent(event);
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        System.out.println("NATIVE_JAVA_TOUCH " + event.getActionMasked() + " " + Math.round(event.getX()) + ","
                + Math.round(event.getY()));
        return super.dispatchTouchEvent(event);
    }

    // activity_window.c's op-3 record: type, action, source, device, then the key or pointer words.
    private static long[] key(int code, int action) {
        long[] e = new long[55];
        long now = SystemClock.uptimeMillis() * 1000000L;
        e[0] = 1; e[4] = action; e[5] = InputDevice.SOURCE_KEYBOARD; e[6] = 1; e[8] = code;
        e[11] = now; e[12] = now;
        return e;
    }

    private static long[] touch(int action, float x, float y) {
        long[] e = new long[55];
        long now = SystemClock.uptimeMillis() * 1000000L;
        e[0] = 2; e[4] = action; e[5] = InputDevice.SOURCE_TOUCHSCREEN; e[6] = 1; e[13] = 1;
        e[14] = Float.floatToIntBits(x) & 0xffffffffL; e[15] = Float.floatToIntBits(y) & 0xffffffffL;
        e[16] = Float.floatToIntBits(1f) & 0xffffffffL;
        e[11] = now; e[12] = now;
        return e;
    }
}
