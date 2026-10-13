// ClipboardService ("clipboard") for user 0 on the default device. Clips stay
// in this process; the host desktop clipboard is not shared. Reads require
// window focus (clipboardAccessAllowed's OP_READ_CLIPBOARD rule for apps),
// which the play window holds while open. AOSP's auto-clear removes a clip
// an hour after it was last written or read. ClipData crosses the local
// Binder by object reference, so it is copied through a Parcel both ways.
import android.content.ClipData;
import android.content.ClipDescription;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.BooleanSupplier;

final class FrameworkClipboard implements FrameworkServices.Call {
    private static final long AUTO_CLEAR_MILLIS = 3600000;  // DEFAULT_CLIPBOARD_TIMEOUT_MILLIS

    private enum Contract {
        setPrimaryClip,
        clearPrimaryClip,
        getPrimaryClip,
        getPrimaryClipDescription,
        hasPrimaryClip,
        hasClipboardText,
        addPrimaryClipChangedListener,
        removePrimaryClipChangedListener
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final BooleanSupplier focused;
    private final Map<IBinder, Object> listeners = new LinkedHashMap<>();
    private ClipData clip;
    private long expires;

    FrameworkClipboard(FrameworkPackages packages, BooleanSupplier focused) {
        this.packages = packages; this.focused = focused;
    }

    void install(FrameworkServices services) throws Exception {
        services.register("clipboard", "android.content.IClipboard", this);
    }

    private static ClipData copy(ClipData value) {
        if (value == null) return null;
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return ClipData.CREATOR.createFromParcel(parcel);
        } finally { parcel.recycle(); }
    }

    // getIntendingUid: the package must be the caller's and the user its own.
    private void caller(Object pkg, Object userId) {
        if (pkg == null || packages.packageUid((String) pkg) != Process.myUid())
            throw new SecurityException("Package " + pkg + " does not belong to " + Process.myUid());
        if ((Integer) userId != 0) throw new SecurityException("Clipboard of another user");
    }

    private ClipData current() {
        if (clip != null && System.currentTimeMillis() >= expires) clip = null;
        return clip;
    }

    private void set(ClipData value) throws Exception {
        clip = value;
        expires = System.currentTimeMillis() + AUTO_CLEAR_MILLIS;
        for (Object listener : listeners.values().toArray()) {
            try { listener.getClass().getMethod("dispatchPrimaryClipChanged").invoke(listener); }
            catch (java.lang.reflect.InvocationTargetException error) {
                if (!(error.getCause() instanceof android.os.RemoteException)) throw error;
            }
        }
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("clipboard." + method); }
        switch (contract) {
            case setPrimaryClip: {
                ClipData value = (ClipData) a[0];
                if (value == null || value.getItemCount() <= 0) throw new IllegalArgumentException("No items");
                caller(a[1], a[3]);
                ClipData stored = copy(value);
                stored.getDescription().getClass().getMethod("setTimestamp", long.class)
                        .invoke(stored.getDescription(), System.currentTimeMillis());
                set(stored);
                return null;
            }
            case clearPrimaryClip: caller(a[0], a[2]); set(null); return null;
            case getPrimaryClip: {
                caller(a[0], a[2]);
                if (!focused.getAsBoolean() || current() == null) return null;
                expires = System.currentTimeMillis() + AUTO_CLEAR_MILLIS;
                return copy(clip);
            }
            case getPrimaryClipDescription: {
                caller(a[0], a[2]);
                if (!focused.getAsBoolean() || current() == null) return null;
                return new ClipDescription(clip.getDescription());
            }
            case hasPrimaryClip: caller(a[0], a[2]); return focused.getAsBoolean() && current() != null;
            case hasClipboardText: {
                caller(a[0], a[2]);
                if (!focused.getAsBoolean() || current() == null) return false;
                CharSequence text = clip.getItemAt(0).getText();
                return text != null && text.length() > 0;
            }
            case addPrimaryClipChangedListener: {
                caller(a[1], a[3]);
                IBinder binder = (IBinder) a[0].getClass().getMethod("asBinder").invoke(a[0]);
                if (listeners.size() >= 256 && !listeners.containsKey(binder))
                    throw new IllegalStateException("Clipboard listener limit");
                listeners.put(binder, a[0]);
                return null;
            }
            case removePrimaryClipChangedListener: {
                caller(a[1], a[3]);
                listeners.remove(a[0].getClass().getMethod("asBinder").invoke(a[0]));
                return null;
            }
            // Source attribution and access-notification settings are
            // privileged (SET_CLIP_SOURCE, MANAGE_CLIPBOARD_ACCESS_NOTIFICATION).
            default: throw new UnsupportedOperationException("clipboard." + method);
        }
    }
}
