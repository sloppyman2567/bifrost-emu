// Runs inside the real Android 15 framework through test_android_framework.py.
import android.content.ComponentName;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.os.Bundle;
import android.os.Looper;
import android.os.Process;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Arrays;

public final class FrameworkPackagesProbe {
    private static final String NAME = "org.bifrost.packagecontract";
    private static int checks;

    private static void check(boolean result, String message) {
        if (!result) throw new AssertionError(message);
        checks++;
    }

    private static int size(Object[] array) { return array == null ? 0 : array.length; }

    private interface Query { void run() throws Exception; }
    private static void missing(Query query) throws Exception {
        try {
            query.run();
            throw new AssertionError("missing component/package was returned");
        } catch (PackageManager.NameNotFoundException expected) { checks++; }
    }
    private static void invalid(Query query) throws Exception {
        try {
            query.run();
            throw new AssertionError("invalid component update was accepted");
        } catch (IllegalArgumentException expected) { checks++; }
    }

    private static <T extends ComponentInfo> T component(T info, ApplicationInfo app, String name) {
        info.name = NAME + "." + name;
        info.packageName = NAME;
        info.applicationInfo = app;
        info.enabled = true;
        info.metaData = new Bundle();
        info.metaData.putStringArray("labels", new String[] {"original"});
        return info;
    }

    private static void run() throws Exception {
        Looper.prepareMainLooper();
        Method install = ManagedLauncher.class.getDeclaredMethod("installServices", boolean.class);
        install.setAccessible(true);
        install.invoke(null, false);  // the session process, not a service child
        Object thread = Class.forName("android.app.ActivityThread").getMethod("systemMain").invoke(null);
        Context context = (Context) thread.getClass().getMethod("getSystemContext").invoke(thread);
        Field field = ManagedLauncher.class.getDeclaredField("packages");
        field.setAccessible(true);
        FrameworkPackages packages = (FrameworkPackages) field.get(null);
        PackageInfo pkg = new PackageInfo();
        pkg.packageName = NAME;
        ApplicationInfo app = pkg.applicationInfo = new ApplicationInfo();
        app.packageName = NAME;
        app.uid = Process.myUid();
        check(app.uid == 10000 && android.system.Os.getuid() == app.uid
                && android.system.Os.geteuid() == app.uid && android.os.Binder.getCallingUid() == app.uid,
                "framework/native/Binder app identity incorrect");
        app.enabled = true;
        app.targetSdkVersion = 35;
        app.sourceDir = app.publicSourceDir = "/data/app/contract/base.apk";
        app.splitSourceDirs = new String[] {"/data/app/contract/split.apk"};
        app.sharedLibraryFiles = new String[] {"/system/framework/example.jar"};
        app.metaData = new Bundle();
        app.metaData.putString("app", "original");
        pkg.activities = new ActivityInfo[] {component(new ActivityInfo(), app, "Activity")};
        pkg.receivers = new ActivityInfo[] {component(new ActivityInfo(), app, "Receiver")};
        pkg.services = new ServiceInfo[] {component(new ServiceInfo(), app, "Service")};
        pkg.providers = new ProviderInfo[] {component(new ProviderInfo(), app, "Provider")};
        pkg.providers[0].authority = NAME + ".first;" + NAME + ".second";
        pkg.receivers[0].directBootAware = true;
        pkg.providers[0].uriPermissionPatterns = new android.os.PatternMatcher[] {
                new android.os.PatternMatcher("/shared", android.os.PatternMatcher.PATTERN_PREFIX)};
        packages.install(context, pkg);
        PackageManager pm = context.getPackageManager();
        PackageInfo plain = pm.getPackageInfo(NAME, 0);
        check(plain.activities == null && plain.receivers == null && plain.services == null
                && plain.providers == null && plain.permissions == null, "default query leaked component arrays");
        check(plain.applicationInfo.metaData == null && plain.applicationInfo.sharedLibraryFiles == null,
                "default query leaked application metadata/libraries");
        int all = PackageManager.GET_ACTIVITIES | PackageManager.GET_RECEIVERS
                | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS;
        PackageInfo full = pm.getPackageInfo(NAME, all | PackageManager.GET_META_DATA);
        check(full.activities.length == 1 && full.receivers.length == 1 && full.services.length == 1
                && full.providers.length == 1, "explicit query lost components");
        check(full.applicationInfo.metaData.getString("app").equals("original"), "application metadata missing");
        full.services[0].metaData.getStringArray("labels")[0] = "changed";
        full.services[0].applicationInfo.splitSourceDirs[0] = "changed";
        full.applicationInfo.sourceDir = "changed";
        full.applicationInfo.metaData.clear();
        ComponentName service = new ComponentName(NAME, NAME + ".Service");
        ServiceInfo fresh = pm.getServiceInfo(service, PackageManager.GET_META_DATA);
        check(fresh.metaData.getStringArray("labels")[0].equals("original")
                && fresh.applicationInfo.splitSourceDirs[0].endsWith("split.apk")
                && fresh.applicationInfo.sourceDir.endsWith("base.apk"), "query mutated installed state");
        ApplicationInfo application = pm.getApplicationInfo(NAME, PackageManager.GET_META_DATA
                | PackageManager.GET_SHARED_LIBRARY_FILES);
        check(application.sharedLibraryFiles.length == 1, "explicit shared library query lost files");
        application.metaData.clear();
        application.splitSourceDirs[0] = "changed";
        check(pm.getApplicationInfo(NAME, PackageManager.GET_META_DATA).metaData.containsKey("app")
                && pm.getApplicationInfo(NAME, 0).splitSourceDirs[0].endsWith("split.apk"),
                "application query aliases installed state");
        check(pm.getApplicationInfo(NAME, 0).metaData == null, "application query ignored metadata flag");
        check(pm.getInstalledPackages(0).size() == 1
                && NAME.equals(pm.getInstalledPackages(0).get(0).packageName), "installed package list incorrect");
        check(pm.getInstalledApplications(0).size() == 1
                && NAME.equals(pm.getInstalledApplications(0).get(0).packageName), "installed application list incorrect");
        check(pm.getInstalledPackages(PackageManager.MATCH_SYSTEM_ONLY).isEmpty()
                && pm.getInstalledApplications(PackageManager.MATCH_SYSTEM_ONLY).isEmpty(), "system-only filter ignored");
        check(pm.getPackageUid(NAME, 0) == Process.myUid(), "package UID incorrect");
        check(Arrays.equals(pm.getPackagesForUid(Process.myUid()), new String[] {NAME})
                && NAME.equals(pm.getNameForUid(Process.myUid())), "reverse UID lookup incorrect");
        check(pm.getPackagesForUid(Process.myUid() + 1) == null
                && pm.getNameForUid(Process.myUid() + 1) == null, "foreign UID was found");
        missing(() -> pm.getPackageUid(NAME + ".missing", 0));
        missing(() -> pm.getApplicationInfo(NAME + ".missing", 0));
        missing(() -> pm.getPackageInfo(NAME, PackageManager.MATCH_SYSTEM_ONLY));
        ComponentName receiver = new ComponentName(NAME, NAME + ".Receiver");
        ComponentName provider = new ComponentName(NAME, NAME + ".Provider");
        check(pm.getReceiverInfo(receiver, 0).directBootAware, "receiver lookup lost direct-boot state");
        check(pm.getProviderInfo(provider, 0).authority.equals(pkg.providers[0].authority), "provider lookup incorrect");
        check(pm.getProviderInfo(provider, 0).uriPermissionPatterns == null
                && pm.getProviderInfo(provider, PackageManager.GET_URI_PERMISSION_PATTERNS)
                        .uriPermissionPatterns[0].match("/shared/file"), "provider URI pattern flag ignored");
        check(pm.resolveContentProvider(NAME + ".second", 0).name.equals(provider.getClassName()),
                "second provider authority missing");
        missing(() -> pm.getReceiverInfo(receiver, PackageManager.MATCH_DIRECT_BOOT_UNAWARE));
        missing(() -> pm.getServiceInfo(service, PackageManager.MATCH_DIRECT_BOOT_AWARE));
        PackageInfo aware = pm.getPackageInfo(NAME, all | PackageManager.MATCH_DIRECT_BOOT_AWARE);
        check(aware.receivers.length == 1 && aware.activities == null && aware.services == null
                && aware.providers == null, "package arrays ignored direct-boot filter");
        for (ComponentName name : new ComponentName[] {new ComponentName(NAME, NAME + ".Activity"),
                receiver, service, provider}) {
            pm.setComponentEnabledSetting(name, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                    PackageManager.DONT_KILL_APP);
            check(pm.getComponentEnabledSetting(name) == PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                    "component state was not retained");
            PackageInfo disabled = pm.getPackageInfo(NAME, all);
            int count = size(disabled.activities) + size(disabled.receivers)
                    + size(disabled.services) + size(disabled.providers);
            check(count == 3, "disabled component remained in package query");
            PackageInfo including = pm.getPackageInfo(NAME, all | PackageManager.MATCH_DISABLED_COMPONENTS);
            check(including.activities.length + including.receivers.length
                    + including.services.length + including.providers.length == 4,
                    "MATCH_DISABLED_COMPONENTS lost component");
            pm.setComponentEnabledSetting(name, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                    PackageManager.DONT_KILL_APP);
            check(pm.getComponentEnabledSetting(name) == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                    "default state did not restore manifest");
        }
        invalid(() -> pm.setComponentEnabledSetting(new ComponentName(NAME, NAME + ".Missing"),
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP));
        invalid(() -> pm.setComponentEnabledSetting(service, 99, PackageManager.DONT_KILL_APP));
        check(pm.getComponentEnabledSetting(service) == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                "rejected state update changed installed state");
        missing(() -> pm.getReceiverInfo(new ComponentName(NAME, NAME + ".Missing"), 0));
        missing(() -> pm.getProviderInfo(new ComponentName(NAME, NAME + ".Missing"), 0));
        check(pm.resolveContentProvider(NAME + ".missing", 0) == null, "unknown authority found");
        Field cache = Class.forName("android.os.ServiceManager").getDeclaredField("sCache");
        cache.setAccessible(true);
        android.os.IBinder binder = (android.os.IBinder) ((java.util.Map<?, ?>) cache.get(null)).get("package");
        Object remote = binder.queryLocalInterface("android.content.pm.IPackageManager");
        Class<?> iface = Class.forName("android.content.pm.IPackageManager");
        int otherUser = Process.myUid() / 100000 + 1;
        check(iface.getMethod("getApplicationInfo", String.class, long.class, int.class)
                .invoke(remote, NAME, 0L, otherUser) == null, "application query crossed users");
        check(iface.getMethod("getPackageInfo", String.class, long.class, int.class)
                .invoke(remote, NAME, 0L, otherUser) == null, "package query crossed users");
        try {
            iface.getMethod("setComponentEnabledSetting", ComponentName.class, int.class, int.class,
                    int.class, String.class).invoke(remote, service, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                            PackageManager.DONT_KILL_APP, otherUser, NAME);
            throw new AssertionError("component update crossed users");
        } catch (java.lang.reflect.InvocationTargetException expected) {
            check(expected.getCause() instanceof SecurityException, "foreign-user update exception incorrect");
        }
        check(remote.equals(remote) && !remote.equals(new Object())
                && remote.hashCode() == System.identityHashCode(remote), "local proxy identity broken");
        // Installing keeps its own snapshot, independently of the launcher's LoadedApk objects.
        app.sourceDir = "mutated by app";
        pkg.services[0].metaData.clear();
        check(pm.getServiceInfo(service, PackageManager.GET_META_DATA).metaData.containsKey("labels")
                && pm.getApplicationInfo(NAME, 0).sourceDir.endsWith("base.apk"), "install snapshot aliases launcher");
        System.out.println("FRAMEWORK_PACKAGE_CONTRACT_OK checks=" + checks);
        System.out.println("FRAMEWORK_OK");
    }

    public static void main(String[] args) {
        try { run(); System.exit(0); }
        catch (Throwable error) { error.printStackTrace(); System.exit(1); }
    }
}
