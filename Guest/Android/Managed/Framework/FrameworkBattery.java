// BatteryService + healthd for the host. The state is the machine's own, read from /sys/class/power_supply (the VFS
// mirrors the host's supplies, read-only) the way Android 15's BatteryMonitor reads them: the first system battery
// (not a peripheral with scope "Device") gives presence, capacity, status, health, voltage, temperature and
// technology, and every online charger adds to the plug type (Mains/UPS/USB_*: AC, USB, USB_PD_DRP: USB, Wireless,
// Dock). A host without a battery (a desktop) runs on AC mains and reports a full battery: present, 100%, FULL, GOOD.
// The state is re-read every 30 s and a change is broadcast as BatteryService does: ACTION_BATTERY_CHANGED with a
// new sequence number, ACTION_POWER_CONNECTED/DISCONNECTED when the plug changes, ACTION_BATTERY_LOW/OKAY at 15%/20%.
// BatteryManager uses ServiceManager.getServiceOrThrow for "batteryproperties" and "batterystats", so both must exist
// for getSystemService to succeed.
import android.content.Intent;
import android.os.BatteryManager;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Objects;

final class FrameworkBattery {
    // The directory of supplies; a test points it at a fake tree.
    static String suppliesRoot = "/sys/class/power_supply";
    private static final int OK = 0, NAME_NOT_FOUND = -2;  // healthd status_t
    // BatteryManager.BATTERY_PROPERTY_*
    private static final int PROPERTY_CHARGE_COUNTER = 1, PROPERTY_CURRENT_NOW = 2, PROPERTY_CURRENT_AVERAGE = 3,
            PROPERTY_CAPACITY = 4, PROPERTY_ENERGY_COUNTER = 5, PROPERTY_STATUS = 6;
    // config_lowBatteryWarningLevel, its close bump, config_criticalBatteryWarningLevel.
    private static final int LOW_WARNING = 15, LOW_CLOSE_WARNING = 20;
    // The battery a host without one reports: full, on mains, nominal Li-ion voltage and room temperature.
    private static final int FULL_VOLTAGE_MV = 4200, ROOM_TEMPERATURE_TENTHS = 250;
    private static final long POLL_MS = 30_000;

    private enum Properties { getProperty, scheduleUpdate }
    // BatteryManager's IBatteryStats calls; note*/stats methods need
    // UPDATE_DEVICE_STATS or BATTERY_STATS and are not app APIs.
    private enum Stats { isCharging, computeBatteryTimeRemaining, computeChargeTimeRemaining, setChargingStateUpdateDelayMillis,
        takeUidSnapshot, takeUidSnapshots }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    // BatteryProperties / HealthInfo, as BatteryService holds them.
    private static final class State {
        boolean present = true, real;
        int level = 100, status = BatteryManager.BATTERY_STATUS_FULL, health = BatteryManager.BATTERY_HEALTH_GOOD;
        int plugged = BatteryManager.BATTERY_PLUGGED_AC, voltageMv = FULL_VOLTAGE_MV, temperature = ROOM_TEMPERATURE_TENTHS;
        int chargeCounterUah, cycleCount, maxChargingCurrentUa, maxChargingVoltageUv;
        long currentNowUa, currentAverageUa, energyNwh;
        boolean hasChargeCounter, hasCurrentNow, hasCurrentAverage, hasEnergy;
        String technology = "Li-ion";

        // The fields BatteryService compares to decide whether to broadcast.
        Object[] key() {
            return new Object[] {present, level, status, health, plugged, voltageMv, temperature, chargeCounterUah,
                    cycleCount, maxChargingCurrentUa, maxChargingVoltageUv};
        }
    }

    private final FrameworkPackages packages;
    private final Object lock = new Object();
    private State state = read();
    private int sequence = 1;
    private boolean levelLow;
    private Thread poller;

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

    // ── sysfs (BatteryMonitor::updateValues) ──────────────────

    private static String attribute(String supply, String name) {
        try {
            return new String(Files.readAllBytes(new File(suppliesRoot + "/" + supply + "/" + name).toPath()), StandardCharsets.UTF_8).trim();
        } catch (java.io.IOException | RuntimeException unreadable) { return null; }
    }

    private static Long number(String supply, String name) {
        String text = attribute(supply, name);
        if (text == null || text.isEmpty()) return null;
        try { return Long.parseLong(text); } catch (NumberFormatException invalid) { return null; }
    }

    private static int status(String text) {
        if (text == null) return BatteryManager.BATTERY_STATUS_UNKNOWN;
        switch (text) {
            case "Charging": return BatteryManager.BATTERY_STATUS_CHARGING;
            case "Discharging": return BatteryManager.BATTERY_STATUS_DISCHARGING;
            case "Not charging": return BatteryManager.BATTERY_STATUS_NOT_CHARGING;
            case "Full": return BatteryManager.BATTERY_STATUS_FULL;
            default: return BatteryManager.BATTERY_STATUS_UNKNOWN;
        }
    }

    private static int health(String text) {
        if (text == null) return BatteryManager.BATTERY_HEALTH_UNKNOWN;
        switch (text) {
            case "Good": case "Warm": case "Cool": return BatteryManager.BATTERY_HEALTH_GOOD;
            case "Overheat": case "Hot": return BatteryManager.BATTERY_HEALTH_OVERHEAT;
            case "Dead": return BatteryManager.BATTERY_HEALTH_DEAD;
            case "Over voltage": return BatteryManager.BATTERY_HEALTH_OVER_VOLTAGE;
            case "Unspecified failure": return BatteryManager.BATTERY_HEALTH_UNSPECIFIED_FAILURE;
            case "Cold": return BatteryManager.BATTERY_HEALTH_COLD;
            default: return BatteryManager.BATTERY_HEALTH_UNKNOWN;
        }
    }

    // readPowerSupplyType's map: the plug bit a charger of that type sets (0: not a charger).
    private static int plug(String type) {
        if (type == null) return 0;
        switch (type) {
            case "UPS": case "Mains": case "USB_DCP": case "USB_HVDCP": case "USB_CDP": case "USB_ACA": case "USB_C":
            case "USB_PD": return BatteryManager.BATTERY_PLUGGED_AC;
            case "USB": case "USB_PD_DRP": return BatteryManager.BATTERY_PLUGGED_USB;
            case "Wireless": return BatteryManager.BATTERY_PLUGGED_WIRELESS;
            case "Dock": return 8;  // BATTERY_PLUGGED_DOCK
            default: return 0;
        }
    }

    // The host's state, or the full-battery profile when it has no battery (a desktop on mains).
    private static State read() {
        State state = new State();
        String[] supplies = new File(suppliesRoot).list();
        if (supplies == null) return state;
        Arrays.sort(supplies);
        String battery = null;
        int plugged = 0;
        long bestPower = -1;
        for (String supply : supplies) {
            String type = attribute(supply, "type");
            if ("Battery".equals(type)) {
                // A peripheral's battery (mouse, headset) is not the device's.
                if (battery == null && !"Device".equals(attribute(supply, "scope"))) battery = supply;
                continue;
            }
            int bit = plug(type);
            Long online = number(supply, "online");
            if (bit == 0 || online == null || online == 0) continue;
            plugged |= bit;
            Long current = number(supply, "current_max"), voltage = number(supply, "voltage_max");
            long power = (current == null ? 0 : current / 1000) * ((voltage == null ? 5_000_000L : voltage) / 1000);
            if (power > bestPower) {
                bestPower = power;
                state.maxChargingCurrentUa = current == null ? 0 : (int) Math.min(current, Integer.MAX_VALUE);
                state.maxChargingVoltageUv = voltage == null ? 5_000_000 : (int) Math.min(voltage, Integer.MAX_VALUE);
            }
        }
        if (battery == null) return state;
        state.real = true;
        state.plugged = plugged;
        String present = attribute(battery, "present");
        state.present = present == null || !present.startsWith("0");
        Long capacity = number(battery, "capacity");
        if (capacity == null) capacity = fraction(battery, "charge_now", "charge_full", "energy_now", "energy_full");
        state.level = capacity == null ? 0 : (int) Math.max(0, Math.min(100, capacity));
        state.status = status(attribute(battery, "status"));
        state.health = health(attribute(battery, "health"));
        String technology = attribute(battery, "technology");
        state.technology = technology == null ? "" : technology;
        Long voltage = number(battery, "voltage_now"), temperature = number(battery, "temp"), cycles = number(battery, "cycle_count");
        state.voltageMv = voltage == null ? 0 : (int) (voltage / 1000);
        state.temperature = temperature == null ? 0 : temperature.intValue();
        state.cycleCount = cycles == null ? 0 : cycles.intValue();
        Long counter = number(battery, "charge_counter");
        if (counter == null) counter = number(battery, "charge_now");
        state.hasChargeCounter = counter != null;
        state.chargeCounterUah = counter == null ? 0 : counter.intValue();
        Long now = number(battery, "current_now"), average = number(battery, "current_avg");
        state.hasCurrentNow = now != null; state.currentNowUa = now == null ? 0 : now;
        state.hasCurrentAverage = average != null; state.currentAverageUa = average == null ? 0 : average;
        Long energy = number(battery, "energy_now");
        state.hasEnergy = energy != null; state.energyNwh = energy == null ? 0 : energy * 1000;  // µWh to nWh
        return state;
    }

    // Percent from now/full of the first pair the battery reports (charge in µAh, else energy in µWh).
    private static Long fraction(String supply, String... pairs) {
        for (int i = 0; i + 1 < pairs.length; i += 2) {
            Long now = number(supply, pairs[i]), full = number(supply, pairs[i + 1]);
            if (now != null && full != null && full > 0) return Math.round(100.0 * now / full);
        }
        return null;
    }

    // ── Intents ────────────────────────────────────────────

    // BatteryService.sendBatteryChangedIntentLocked's extras for the current state.
    Intent stickyIntent() {
        synchronized (lock) { return batteryChanged(state); }
    }

    private Intent batteryChanged(State s) {
        Intent intent = new Intent(Intent.ACTION_BATTERY_CHANGED);
        intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_REPLACE_PENDING);
        intent.putExtra("seq", sequence);
        intent.putExtra(BatteryManager.EXTRA_STATUS, s.status);
        intent.putExtra(BatteryManager.EXTRA_HEALTH, s.health);
        intent.putExtra(BatteryManager.EXTRA_PRESENT, s.present);
        intent.putExtra(BatteryManager.EXTRA_LEVEL, s.level);
        intent.putExtra("battery_low", levelLow);
        intent.putExtra(BatteryManager.EXTRA_SCALE, 100);
        intent.putExtra(BatteryManager.EXTRA_ICON_SMALL, 0);
        intent.putExtra(BatteryManager.EXTRA_PLUGGED, s.plugged);
        intent.putExtra(BatteryManager.EXTRA_VOLTAGE, s.voltageMv);
        intent.putExtra(BatteryManager.EXTRA_TEMPERATURE, s.temperature);
        intent.putExtra(BatteryManager.EXTRA_TECHNOLOGY, s.technology);
        intent.putExtra("invalid_charger", 0);
        intent.putExtra("max_charging_current", s.maxChargingCurrentUa);
        intent.putExtra("max_charging_voltage", s.maxChargingVoltageUv);
        intent.putExtra("charge_counter", s.chargeCounterUah);
        intent.putExtra("android.os.extra.CYCLE_COUNT", s.cycleCount);
        intent.putExtra("android.os.extra.CHARGING_STATUS", 0);
        return intent;
    }

    // BatteryService.processValuesLocked: low/OK transitions, a new sequence, and the intents for a change.
    private void update(State next, java.util.function.Consumer<Intent> sink) {
        Intent[] intents;
        synchronized (lock) {
            State last = state;
            if (Arrays.equals(last.key(), next.key())) { state = next; return; }
            state = next;
            boolean wasLow = levelLow;
            if (!levelLow) {
                if (next.plugged == 0 && next.status != BatteryManager.BATTERY_STATUS_UNKNOWN && next.level <= LOW_WARNING) levelLow = true;
            } else if (next.plugged != 0 || next.level >= LOW_CLOSE_WARNING) levelLow = false;
            sequence++;
            java.util.List<Intent> sent = new java.util.ArrayList<>();
            if (next.plugged != 0 && last.plugged == 0) sent.add(new Intent(Intent.ACTION_POWER_CONNECTED).putExtra("seq", sequence));
            else if (next.plugged == 0 && last.plugged != 0) sent.add(new Intent(Intent.ACTION_POWER_DISCONNECTED).putExtra("seq", sequence));
            sent.add(batteryChanged(next));
            if (levelLow && !wasLow) sent.add(new Intent(Intent.ACTION_BATTERY_LOW).putExtra("seq", sequence));
            else if (!levelLow && wasLow) sent.add(new Intent(Intent.ACTION_BATTERY_OKAY).putExtra("seq", sequence));
            intents = sent.toArray(new Intent[0]);
        }
        for (Intent intent : intents) sink.accept(intent);
    }

    // Reads the host once and hands the intents of a change to the sink.
    void refresh(java.util.function.Consumer<Intent> sink) { update(read(), sink); }

    // Polls the host while the session runs (healthd's slow poll); the thread never keeps the process alive.
    synchronized void watch(java.util.function.Consumer<Intent> sink) {
        if (poller != null) return;
        poller = new Thread(() -> {
            while (true) {
                try { Thread.sleep(POLL_MS); }
                catch (InterruptedException stopped) { return; }
                try { refresh(sink); }
                catch (RuntimeException error) { android.util.Log.w("FrameworkBattery", "battery poll failed", error); }
            }
        }, "BatteryMonitor");
        poller.setDaemon(true);
        poller.start();
    }

    // ── IBatteryPropertiesRegistrar / IBatteryStats ────────

    private Object properties(String method, Object[] a) throws Exception {
        Properties contract;
        try { contract = Properties.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("batteryproperties." + method); }
        switch (contract) {
            case getProperty: {
                Object out = Objects.requireNonNull(a[1], "prop");
                State s = read();
                long value;
                switch ((Integer) a[0]) {
                    case PROPERTY_CAPACITY: value = s.level; break;
                    case PROPERTY_STATUS: value = s.status; break;
                    // Counters and currents need battery hardware that reports them.
                    case PROPERTY_CHARGE_COUNTER: if (!s.hasChargeCounter) return NAME_NOT_FOUND; value = s.chargeCounterUah; break;
                    case PROPERTY_CURRENT_NOW: if (!s.hasCurrentNow) return NAME_NOT_FOUND; value = s.currentNowUa; break;
                    case PROPERTY_CURRENT_AVERAGE: if (!s.hasCurrentAverage) return NAME_NOT_FOUND; value = s.currentAverageUa; break;
                    case PROPERTY_ENERGY_COUNTER: if (!s.hasEnergy) return NAME_NOT_FOUND; value = s.energyNwh; break;
                    default: return NAME_NOT_FOUND;
                }
                out.getClass().getMethod("setLong", long.class).invoke(out, value);
                return OK;
            }
            // BatteryManager asks for a fresh reading; the poll picks up changes, so an update is read at once.
            case scheduleUpdate: return null;
            default: throw new UnsupportedOperationException("batteryproperties." + method);
        }
    }

    private Object stats(String method, Object[] a) throws Exception {
        Stats contract;
        try { contract = Stats.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("batterystats." + method); }
        switch (contract) {
            // BatteryStatsImpl.isCharging: plugged in.
            case isCharging: return read().plugged != 0;
            // Time estimates need the discharge history this device does not keep: -1 is "unknown".
            case computeBatteryTimeRemaining:
            case computeChargeTimeRemaining: return -1L;
            case setChargingStateUpdateDelayMillis:
                if (!packages.granted("android.permission.POWER_SAVER")) throw new SecurityException("android.permission.POWER_SAVER");
                return true;
            // SystemHealthManager.takeUidSnapshot: an app reads its own uid (another needs BATTERY_STATS). This device
            // keeps no per-uid battery statistics, so the snapshot has the uid's health stat layout and no measurements.
            case takeUidSnapshot:
                if ((Integer) a[0] != android.os.Process.myUid() && !packages.granted("android.permission.BATTERY_STATS"))
                    throw new SecurityException("android.permission.BATTERY_STATS");
                return uidSnapshot();
            case takeUidSnapshots: {
                int[] uids = (int[]) a[0];
                for (int uid : uids)
                    if (uid != android.os.Process.myUid() && !packages.granted("android.permission.BATTERY_STATS"))
                        throw new SecurityException("android.permission.BATTERY_STATS");
                Class<?> parcelers = Class.forName("android.os.health.HealthStatsParceler");
                Object[] result = (Object[]) java.lang.reflect.Array.newInstance(parcelers, uids.length);
                for (int i = 0; i < uids.length; i++) result[i] = uidSnapshot();
                return result;
            }
            default: throw new UnsupportedOperationException("batterystats." + method);
        }
    }

    private static Object uidSnapshot() throws Exception {
        Class<?> constants = Class.forName("android.os.health.UidHealthStats");
        Object layout = constants.getField("CONSTANTS").get(null);
        Object writer = Class.forName("android.os.health.HealthStatsWriter")
                .getConstructor(layout.getClass()).newInstance(layout);
        return Class.forName("android.os.health.HealthStatsParceler")
                .getConstructor(writer.getClass()).newInstance(writer);
    }
}
