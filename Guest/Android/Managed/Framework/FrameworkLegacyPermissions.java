// LegacyPermissionManagerService ("legacy_permission", android.permission.ILegacyPermissionManager): what a
// TelephonyManager asks before it hands out a device identifier or the phone number, answered as Android 15's service
// does for an app. The default-permission grants are the system's (system or root only).
import java.util.Objects;

final class FrameworkLegacyPermissions implements FrameworkServices.Call {
    private enum Contract {
        checkDeviceIdentifierAccess, checkPhoneNumberAccess, grantDefaultPermissionsToEnabledCarrierApps,
        grantDefaultPermissionsToEnabledImsServices, grantDefaultPermissionsToEnabledTelephonyDataServices,
        revokeDefaultPermissionsFromDisabledTelephonyDataServices, grantDefaultPermissionsToActiveLuiApp,
        revokeDefaultPermissionsFromLuiApps, grantDefaultPermissionsToCarrierServiceApp
    }
    // PackageManager.PERMISSION_GRANTED/DENIED and AppOpsManager.MODE_IGNORED, as the checks return them.
    private static final int GRANTED = 0, DENIED = -1, MODE_IGNORED = 1;

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final FrameworkAppOps appOps;

    FrameworkLegacyPermissions(FrameworkPackages packages, FrameworkAppOps appOps) { this.packages = packages; this.appOps = appOps; }

    void install(FrameworkServices services) throws Exception {
        services.register("legacy_permission", "android.permission.ILegacyPermissionManager", this);
    }

    // verifyCallerCanCheckAccess: an app may only ask about itself.
    private void verifyCaller(String packageName, String message, int pid, int uid) {
        boolean error = uid != android.os.Process.myUid() || pid != android.os.Process.myPid();
        if (packageName != null && packages.packageUid(packageName) != uid) error = true;
        if (error)
            throw new SecurityException(String.format("Calling uid %d, pid %d cannot access for package %s (uid=%d, pid=%d): %s",
                    android.os.Process.myUid(), android.os.Process.myPid(), packageName, uid, pid, message));
    }

    private boolean permitted(String permission, int uid) {
        return packages.checkPermission(permission, uid, android.content.Context.DEVICE_ID_DEFAULT) == GRANTED;
    }

    private int op(String name) throws Exception {
        return (Integer) android.app.AppOpsManager.class.getMethod("strOpToOp", String.class).invoke(null, name);
    }

    // checkPermissionAndAppop: the permission (if any), then the op in ALLOWED mode.
    private int permissionAndOp(String packageName, String permission, String opName, int uid) throws Exception {
        if (permission != null && !permitted(permission, uid)) return DENIED;
        return appOps.mode(op(opName), uid, packageName) == FrameworkAppOps.MODE_ALLOWED ? GRANTED : MODE_IGNORED;
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("legacy_permission." + method); }
        switch (contract) {
            case checkDeviceIdentifierAccess: {
                String packageName = (String) a[0];
                int pid = (Integer) a[3], uid = (Integer) a[4];
                verifyCaller(packageName, (String) a[1], pid, uid);
                if (permitted("android.permission.READ_PRIVILEGED_PHONE_STATE", uid)) return GRANTED;
                // The READ_DEVICE_IDENTIFIERS op; a device-policy identifier grant needs a device owner, which there is none of.
                if (packageName != null && appOps.mode(op("android:read_device_identifiers"), uid, packageName) == FrameworkAppOps.MODE_ALLOWED)
                    return GRANTED;
                return DENIED;
            }
            case checkPhoneNumberAccess: {
                String packageName = (String) a[0];
                int pid = (Integer) a[3], uid = (Integer) a[4];
                verifyCaller(packageName, (String) a[1], pid, uid);
                if (permitted("android.permission.READ_PRIVILEGED_PHONE_STATE", uid)) return GRANTED;
                if (packageName == null) return DENIED;
                int result = DENIED;
                android.content.pm.PackageInfo info = packages.processPackage(packageName);
                if (info != null && info.applicationInfo.targetSdkVersion <= android.os.Build.VERSION_CODES.Q) {
                    result = permissionAndOp(packageName, "android.permission.READ_PHONE_STATE", "android:read_phone_state", uid);
                    if (result == GRANTED) return result;
                }
                if (permissionAndOp(packageName, null, "android:write_sms", uid) == GRANTED) return GRANTED;
                if (permissionAndOp(packageName, "android.permission.READ_PHONE_NUMBERS", "android:read_phone_numbers", uid) == GRANTED) return GRANTED;
                if (permissionAndOp(packageName, "android.permission.READ_SMS", "android:read_sms", uid) == GRANTED) return GRANTED;
                return result;
            }
            default:
                // Default permission grants for carrier, IMS, telephony-data and LUI apps: system or root.
                throw new SecurityException("Caller must be system or root: " + method);
        }
    }
}
