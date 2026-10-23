// Real ART compatibility boundary: legacy writable DEX versus API 34+ enforcement.
import android.content.pm.ApplicationInfo;
import dalvik.system.DexClassLoader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Arrays;

public final class FrameworkCompatibilityProbe {
    public static final class Payload {
        public static int value() { return 42; }
    }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void load(File file) throws Exception {
        Class<?> payload = new DexClassLoader(file.getPath(), file.getParent(), null, null)
                .loadClass("FrameworkCompatibilityProbe$Payload");
        check(((Integer) payload.getMethod("value").invoke(null)) == 42, "loaded DEX returned wrong value");
    }
    private static void copy(File source, File target) throws Exception {
        try (FileInputStream input = new FileInputStream(source); FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[16384];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
        }
    }
    public static void main(String[] args) throws Exception {
        int target = Integer.parseInt(args[0]);
        ApplicationInfo app = new ApplicationInfo(); app.targetSdkVersion = target;
        FrameworkCompatibility.install(app);
        Class<?> compat = Class.forName("android.compat.Compatibility");
        boolean enforced = (Boolean) compat.getMethod("isChangeEnabled", long.class).invoke(null, 218865702L);
        check(enforced == (target >= 34), "wrong read-only DEX compatibility gate");
        check(Arrays.binarySearch(FrameworkCompatibility.disabledChanges(29), 218865702L) >= 0, "legacy gate missing");
        check(Arrays.binarySearch(FrameworkCompatibility.disabledChanges(33), 218865702L) >= 0, "API 33 boundary wrong");
        check(Arrays.binarySearch(FrameworkCompatibility.disabledChanges(34), 218865702L) < 0, "API 34 boundary wrong");
        check(Arrays.binarySearch(FrameworkCompatibility.disabledChanges(35), 166236554L) >= 0, "disabled-by-default gate enabled");
        File source = new File(System.getProperty("java.class.path"));
        File writable = new File("/data/local/tmp/compat-writable.dex");
        File readonly = new File("/data/local/tmp/compat-readonly.dex");
        try {
            copy(source, writable);
            check(writable.setWritable(true, true) && writable.canWrite(), "writable fixture is not writable");
            boolean rejected = false;
            try { load(writable); } catch (SecurityException expected) { rejected = true; }
            check(rejected == enforced, "ART writable DEX result disagrees with target SDK");
            copy(source, readonly);
            check(readonly.setReadOnly() && !readonly.canWrite(), "read-only fixture is writable");
            load(readonly);
            System.out.println("FRAMEWORK_COMPATIBILITY_OK target=" + target);
            System.out.println("FRAMEWORK_OK");
        } finally { writable.delete(); readonly.delete(); }
        System.exit(0);
    }
}
