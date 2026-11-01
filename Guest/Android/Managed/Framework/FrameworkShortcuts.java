// ShortcutService ("shortcut", android.content.pm.IShortcutService) for a device with no home screen: no launcher
// shows or pins a shortcut, and ShortcutManager is null without this service. Nothing is stored, so reads are empty
// and removals validate their arguments and find nothing, as Android 15's service does for an unknown id. Publishing
// a shortcut stays UnsupportedOperationException rather than a success that a later read would contradict.
import android.os.Process;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

final class FrameworkShortcuts implements FrameworkServices.Call {
    private enum Contract {
        getMaxShortcutCountPerActivity, getShortcuts, isRequestPinItemSupported,
        removeAllDynamicShortcuts, removeDynamicShortcuts, reportShortcutUsed
    }
    // ShortcutService.DEFAULT_MAX_SHORTCUTS_PER_ACTIVITY.
    private static final int MAX_SHORTCUTS_PER_ACTIVITY = 15;

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkShortcuts(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("shortcut", "android.content.pm.IShortcutService", this);
    }

    private static void verifyUser(int userId) {
        if (userId != Process.myUid() / 100000) throw new SecurityException("Invalid user-ID");
    }

    // ShortcutService.verifyCaller: a non-empty package of the caller's own, on the caller's user.
    private void verifyCaller(String packageName, int userId) {
        notEmpty(packageName, "packageName");
        verifyUser(userId);
        if (packages.packageUid(packageName) != Process.myUid())
            throw new SecurityException("Calling package name mismatch");
    }

    private static void notEmpty(String value, String name) {
        if (value == null || value.isEmpty()) throw new IllegalArgumentException(name + " must not be empty");
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("shortcut." + method); }
        switch (contract) {
            case isRequestPinItemSupported:
                // ShortcutRequestPinProcessor: pinning needs a default launcher that confirms requests.
                verifyUser((Integer) a[0]);
                return false;
            case getShortcuts:
                verifyCaller((String) a[0], (Integer) a[2]);
                return Class.forName("android.content.pm.ParceledListSlice")
                        .getConstructor(List.class).newInstance(new ArrayList<Object>());
            case removeDynamicShortcuts:
                verifyCaller((String) a[0], (Integer) a[2]);
                for (Object id : (List<?>) Objects.requireNonNull(a[1], "shortcutIds must be provided"))
                    notEmpty((String) id, "shortcutId");
                return null;
            case removeAllDynamicShortcuts:
                verifyCaller((String) a[0], (Integer) a[1]);
                return null;
            case reportShortcutUsed:
                verifyCaller((String) a[0], (Integer) a[2]);
                Objects.requireNonNull(a[1], "shortcutId must be provided");
                return null;
            case getMaxShortcutCountPerActivity:
                verifyCaller((String) a[0], (Integer) a[1]);
                return MAX_SHORTCUTS_PER_ACTIVITY;
            default: throw new UnsupportedOperationException("shortcut." + method);
        }
    }
}
