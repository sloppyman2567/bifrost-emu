// Compile-time declaration of a hidden framework class (android-15.0.0_r1, core/java/android/view/InputEventSender.java):
// only what the launcher calls. The launcher build reads Stubs/ as a source path without generating classes from it
// (javac -implicit:none), so the device's own class runs.
package android.view;

import android.os.Looper;

public abstract class InputEventSender {
    public InputEventSender(InputChannel inputChannel, Looper looper) {
        throw new UnsupportedOperationException("stub");
    }

    public void dispose() {
        throw new UnsupportedOperationException("stub");
    }

    public void onInputEventFinished(int seq, boolean handled) {
    }

    public final boolean sendInputEvent(int seq, InputEvent event) {
        throw new UnsupportedOperationException("stub");
    }
}
