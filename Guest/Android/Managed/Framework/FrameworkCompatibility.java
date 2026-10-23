// Per-process compatibility defaults from the provisioned Android change metadata.
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.util.Xml;
import java.io.File;
import java.io.FileInputStream;
import java.util.Map;
import java.util.TreeMap;
import org.xmlpull.v1.XmlPullParser;

final class FrameworkCompatibility {
    private FrameworkCompatibility() { }

    private static void read(File file, int target, Map<Long, Boolean> changes) throws Exception {
        try (FileInputStream input = new FileInputStream(file)) {
            XmlPullParser parser = Xml.newPullParser();
            parser.setInput(input, "UTF-8");
            while (parser.next() != XmlPullParser.END_DOCUMENT) {
                if (parser.getEventType() != XmlPullParser.START_TAG || !"compat-change".equals(parser.getName())) continue;
                long id = Long.parseLong(parser.getAttributeValue(null, "id"));
                String since = parser.getAttributeValue(null, "enableSinceTargetSdk");
                String after = parser.getAttributeValue(null, "enableAfterTargetSdk");
                int minimum = since != null ? Integer.parseInt(since)
                        : after != null ? Math.addExact(Integer.parseInt(after), 1) : -1;
                boolean enabled = !Boolean.parseBoolean(parser.getAttributeValue(null, "disabled"))
                        && (minimum == -1 || target >= minimum);
                Boolean previous = changes.put(id, enabled);
                if (previous != null && previous != enabled)
                    throw new IllegalStateException("Conflicting compatibility change " + id);
            }
        }
    }

    private static void directory(File directory, int target, Map<Long, Boolean> changes) throws Exception {
        File[] files = directory.listFiles();
        if (files == null) {
            if (directory.exists()) throw new IllegalStateException("Cannot read compatibility metadata " + directory);
            return;
        }
        for (File file : files) if (file.isFile() && file.getName().endsWith(".xml")) read(file, target, changes);
    }

    static long[] disabledChanges(int targetSdk) throws Exception {
        int target = Math.min(targetSdk, Build.VERSION.SDK_INT);
        Map<Long, Boolean> changes = new TreeMap<>();
        for (String partition : new String[] {"system", "system_ext", "product", "vendor"})
            directory(new File("/" + partition + "/etc/compatconfig"), target, changes);
        File[] modules = new File("/apex").listFiles();
        if (modules != null) for (File module : modules)
            if (!module.getName().contains("@")) directory(new File(module, "etc/compatconfig"), target, changes);
        if (changes.isEmpty()) throw new IllegalStateException("Missing Android compatibility metadata");
        return changes.entrySet().stream().filter(entry -> !entry.getValue()).mapToLong(Map.Entry::getKey).toArray();
    }

    static void install(ApplicationInfo app) throws Exception {
        long[] disabled = disabledChanges(app.targetSdkVersion);
        Class<?> runtime = Class.forName("dalvik.system.VMRuntime");
        Object vm = runtime.getMethod("getRuntime").invoke(null);
        runtime.getMethod("setTargetSdkVersion", int.class).invoke(vm, app.targetSdkVersion);
        runtime.getMethod("setDisabledCompatChanges", long[].class).invoke(vm, (Object) disabled);
        Class.forName("android.app.AppCompatCallbacks").getMethod("install", long[].class, long[].class)
                .invoke(null, disabled, new long[0]);
        Class.forName("dalvik.system.AppSpecializationHooks")
                .getMethod("handleCompatChangesBeforeBindingApplication").invoke(null);
    }
}
