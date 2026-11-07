// Host gamepads as Android 15 input devices (android-15.0.0_r1). The host reports each one like an
// Xbox 360 pad (Vendor_045e_Product_028e.kl): KEYCODE_BUTTON_* keys, sticks X/Y and Z/RZ, triggers
// and the d-pad as hat axes. This class is the device side InputManagerService and InputDispatcher
// keep on a device, because host input reaches the Activity without ViewRootImpl's input stages:
//  - InputManager's device list and its change listener (IInputDevicesChangedListener pairs),
//  - an InputDevice per pad (KEYBOARD|GAMEPAD|JOYSTICK, Generic.kcm, the pad's motion ranges),
//  - key repeat and Generic.kcm fallbacks for buttons the app leaves unhandled (B is BACK),
//  - ViewRootImpl.SyntheticJoystickHandler: unhandled stick and hat motion as d-pad keys.
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewConfiguration;
import java.lang.reflect.Method;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Map;
import java.util.TreeMap;

final class FrameworkGamepads {
    interface KeyTarget { boolean dispatch(KeyEvent event) throws Exception; }

    // InputReader's sources for a pad: keys from KEYBOARD|GAMEPAD, axes from JOYSTICK.
    static final int SOURCES = InputDevice.SOURCE_KEYBOARD | InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK;
    // The op-3 record's axis slots (BIFROST_GAMEPAD_AXIS_*), from word 20.
    private static final int[] AXES = {MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z,
            MotionEvent.AXIS_RZ, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
            MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y};
    private static final int FIRST_AXIS_WORD = 20;

    private static final class Pad {
        int id, controller, vendor, product;
        String name;
        InputDevice device;
    }

    private final Map<Integer, Pad> pads = new TreeMap<>();
    private final ArrayList<Object> listeners = new ArrayList<>();
    // Made on first use: ManagedLauncher builds this class before the main Looper exists.
    private Handler handler;
    private KeyCharacterMap characterMap;
    // InputDispatcher: the fallback each held key was turned into, and the held key that repeats.
    private final Map<Integer, Integer> fallbacks = new TreeMap<>();
    private Runnable repeat;

    synchronized int[] deviceIds(int virtualKeyboard) {
        int[] ids = new int[pads.size() + 1];
        ids[0] = virtualKeyboard;
        int i = 1;
        for (int id : pads.keySet()) ids[i++] = id;
        return ids;
    }

    // The keys a pad reports (Vendor_045e_Product_028e.kl: the triggers are axes only). IInputManager.hasKeys answers from this:
    // Chromium's WebView, in an ad, lists the gamepads on its first hover event and asks each which buttons it has, and an
    // exception out of that kills the app.
    private static final int[] PAD_KEYS = {KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_THUMBL,
            KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_BUTTON_MODE};

    // IInputManager.hasKeys: `exists[i]` says whether the device has key `keyCodes[i]`. False for a device that does not exist
    // (nothing is written then), as InputManagerService answers. The virtual keyboard has every key except the gamepad buttons.
    synchronized boolean hasKeys(int id, int[] keyCodes, boolean[] exists, int virtualKeyboard) {
        final boolean keyboard = id == virtualKeyboard;
        if (!keyboard && !pads.containsKey(id)) return false;
        for (int i = 0; i < keyCodes.length && i < exists.length; i++) {
            boolean pad = false;
            for (int key : PAD_KEYS) pad |= key == keyCodes[i];
            exists[i] = keyboard ? keyCodes[i] > 0 && !KeyEvent.isGamepadButton(keyCodes[i]) : pad;
        }
        return true;
    }

    synchronized InputDevice device(int id) throws Exception {
        Pad pad = pads.get(id);
        if (pad == null) return null;
        if (pad.device == null) pad.device = build(pad);
        return pad.device;
    }

    // IInputManager.registerInputDevicesChangedListener: InputManagerGlobal registers one per process.
    synchronized void register(Object listener) {
        if (listener != null) listeners.add(listener);
    }

    // An op-3 device record: word 4 action (1 added, 0 removed), 6 the device id, 7 and 8 the USB ids,
    // 9 the controller number, 13 the name's length and its bytes from word 14.
    void deviceEvent(long[] e, int virtualKeyboard) throws Exception {
        int[] pairs;
        Object[] targets;
        synchronized (this) {
            int id = (int) e[6];
            if (e[4] == 1) {
                Pad pad = new Pad();
                pad.id = id;
                pad.vendor = (int) e[7];
                pad.product = (int) e[8];
                pad.controller = (int) e[9];
                byte[] bytes = new byte[(int) Math.max(0, Math.min(e[13], 255))];
                for (int i = 0; i < bytes.length; i++) bytes[i] = (byte) (e[14 + i / 8] >>> (8 * (i % 8)));
                pad.name = new String(bytes, java.nio.charset.StandardCharsets.UTF_8);
                pads.put(id, pad);
                System.out.println("APP_GAMEPAD_ADDED " + id + " " + pad.name);
            } else if (pads.remove(id) != null) {
                System.out.println("APP_GAMEPAD_REMOVED " + id);
            } else {
                return;
            }
            // (id, generation) of every device; a pad never changes, so its generation stays 1.
            int[] ids = deviceIds(virtualKeyboard);
            pairs = new int[ids.length * 2];
            for (int i = 0; i < ids.length; i++) {
                pairs[2 * i] = ids[i];
                pairs[2 * i + 1] = ids[i] == virtualKeyboard ? 0 : 1;
            }
            targets = listeners.toArray();
        }
        // Through the AIDL interface: InputManagerGlobal's listener class is private.
        Method changed = Class.forName("android.hardware.input.IInputDevicesChangedListener")
                .getMethod("onInputDevicesChanged", int[].class);
        for (Object listener : targets) changed.invoke(listener, (Object) pairs);
    }

    // A pad's button: the app first, then its Generic.kcm fallback when the press went unhandled
    // (InputDispatcher::afterKeyEventLockedInterruptable); held buttons repeat like InputDispatcher's.
    void key(KeyEvent event, KeyTarget target) throws Exception {
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) startRepeat(event, target);
        if (event.getAction() == KeyEvent.ACTION_UP) stopRepeat();
        deliver(event, target);
    }

    private void deliver(KeyEvent event, KeyTarget target) throws Exception {
        int code = event.getKeyCode();
        boolean handled = target.dispatch(event);
        Integer fallback = fallbacks.get(code);
        if (event.getAction() == KeyEvent.ACTION_UP) {
            if (fallback == null) return;
            fallbacks.remove(code);
            target.dispatch(new KeyEvent(event.getDownTime(), event.getEventTime(), KeyEvent.ACTION_UP, fallback, 0,
                    event.getMetaState(), event.getDeviceId(), 0, event.getFlags() | KeyEvent.FLAG_FALLBACK,
                    event.getSource()));
            return;
        }
        if (handled && fallback == null) return;
        if (fallback == null) {
            fallback = fallbackKey(code, event.getMetaState());
            // HOME leaves the app on a device; here it would only confuse the Activity.
            if (fallback == null || fallback == KeyEvent.KEYCODE_HOME) return;
            fallbacks.put(code, fallback);
        }
        target.dispatch(new KeyEvent(event.getDownTime(), event.getEventTime(), KeyEvent.ACTION_DOWN, fallback,
                event.getRepeatCount(), event.getMetaState(), event.getDeviceId(), 0,
                event.getFlags() | KeyEvent.FLAG_FALLBACK, event.getSource()));
    }

    private Handler handler() {
        if (handler == null) handler = new Handler(Looper.getMainLooper());
        return handler;
    }

    private Integer fallbackKey(int code, int metaState) throws Exception {
        if (characterMap == null) characterMap = FrameworkKeyboard.gamepadCharacterMap();
        Object action = KeyCharacterMap.class.getMethod("getFallbackAction", int.class, int.class)
                .invoke(characterMap, code, metaState);
        if (action == null) return null;
        int fallback = action.getClass().getField("keyCode").getInt(action);
        action.getClass().getMethod("recycle").invoke(action);
        return fallback == KeyEvent.KEYCODE_UNKNOWN ? null : fallback;
    }

    private void startRepeat(KeyEvent down, KeyTarget target) {
        stopRepeat();
        repeat = new Runnable() {
            KeyEvent last = down;
            @Override
            public void run() {
                if (repeat != this) return;
                last = KeyEvent.changeTimeRepeat(last, SystemClock.uptimeMillis(), last.getRepeatCount() + 1);
                try { deliver(last, target); } catch (Exception error) { throw new RuntimeException(error); }
                handler().postDelayed(this, ViewConfiguration.getKeyRepeatDelay());
            }
        };
        handler().postDelayed(repeat, ViewConfiguration.getKeyRepeatTimeout());
    }

    private void stopRepeat() {
        if (repeat != null) handler().removeCallbacks(repeat);
        repeat = null;
    }

    // A joystick motion from an op-3 record: one pointer whose coordinates carry the pad's axes.
    static MotionEvent motion(long[] e) {
        final long ms = 1000000;
        MotionEvent.PointerProperties[] properties = {new MotionEvent.PointerProperties()};
        properties[0].id = 0;
        properties[0].toolType = MotionEvent.TOOL_TYPE_UNKNOWN;
        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        for (int i = 0; i < AXES.length; i++)
            coords[0].setAxisValue(AXES[i], Float.intBitsToFloat((int) e[FIRST_AXIS_WORD + i]));
        return MotionEvent.obtain(e[11] / ms, e[12] / ms, MotionEvent.ACTION_MOVE, 1, properties, coords, 0, 0,
                1f, 1f, (int) e[6], 0, (int) e[5], 0);
    }

    // ViewRootImpl's SyntheticInputStage: a joystick motion the app did not handle moves the d-pad focus;
    // one it handled cancels any synthetic key still held.
    void joystick(MotionEvent event, boolean handled, KeyTarget target, java.util.function.BooleanSupplier focused)
            throws Exception {
        if (handled) { joystickCancel(target); return; }
        long time = event.getEventTime();
        joystickAxis(event, time, MotionEvent.AXIS_X, target, focused);
        joystickAxis(event, time, MotionEvent.AXIS_Y, target, focused);
        joystickAxis(event, time, MotionEvent.AXIS_HAT_X, target, focused);
        joystickAxis(event, time, MotionEvent.AXIS_HAT_Y, target, focused);
    }

    private static final int STATE_UP_OR_LEFT = -1, STATE_NEUTRAL = 0, STATE_DOWN_OR_RIGHT = 1;
    private final int[] hatStates = {STATE_NEUTRAL, STATE_NEUTRAL}, stickStates = {STATE_NEUTRAL, STATE_NEUTRAL};
    private final Map<Integer, KeyEvent> joystickUps = new TreeMap<>();
    private final Runnable[] joystickRepeats = new Runnable[2];

    private void joystickAxis(MotionEvent event, long time, int axis, KeyTarget target,
                              java.util.function.BooleanSupplier focused) throws Exception {
        boolean x = axis == MotionEvent.AXIS_X || axis == MotionEvent.AXIS_HAT_X;
        int index = x ? 0 : 1;
        boolean stick = axis == MotionEvent.AXIS_X || axis == MotionEvent.AXIS_Y;
        float value = event.getAxisValue(axis);
        int next = value >= 0.5f ? STATE_DOWN_OR_RIGHT : value <= -0.5f ? STATE_UP_OR_LEFT : STATE_NEUTRAL;
        int current = stick ? stickStates[index] : hatStates[index];
        if (current == next) return;
        int meta = event.getMetaState(), device = event.getDeviceId(), source = event.getSource();
        if (current != STATE_NEUTRAL) {
            target.dispatch(new KeyEvent(time, time, KeyEvent.ACTION_UP, dpadKey(x, current), 0, meta, device, 0,
                    KeyEvent.FLAG_FALLBACK, source));
            joystickUps.put(device, null);
            if (joystickRepeats[index] != null) handler().removeCallbacks(joystickRepeats[index]);
            joystickRepeats[index] = null;
        }
        if (next != STATE_NEUTRAL) {
            int code = dpadKey(x, next);
            KeyEvent down = new KeyEvent(time, time, KeyEvent.ACTION_DOWN, code, 0, meta, device, 0,
                    KeyEvent.FLAG_FALLBACK, source);
            target.dispatch(down);
            Runnable again = new Runnable() {
                KeyEvent last = down;
                @Override
                public void run() {
                    if (joystickRepeats[index] != this || !focused.getAsBoolean()) return;
                    last = KeyEvent.changeTimeRepeat(last, SystemClock.uptimeMillis(), last.getRepeatCount() + 1);
                    try { target.dispatch(last); } catch (Exception error) { throw new RuntimeException(error); }
                    handler().postDelayed(this, ViewConfiguration.getKeyRepeatDelay());
                }
            };
            joystickRepeats[index] = again;
            handler().postDelayed(again, ViewConfiguration.getKeyRepeatTimeout());
            joystickUps.put(device, new KeyEvent(time, time, KeyEvent.ACTION_UP, code, 0, meta, device, 0,
                    KeyEvent.FLAG_FALLBACK | KeyEvent.FLAG_CANCELED, source));
        }
        if (stick) stickStates[index] = next; else hatStates[index] = next;
    }

    private void joystickCancel(KeyTarget target) throws Exception {
        for (int i = 0; i < 2; i++) {
            if (joystickRepeats[i] != null) handler().removeCallbacks(joystickRepeats[i]);
            joystickRepeats[i] = null;
        }
        for (KeyEvent up : joystickUps.values())
            if (up != null) target.dispatch(KeyEvent.changeTimeRepeat(up, SystemClock.uptimeMillis(), 0));
        joystickUps.clear();
        hatStates[0] = hatStates[1] = stickStates[0] = stickStates[1] = STATE_NEUTRAL;
    }

    private static int dpadKey(boolean x, int state) {
        if (x) return state == STATE_UP_OR_LEFT ? KeyEvent.KEYCODE_DPAD_LEFT : KeyEvent.KEYCODE_DPAD_RIGHT;
        return state == STATE_UP_OR_LEFT ? KeyEvent.KEYCODE_DPAD_UP : KeyEvent.KEYCODE_DPAD_DOWN;
    }

    // InputDevice.Builder is hidden; reached by reflection like the virtual keyboard's.
    private InputDevice build(Pad pad) throws Exception {
        Object builder = Class.forName("android.view.InputDevice$Builder").getConstructor().newInstance();
        set(builder, "setId", int.class, pad.id);
        set(builder, "setGeneration", int.class, 1);
        set(builder, "setControllerNumber", int.class, pad.controller);
        set(builder, "setName", String.class, pad.name);
        set(builder, "setVendorId", int.class, pad.vendor);
        set(builder, "setProductId", int.class, pad.product);
        set(builder, "setDescriptor", String.class, descriptor(pad));
        set(builder, "setExternal", boolean.class, true);
        set(builder, "setSources", int.class, SOURCES);
        set(builder, "setKeyboardType", int.class, InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC);
        if (characterMap == null) characterMap = FrameworkKeyboard.gamepadCharacterMap();
        set(builder, "setKeyCharacterMap", KeyCharacterMap.class, characterMap);
        Method range = builder.getClass().getMethod("addMotionRange", int.class, int.class, float.class,
                float.class, float.class, float.class, float.class);
        // JoystickInputMapper's normalized ranges; the sticks keep the key layout's flat 4096 of 32767.
        for (int axis : AXES) {
            boolean trigger = axis == MotionEvent.AXIS_LTRIGGER || axis == MotionEvent.AXIS_RTRIGGER;
            boolean hat = axis == MotionEvent.AXIS_HAT_X || axis == MotionEvent.AXIS_HAT_Y;
            range.invoke(builder, axis, InputDevice.SOURCE_JOYSTICK, trigger ? 0f : -1f, 1f,
                    trigger || hat ? 0f : 4096f / 32767f, 0f, 0f);
        }
        return (InputDevice) builder.getClass().getMethod("build").invoke(builder);
    }

    private static void set(Object builder, String method, Class<?> type, Object value) throws Exception {
        builder.getClass().getMethod(method, type).invoke(builder, value);
    }

    // EventHub's descriptor is a SHA-1 of the device's identity; this one is stable for a pad's connection.
    private static String descriptor(Pad pad) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-1").digest(
                (pad.vendor + ":" + pad.product + ":" + pad.name + ":" + pad.id).getBytes("UTF-8"));
        StringBuilder text = new StringBuilder();
        for (byte b : digest) text.append(String.format("%02x", b & 0xff));
        return text.toString();
    }
}
