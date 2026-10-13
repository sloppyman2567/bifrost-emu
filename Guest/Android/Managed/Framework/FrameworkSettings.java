// Android 15 SettingsProvider call protocol for a fresh user-0 phone.
// System overrides are private to the installed app's persistent runtime.
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Properties;

final class FrameworkSettings implements AutoCloseable {
    private final FrameworkPackages packages;
    private final FrameworkContent content;
    private final Map<String, String> system = new HashMap<>(), secure = new HashMap<>(), global = new HashMap<>();
    private final Map<String, String> writes = new HashMap<>();
    private Object generations;
    private boolean loaded;
    private final File overrides;

    FrameworkSettings(FrameworkPackages packages, FrameworkContent content) {
        this(packages, content, new File("/data/system/users/0/bifrost-system-settings.xml"));
    }

    FrameworkSettings(FrameworkPackages packages, FrameworkContent content, File overrides) {
        this.packages = packages; this.content = content; this.overrides = overrides;
    }

    private void load() throws Exception {
        if (loaded) return;
        FrameworkSettingsDefaults.load(system, secure, global);
        if (overrides.isFile()) {
            Properties saved = new Properties();
            try (FileInputStream input = new FileInputStream(overrides)) { saved.loadFromXML(input); }
            for (String key : saved.stringPropertyNames()) {
                if (!FrameworkSettingsDefaults.PUBLIC.contains(key)) throw new IllegalStateException("Invalid persisted System key " + key);
                String encoded = saved.getProperty(key);
                if (!encoded.equals("0") && !encoded.startsWith("1")) throw new IllegalStateException("Invalid persisted System value");
                String value = encoded.equals("0") ? null : encoded.substring(1);
                validate(key, value);
                system.put(key, value);
                writes.put(key, value);
            }
        }
        Class<?> type = Class.forName("android.util.MemoryIntArray");
        generations = type.getConstructor(int.class).newInstance(4);
        for (int i = 0; i < 4; i++) type.getMethod("set", int.class, int.class).invoke(generations, i, 1);
        loaded = true;
    }

    private int generation(int index) throws Exception {
        return (Integer) generations.getClass().getMethod("get", int.class).invoke(generations, index);
    }

    synchronized void identityChanged() throws Exception {
        if (generations != null) generations.getClass().getMethod("set", int.class, int.class)
                .invoke(generations, 1, generation(1) + 1);
    }

    @Override public synchronized void close() throws Exception {
        if (generations != null) generations.getClass().getMethod("close").invoke(generations);
        generations = null; loaded = false;
    }

    private void tracking(Bundle result, Bundle extras, int index) throws Exception {
        if (extras == null || !extras.containsKey("_track_generation")) return;
        // Local calls still return a read-only duplicate, as Binder parceling
        // would. Clients close their tracker when their cache is invalidated.
        Parcel parcel = Parcel.obtain();
        try {
            ((Parcelable) generations).writeToParcel(parcel, 0); parcel.setDataPosition(0);
            Parcelable.Creator<?> creator = (Parcelable.Creator<?>) generations.getClass().getField("CREATOR").get(null);
            result.putParcelable("_track_generation", (Parcelable) creator.createFromParcel(parcel));
        } finally { parcel.recycle(); }
        result.putInt("_generation_index", index); result.putInt("_generation", generation(index));
    }

    // Descriptors are extracted from AOSP PUBLIC_SETTINGS / VALIDATORS.
    private static void validate(String name, String value) {
        String rule = FrameworkSettingsDefaults.validator(name);
        boolean valid;
        try {
            switch (rule) {
                case "ANY_STRING_VALIDATOR": valid = true; break;
                case "BOOLEAN_VALIDATOR": valid = "0".equals(value) || "1".equals(value); break;
                case "NON_NEGATIVE_INTEGER_VALIDATOR": valid = value == null || Integer.parseInt(value) >= 0; break;
                case "LENIENT_IP_ADDRESS_VALIDATOR": valid = value == null || value.length() <= 45; break;
                case "URI_VALIDATOR": if (value != null) Uri.decode(value); valid = true; break;
                case "FONT_SCALE_VALIDATOR": valid = value == null || Float.parseFloat(value) >= .25f && Float.parseFloat(value) <= 5f; break;
                case "time": valid = value == null || value.equals("12") || value.equals("24"); break;
                case "length:999": valid = value == null || value.length() < 1000; break;
                case "component:1000": valid = value != null && value.length() <= 1000 && android.content.ComponentName.unflattenFromString(value) != null; break;
                default:
                    String[] parts = rule.split(":");
                    if (!parts[0].equals("int") && !parts[0].equals("float")) throw new IllegalStateException("Unknown validator " + rule);
                    if (value == null) valid = true;
                    else {
                        double number = parts[0].equals("int") ? Integer.parseInt(value) : Float.parseFloat(value);
                        valid = number >= Double.parseDouble(parts[1]) && number <= Double.parseDouble(parts[2]);
                    }
            }
        } catch (NumberFormatException | NullPointerException error) { valid = false; }
        if (!valid) throw new IllegalArgumentException("Invalid value for System." + name + ": " + value);
    }

    synchronized Bundle call(String method, String name, Bundle extras, String androidId) throws Exception {
        int user = extras == null ? 0 : extras.getInt("_user", 0);
        if (user != 0 && user != -2 && user != -3) throw new SecurityException("Foreign settings user " + user);
        load();
        int separator = method.indexOf('_');
        if (separator < 0) throw new UnsupportedOperationException("settings." + method);
        String operation = method.substring(0, separator), table = method.substring(separator + 1);
        Map<String, String> values;
        int index;
        switch (table) {
            case "system": values = system; index = 0; break;
            case "secure": values = secure; index = 1; break;
            case "global": values = global; index = 2; break;
            case "config": values = java.util.Collections.emptyMap(); index = 3; break;
            default: throw new UnsupportedOperationException("settings." + method);
        }
        Bundle result = new Bundle();
        if (operation.equals("LIST")) {
            if (table.equals("config")) result.putSerializable("value", new HashMap<String, String>());
            else {
                ArrayList<String> list = new ArrayList<>();
                for (Map.Entry<String, String> entry : values.entrySet()) list.add(entry.getKey() + "=" + entry.getValue());
                if (table.equals("secure") && androidId != null) list.add("android_id=" + androidId);
                java.util.Collections.sort(list); result.putStringArrayList("result_settings_list", list);
            }
            tracking(result, extras, index); return result;
        }
        if (name == null) throw new IllegalArgumentException("Missing setting name");
        if (operation.equals("GET")) {
            result.putString("value", table.equals("secure") && name.equals("android_id") ? androidId : values.get(name));
            tracking(result, extras, index); return result;
        }
        if (!operation.equals("PUT")) throw new UnsupportedOperationException("settings." + method);
        if (!table.equals("system")) throw new SecurityException("android.permission.WRITE_SECURE_SETTINGS");
        // AppOps legacy default allows pre-M apps requesting WRITE_SETTINGS.
        // There is no special-access grant UI for newer apps in this profile.
        if (!packages.requested("android.permission.WRITE_SETTINGS") || packages.targetSdk() >= 23)
            throw new SecurityException("android.permission.WRITE_SETTINGS");
        if (!FrameworkSettingsDefaults.PUBLIC.contains(name)) throw new IllegalArgumentException("Non-public System setting " + name);
        String value = extras == null ? null : extras.getString("value");
        validate(name, value);
        if (Objects.equals(system.get(name), value)) return null;
        Properties saved = new Properties();
        Map<String, String> updated = new HashMap<>(writes);
        updated.put(name, value);
        for (String key : FrameworkSettingsDefaults.PUBLIC)
            if (updated.containsKey(key)) saved.setProperty(key, updated.get(key) == null ? "0" : "1" + updated.get(key));
        File parent = overrides.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new java.io.IOException("Cannot create settings directory");
        File temporary = new File(parent, overrides.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            saved.storeToXML(output, "Asgard System settings"); output.getFD().sync();
        }
        if (!temporary.renameTo(overrides)) throw new java.io.IOException("Cannot persist System settings");
        system.put(name, value);
        writes.put(name, value);
        generations.getClass().getMethod("set", int.class, int.class).invoke(generations, index, generation(index) + 1);
        content.call("notifyChange", new Object[] {new Uri[] {Uri.parse("content://settings/system/" + name)}, null, false, 0, 0});
        return null;
    }
}
