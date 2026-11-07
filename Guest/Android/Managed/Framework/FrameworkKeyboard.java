// The virtual keyboard's KeyCharacterMap: Android 15's Virtual.kcm (FrameworkKeyLayout).
// KeyCharacterMap.load falls back to this device for every device id, so its type
// decides how a key reaches a text field: FULL gives TextKeyListener its QWERTY
// listener (Backspace, Delete and arrows edit) and KeyEvent.getUnicodeChar a character.
// A gamepad's map is Generic.kcm, whose fallbacks (B is BACK, A DPAD_CENTER) apply to
// the buttons an app leaves unhandled.
import android.os.Parcel;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

final class FrameworkKeyboard {

    // A .kcm key label is the AKEYCODE_ name without its prefix; keyCodeFromString
    // reads the native label table only for the KEYCODE_ spelling.
    private static int keyCode(String label) {
        int code = KeyEvent.keyCodeFromString("KEYCODE_" + label);
        if (code <= 0) throw new IllegalStateException("Unknown key label " + label);
        return code;
    }

    // libinput's KeyCharacterMap::writeToParcel (android-15.0.0_r1) in the native
    // Parcel encoding: the file name as a padded C string, then 32-bit words.
    static KeyCharacterMap characterMap() {
        return characterMap("/system/usr/keychars/Virtual.kcm", FrameworkKeyLayout.TYPE, FrameworkKeyLayout.KEYS);
    }

    static KeyCharacterMap gamepadCharacterMap() {
        return characterMap("/system/usr/keychars/Generic.kcm", FrameworkKeyLayout.GAMEPAD_TYPE,
                FrameworkKeyLayout.GAMEPAD_KEYS);
    }

    private static KeyCharacterMap characterMap(String loadFile, int type, String[] keys) {
        ByteBuffer out = ByteBuffer.allocate(1 << 17).order(ByteOrder.LITTLE_ENDIAN);
        byte[] name = loadFile.getBytes(StandardCharsets.UTF_8);
        out.put(name).put(new byte[((name.length + 4) & ~3) - name.length]);
        out.putInt(type).putInt(0).putInt(keys.length);
        for (String row : keys) {
            String[] parts = row.split(" \\| ");
            String[] key = parts[0].split(" ");
            out.putInt(keyCode(key[0])).putInt(Integer.parseInt(key[1])).putInt(Integer.parseInt(key[2]));
            for (int i = 1; i < parts.length; i++) {
                String[] behavior = parts[i].split(" ");
                out.putInt(1).putInt(Integer.parseInt(behavior[0])).putInt(Integer.parseInt(behavior[1]))
                        .putInt(behavior[2].equals("-") ? 0 : keyCode(behavior[2]))
                        .putInt(behavior[3].equals("-") ? 0 : keyCode(behavior[3]));
            }
            out.putInt(0);
        }
        out.putInt(0).putInt(0).putInt(0);  // no key remapping, scan code or usage code tables
        Parcel parcel = Parcel.obtain();
        try {
            parcel.unmarshall(out.array(), 0, out.position());
            parcel.setDataPosition(0);
            return KeyCharacterMap.CREATOR.createFromParcel(parcel);
        } finally {
            parcel.recycle();
        }
    }
}
