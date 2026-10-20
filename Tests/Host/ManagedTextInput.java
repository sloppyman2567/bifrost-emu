package org.bifrost.fixture;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

/**
 * A focused EditText receives the host's text through its InputConnection. The
 * window's BIFROST_TEXT_SCRIPT presses H (which produced "h"), commits "é",
 * composes "ni", replaces it with a commit ("你"), presses Backspace and
 * commits "!". Prints the text as code points.
 */
public final class ManagedTextInput extends Activity {
    private static final String EXPECTED = "hé!";
    private final Handler main = new Handler();
    private EditText edit;
    private boolean composed;

    private static String points(CharSequence text) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length(); ) {
            int point = Character.codePointAt(text, i);
            out.append(Integer.toHexString(point)).append(' ');
            i += Character.charCount(point);
        }
        return out.toString().trim();
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        // Any device id falls back to the virtual keyboard, which is Virtual.kcm: type FULL,
        // US QWERTY. An empty map (type 0) makes TextKeyListener ignore Backspace.
        KeyCharacterMap map = KeyCharacterMap.load(1);
        if (map.getKeyboardType() == KeyCharacterMap.FULL && map.get(KeyEvent.KEYCODE_A, 0) == 'a'
                && map.get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON) == 'A')
            System.out.println("MANAGED_TEXT_INPUT_KEYMAP_OK");
        FrameLayout content = new FrameLayout(this);
        edit = new EditText(this);
        edit.setSingleLine(true);
        content.addView(edit, new FrameLayout.LayoutParams(600, 160));
        setContentView(content);
        edit.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable text) {
                int composing = BaseInputConnection.getComposingSpanStart(text);
                System.out.println("MANAGED_TEXT_INPUT_TEXT " + points(text) + " composing=" + composing);
                // "hé" is followed by the composition "ni", underlined.
                if (composing == 2 && text.toString().equals("hé" + "ni") && !composed) {
                    composed = true;
                    System.out.println("MANAGED_TEXT_INPUT_COMPOSING");
                }
                if (text.toString().equals(EXPECTED)) {
                    System.out.println("MANAGED_TEXT_INPUT_OK");
                    main.post(ManagedTextInput.this::finish);
                }
            }
        });
    }

    @Override public void onResume() {
        super.onResume();
        edit.requestFocus();
        // As an app opening a text field does.
        main.postDelayed(() -> getSystemService(InputMethodManager.class).showSoftInput(edit, 0), 300);
        main.postDelayed(() -> {
            if (!isFinishing()) {
                System.out.println("MANAGED_TEXT_INPUT_TIMEOUT " + points(edit.getText()));
                finish();
            }
        }, 25000);
    }

    @Override public void onDestroy() {
        super.onDestroy();
        System.out.println("MANAGED_TEXT_INPUT_DESTROYED");
    }
}
