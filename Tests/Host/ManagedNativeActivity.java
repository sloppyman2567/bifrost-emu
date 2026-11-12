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
                Class<?> launcher = ClassLoader.getSystemClassLoader().loadClass("ManagedLauncher");
                Method dispatch = launcher.getDeclaredMethod("dispatch", Activity.class, long[].class);
                dispatch.setAccessible(true);
                for (int code : new int[] {KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_B})
                    for (int action : new int[] {KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP})
                        dispatch.invoke(null, this, key(code, action, InputDevice.SOURCE_KEYBOARD, 1));
                dispatch.invoke(null, this, touch(MotionEvent.ACTION_DOWN, 40, 50));
                dispatch.invoke(null, this, touch(MotionEvent.ACTION_UP, 40, 50));
                // A pad connects (the play loop's 0x101 record), then its B button and its d-pad, a hat axis.
                java.lang.reflect.Field field = launcher.getDeclaredField("gamepads");
                field.setAccessible(true);
                Object gamepads = field.get(null);
                Method connected = gamepads.getClass().getDeclaredMethod("deviceEvent", long[].class, int.class);
                connected.setAccessible(true);
                long[] pad = new long[55];
                pad[0] = 0x101; pad[4] = 1; pad[6] = PAD; pad[7] = 0x045e; pad[8] = 0x028e; pad[9] = 1;
                pad[13] = 3; pad[14] = 'P' | 'a' << 8 | 'd' << 16;
                connected.invoke(gamepads, pad, android.view.KeyCharacterMap.VIRTUAL_KEYBOARD);
                int buttons = InputDevice.SOURCE_KEYBOARD | InputDevice.SOURCE_GAMEPAD;
                dispatch.invoke(null, this, key(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.ACTION_DOWN, buttons, PAD));
                dispatch.invoke(null, this, key(KeyEvent.KEYCODE_BUTTON_B, KeyEvent.ACTION_UP, buttons, PAD));
                dispatch.invoke(null, this, hat(-1f));
                // Released a moment later: moves sent back to back reach the window as one batched event, as on Android.
                main.postDelayed(() -> {
                    try {
                        dispatch.invoke(null, this, hat(0f));
                        System.out.println("NATIVE_JAVA_INPUT_SENT");
                    } catch (Exception error) {
                        System.out.println("NATIVE_JAVA_INPUT_FAILED " + error);
                    }
                }, 300);
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
    private static final int PAD = 2;

    private static long[] key(int code, int action, int source, int device) {
        long[] e = new long[55];
        long now = SystemClock.uptimeMillis() * 1000000L;
        e[0] = 1; e[4] = action; e[5] = source; e[6] = device; e[8] = code;
        e[11] = now; e[12] = now;
        return e;
    }

    // A gamepad axis record: the axes from word 20 in BIFROST_GAMEPAD_AXIS_* order, HAT_X seventh.
    private static long[] hat(float x) {
        long[] e = new long[55];
        long now = SystemClock.uptimeMillis() * 1000000L;
        e[0] = 2; e[5] = InputDevice.SOURCE_JOYSTICK; e[6] = PAD; e[13] = 1;
        e[20 + 6] = Float.floatToIntBits(x) & 0xffffffffL;
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
