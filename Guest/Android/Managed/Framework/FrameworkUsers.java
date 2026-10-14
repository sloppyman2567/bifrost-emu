// UserManagerService for the runner's one full, unlocked system user. Keep
// Android 15's per-method permission gates and redact app-visible profiles.
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.os.Bundle;
import android.os.Process;
import java.util.ArrayList;
import java.util.Objects;

final class FrameworkUsers implements FrameworkServices.Call {
    private static final String TYPE = "android.os.usertype.full.SYSTEM";
    private final FrameworkPackages packages;
    private enum Contract {
        isUserUnlockingOrUnlocked, isUserUnlocked, isUserRunning, isUserForeground, isUserVisible,
        getProfileIds, getProfileIdsExcludingHidden, getProfiles, getProfileType,
        isSameProfileGroup, isHeadlessSystemUserMode, isQuietModeEnabled,
        isDemoUser, isAdminUser, isPreCreated, isRestricted,
        getUserSerialNumber, getUserHandle, getUserRestrictions, hasUserRestriction,
        getUserInfo, getMainUserId, getCommunalProfileId, isUserOfType,
        getApplicationRestrictions, getApplicationRestrictionsForUser, setApplicationRestrictions
    }

    FrameworkUsers(FrameworkPackages packages) { this.packages = packages; }
    void install(FrameworkServices services) throws Exception {
        services.register("user", "android.os.IUserManager", this);
    }
    @Override public String[] methods() {
        Contract[] values = Contract.values();
        ArrayList<String> names = new ArrayList<>();
        for (Contract value : values)
            if (value != Contract.setApplicationRestrictions) names.add(value.name());
        return names.toArray(new String[0]);
    }
    private boolean granted(String name) {
        return packages.checkPermission("android.permission." + name, Process.myUid(),
                Context.DEVICE_ID_DEFAULT) == PackageManager.PERMISSION_GRANTED;
    }
    private boolean manage() {
        return Process.myUid() == 0 || Process.myUid() % 100000 == 1000 || granted("MANAGE_USERS");
    }
    private boolean create() { return manage() || granted("CREATE_USERS"); }
    private void query() {
        if (!create() && !granted("QUERY_USERS")) throw new SecurityException("User query requires MANAGE_USERS, CREATE_USERS or QUERY_USERS");
    }
    private void cross(int user, boolean queryAllowed) {
        if (user == Process.myUid() / 100000) return;
        if (!manage() && !(queryAllowed && granted("QUERY_USERS")) && !granted("INTERACT_ACROSS_USERS"))
            throw new SecurityException("Foreign user query requires INTERACT_ACROSS_USERS or user management");
    }
    private Object info(boolean full) throws Exception {
        Class<?> type = Class.forName("android.content.pm.UserInfo");
        int flags = 0;
        for (String flag : new String[] {"FLAG_SYSTEM", "FLAG_FULL", "FLAG_MAIN", "FLAG_ADMIN", "FLAG_INITIALIZED"})
            flags |= type.getField(flag).getInt(null);
        String name = null;
        if (full) {
            Resources resources = Resources.getSystem();
            int id = resources.getIdentifier("owner_name", "string", "android");
            if (id != 0) name = resources.getString(id);
        }
        return type.getConstructor(int.class, String.class, String.class, int.class, String.class)
                .newInstance(0, name, null, flags, TYPE);
    }
    private Bundle applicationRestrictions(String name, int user) {
        // Android restricts these reads to the owning uid or system/root.
        PackageInfo pkg = packages.processPackage(Objects.requireNonNull(name, "package"));
        if (user != 0 || pkg == null || (pkg.applicationInfo.uid != Process.myUid()
                && Process.myUid() != 0 && Process.myUid() % 100000 != 1000))
            throw new SecurityException("Application restrictions for another app/user");
        return new Bundle(); // No administrator provisions restrictions in this profile.
    }
    private Bundle restrictions(int user) {
        Bundle result = new Bundle();
        if (user == 0) {
            Resources resources = Resources.getSystem();
            int id = resources.getIdentifier("config_defaultFirstUserRestrictions", "array", "android");
            if (id != 0) for (String key : resources.getStringArray(id)) result.putBoolean(key, true);
        }
        return result;
    }
    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("user." + method); }
        switch (contract) {
            case isUserUnlockingOrUnlocked: case isUserUnlocked: case isUserRunning:
            case isUserForeground: case isUserVisible:
                cross((Integer) a[0], false); return (Integer) a[0] == 0;
            case getProfileIds: case getProfileIdsExcludingHidden:
                if ((Integer) a[0] != Process.myUid() / 100000) query();
                return (Integer) a[0] == 0 ? new int[] {0} : new int[0];
            case getProfiles: {
                boolean foreign = (Integer) a[0] != Process.myUid() / 100000;
                if (foreign) query();
                ArrayList<Object> result = new ArrayList<>();
                if ((Integer) a[0] == 0) result.add(info(foreign || create()));
                return result;
            }
            case getProfileType:
                cross((Integer) a[0], true); return (Integer) a[0] == 0 ? "" : null;
            case isSameProfileGroup:
                if (a[0].equals(a[1])) return true;
                if (!manage() && !granted("QUERY_USERS")) throw new SecurityException("QUERY_USERS");
                return false;
            case isHeadlessSystemUserMode: case isQuietModeEnabled: return false;
            case isDemoUser:
                if ((Integer) a[0] != Process.myUid() / 100000 && !manage()) throw new SecurityException("MANAGE_USERS");
                return false;
            case isAdminUser: case isRestricted:
                if ((Integer) a[0] != Process.myUid() / 100000) query();
                return contract == Contract.isAdminUser && (Integer) a[0] == 0;
            case isPreCreated: cross((Integer) a[0], false); return false;
            case getUserSerialNumber: case getUserHandle: return (Integer) a[0] == 0 ? 0 : -1;
            case getUserRestrictions: cross((Integer) a[0], false); return restrictions((Integer) a[0]);
            case hasUserRestriction:
                if ((Integer) a[1] != 0) return false; // AOSP checks existence before permissions.
                cross((Integer) a[1], false); return restrictions(0).getBoolean((String) a[0]);
            case getUserInfo: query(); return (Integer) a[0] == 0 ? info(true) : null;
            case getMainUserId: query(); return 0;
            case getCommunalProfileId: query(); return -10000; // USER_NULL
            case isUserOfType: query(); return (Integer) a[0] == 0 && TYPE.equals(a[1]);
            case getApplicationRestrictions:
                return applicationRestrictions((String) a[0], Process.myUid() / 100000);
            case getApplicationRestrictionsForUser:
                return applicationRestrictions((String) a[0], (Integer) a[1]);
            case setApplicationRestrictions:
                if (Process.myUid() != 0 && Process.myUid() % 100000 != 1000) throw new SecurityException("Application restrictions require system/root");
                throw new UnsupportedOperationException("User administration is not available");
            default: throw new UnsupportedOperationException("user." + method);
        }
    }
}
