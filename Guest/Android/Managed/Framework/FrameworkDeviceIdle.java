// App-facing DeviceIdleController for the existing always-active device
// profile: Doze is disabled, and no system/user/temporary exemptions exist.
// Administrative operations enforce Android 15 permissions before declining
// privileged changes; apps cannot invent an exemption or wake policy.
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Process;

final class FrameworkDeviceIdle implements FrameworkServices.Call {
    private final FrameworkPackages packages;
    private enum Contract {
        getRemovedSystemPowerWhitelistApps, getSystemPowerWhitelistExceptIdle,
        getSystemPowerWhitelist, getUserPowerWhitelist, getFullPowerWhitelistExceptIdle,
        getFullPowerWhitelist, getAppIdWhitelistExceptIdle, getAppIdWhitelist,
        getAppIdUserWhitelist, getAppIdTempWhitelist, isPowerSaveWhitelistExceptIdleApp,
        isPowerSaveWhitelistApp, addPowerSaveWhitelistApp, addPowerSaveWhitelistApps,
        removePowerSaveWhitelistApp, removeSystemPowerWhitelistApp, restoreSystemPowerWhitelistApp,
        addPowerSaveTempWhitelistApp, addPowerSaveTempWhitelistAppForMms,
        addPowerSaveTempWhitelistAppForSms, whitelistAppTemporarily, exitIdle
    }
    FrameworkDeviceIdle(FrameworkPackages packages) { this.packages = packages; }
    void install(FrameworkServices services) throws Exception {
        services.register("deviceidle", "android.os.IDeviceIdleController", this);
    }
    @Override public String[] methods() {
        return new String[] {"getRemovedSystemPowerWhitelistApps", "getSystemPowerWhitelistExceptIdle",
                "getSystemPowerWhitelist", "getUserPowerWhitelist", "getFullPowerWhitelistExceptIdle",
                "getFullPowerWhitelist", "getAppIdWhitelistExceptIdle", "getAppIdWhitelist",
                "getAppIdUserWhitelist", "getAppIdTempWhitelist", "isPowerSaveWhitelistExceptIdleApp",
                "isPowerSaveWhitelistApp", "exitIdle"};
    }
    private void permission(String name) {
        if (packages.checkPermission("android.permission." + name, Process.myUid(),
                Context.DEVICE_ID_DEFAULT) != PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("android.permission." + name);
    }
    @Override public Object invoke(String method, Object[] a) {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("deviceidle." + method); }
        switch (contract) {
            case getRemovedSystemPowerWhitelistApps: case getSystemPowerWhitelistExceptIdle:
            case getSystemPowerWhitelist: case getUserPowerWhitelist: case getFullPowerWhitelistExceptIdle:
            case getFullPowerWhitelist: return new String[0];
            case getAppIdWhitelistExceptIdle: case getAppIdWhitelist: case getAppIdUserWhitelist:
            case getAppIdTempWhitelist: return new int[0];
            case isPowerSaveWhitelistExceptIdleApp: case isPowerSaveWhitelistApp: return false;
            case addPowerSaveTempWhitelistApp: case addPowerSaveTempWhitelistAppForMms:
            case addPowerSaveTempWhitelistAppForSms: case whitelistAppTemporarily:
                permission("CHANGE_DEVICE_IDLE_TEMP_WHITELIST"); break;
            case exitIdle:
                permission("DEVICE_POWER"); return null; // Already active; no Doze transition exists.
            default: permission("DEVICE_POWER"); break;
        }
        throw new UnsupportedOperationException("Device idle administration is not available: " + method);
    }
}
