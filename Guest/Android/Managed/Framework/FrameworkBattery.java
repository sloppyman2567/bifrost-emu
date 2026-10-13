// BatteryService state for a device without a battery that runs on AC mains,
// as healthd reports when no power_supply battery exists: battery absent,
// AC online, unknown status and health. The guest sysfs has no power_supply
// nodes, so a host laptop battery is not reflected. Level and capacity report
// 100 (rather than healthd's empty-path 0) so apps do not act on a
// nonexistent low battery; EXTRA_PRESENT=false identifies the profile.
// BatteryManager uses ServiceManager.getServiceOrThrow for "batteryproperties"
// and "batterystats", so both must exist for getSystemService to succeed.
import android.content.Intent;
import android.os.BatteryManager;
import java.util.Objects;

final class FrameworkBattery {
    private static final int OK = 0, NAME_NOT_FOUND = -2;  // healthd status_t
    private static final int LEVEL = 100;
    // BatteryManager.BATTERY_PROPERTY_* identifiers healthd answers without a battery.
    private static final int PROPERTY_CAPACITY = 4, PROPERTY_STATUS = 6;

    private enum Properties { getProperty, scheduleUpdate }
    // BatteryManager's IBatteryStats calls; note*/stats methods need
    // UPDATE_DEVICE_STATS or BATTERY_STATS and are not app APIs.
    private enum Stats { isCharging, computeBatteryTimeRemaining, computeChargeTimeRemaining, setChargingStateUpdateDelayMillis }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkBattery(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("batteryproperties", "android.os.IBatteryPropertiesRegistrar", new FrameworkServices.Call() {
            @Override public String[] methods() { return names(Properties.values()); }
            @Override public Object invoke(String method, Object[] a) throws Exception { return properties(method, a); }
        });
        services.register("batterystats", "com.android.internal.app.IBatteryStats", new FrameworkServices.Call() {
            @Override public String[] methods() { return names(Stats.values()); }
            @Override public Object invoke(String method, Object[] a) throws Exception { return stats(method, a); }
        });
    }

    // BatteryService.sendBatteryChangedIntentLocked's extras for this state.
    Intent stickyIntent() {
        Intent intent = new Intent(Intent.ACTION_BATTERY_CHANGED);
        intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_REPLACE_PENDING);
        intent.putExtra("seq", 1);
        intent.putExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN);
        intent.putExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_UNKNOWN);
        intent.putExtra(BatteryManager.EXTRA_PRESENT, false);
        intent.putExtra(BatteryManager.EXTRA_LEVEL, LEVEL);
        intent.putExtra("battery_low", false);
        intent.putExtra(BatteryManager.EXTRA_SCALE, 100);
        intent.putExtra(BatteryManager.EXTRA_ICON_SMALL, 0);
        intent.putExtra(BatteryManager.EXTRA_PLUGGED, BatteryManager.BATTERY_PLUGGED_AC);
        intent.putExtra(BatteryManager.EXTRA_VOLTAGE, 0);
        intent.putExtra(BatteryManager.EXTRA_TEMPERATURE, 0);
        intent.putExtra(BatteryManager.EXTRA_TECHNOLOGY, "");
        intent.putExtra("invalid_charger", 0);
        intent.putExtra("max_charging_current", 0);
        intent.putExtra("max_charging_voltage", 0);
        intent.putExtra("charge_counter", 0);
        intent.putExtra("android.os.extra.CYCLE_COUNT", 0);
        intent.putExtra("android.os.extra.CHARGING_STATUS", 0);
        return intent;
    }

    private Object properties(String method, Object[] a) throws Exception {
        Properties contract;
        try { contract = Properties.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("batteryproperties." + method); }
        switch (contract) {
            case getProperty: {
                Object out = Objects.requireNonNull(a[1], "prop");
                long value;
                switch ((Integer) a[0]) {
                    case PROPERTY_CAPACITY: value = LEVEL; break;
                    case PROPERTY_STATUS: value = BatteryManager.BATTERY_STATUS_UNKNOWN; break;
                    // Charge, current, energy and dates need battery hardware.
                    default: return NAME_NOT_FOUND;
                }
                out.getClass().getMethod("setLong", long.class).invoke(out, value);
                return OK;
            }
            // The state never changes, so there is nothing to refresh.
            case scheduleUpdate: return null;
            default: throw new UnsupportedOperationException("batteryproperties." + method);
        }
    }

    private Object stats(String method, Object[] a) {
        Stats contract;
        try { contract = Stats.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("batterystats." + method); }
        switch (contract) {
            // No battery is charging; time estimates are unknown (-1).
            case isCharging: return false;
            case computeBatteryTimeRemaining:
            case computeChargeTimeRemaining: return -1L;
            case setChargingStateUpdateDelayMillis:
                if (!packages.granted("android.permission.POWER_SAVER")) throw new SecurityException("android.permission.POWER_SAVER");
                return true;
            default: throw new UnsupportedOperationException("batterystats." + method);
        }
    }
}
