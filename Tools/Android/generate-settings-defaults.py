#!/usr/bin/env python3
"""Generate the fresh user-0 phone Settings profile from pinned Android 15 inputs.

No downloaded sources enter the checkout. --check verifies the committed table.
Upgrade-only migrations, Wear, VR listeners, gesture insets and refresh-rate
rewrites are intentionally excluded; this is not an existing-device upgrader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import urllib.request
import xml.etree.ElementTree as ET

TAG = "android-15.0.0_r1"
BASE = f"https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/{TAG}/"
INPUTS = {
    "database": "packages/SettingsProvider/src/com/android/providers/settings/DatabaseHelper.java",
    "provider": "packages/SettingsProvider/src/com/android/providers/settings/SettingsProvider.java",
    "defaults": "packages/SettingsProvider/res/values/defaults.xml",
    "strings": "packages/SettingsProvider/res/values/strings.xml",
    "settings": "core/java/android/provider/Settings.java",
    "validators": "packages/SettingsProvider/src/android/provider/settings/validators/SystemSettingsValidators.java",
}


def body(source, marker):
    start = source.index("{", source.index(marker)) + 1
    depth = 1
    for end in range(start, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end]
    raise ValueError(marker)


def generate(inputs):
    clean = lambda s: re.sub(r"/\*.*?\*/|//[^\n]*", "", s, flags=re.S)
    database = clean(inputs["database"])
    settings = clean(inputs["settings"])
    constants = {}
    for namespace in ("System", "Secure", "Global"):
        section = body(settings, "class " + namespace + " ")
        for name, value in re.findall(r'\bString\s+(\w+)\s*=\s*("[^"\n]*"|(?:Settings\.)?(?:System|Secure|Global)\.\w+)\s*;', section):
            constants[namespace + "." + name] = value

    def key(constant):
        value = constants[constant.removeprefix("Settings.")]
        return value if value.startswith('"') else key(value)

    resources = {}
    for name in ("defaults", "strings"):
        for element in ET.fromstring(inputs[name]):
            if element.get("name"):
                resources[element.tag + "." + element.get("name")] = "".join(element.itertext())

    def resource(reference):
        framework = reference.startswith("com.android.internal.")
        kind, name = reference.split("R.")[1].split(".")
        if framework:
            return f'framework("{kind}", "{name}")'
        value = resources[kind + "." + name]
        if value.startswith("@"):
            return resource("R." + value[1:].replace("/", "."))
        if kind == "bool": value = "1" if value == "true" else "0"
        elif kind == "integer": value = str(int(value, 0))
        elif kind == "fraction": value = str(float(value.rstrip("%p")) / 100)
        return json.dumps(value)

    rows = []
    def put(table, constant, expression, conditional=False):
        rows.append(f'        {"nonempty" if conditional else "put"}({table}, {key(constant)}, {expression});')

    specials = {
        "Secure.ALLOW_MOCK_LOCATION": 'property("ro.allow.mock.location", "0").equals("1") ? "1" : "0"',
        "System.LOCKSCREEN_DISABLED": 'property("ro.lockscreen.disable.default", "false").equals("true") ? "1" : "0"',
        "Global.STAY_ON_WHILE_PLUGGED_IN": resource("R.bool.def_stay_on_while_plugged_in"),
        "Global.MODE_RINGER": '"2"', "Global.CDMA_CELL_BROADCAST_SMS": '"1"',
        "Global.DATA_ROAMING": 'telephonyBoolean("data_roaming", false)',
        "Global.MOBILE_DATA": 'telephonyBoolean("mobile_data", true)',
        "Global.DEFAULT_INSTALL_LOCATION": '"0"',
        "Global.PREFERRED_NETWORK_MODE": 'preferredNetworkMode()',
        "Global.CDMA_SUBSCRIPTION_MODE": 'Integer.toString(integerProperty("ro.telephony.default_cdma_sub", 1))',
        "Global.DEVICE_NAME": 'String.format(java.util.Locale.getDefault(), ' + resource("R.string.def_device_name_simple") + ', android.os.Build.MODEL)',
        "Global.LID_BEHAVIOR": '"1".equals(framework("bool", "config_lidControlsSleep")) ? "1" : "1".equals(framework("bool", "config_lidControlsScreenLock")) ? "2" : "0"',
    }
    # The destination table follows the loader, not the constant's class.
    def loader(method, table, seen):
        section = body(database, "private void " + method + "(")
        for match in re.finditer(r'load(Setting|BooleanSetting|IntegerSetting|StringSetting|FractionSetting)\(\s*stmt,\s*((?:Settings\.)?(?:System|Secure|Global)\.\w+)\s*,\s*(.*?)\);', section, re.S):
            kind, constant, expression = match.groups()
            constant = constant.removeprefix("Settings.")
            if constant in seen: continue
            seen.add(constant)
            if constant == "System.LOCKSCREEN_DISABLED":
                expression = f'booleanProperty("ro.lockscreen.disable.default", false) ? "1" : {resource("R.bool.def_lockscreen_disabled")}'
            elif constant in specials: expression = specials[constant]
            elif kind != "Setting": expression = resource(expression.split(",")[0].strip())
            elif expression.strip() in ("0", "1", '"0"', '"1"'): expression = json.dumps(expression.strip().strip('"'))
            elif constant.endswith("DOWNLOAD_MAX_BYTES_OVER_MOBILE") or constant.endswith("DOWNLOAD_RECOMMENDED_MAX_BYTES_OVER_MOBILE"):
                ref = "R.integer.def_download_manager_" + ("recommended_" if "RECOMMENDED" in constant else "") + "max_bytes_over_mobile"
                expression = resource(ref)
                if int(json.loads(expression)) <= 0: continue
            else: raise ValueError("Unclassified loader expression: " + constant + " = " + expression)
            put(table, constant, expression)
        children = {"loadSystemSettings": ("loadDefaultHapticSettings", "loadUISoundEffectsSettings"),
                    "loadSecureSettings": ("loadSecure35Settings",),
                    "loadGlobalSettings": ("loadDefaultAnimationSettings",)}
        for child in children.get(method, ()): loader(child, table, seen)

    for table in ("system", "secure", "global"):
        loader("load" + table.capitalize() + "Settings", table, set())

    # Reviewed fresh-phone effects of UpgradeController v119..225. Each entry
    # names its source resource; missing resources/constants fail generation.
    upgrades = """
secure DOUBLE_TAP_TO_WAKE bool.def_double_tap_to_wake
secure NFC_PAYMENT_DEFAULT_COMPONENT string.def_nfc_payment_component nonempty
global ADD_USERS_WHEN_LOCKED bool.def_add_users_from_lockscreen
global BLUETOOTH_DISABLED_PROFILES string.def_bluetooth_disabled_profiles
secure SHOW_IME_WITH_HARD_KEYBOARD bool.def_show_ime_with_hard_keyboard
secure MULTI_PRESS_TIMEOUT integer.def_multi_press_timeout_millis
secure SYNC_PARENT_SOUNDS bool.def_sync_parent_sounds
system END_BUTTON_BEHAVIOR integer.def_end_button_behavior
global WIFI_WAKEUP_ENABLED bool.def_wifi_wakeup_enabled
secure AUTOFILL_SERVICE framework.string.config_defaultAutofillService nonempty
global DEFAULT_RESTRICT_BACKGROUND_DATA bool.def_restrict_background_data
secure BACKUP_MANAGER_CONSTANTS string.def_backup_manager_constants nonempty
global MOBILE_DATA_ALWAYS_ON bool.def_mobile_data_always_on
secure NOTIFICATION_BADGING framework.bool.config_notificationBadging
secure BACKUP_LOCAL_TRANSPORT_PARAMETERS string.def_backup_local_transport_parameters nonempty
secure ZEN_DURATION integer.def_zen_duration
global BACKUP_AGENT_TIMEOUT_PARAMETERS string.def_backup_agent_timeout_parameters nonempty
global MAX_SOUND_TRIGGER_DETECTION_SERVICE_OPS_PER_DAY integer.def_max_sound_trigger_detection_service_ops_per_day
global SOUND_TRIGGER_DETECTION_SERVICE_OP_TIMEOUT integer.def_sound_trigger_detection_service_op_timeout
system VIBRATE_WHEN_RINGING bool.def_vibrate_when_ringing
secure CHARGING_SOUNDS_ENABLED bool.def_charging_sounds_enabled
secure CHARGING_VIBRATION_ENABLED bool.def_charging_vibration_enabled
secure LOCATION_MODE integer.def_location_mode
secure AWARE_ENABLED bool.def_aware_enabled
secure SKIP_GESTURE bool.def_skip_gesture
secure SILENCE_GESTURE bool.def_silence_gesture
secure AWARE_LOCK_ENABLED bool.def_aware_lock_enabled
global WIRELESS_CHARGING_STARTED_SOUND string.def_wireless_charging_started_sound nonempty
global CHARGING_STARTED_SOUND string.def_charging_started_sound nonempty
secure SWIPE_BOTTOM_TO_NOTIFICATION_ENABLED bool.def_swipe_bottom_to_notification_enabled
secure ONE_HANDED_MODE_ENABLED bool.def_one_handed_mode_enabled
global DEVELOPMENT_ENABLE_NON_RESIZABLE_MULTI_WINDOW bool.def_enable_non_resizable_multi_window
secure ACCESSIBILITY_BUTTON_MODE integer.def_accessibility_button_mode
secure ONE_HANDED_MODE_ACTIVATED bool.def_one_handed_mode_activated
secure LOCK_SCREEN_SHOW_QR_CODE_SCANNER bool.def_lock_screen_show_qr_code_scanner
system APPLY_RAMPING_RINGER bool.def_apply_ramping_ringer
secure CREDENTIAL_SERVICE framework.array.config_enabledCredentialProviderService nonempty
secure CREDENTIAL_SERVICE_PRIMARY framework.array.config_primaryCredentialProviderService nonempty
secure STATUS_BAR_SHOW_VIBRATE_ICON integer.def_statusBarVibrateIconEnabled
secure LOCK_SCREEN_SHOW_ONLY_UNSEEN_NOTIFICATIONS bool.def_lock_screen_show_only_unseen_notifications
global ENABLE_BACK_ANIMATION bool.def_enable_back_animation
global WIFI_ALWAYS_REQUESTED bool.def_enable_wifi_always_requested
global BATTERY_CHARGING_STATE_UPDATE_DELAY integer.def_battery_charging_state_update_delay_ms
global BATTERY_CHARGING_STATE_ENFORCE_LEVEL integer.def_battery_charging_state_enforce_level
"""
    for line in upgrades.strip().splitlines():
        table, constant, ref, *condition = line.split()
        put(table, table.capitalize() + "." + constant,
            resource(ref.replace("framework.", "com.android.internal.R.") if ref.startswith("framework.") else "R." + ref), bool(condition))
    rows += [
        '        secure.put("volume_hush_gesture", "1");',
        '        if ("0".equals(secure.get("install_non_market_apps"))) { secure.put("install_non_market_apps", "1"); secure.put("unknown_sources_default_reversed", "1"); }',
        '        secure.put("hush_gesture_used", "0"); secure.put("manual_ringer_toggle_count", "0");',
        '        secure.put("show_zen_settings_suggestion", "1"); secure.put("zen_settings_updated", "0"); secure.put("zen_settings_suggestion_viewed", "0");',
        '        secure.put("notification_bubbles", "1");',
        '        String locked = "1".equals(secure.get("lock_screen_show_notifications")) && "1".equals(secure.get("lock_screen_allow_private_notifications")) ? "1" : "0";',
        '        secure.put("power_menu_locked_show_content", locked); secure.put("lockscreen_show_controls", locked); secure.put("lockscreen_show_wallet", locked);',
        '        secure.put("accessibility_magnification_capability", "1".equals(framework("bool", "config_magnification_area")) ? ' + resource("R.integer.def_accessibility_magnification_capabilities") + ' : "1");',
        '        system.put("keyboard_vibration_enabled", system.get("haptic_feedback_enabled"));',
    ]
    public_names = re.findall(r'PUBLIC_SETTINGS.add\((\w+)\)', body(settings, "class System "))
    public = [json.loads(key("System." + name)) for name in public_names]
    rules = []
    for name in public_names:
        match = re.search(r'VALIDATORS.put\(\s*System\.' + name + r',\s*(.*?)\);', inputs["validators"], re.S)
        rule = match.group(1).strip() if match else "ANY_STRING_VALIDATOR"
        if name == "NEXT_ALARM_FORMATTED": rule = "length:999"
        elif name == "WALLPAPER_ACTIVITY": rule = "component:1000"
        elif rule.startswith("new InclusiveIntegerRangeValidator"):
            rule = "int:" + ":".join(re.findall(r'-?\d+', rule))
        elif rule.startswith("new InclusiveFloatRangeValidator"):
            rule = "float:" + ":".join(re.findall(r'-?\d+(?:\.\d+)?', rule))
        elif name == "TIME_12_24": rule = "time"
        elif not re.fullmatch(r'[A-Z_]+_VALIDATOR', rule): raise ValueError("Unknown public validator " + name)
        rules.append(f'            case {key("System." + name)}: return {json.dumps(rule)};')
    header = "// Generated by Tools/Android/generate-settings-defaults.py; do not edit.\n"
    header += "// AOSP " + TAG + "; Apache-2.0. Inputs (SHA-256):\n"
    for name, path in INPUTS.items():
        header += "// " + hashlib.sha256(inputs[name].encode()).hexdigest() + " " + path + "\n"
    return header + '''import android.content.res.Resources;
import java.util.Map;

final class FrameworkSettingsDefaults {
    static final java.util.Set<String> PUBLIC = new java.util.HashSet<>(java.util.Arrays.asList(
        ''' + ", ".join(json.dumps(v) for v in public) + '''));
    static String validator(String name) {
        switch (name) {
''' + "\n".join(rules) + '''
            default: throw new IllegalArgumentException("Non-public System setting " + name);
        }
    }
    static String property(String name, String fallback) {
        try { return (String) Class.forName("android.os.SystemProperties").getMethod("get", String.class, String.class).invoke(null, name, fallback); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }
    private static boolean booleanProperty(String name, boolean fallback) {
        try { return (Boolean) Class.forName("android.os.SystemProperties").getMethod("getBoolean", String.class, boolean.class).invoke(null, name, fallback); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }
    private static int integerProperty(String name, int fallback) {
        try { return (Integer) Class.forName("android.os.SystemProperties").getMethod("getInt", String.class, int.class).invoke(null, name, fallback); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }
    private static String telephonyBoolean(String name, boolean fallback) {
        try {
            java.util.Optional<?> value = (java.util.Optional<?>) Class.forName("android.sysprop.TelephonyProperties").getMethod(name).invoke(null);
            return (value.isPresent() ? (Boolean) value.get() : fallback) ? "1" : "0";
        } catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }
    private static String preferredNetworkMode() {
        try {
            java.util.List<?> values = (java.util.List<?>) Class.forName("android.sysprop.TelephonyProperties").getMethod("default_network").invoke(null);
            Object mode = values.isEmpty() ? null : values.get(0);
            if (mode == null) mode = Class.forName("android.telephony.TelephonyManager").getField("DEFAULT_PREFERRED_NETWORK_MODE").get(null);
            return mode.toString();
        } catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
    }
    private static String framework(String type, String name) {
        Resources res = Resources.getSystem();
        int id = res.getIdentifier(name, type, "android");
        if (id == 0) throw new IllegalStateException("Missing framework resource " + name);
        switch (type) {
            case "bool": return res.getBoolean(id) ? "1" : "0";
            case "integer": return Integer.toString(res.getInteger(id));
            case "string": return res.getString(id);
            case "array": return android.text.TextUtils.join(":", res.getStringArray(id));
            default: throw new AssertionError(type);
        }
    }
    private static void put(Map<String, String> table, String key, String value) { table.putIfAbsent(key, value); }
    private static void nonempty(Map<String, String> table, String key, String value) { if (value != null && !value.isEmpty()) put(table, key, value); }
    static void load(Map<String, String> system, Map<String, String> secure, Map<String, String> global) {
''' + "\n".join(rows) + '''
        // DatabaseHelper.loadVolumeLevels/loadVibrateSetting. Consult this
        // runtime's AudioSystem rather than assuming host volume indices.
        try {
            Class<?> audio = Class.forName("android.media.AudioSystem");
            String[] keys = {"volume_voice", "volume_system", "volume_ring", "volume_music", "volume_alarm", "volume_notification", "volume_bluetooth_sco"};
            for (int i = 0; i < keys.length; i++) system.put(keys[i], audio.getMethod("getDefaultStreamVolume", int.class).invoke(null, i).toString());
            int affected = (Integer) audio.getField("DEFAULT_MUTE_STREAMS_AFFECTED").get(null);
            system.put("mute_streams_affected", Integer.toString(affected | 1 | (1 << 8)));
        } catch (ReflectiveOperationException error) { throw new IllegalStateException(error); }
        system.put("mode_ringer_streams_affected", "1".equals(framework("bool", "config_voice_capable")) ? "166" : "174");
        system.put("vibrate_on", "10");
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path("/tmp/bifrost-settings-aosp") / TAG)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[2] / "Guest/Android/Managed/Framework/FrameworkSettingsDefaults.java")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    inputs = {}
    for name, path in INPUTS.items():
        cached = args.cache / (name + Path(path).suffix)
        if not cached.exists():
            with urllib.request.urlopen(BASE + path, timeout=30) as response:
                cached.write_bytes(response.read())
        inputs[name] = cached.read_text()
    output = generate(inputs)
    if args.check:
        if args.output.read_text() != output: raise SystemExit("Settings defaults are stale")
    else: args.output.write_text(output)


if __name__ == "__main__": main()
