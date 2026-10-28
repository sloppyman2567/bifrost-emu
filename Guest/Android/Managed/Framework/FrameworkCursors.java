// Pointer icons for a mouse-driven window. Android resolves the icon in ViewRootImpl's input stages
// (ViewPostImeInputStage.maybeUpdatePointerIcon, then updatePointerIcon: View.onResolvePointerIcon, the
// default arrow when it returns null, reported only when the icon changes). Host input here bypasses
// those stages, so the play loop asks the Activity's view tree after each mouse hover and shows the
// nearest host system cursor. An app's own bitmap cursor (TYPE_CUSTOM) is not carried: it shows the arrow.
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.View;
import java.lang.reflect.Method;
import java.util.function.IntUnaryOperator;
import java.util.function.Supplier;

final class FrameworkCursors {
    // BIFROST_WINDOW_CURSOR_* (Include/Bifrost/AndroidWindowPlugin.h).
    static final int DEFAULT = 0, HIDDEN = 1, TEXT = 2, POINTER = 3, WAIT = 4, CROSSHAIR = 5, MOVE = 6,
            NOT_ALLOWED = 7, EW_RESIZE = 8, NS_RESIZE = 9, NESW_RESIZE = 10, NWSE_RESIZE = 11;

    private final IntUnaryOperator show;  // sends one shape to the host: 0 or a negative errno
    private int shown = DEFAULT;          // the host window starts with the arrow
    private boolean unavailable;

    FrameworkCursors(IntUnaryOperator show) {
        this.show = show;
    }

    /** The host cursor nearest a PointerIcon type; the arrow for any type without a closer match. */
    static int hostCursor(int type) {
        switch (type) {
            case PointerIcon.TYPE_NULL: return HIDDEN;
            case PointerIcon.TYPE_TEXT:
            case PointerIcon.TYPE_VERTICAL_TEXT: return TEXT;
            case PointerIcon.TYPE_HAND: return POINTER;
            case PointerIcon.TYPE_WAIT: return WAIT;
            case PointerIcon.TYPE_CROSSHAIR:
            case PointerIcon.TYPE_CELL: return CROSSHAIR;
            case PointerIcon.TYPE_ALL_SCROLL:
            case PointerIcon.TYPE_GRAB:
            case PointerIcon.TYPE_GRABBING: return MOVE;
            case PointerIcon.TYPE_NO_DROP: return NOT_ALLOWED;
            case PointerIcon.TYPE_HORIZONTAL_DOUBLE_ARROW: return EW_RESIZE;
            case PointerIcon.TYPE_VERTICAL_DOUBLE_ARROW: return NS_RESIZE;
            case PointerIcon.TYPE_TOP_RIGHT_DIAGONAL_DOUBLE_ARROW: return NESW_RESIZE;
            case PointerIcon.TYPE_TOP_LEFT_DIAGONAL_DOUBLE_ARROW: return NWSE_RESIZE;
            default: return DEFAULT;
        }
    }

    /** After a mouse hover over the Activity: show the pointer icon its view tree resolves there. */
    void hover(Supplier<View> decorView, MotionEvent event) {
        if (unavailable) return;
        try {
            final View decor = decorView.get();  // null until the Activity's window has a decor view
            if (decor == null) return;
            // A touchscreen never hovers, so a hover is the mouse; views resolve icons for mouse events.
            MotionEvent mouse = MotionEvent.obtain(event);
            PointerIcon icon;
            try {
                mouse.setSource(InputDevice.SOURCE_MOUSE);
                icon = decor.onResolvePointerIcon(mouse, 0);
            } finally {
                mouse.recycle();
            }
            final int cursor = icon == null ? DEFAULT : hostCursor(typeOf(icon));
            if (cursor == shown) return;
            final int error = show.applyAsInt(cursor);
            if (error != 0) throw new IllegalStateException("the window refused cursor " + cursor + ": " + error);
            shown = cursor;
        } catch (Throwable problem) {
            // Pointer icons are cosmetic: never let one break input delivery.
            unavailable = true;
            System.out.println("APP_CURSOR_UNAVAILABLE " + problem);
        }
    }

    private static Method getType;

    private static int typeOf(PointerIcon icon) throws ReflectiveOperationException {
        if (getType == null) getType = PointerIcon.class.getMethod("getType");  // @hide
        return (Integer) getType.invoke(icon);
    }
}
