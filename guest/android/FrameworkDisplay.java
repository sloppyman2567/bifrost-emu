// One virtual display; real DisplayManagerGlobal owns client listeners and dispatch.
import android.os.Handler;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.Parcelable;
import java.lang.reflect.Array;

final class FrameworkDisplay {
    private static final long CHANGED_MASK = 4; // DisplayManager.EVENT_FLAG_DISPLAY_CHANGED
    private static final int CHANGED_EVENT = 2; // DisplayManagerGlobal.EVENT_DISPLAY_CHANGED
    private Object info;
    private int width, height;
    private IInterface callback;
    private long eventsMask;
    private Handler main;

    void install(FrameworkServices services) throws Exception {
        main = new Handler(Looper.getMainLooper());
        info = displayInfo(width = 480, height = 800);
        services.register("display", "android.hardware.display.IDisplayManager", (method, args) -> {
            switch (method) {
                case "getDisplayInfo": return ((Integer) args[0]) == 0 ? snapshot() : null;
                case "getDisplayIds": return new int[] {0};
                case "registerCallback":
                    register(args[0], 7L); // legacy ADDED | REMOVED | CHANGED
                    return null;
                case "registerCallbackWithEventMask":
                    register(args[0], ((Number) args[1]).longValue());
                    return null;
                case "getPreferredWideGamutColorSpaceId": return 0;
                case "getOverlaySupport":
                    return Class.forName("android.hardware.OverlayProperties")
                            .getMethod("getDefault").invoke(null);
                default: throw new UnsupportedOperationException("display." + method);
            }
        });
    }

    private synchronized Object snapshot() throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            ((Parcelable) info).writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return ((Parcelable.Creator<?>) info.getClass().getField("CREATOR").get(null)).createFromParcel(parcel);
        } finally { parcel.recycle(); }
    }

    private synchronized void register(Object listener, long mask) {
        if (listener == null) throw new IllegalArgumentException("listener must not be null");
        // AOSP retains the first callback per PID and updates its event mask.
        // Every client in this profile belongs to the same guest process.
        if (callback == null) callback = (IInterface) listener;
        eventsMask = mask;
    }

    void setSize(int newWidth, int newHeight) throws Exception {
        if (newWidth <= 0 || newHeight <= 0) throw new IllegalArgumentException("Invalid display dimensions");
        synchronized (this) {
            if (width == newWidth && height == newHeight) return;
            Object next = displayInfo(newWidth, newHeight);
            info = next;
            width = newWidth;
            height = newHeight;
        }
        // Publish the new info before notifying, and never call clients under our lock.
        main.post(() -> {
            IInterface listener;
            synchronized (FrameworkDisplay.this) {
                if ((eventsMask & CHANGED_MASK) == 0) return;
                listener = callback;
            }
            if (listener == null) return;
            long trace = FrameworkTrace.begin("display.callback", "onDisplayEvent", new Object[] {0, CHANGED_EVENT});
            try {
                Class.forName("android.hardware.display.IDisplayManagerCallback")
                        .getMethod("onDisplayEvent", int.class, int.class).invoke(listener, 0, CHANGED_EVENT);
                FrameworkTrace.end(trace, null, null);
            } catch (Exception error) {
                FrameworkTrace.end(trace, null, error);
                System.err.println("DISPLAY_CALLBACK_FAILED " + error.getClass().getName());
            }
        });
    }

    // One 160 dpi, 60 Hz display with id 0: 480x800 unless play mode sizes
    // it to the host window before the app first queries it.
    private static Object displayInfo(int width, int height) throws Exception {
        Class<?> type = Class.forName("android.view.DisplayInfo");
        Object info = type.getConstructor().newInstance();
        type.getField("name").set(info, "Bifrost");
        type.getField("uniqueId").set(info, "bifrost:0");
        for (String field : new String[] {"appWidth", "logicalWidth", "largestNominalAppWidth"})
            type.getField(field).setInt(info, width);
        for (String field : new String[] {"appHeight", "logicalHeight", "largestNominalAppHeight"})
            type.getField(field).setInt(info, height);
        type.getField("smallestNominalAppWidth").setInt(info, Math.min(width, height));
        type.getField("smallestNominalAppHeight").setInt(info, Math.min(width, height));
        type.getField("logicalDensityDpi").setInt(info, 160);
        type.getField("physicalXDpi").setFloat(info, 160);
        type.getField("physicalYDpi").setFloat(info, 160);
        type.getField("state").setInt(info, 2);  // Display.STATE_ON
        type.getField("type").setInt(info, 1);   // Display.TYPE_INTERNAL
        Class<?> modeType = Class.forName("android.view.Display$Mode");
        Object mode = modeType.getConstructor(int.class, int.class, int.class, float.class)
                .newInstance(1, width, height, 60f);
        Object modes = Array.newInstance(modeType, 1);
        Array.set(modes, 0, mode);
        type.getField("supportedModes").set(info, modes);
        type.getField("appsSupportedModes").set(info, modes);
        type.getField("modeId").setInt(info, 1);
        type.getField("defaultModeId").setInt(info, 1);
        return info;
    }

}
