// RoleService ("role", android.app.role.IRoleManager) for a device with no roles: the permission controller that
// would define and grant them is not provisioned, so an app cannot be asked to become the default browser, dialer
// or similar. RoleManager is null without this service. Reads answer as Android 15's service does for a role that
// does not exist; changing role holders is UnsupportedOperationException rather than a silent success.
import android.os.Process;

final class FrameworkRoles implements FrameworkServices.Call {
    private enum Contract { isRoleAvailableAsUser, isRoleHeldAsUser }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkRoles(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("role", "android.app.role.IRoleManager", this);
    }

    // UserUtils.enforceCrossUserPermission: another user needs INTERACT_ACROSS_USERS_FULL, which no app here holds.
    private static void enforceUser(int userId, String method) {
        if (userId != Process.myUid() / 100000)
            throw new SecurityException(method + ": cross-user access to user " + userId);
    }

    // Preconditions.checkStringNotEmpty.
    private static void notEmpty(String value, String name) {
        if (value == null || value.isEmpty()) throw new IllegalArgumentException(name + " cannot be null or empty");
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("role." + method); }
        switch (contract) {
            case isRoleAvailableAsUser:
                enforceUser((Integer) a[1], method);
                notEmpty((String) a[0], "roleName");
                return false;
            case isRoleHeldAsUser:
                // AppOpsManager.checkPackage comes first: the package must be the caller's own.
                if (packages.packageUid((String) a[1]) != Process.myUid())
                    throw new SecurityException("Package " + a[1] + " does not belong to " + Process.myUid());
                enforceUser((Integer) a[2], method);
                notEmpty((String) a[0], "roleName");
                return false;
            default: throw new UnsupportedOperationException("role." + method);
        }
    }
}
