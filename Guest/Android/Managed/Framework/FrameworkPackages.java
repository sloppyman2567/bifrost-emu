// Manifest-backed package service for the single installed-app profile.
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.Signature;
import android.os.Process;
import android.os.Parcel;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.Map;
import java.util.Objects;

final class FrameworkPackages {
    // AOSP PermissionController module package.
    static final String PERMISSION_CONTROLLER = "com.android.permissioncontroller";
    private volatile PackageInfo installed;
    private final Map<String, PackageInfo> systemPackages = new ConcurrentHashMap<>();
    private final java.util.Set<String> grantedPermissions = ConcurrentHashMap.newKeySet();
    private volatile Map<String, PermissionInfo> permissionDefinitions = java.util.Collections.emptyMap();
    private Map<String, PermissionInfo> platformPermissions;
    private final java.util.Set<String> serviceFeatures = ConcurrentHashMap.newKeySet();
    private final Map<String, Map<ComponentName, List<IntentFilter>>> providerFilters = new ConcurrentHashMap<>();
    private final Map<String, Map<ComponentName, List<IntentFilter>>> receiverFilters = new ConcurrentHashMap<>();
    private final Map<String, Map<ComponentName, List<IntentFilter>>> serviceFilters = new ConcurrentHashMap<>();
    private final Map<String, Map<ComponentName, List<IntentFilter>>> activityFilters = new ConcurrentHashMap<>();
    private final FrameworkPackageChecksums checksums = new FrameworkPackageChecksums();
    private java.util.Set<String> protectedBroadcasts;

    synchronized boolean protectedBroadcast(String action) throws Exception {
        if (protectedBroadcasts == null) {
            protectedBroadcasts = new java.util.HashSet<>(FrameworkParseCache.strings(
                    new java.io.File("/system/framework/framework-res.apk"), "protected-broadcasts", () -> {
                List<String> names = new ArrayList<>();
                for (Object name : (List<?>) invoke(parseArchive(new java.io.File("/system/framework/framework-res.apk")),
                        "getProtectedBroadcasts")) names.add((String) name);
                return names;
            }));
        }
        return protectedBroadcasts.contains(action);
    }

    private FrameworkSharedLibraries sharedLibraries;

    // PackageManagerService resolves the manifest's <uses-library> entries against the system's shared libraries at
    // install time and records them in the ApplicationInfo (FrameworkSharedLibraries.apply); LoadedApk then builds
    // a class loader for each. A package whose manifest cannot be read keeps what it had (none).
    private void applySharedLibraries(PackageInfo pkg) {
        ApplicationInfo app = pkg.applicationInfo;
        java.io.File base = new java.io.File(app.sourceDir == null ? "" : app.sourceDir);
        if (!base.isFile()) return;
        java.io.File archive = app.splitSourceDirs != null && app.splitSourceDirs.length != 0
                ? base.getParentFile() : base;
        try {
            List<String> required = FrameworkParseCache.strings(archive, "uses-libraries",
                    () -> libraryNames(archive, "getUsesLibraries"));
            List<String> optional = FrameworkParseCache.strings(archive, "uses-optional-libraries",
                    () -> libraryNames(archive, "getUsesOptionalLibraries"));
            synchronized (this) {
                if (sharedLibraries == null) sharedLibraries = FrameworkSharedLibraries.system();
                FrameworkSharedLibraries.apply(app, sharedLibraries.resolve(app.targetSdkVersion, required, optional));
            }
        } catch (Exception error) {
            System.out.println("APP_SHARED_LIBRARIES_UNAVAILABLE " + pkg.packageName + ": " + error);
        }
    }

    private List<String> libraryNames(java.io.File archive, String getter) throws Exception {
        List<String> names = new ArrayList<>();
        for (Object name : (List<?>) invoke(parseArchive(archive), getter)) names.add((String) name);
        return names;
    }

    void install(Context system, PackageInfo pkg) {
        installPermissions(system, pkg);
        applySharedLibraries(pkg);
        installed = copy(pkg, PackageInfo.CREATOR);
        componentStates.clear();
        providerFilters.remove(pkg.packageName);
        serviceFilters.remove(pkg.packageName);
        receiverFilters.remove(pkg.packageName);
    }

    void installSystem(PackageInfo pkg) {
        if ((pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0)
            throw new IllegalArgumentException("Not a system package: " + pkg.packageName);
        Signature fake = fakeSignature(pkg);
        if (fake != null) fakeSignatures.put(pkg.packageName, fake);
        else fakeSignatures.remove(pkg.packageName);
        systemPackages.put(pkg.packageName, copy(pkg, PackageInfo.CREATOR));
        providerFilters.remove(pkg.packageName);
        serviceFilters.remove(pkg.packageName);
    }

    // The session app as seen from one of its child processes hosting
    // another package (microG): PackageManagerService knows every package,
    // and that package's code checks its callers by uid, name and request.
    // Its install-time grants travel in requestedPermissionsFlags.
    private final Map<String, PackageInfo> peers = new ConcurrentHashMap<>();

    void installPeer(PackageInfo pkg) {
        if ((pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0
                || systemPackages.containsKey(pkg.packageName) || pendingSystem.containsKey(pkg.packageName))
            throw new IllegalArgumentException("Not an app package: " + pkg.packageName);
        peers.put(pkg.packageName, copy(pkg, PackageInfo.CREATOR));
    }

    PackageInfo installedPackage() {
        PackageInfo pkg = installed;
        return pkg == null ? null : copy(pkg, PackageInfo.CREATOR);
    }

    // Installed app, then system packages, then peers: the names are unique.
    private PackageInfo known(String name) {
        if (name == null) return null;
        PackageInfo pkg = installed;
        if (pkg != null && pkg.packageName.equals(name)) return pkg;
        pkg = system(name);
        return pkg != null ? pkg : peers.get(name);
    }

    // microG signature spoofing (github.com/microg/patches, PackageManagerService
    // mayFakeSignature): a package targeting API > 22 that declares
    // "fake-signature" meta-data and holds FAKE_PACKAGE_SIGNATURE is reported
    // with that certificate. Provisioned system packages receive the grant, as
    // microG-enabled images give their privileged microG apps.
    private static final String FAKE_SIGNATURE_PERMISSION = "android.permission.FAKE_PACKAGE_SIGNATURE";
    private final Map<String, Signature> fakeSignatures = new ConcurrentHashMap<>();

    private static Signature fakeSignature(PackageInfo pkg) {
        String value = pkg.applicationInfo.metaData == null ? null
                : pkg.applicationInfo.metaData.getString("fake-signature");
        if (value == null || pkg.applicationInfo.targetSdkVersion <= android.os.Build.VERSION_CODES.LOLLIPOP_MR1
                || pkg.requestedPermissions == null) return null;
        for (int i = 0; i < pkg.requestedPermissions.length; i++) {
            if (!FAKE_SIGNATURE_PERMISSION.equals(pkg.requestedPermissions[i])) continue;
            pkg.requestedPermissionsFlags[i] |= PackageInfo.REQUESTED_PERMISSION_GRANTED;
            return new Signature(value);
        }
        return null;
    }

    private static android.content.pm.SigningInfo signingInfo(Signature signature) {
        try {
            // Hidden in Android 15: SigningDetails(Signature[], scheme) and
            // SigningInfo(SigningDetails). Scheme 2 is APK Signature Scheme v2.
            Class<?> details = Class.forName("android.content.pm.SigningDetails");
            Object value = details.getConstructor(Signature[].class, int.class)
                    .newInstance(new Signature[] {signature}, 2);
            return android.content.pm.SigningInfo.class.getConstructor(details).newInstance(value);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Android 15 SigningInfo construction unavailable", error);
        }
    }

    // A provisioned system package whose archive is parsed on first lookup
    // (GmsCore takes about 1.8 s in the JIT); most apps never ask for it.
    private final Map<String, java.util.concurrent.Callable<PackageInfo>> pendingSystem = new ConcurrentHashMap<>();
    private final Map<Integer, String> pendingUids = new ConcurrentHashMap<>();

    void installSystemLazily(String name, int uid, java.util.concurrent.Callable<PackageInfo> loader) {
        if (systemPackages.containsKey(name)) return;
        pendingSystem.put(name, loader);
        pendingUids.put(uid, name);
    }

    private PackageInfo system(String name) {
        if (name != null && pendingSystem.containsKey(name)) materialize(name);
        return name == null ? null : systemPackages.get(name);
    }

    private java.util.Collection<PackageInfo> allSystem() {
        for (String name : new ArrayList<>(pendingSystem.keySet())) materialize(name);
        return systemPackages.values();
    }

    private synchronized void materialize(String name) {
        java.util.concurrent.Callable<PackageInfo> loader = pendingSystem.get(name);
        if (loader == null) return;
        try {
            PackageInfo pkg = loader.call();
            if (!name.equals(pkg.packageName)) throw new IllegalStateException("Package name changed: " + name);
            installSystem(pkg);
        } catch (Exception error) {
            throw new IllegalStateException("Cannot load system package " + name, error);
        }
        pendingSystem.remove(name);
        pendingUids.values().remove(name);
    }

    PackageInfo systemPackage(String name) {
        PackageInfo pkg = system(name);
        return pkg == null ? null : copy(pkg, PackageInfo.CREATOR);
    }

    // An Activity of a provisioned system package (microG's :ui Activities
    // run in their own process on Android), or null.
    ActivityInfo systemActivity(ComponentName component) {
        PackageInfo pkg = system(component.getPackageName());
        return pkg == null ? null : activityInfo(component, 0, 0, pkg.activities);
    }

    // The best Activity for an implicit Intent when it belongs to a system
    // package, or null. Resolving materializes every pending system package.
    ActivityInfo resolveSystemActivity(Intent intent) throws Exception {
        List<android.content.pm.ResolveInfo> query = queryActivities(intent, intent.getType(),
                PackageManager.MATCH_DEFAULT_ONLY, 0);
        if (query.isEmpty() || !systemPackages.containsKey(query.get(0).activityInfo.packageName)) return null;
        return query.get(0).activityInfo;
    }

    // A parsed system package, without parsing a pending one.
    PackageInfo loadedSystemPackage(String name) {
        PackageInfo pkg = systemPackages.get(name);
        return pkg == null ? null : copy(pkg, PackageInfo.CREATOR);
    }

    // Names of the known (installed, system and peer) packages, as the package manager lists them.
    List<String> visiblePackageNames() {
        java.util.LinkedHashSet<String> names = new java.util.LinkedHashSet<>();
        if (installed != null) names.add(installed.packageName);
        names.addAll(systemPackages.keySet());
        names.addAll(peers.keySet());
        return new ArrayList<>(names);
    }

    // Uid of a known (installed, system or peer) package, or -1 when it does not exist.
    int packageUid(String name) {
        PackageInfo pkg = known(name);
        return pkg == null ? -1 : pkg.applicationInfo.uid;
    }

    PackageInfo processPackage(String name) {
        PackageInfo pkg = known(name);
        return pkg == null ? null : copy(pkg, PackageInfo.CREATOR);
    }

    // The package owning a UID (installed, system or peer), or null.
    String nameForUid(int uid) { return packageForUid(uid); }

    boolean isSystemPackage(String name) {
        PackageInfo pkg = known(name);
        return pkg != null && (pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
    }

    // PackageManagerInternal.hasSignatureCapability(AUTH): the packages share a
    // signing certificate. microG's spoofed certificate counts as the package's own.
    boolean sameSigner(String first, String second) {
        Signature[] left = signatures(first), right = signatures(second);
        if (left == null || right == null) return false;
        for (Signature one : left) for (Signature two : right) if (one.equals(two)) return true;
        return false;
    }

    private Signature[] signatures(String name) {
        PackageInfo pkg = known(name);  // loads a pending system package, registering its fake signature
        if (pkg == null) return null;
        Signature fake = fakeSignatures.get(name);
        if (fake != null) return new Signature[] {fake};
        if (pkg.signatures != null) return pkg.signatures;
        return pkg.signingInfo == null ? null : pkg.signingInfo.getApkContentsSigners();
    }

    // The manifest's <receiver> intent filters, parsed once per package from its archive (and splits).
    private Map<ComponentName, List<IntentFilter>> receiverFilters(PackageInfo pkg) throws Exception {
        Map<ComponentName, List<IntentFilter>> filters = receiverFilters.get(pkg.packageName);
        if (filters != null) return filters;
        java.io.File base = new java.io.File(pkg.applicationInfo.sourceDir);
        if (!base.isFile()) return java.util.Collections.emptyMap();
        java.io.File archive = pkg.applicationInfo.splitSourceDirs != null
                && pkg.applicationInfo.splitSourceDirs.length != 0 ? base.getParentFile() : base;
        filters = FrameworkParseCache.filters(archive, "receivers", () -> {
            Map<ComponentName, List<IntentFilter>> found = new java.util.HashMap<>();
            for (Object receiver : (List<?>) invoke(parseArchive(archive), "getReceivers")) {
                List<IntentFilter> entries = new ArrayList<>();
                for (Object entry : (List<?>) invoke(receiver, "getIntents"))
                    entries.add(new IntentFilter((IntentFilter) invoke(entry, "getIntentFilter")));
                found.put(new ComponentName(pkg.packageName, (String) invoke(receiver, "getName")), entries);
            }
            return java.util.Collections.singletonMap("receivers", found);
        }).get("receivers");
        receiverFilters.put(pkg.packageName, filters);
        return filters;
    }

    List<ActivityInfo> receivers(Intent intent, String type, int user) throws Exception {
        if (user != 0) throw new SecurityException("Foreign broadcast user");
        List<ActivityInfo> results = new ArrayList<>();
        PackageInfo pkg = installed;
        if (pkg == null || pkg.receivers == null
                || intent.getPackage() != null && !pkg.packageName.equals(intent.getPackage())) return results;
        if (intent.getComponent() != null) {
            for (ActivityInfo info : pkg.receivers)
                if (new ComponentName(pkg.packageName, info.name).equals(intent.getComponent()) && componentMatches(info, 0, user))
                    results.add(copy(info, ActivityInfo.CREATOR));
            return results;
        }
        Map<ComponentName, List<IntentFilter>> filters = receiverFilters(pkg);
        for (ActivityInfo info : pkg.receivers) {
            ComponentName name = new ComponentName(pkg.packageName, info.name);
            if (!componentMatches(info, 0, user)) continue;
            boolean matches = name.equals(intent.getComponent());
            if (intent.getComponent() == null) for (IntentFilter filter : filters.getOrDefault(name, java.util.Collections.emptyList()))
                if (filter.match(intent.getAction(), type, intent.getScheme(), intent.getData(), intent.getCategories(), "AsgardReceiver") >= 0) matches = true;
            if (matches) results.add(copy(info, ActivityInfo.CREATOR));
        }
        return results;
    }

    void provideFeature(String name) { serviceFeatures.add(name); }

    void register(FrameworkServices services) throws Exception {
        services.register("package", "android.content.pm.IPackageManager", (method, a) -> {
            switch (method) {
                case "hasSystemFeature":
                    return hasSystemFeature((String) a[0], (Integer) a[1]);
                case "getSystemAvailableFeatures": {
                    List<String> names = new ArrayList<>(serviceFeatures);
                    if (systemPackages.containsKey("com.android.webview")) names.add(PackageManager.FEATURE_WEBVIEW);
                    java.util.Collections.sort(names);
                    List<FeatureInfo> features = new ArrayList<>();
                    for (String name : names) {
                        FeatureInfo feature = new FeatureInfo(); feature.name = name;
                        features.add(feature);
                    }
                    return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                            .newInstance(features);
                }
                case "isInstantApp": return false;  // This profile installs full APKs only.
                // Activity.requestPermissions targets this package; FrameworkActivities
                // answers its REQUEST_PERMISSIONS intent from the install-time grants.
                case "getPermissionControllerPackageName": return PERMISSION_CONTROLLER;
                case "notifyPackageUse": return null;  // one-way usage report
                // (name, flags, user): null means "not installed", which the
                // client turns into NameNotFoundException.
                case "getPackageInfo":
                    return packageInfo((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getApplicationInfo":
                    return applicationInfo((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getInstalledPackages":
                case "getInstalledApplications": {
                    List<Object> values = new ArrayList<>();
                    allSystem();
                    List<String> names = new ArrayList<>(systemPackages.keySet());
                    if (installed != null) names.add(installed.packageName);
                    java.util.Collections.sort(names);
                    for (String name : names) {
                        PackageInfo info = packageInfo(name, ((Number) a[0]).longValue(), (Integer) a[1]);
                        if (info != null) values.add(method.equals("getInstalledPackages") ? info : info.applicationInfo);
                    }
                    return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                            .newInstance(values);
                }
                case "getPackageUid": {
                    PackageInfo pkg = findPackage((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                    return pkg == null ? -1 : pkg.applicationInfo.uid;
                }
                case "getPackagesForUid": {
                    String name = packageForUid((Integer) a[0]);
                    return name == null ? null : new String[] {name};
                }
                case "getNameForUid":
                    return packageForUid((Integer) a[0]);
                case "getReceiverInfo":
                    return activityInfo((ComponentName) a[0], ((Number) a[1]).longValue(),
                            (Integer) a[2], installed == null ? null : installed.receivers);
                case "getProviderInfo":
                    return providerInfo((ComponentName) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getActivityInfo":
                    return activityInfo((ComponentName) a[0], ((Number) a[1]).longValue(),
                            (Integer) a[2], installed == null ? null : installed.activities);
                case "resolveIntent":
                    return resolveActivity((Intent) a[0], (String) a[1], ((Number) a[2]).longValue(), (Integer) a[3]);
                case "queryIntentActivities":
                    return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                            .newInstance(queryActivityComponents((Intent) a[0], (String) a[1],
                                    ((Number) a[2]).longValue(), (Integer) a[3], false));
                case "queryIntentReceivers":
                    return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                            .newInstance(queryActivityComponents((Intent) a[0], (String) a[1],
                                    ((Number) a[2]).longValue(), (Integer) a[3], true));
                case "getApplicationEnabledSetting":
                    return applicationEnabledSetting((String) a[0], (Integer) a[1]);
                // (packageName, includeSplits, optional, required, trustedInstallers, listener, userId)
                case "requestPackageChecksums": {
                    ApplicationInfo info = applicationInfo((String) a[0], 0, (Integer) a[6]);
                    if (info == null) throw (RuntimeException) Class.forName("android.os.ParcelableException")
                            .getConstructor(Throwable.class)
                            .newInstance(new PackageManager.NameNotFoundException((String) a[0]));
                    checksums.request(info, (Boolean) a[1], (Integer) a[2], (Integer) a[3], a[5]);
                    return null;
                }
                case "resolveService":
                    return resolveService((Intent) a[0], (String) a[1], ((Number) a[2]).longValue(), (Integer) a[3]);
                case "queryIntentServices":
                    return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                            .newInstance(queryServices((Intent) a[0], (String) a[1],
                                    ((Number) a[2]).longValue(), (Integer) a[3]));
                case "getServiceInfo":
                    return serviceInfo((ComponentName) a[0], ((Number) a[1]).longValue(),
                            (Integer) a[2]);
                case "resolveContentProvider":
                    return resolveProvider((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "queryIntentContentProviders":
                    return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                            .newInstance(queryProviders((Intent) a[0], (String) a[1],
                                    ((Number) a[2]).longValue(), (Integer) a[3]));
                case "getInstallerPackageName":
                    if (installed == null || !installed.packageName.equals(a[0]))
                        throw new IllegalArgumentException("Unknown package: " + a[0]);
                    return null;  // Host-installed APK, no Android installer package.
                case "getInstallSourceInfo":
                    if (installed == null || !installed.packageName.equals(a[0])
                            || (Integer) a[1] != installed.applicationInfo.uid / 100000) return null;
                    return InstallSourceInfo.class.getConstructor(String.class,
                            android.content.pm.SigningInfo.class, String.class, String.class)
                            .newInstance(null, null, null, null);
                // An app may change its own components (PackageManagerService
                // rejects components of packages that are not installed).
                case "getComponentEnabledSetting":
                    return componentEnabledSetting((ComponentName) a[0], (Integer) a[1]);
                case "setComponentEnabledSetting":
                    ComponentName component = ownComponent(a[0], (Integer) a[3], true);
                    int state = (Integer) a[1];
                    if (state < PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
                            || state > PackageManager.COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED)
                        throw new IllegalArgumentException("Invalid new component state: " + state);
                    if (state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT) componentStates.remove(component);
                    else componentStates.put(component, state);
                    return null;
                default: throw new UnsupportedOperationException("package." + method);
            }
        });
    }

    private final Map<ComponentName, Integer> componentStates = new ConcurrentHashMap<>();

    private int componentEnabledSetting(ComponentName component, int userId) {
        if (userId != Process.myUid() / 100000)
            throw new SecurityException("Foreign package user: " + userId);
        if (component == null) return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
        if (findPackage(component.getPackageName(), 0, userId) == null)
            throw new IllegalArgumentException("Unknown component: " + component);
        // Reading a visible system package's override is allowed; changing
        // another UID's components still goes through ownComponent below.
        return componentStates.getOrDefault(component, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);
    }

    // ComputerEngine.getApplicationEnabledSetting: a visible package's
    // state; no application-level state is ever changed in this profile.
    private int applicationEnabledSetting(String name, int userId) {
        if (userId != Process.myUid() / 100000)
            throw new SecurityException("Foreign package user: " + userId);
        if (findPackage(name, 0, userId) == null) throw new IllegalArgumentException("Unknown package: " + name);
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    private ComponentName ownComponent(Object argument, int userId, boolean declared) {
        ComponentName component = (ComponentName) argument;
        if (installed == null || component == null
                || !installed.packageName.equals(component.getPackageName()))
            throw new IllegalArgumentException("Unknown component: " + component);
        if (userId != installed.applicationInfo.uid / 100000)
            throw new SecurityException("Foreign package user: " + userId);
        if (declared) {
            for (ComponentInfo[] group : new ComponentInfo[][] {installed.activities, installed.receivers,
                    installed.services, installed.providers}) {
                if (group == null) continue;
                for (ComponentInfo info : group)
                    if (info.name.equals(component.getClassName())) return component;
            }
            throw new IllegalArgumentException("Component class does not exist: " + component);
        }
        return component;
    }

    // Every package here has its own UID (no shared user IDs).
    private String packageForUid(int uid) {
        PackageInfo pkg = installed;
        if (pkg != null && pkg.applicationInfo.uid == uid) return pkg.packageName;
        String pending = pendingUids.get(uid);
        if (pending != null) return pending;
        for (PackageInfo system : systemPackages.values())
            if (system.applicationInfo.uid == uid) return system.packageName;
        for (PackageInfo peer : peers.values())
            if (peer.applicationInfo.uid == uid) return peer.packageName;
        return null;
    }

    private boolean packageMatches(String name, long flags, int userId) {
        return findPackage(name, flags, userId) != null;
    }

    private PackageInfo findPackage(String name, long flags, int userId) {
        if (name == null) return null;
        PackageInfo pkg = known(name);
        return pkg != null && userId == pkg.applicationInfo.uid / 100000
                && ((flags & PackageManager.MATCH_SYSTEM_ONLY) == 0
                    || (pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0) ? pkg : null;
    }

    private static <T extends android.os.Parcelable> T copy(T value, android.os.Parcelable.Creator<T> creator) {
        Parcel parcel = Parcel.obtain();
        try {
            value.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return creator.createFromParcel(parcel);
        } finally {
            parcel.recycle();
        }
    }

    private ApplicationInfo applicationInfo(String name, long flags, int userId) {
        if (!packageMatches(name, flags, userId)) return null;
        ApplicationInfo result = copy(findPackage(name, flags, userId).applicationInfo, ApplicationInfo.CREATOR);
        filterApplicationInfo(result, flags);
        return result;
    }

    private static void filterApplicationInfo(ApplicationInfo result, long flags) {
        if ((flags & PackageManager.GET_META_DATA) == 0) result.metaData = null;
        if ((flags & PackageManager.GET_SHARED_LIBRARY_FILES) == 0) {
            result.sharedLibraryFiles = null;
            try {
                // Hidden field paired with sharedLibraryFiles in Android 15.
                ApplicationInfo.class.getField("sharedLibraryInfos").set(result, null);
            } catch (ReflectiveOperationException error) {
                throw new IllegalStateException("Android 15 shared library field unavailable", error);
            }
        }
    }

    // PackageManagerService returns a manifest ServiceInfo, not a running
    // service. Firebase discovers its registrars through this metadata.
    private boolean componentMatches(ComponentInfo info, long flags, int userId) {
        PackageInfo pkg = findPackage(info.packageName, flags, userId);
        if (pkg == null || userId != pkg.applicationInfo.uid / 100000
                || !pkg.packageName.equals(info.packageName)) return false;
        if ((flags & PackageManager.MATCH_SYSTEM_ONLY) != 0
                && (pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0) return false;
        int state = componentStates.getOrDefault(new ComponentName(info.packageName, info.name),
                PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);
        boolean enabled = state == PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                || (state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && info.enabled);
        if ((flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0
                && (!pkg.applicationInfo.enabled || !enabled)) return false;
        long bootFlags = flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE);
        // ComputerEngine.updateFlags supplies the defaults server-side.
        // This profile's user is unlocked, so implicit queries see both.
        if (bootFlags == 0) bootFlags = PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        return (bootFlags & (info.directBootAware ? PackageManager.MATCH_DIRECT_BOOT_AWARE
                : PackageManager.MATCH_DIRECT_BOOT_UNAWARE)) != 0;
    }

    private void installedComponentInfo(ComponentInfo result, long flags) {
        result.applicationInfo = applicationInfo(result.packageName, flags,
                result.applicationInfo.uid / 100000);
        if ((flags & PackageManager.GET_META_DATA) == 0) result.metaData = null;
        if (result instanceof ProviderInfo && (flags & PackageManager.GET_URI_PERMISSION_PATTERNS) == 0)
            ((ProviderInfo) result).uriPermissionPatterns = null;
    }

    // Filter the parcel copy, never the installed record. A local proxy must
    // provide the same ownership boundary as a Binder reply, including arrays.
    private <T extends ComponentInfo> T[] components(T[] values, long flags, int userId, int queryFlag) {
        if ((flags & queryFlag) == 0 || values == null) return null;
        List<T> matches = new ArrayList<>();
        for (T value : values) if (componentMatches(value, flags, userId)) {
            installedComponentInfo(value, flags);
            matches.add(value);
        }
        // AOSP ArrayUtils.trimToSize returns null when no component matches.
        return matches.isEmpty() ? null : matches.toArray(Arrays.copyOf(values, matches.size()));
    }

    private PackageInfo packageInfo(String name, long flags, int userId) {
        if (!packageMatches(name, flags, userId)) return null;
        PackageInfo result = copy(findPackage(name, flags, userId), PackageInfo.CREATOR);
        filterApplicationInfo(result.applicationInfo, flags);
        result.activities = components(result.activities, flags, userId, PackageManager.GET_ACTIVITIES);
        result.receivers = components(result.receivers, flags, userId, PackageManager.GET_RECEIVERS);
        result.services = components(result.services, flags, userId, PackageManager.GET_SERVICES);
        result.providers = components(result.providers, flags, userId, PackageManager.GET_PROVIDERS);
        if ((flags & PackageManager.GET_INSTRUMENTATION) == 0) result.instrumentation = null;
        else if ((flags & PackageManager.GET_META_DATA) == 0 && result.instrumentation != null)
            for (android.content.pm.InstrumentationInfo info : result.instrumentation) info.metaData = null;
        if ((flags & PackageManager.GET_GIDS) == 0) result.gids = null;
        if ((flags & PackageManager.GET_CONFIGURATIONS) == 0) {
            result.configPreferences = null;
            result.reqFeatures = null;
            result.featureGroups = null;
        }
        Signature fake = fakeSignatures.get(name);
        if ((flags & PackageManager.GET_SIGNATURES) == 0) result.signatures = null;
        else if (fake != null) result.signatures = new Signature[] {fake};
        if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) == 0) result.signingInfo = null;
        else if (fake != null) result.signingInfo = signingInfo(fake);
        if ((flags & PackageManager.GET_PERMISSIONS) == 0) {
            result.permissions = null;
            result.requestedPermissions = null;
            result.requestedPermissionsFlags = null;
        } else if ((flags & PackageManager.GET_META_DATA) == 0 && result.permissions != null) {
            for (PermissionInfo permission : result.permissions) permission.metaData = null;
        }
        return result;
    }

    private ActivityInfo activityInfo(ComponentName component, long flags, int userId, ActivityInfo[] activities) {
        if (component == null || activities == null
                || !packageMatches(component.getPackageName(), flags, userId)) return null;
        for (ActivityInfo activity : activities) {
            if (!activity.name.equals(component.getClassName())) continue;
            if (!componentMatches(activity, flags, userId)) return null;
            ActivityInfo result = copy(activity, ActivityInfo.CREATOR);
            installedComponentInfo(result, flags);
            return result;
        }
        return null;
    }

    private ProviderInfo providerInfo(ComponentName component, long flags, int userId) {
        if (component == null) return null;
        PackageInfo pkg = findPackage(component.getPackageName(), flags, userId);
        if (pkg == null || pkg.providers == null) return null;
        for (ProviderInfo provider : pkg.providers) {
            if (!provider.name.equals(component.getClassName())) continue;
            if (!componentMatches(provider, flags, userId)) return null;
            ProviderInfo result = copy(provider, ProviderInfo.CREATOR);
            installedComponentInfo(result, flags);
            return result;
        }
        return null;
    }

    private Map<ComponentName, List<IntentFilter>> providerFilters(PackageInfo pkg) throws Exception {
        if (!providerFilters.containsKey(pkg.packageName)) parseFilters(pkg);
        return providerFilters.get(pkg.packageName);
    }

    private Map<ComponentName, List<IntentFilter>> serviceFilters(PackageInfo pkg) throws Exception {
        if (!serviceFilters.containsKey(pkg.packageName)) parseFilters(pkg);
        return serviceFilters.get(pkg.packageName);
    }

    // PackageInfo omits intent filters. Reuse the image's manifest parser
    // for their full action/category/type/URI semantics rather than guess
    // from component names. Parse the installed cluster, including splits,
    // once for both kinds: GmsCore takes seconds to parse.
    private final Object filterParse = new Object();

    private void parseFilters(PackageInfo pkg) throws Exception {
        synchronized (filterParse) { parseFiltersLocked(pkg); }
    }

    private void parseFiltersLocked(PackageInfo pkg) throws Exception {
        if (providerFilters.containsKey(pkg.packageName) && serviceFilters.containsKey(pkg.packageName)
                && activityFilters.containsKey(pkg.packageName)) return;
        Map<ComponentName, List<IntentFilter>> providers = new java.util.HashMap<>(), services = new java.util.HashMap<>(),
                activities = new java.util.HashMap<>();
        java.io.File base = new java.io.File(pkg.applicationInfo.sourceDir);
        boolean declared = pkg.providers != null && pkg.providers.length != 0
                || pkg.services != null && pkg.services.length != 0
                || pkg.activities != null && pkg.activities.length != 0;
        if (base.isFile() && declared) {
            java.io.File archive = pkg.applicationInfo.splitSourceDirs != null
                    && pkg.applicationInfo.splitSourceDirs.length != 0 ? base.getParentFile() : base;
            Map<String, Map<ComponentName, List<IntentFilter>>> groups = FrameworkParseCache.filters(archive, "filters", () -> {
                Object parsed = parseArchive(archive);
                Map<String, Map<ComponentName, List<IntentFilter>>> found = new java.util.HashMap<>();
                for (String[] kind : new String[][] {{"providers", "getProviders"}, {"services", "getServices"},
                        {"activities", "getActivities"}}) {  // activity-aliases included
                    Map<ComponentName, List<IntentFilter>> filters = new java.util.HashMap<>();
                    componentFilters(pkg, parsed, kind[1], filters);
                    found.put(kind[0], filters);
                }
                return found;
            });
            providers = groups.get("providers"); services = groups.get("services"); activities = groups.get("activities");
        }
        providerFilters.putIfAbsent(pkg.packageName, providers);
        serviceFilters.putIfAbsent(pkg.packageName, services);
        activityFilters.putIfAbsent(pkg.packageName, activities);
    }

    private Map<ComponentName, List<IntentFilter>> activityFilters(PackageInfo pkg) throws Exception {
        if (!activityFilters.containsKey(pkg.packageName)) parseFilters(pkg);
        return activityFilters.get(pkg.packageName);
    }

    private static void componentFilters(PackageInfo pkg, Object parsed, String getter,
            Map<ComponentName, List<IntentFilter>> filters) throws Exception {
        for (Object component : (List<?>) invoke(parsed, getter)) {
            List<IntentFilter> intents = new ArrayList<>();
            for (Object intent : (List<?>) invoke(component, "getIntents"))
                intents.add(new IntentFilter((IntentFilter) invoke(intent, "getIntentFilter")));
            filters.put(new ComponentName(pkg.packageName, (String) invoke(component, "getName")), intents);
        }
    }

    // ParsingPackageUtils.parseDefault, the one-off parse outside
    // PackageManagerService. Unlike the legacy PackageParser it takes split
    // permissions as an argument instead of reading IPermissionManager, which
    // this profile does not publish. Split permissions only extend requested
    // permissions, so an empty list leaves the parsed components unchanged.
    // Inputs mirror PackageManager.getPackageArchiveInfo.
    private Object parseArchive(java.io.File archive) throws Exception {
        Class<?> utils = Class.forName("com.android.internal.pm.pkg.parsing.ParsingPackageUtils");
        Class<?> callbackType = Class.forName(utils.getName() + "$Callback");
        Class<?> input = Class.forName("android.content.pm.parsing.result.ParseInput");
        Method forParsing = Class.forName("com.android.internal.pm.parsing.pkg.PackageImpl").getMethod(
                "forParsing", String.class, String.class, String.class, android.content.res.TypedArray.class,
                boolean.class, callbackType);
        Object callback = java.lang.reflect.Proxy.newProxyInstance(callbackType.getClassLoader(),
                new Class<?>[] {callbackType}, (self, method, a) -> {
                    switch (method.getName()) {
                        case "hasFeature": return hasSystemFeature((String) a[0], 0);
                        case "startParsingPackage":
                            return forParsing.invoke(null, a[0], a[1], a[2], a[3], a[4], self);
                        case "getHiddenApiWhitelistedApps":
                        case "getInstallConstraintsAllowlist": return java.util.Collections.emptySet();
                        case "hashCode": return System.identityHashCode(self);
                        case "equals": return self == a[0];
                        case "toString": return "BifrostParsingCallback";
                        default: throw new AssertionError(method);
                    }
                });
        Object result = utils.getMethod("parseDefault", input, java.io.File.class, int.class, List.class,
                boolean.class, callbackType).invoke(null,
                Class.forName("android.content.pm.parsing.result.ParseTypeImpl")
                        .getMethod("forParsingWithoutPlatformCompat").invoke(null),
                archive, 0, java.util.Collections.emptyList(), false, callback);
        if ((Boolean) invoke(result, "isError"))
            throw new IllegalStateException("Failed to parse " + archive + ": " + invoke(result, "getErrorMessage"),
                    (Exception) invoke(result, "getException"));
        return invoke(result, "getResult");
    }

    private static Object invoke(Object target, String name) throws Exception {
        return target.getClass().getMethod(name).invoke(target);
    }

    private boolean hasSystemFeature(String name, int version) {
        return version <= 0 && (serviceFeatures.contains(name)
                || PackageManager.FEATURE_WEBVIEW.equals(name) && systemPackages.containsKey("com.android.webview"));
    }

    private List<android.content.pm.ResolveInfo> queryProviders(Intent original, String type,
            long flags, int userId) throws Exception {
        if (original == null) throw new IllegalArgumentException("Null provider intent");
        Intent intent = original.getComponent() == null && original.getSelector() != null
                ? original.getSelector() : original;
        List<android.content.pm.ResolveInfo> results = new ArrayList<>();
        if (intent.getComponent() != null) {
            ProviderInfo info = providerInfo(intent.getComponent(), flags, userId);
            if (info != null) {
                android.content.pm.ResolveInfo result = new android.content.pm.ResolveInfo();
                result.providerInfo = info;
                results.add(result);
            }
            return results;
        }
        List<PackageInfo> packages = new ArrayList<>(allSystem());
        if (installed != null) packages.add(installed);
        for (PackageInfo pkg : packages) {
            if (findPackage(pkg.packageName, flags, userId) == null || pkg.providers == null
                    || (intent.getPackage() != null && !intent.getPackage().equals(pkg.packageName))) continue;
            Map<ComponentName, List<IntentFilter>> filters = providerFilters(pkg);
            for (ProviderInfo info : pkg.providers) {
                if (!componentMatches(info, flags, userId)) continue;
                android.content.pm.ResolveInfo best = null;
                for (IntentFilter filter : filters.getOrDefault(new ComponentName(pkg.packageName, info.name),
                        java.util.Collections.emptyList())) {
                    int match = filter.match(intent.getAction(), type, intent.getScheme(), intent.getData(),
                            intent.getCategories(), "BifrostPackageResolver");
                    boolean isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
                    if (match < 0 || ((flags & PackageManager.MATCH_DEFAULT_ONLY) != 0 && !isDefault)) continue;
                    android.content.pm.ResolveInfo result = new android.content.pm.ResolveInfo();
                    result.providerInfo = providerInfo(new ComponentName(pkg.packageName, info.name), flags, userId);
                    result.priority = filter.getPriority();
                    result.match = match;
                    result.isDefault = isDefault;
                    if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) result.filter = new IntentFilter(filter);
                    if (best == null || result.priority > best.priority
                            || (result.priority == best.priority && result.match > best.match)) best = result;
                }
                if (best != null) results.add(best);
            }
        }
        results.sort((a, b) -> {
            int order = Integer.compare(b.priority, a.priority);
            if (order == 0) order = Boolean.compare(b.isDefault, a.isDefault);
            if (order == 0) order = Integer.compare(b.match, a.match);
            return order;
        });
        return results;
    }

    private synchronized void installPermissions(Context system, PackageInfo pkg) {
        // The single-app profile grants manifest-requested normal permissions
        // at install time, including WRITE_SETTINGS's pre-M compatibility
        // grant, and signature permissions of same-signer packages (below).
        // Other runtime/privileged grants require unavailable services;
        // unknown permission names stay denied.
        // The runtime image is immutable. Retain its definitions independently
        // of app installation/grants, as PackageManager's permission registry
        // does; app permissions and target-dependent grants are rebuilt below.
        if (platformPermissions == null) {
            PackageInfo platform = FrameworkParseCache.archiveInfo(system.getPackageManager(),
                    "/system/framework/framework-res.apk", PackageManager.GET_PERMISSIONS);
            if (platform == null || platform.permissions == null)
                throw new IllegalStateException("platform permission definitions unavailable");
            Map<String, PermissionInfo> definitions = new java.util.HashMap<>();
            for (PermissionInfo permission : platform.permissions) definitions.put(permission.name, permission);
            platformPermissions = java.util.Collections.unmodifiableMap(definitions);
        }
        Map<String, PermissionInfo> definitions = new java.util.HashMap<>(platformPermissions);
        if (pkg.permissions != null)
            for (PermissionInfo permission : pkg.permissions) definitions.putIfAbsent(permission.name, permission);
        permissionDefinitions = java.util.Collections.unmodifiableMap(definitions);
        grantedPermissions.clear();
        if (pkg.requestedPermissions == null) return;
        for (int i = 0; i < pkg.requestedPermissions.length; i++) {
            String name = pkg.requestedPermissions[i];
            PermissionInfo definition = definitions.get(name);
            boolean legacySettings = name.equals("android.permission.WRITE_SETTINGS") && pkg.applicationInfo.targetSdkVersion < 23;
            if (definition != null && ((definition.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE)
                    == PermissionInfo.PROTECTION_NORMAL || legacySettings) || signatureGranted(pkg, name)) {
                grantedPermissions.add(name);
                pkg.requestedPermissionsFlags[i] |= PackageInfo.REQUESTED_PERMISSION_GRANTED;
            }
        }
    }

    // Platform (framework-res) and app-defined permissions, as installed.
    // Callers receive copies: PermissionInfo is mutable.
    PermissionInfo permissionInfo(String name) {
        PermissionInfo info = name == null ? null : permissionDefinitions.get(name);
        return info == null ? null : new PermissionInfo(info);
    }

    // Permissions declaring `group`; a null group selects ungrouped ones.
    java.util.List<PermissionInfo> permissionsInGroup(String group) {
        java.util.List<PermissionInfo> result = new java.util.ArrayList<>();
        for (PermissionInfo info : permissionDefinitions.values())
            if (Objects.equals(info.group, group)) result.add(new PermissionInfo(info));
        result.sort(java.util.Comparator.comparing(info -> info.name));
        return result;
    }

    // The installed app's targetSdkVersion (compat-change gates), or 0.
    int targetSdk() {
        PackageInfo pkg = installed;
        return pkg == null ? 0 : pkg.applicationInfo.targetSdkVersion;
    }

    // Whether the installed app's manifest requests `permission`.
    boolean requested(String permission) {
        PackageInfo pkg = installed;
        return pkg != null && pkg.requestedPermissions != null
                && Arrays.asList(pkg.requestedPermissions).contains(permission);
    }

    // Whether the calling app holds the permission (services' enforce checks).
    boolean granted(String permission) {
        return checkPermission(permission, android.os.Process.myUid(), Context.DEVICE_ID_DEFAULT)
                == PackageManager.PERMISSION_GRANTED;
    }

    private static final String ACCOUNT_MANAGER = "android.permission.ACCOUNT_MANAGER";

    int checkPermission(String permission, int uid, int deviceId) {
        if (permission == null || deviceId != Context.DEVICE_ID_DEFAULT) return PackageManager.PERMISSION_DENIED;
        PackageInfo pkg = installed;
        // The account service calls authenticators as the system, which
        // AbstractAccountAuthenticator requires to hold ACCOUNT_MANAGER; its
        // calls arrive from the session app's uid (here, or as a peer in microG's processes).
        if (ACCOUNT_MANAGER.equals(permission) && (pkg != null && uid == pkg.applicationInfo.uid
                || peers.values().stream().anyMatch(peer -> peer.applicationInfo.uid == uid)))
            return PackageManager.PERMISSION_GRANTED;
        if (pkg != null && uid == pkg.applicationInfo.uid)
            return grantedPermissions.contains(permission) ? PackageManager.PERMISSION_GRANTED
                    : PackageManager.PERMISSION_DENIED;
        String pending = pendingUids.get(uid);
        if (pending != null) materialize(pending);
        for (PackageInfo system : systemPackages.values())
            if (system.applicationInfo.uid == uid && (pkg == null || !pkg.packageName.equals(system.packageName)))
                return systemGranted(system, permission) ? PackageManager.PERMISSION_GRANTED
                        : PackageManager.PERMISSION_DENIED;
        for (PackageInfo peer : peers.values()) {
            if (peer.applicationInfo.uid != uid || peer.requestedPermissions == null) continue;
            for (int i = 0; i < peer.requestedPermissions.length; i++)
                if (permission.equals(peer.requestedPermissions[i])
                        && (peer.requestedPermissionsFlags[i] & PackageInfo.REQUESTED_PERMISSION_GRANTED) != 0)
                    return PackageManager.PERMISSION_GRANTED;
        }
        return PackageManager.PERMISSION_DENIED;
    }

    // A provisioned system package's install-time grants, as
    // PermissionManagerService computes them: requested permissions that are
    // normal, or signature permissions declared by a package with the same
    // signing certificates (microG declares its own and shares its key with
    // the Play Store companion). Privileged allowlists and runtime grants are
    // not modelled.
    private boolean systemGranted(PackageInfo pkg, String permission) {
        int index = pkg.requestedPermissions == null ? -1
                : Arrays.asList(pkg.requestedPermissions).indexOf(permission);
        if (index < 0) return false;
        if ((pkg.requestedPermissionsFlags[index] & PackageInfo.REQUESTED_PERMISSION_GRANTED) != 0) return true;
        PermissionInfo definition = platformPermissions == null ? null : platformPermissions.get(permission);
        if (definition != null && (definition.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE)
                == PermissionInfo.PROTECTION_NORMAL) return true;
        return signatureGranted(pkg, permission);
    }

    // A normal or signature permission declared by `pkg` itself or by a known
    // package with the same signing certificates (PermissionManagerService's
    // signature grant, without the platform-key case). A normal permission
    // declared by an unrelated package is not granted: system packages load
    // lazily, so that grant would depend on parse order.
    private boolean signatureGranted(PackageInfo pkg, String permission) {
        List<PackageInfo> declarers = new ArrayList<>();
        declarers.add(pkg);
        declarers.addAll(systemPackages.values());
        declarers.addAll(peers.values());
        if (installed != null) declarers.add(installed);
        for (PackageInfo owner : declarers) {
            if (owner.permissions == null) continue;
            for (PermissionInfo declared : owner.permissions) {
                if (!declared.name.equals(permission)) continue;
                int base = declared.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE;
                boolean signer = owner == pkg || owner.packageName.equals(pkg.packageName)
                        || owner.signatures != null && Arrays.equals(owner.signatures, pkg.signatures);
                return signer && (base == PermissionInfo.PROTECTION_NORMAL || base == PermissionInfo.PROTECTION_SIGNATURE);
            }
        }
        return false;
    }

    // ResolveIntentHelper.resolveIntentInternal (not for a start) and
    // chooseBestActivity: no match, the only match, or a first match with a
    // higher priority or a different default. An even tie asks the user
    // through the platform ResolverActivity, which this profile does not
    // model (no preferred activities, no "android" package).
    private android.content.pm.ResolveInfo resolveActivity(Intent intent, String type, long flags, int userId)
            throws Exception {
        List<android.content.pm.ResolveInfo> query = queryActivities(intent, type, flags, userId);
        if (query.isEmpty()) return null;
        if (query.size() == 1) return query.get(0);
        android.content.pm.ResolveInfo first = query.get(0), second = query.get(1);
        if (first.priority != second.priority || first.preferredOrder != second.preferredOrder
                || first.isDefault != second.isDefault) return first;
        throw new UnsupportedOperationException("package.resolveIntent: ResolverActivity for " + intent);
    }

    private List<android.content.pm.ResolveInfo> queryActivities(Intent original, String type, long flags, int userId)
            throws Exception {
        return queryActivityComponents(original, type, flags, userId, false);
    }

    // ComputerEngine.queryIntentActivitiesInternal: an explicit component, or
    // every enabled activity whose filters match (only CATEGORY_DEFAULT
    // filters with MATCH_DEFAULT_ONLY, as ActivityIntentResolver), restricted
    // to the Intent's package and ordered like queryServices. `receivers` runs the same query over the
    // manifest's <receiver> elements for queryIntentReceivers (PackageManager.queryBroadcastReceivers),
    // modelled on the activity query: same matching, ordering and flags.
    private List<android.content.pm.ResolveInfo> queryActivityComponents(Intent original, String type, long flags,
            int userId, boolean receivers) throws Exception {
        Intent intent = original.getComponent() == null && original.getSelector() != null
                ? original.getSelector() : original;
        List<android.content.pm.ResolveInfo> results = new ArrayList<>();
        if (intent.getComponent() != null) {
            PackageInfo owner = findPackage(intent.getComponent().getPackageName(), flags, userId);
            ActivityInfo info = owner == null ? null : activityInfo(intent.getComponent(), flags, userId, receivers ? owner.receivers : owner.activities);
            if (info != null) {
                android.content.pm.ResolveInfo result = new android.content.pm.ResolveInfo();
                result.activityInfo = info;
                results.add(result);
            }
            return results;
        }
        List<PackageInfo> candidates = new ArrayList<>();
        if (intent.getPackage() != null) {
            PackageInfo pkg = findPackage(intent.getPackage(), flags, userId);
            if (pkg != null) candidates.add(pkg);
        } else {
            candidates.addAll(allSystem());
            candidates.addAll(peers.values());
            if (installed != null) candidates.add(installed);
        }
        boolean defaultOnly = (flags & PackageManager.MATCH_DEFAULT_ONLY) != 0;
        for (PackageInfo pkg : candidates) {
            final ActivityInfo[] components = receivers ? pkg.receivers : pkg.activities;
            if (components == null || findPackage(pkg.packageName, flags, userId) == null) continue;
            Map<ComponentName, List<IntentFilter>> filters = receivers ? receiverFilters(pkg) : activityFilters(pkg);
            for (ActivityInfo info : components) {
                if (!componentMatches(info, flags, userId)) continue;
                ComponentName component = new ComponentName(pkg.packageName, info.name);
                android.content.pm.ResolveInfo best = null;
                for (IntentFilter filter : filters.getOrDefault(component, java.util.Collections.emptyList())) {
                    boolean isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
                    if (defaultOnly && !isDefault) continue;
                    int match = filter.match(intent.getAction(), type, intent.getScheme(), intent.getData(),
                            intent.getCategories(), "BifrostPackageResolver");
                    if (match < 0) continue;
                    android.content.pm.ResolveInfo result = new android.content.pm.ResolveInfo();
                    result.activityInfo = activityInfo(component, flags, userId, components);
                    result.priority = filter.getPriority();
                    result.match = match;
                    result.isDefault = isDefault;
                    if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) result.filter = new IntentFilter(filter);
                    if (best == null || result.priority > best.priority
                            || (result.priority == best.priority && result.match > best.match)) best = result;
                }
                if (best != null) results.add(best);
            }
        }
        results.sort((a, b) -> {
            int order = Integer.compare(b.priority, a.priority);
            if (order == 0) order = Boolean.compare(b.isDefault, a.isDefault);
            if (order == 0) order = Integer.compare(b.match, a.match);
            return order;
        });
        return results;
    }

    private android.content.pm.ResolveInfo resolveService(Intent intent, String type, long flags, int userId) throws Exception {
        List<android.content.pm.ResolveInfo> results = queryServices(intent, type, flags, userId);
        return results.isEmpty() ? null : results.get(0);
    }

    // ComputerEngine.queryIntentServicesInternal: an explicit component, or
    // every enabled service whose filters match, restricted to the Intent's
    // package and ordered by priority.
    List<android.content.pm.ResolveInfo> queryServices(Intent original, String type, long flags, int userId) throws Exception {
        if (original == null) throw new IllegalArgumentException("Null service intent");
        Intent intent = original.getComponent() == null && original.getSelector() != null
                ? original.getSelector() : original;
        List<android.content.pm.ResolveInfo> results = new ArrayList<>();
        if (intent.getComponent() != null) {
            ServiceInfo info = serviceInfo(intent.getComponent(), flags, userId);
            if (info != null) {
                android.content.pm.ResolveInfo result = new android.content.pm.ResolveInfo();
                result.serviceInfo = info;
                results.add(result);
            }
            return results;
        }
        List<PackageInfo> candidates = new ArrayList<>();
        if (intent.getPackage() != null) {
            PackageInfo pkg = findPackage(intent.getPackage(), flags, userId);
            if (pkg != null) candidates.add(pkg);
        } else {
            candidates.addAll(allSystem());
            candidates.addAll(peers.values());
            if (installed != null) candidates.add(installed);
        }
        for (PackageInfo pkg : candidates) {
            if (pkg.services == null || findPackage(pkg.packageName, flags, userId) == null) continue;
            Map<ComponentName, List<IntentFilter>> filters = serviceFilters(pkg);
            for (ServiceInfo info : pkg.services) {
                if (!componentMatches(info, flags, userId)) continue;
                android.content.pm.ResolveInfo best = null;
                for (IntentFilter filter : filters.getOrDefault(new ComponentName(pkg.packageName, info.name),
                        java.util.Collections.emptyList())) {
                    int match = filter.match(intent.getAction(), type,
                            intent.getScheme(), intent.getData(), intent.getCategories(), "BifrostPackageResolver");
                    if (match < 0) continue;
                    android.content.pm.ResolveInfo result = new android.content.pm.ResolveInfo();
                    result.serviceInfo = serviceInfo(new ComponentName(pkg.packageName, info.name), flags, userId);
                    result.priority = filter.getPriority();
                    result.match = match;
                    result.isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
                    if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) result.filter = new IntentFilter(filter);
                    if (best == null || result.priority > best.priority
                            || (result.priority == best.priority && result.match > best.match)) best = result;
                }
                if (best != null) results.add(best);
            }
        }
        results.sort((a, b) -> {
            int order = Integer.compare(b.priority, a.priority);
            if (order == 0) order = Boolean.compare(b.isDefault, a.isDefault);
            if (order == 0) order = Integer.compare(b.match, a.match);
            return order;
        });
        return results;
    }

    ServiceInfo serviceInfo(ComponentName component, long flags, int userId) {
        if (component == null) return null;
        PackageInfo pkg = findPackage(component.getPackageName(), flags, userId);
        if (pkg == null || pkg.services == null)
            return null;
        for (ServiceInfo service : pkg.services) {
            if (!service.name.equals(component.getClassName())) continue;
            if (!componentMatches(service, flags, userId)) return null;
            ServiceInfo result = copy(service, ServiceInfo.CREATOR);
            installedComponentInfo(result, flags);
            return result;
        }
        return null;
    }

    // The installed app's own declaration first: it needs no system parse.
    private ProviderInfo declaredProvider(String authority) {
        if (authority == null) return null;
        List<PackageInfo> all = new ArrayList<>();
        if (installed != null) all.add(installed);
        if (findDeclared(all, authority) == null) {
            all.clear(); all.addAll(allSystem()); all.addAll(peers.values());
        }
        return findDeclared(all, authority);
    }

    private static ProviderInfo findDeclared(List<PackageInfo> packages, String authority) {
        for (PackageInfo pkg : packages) {
            if (pkg.providers == null) continue;
            for (ProviderInfo provider : pkg.providers)
                if (provider.authority != null
                        && Arrays.asList(provider.authority.split(";")).contains(authority)) return provider;
        }
        return null;
    }

    // ComputerEngine.resolveContentProvider over every known package:
    // authorities are unique, and a disabled declaration resolves to null.
    ProviderInfo resolveProvider(String authority, long flags, int userId) {
        ProviderInfo provider = declaredProvider(authority);
        if (provider == null || !componentMatches(provider, flags, userId)) return null;
        ProviderInfo result = copy(provider, ProviderInfo.CREATOR);
        installedComponentInfo(result, flags);
        return result;
    }

    // ContentProviderHelper.checkContentProviderPermission through
    // ActivityManagerService.checkComponentPermission: the owner's app id,
    // or an exported provider whose read, write or a path permission is
    // absent or held by `uid`. No URI permission grants exist in this profile.
    void enforceProviderPermission(ProviderInfo info, int uid) {
        if (uid % 100000 == info.applicationInfo.uid % 100000) return;
        if (info.exported) {
            List<String> permissions = new ArrayList<>(Arrays.asList(info.readPermission, info.writePermission));
            if (permissions.contains(null)) return;
            if (info.pathPermissions != null) for (android.content.pm.PathPermission path : info.pathPermissions) {
                if (path.getReadPermission() != null) permissions.add(path.getReadPermission());
                if (path.getWritePermission() != null) permissions.add(path.getWritePermission());
            }
            for (String permission : permissions)
                if (checkPermission(permission, uid, Context.DEVICE_ID_DEFAULT) == PackageManager.PERMISSION_GRANTED)
                    return;
        }
        throw new SecurityException("Permission Denial: opening provider " + info.name + " (uid=" + uid + ")"
                + (!info.exported ? " that is not exported from UID " + info.applicationInfo.uid
                        : " requires " + info.readPermission + " or " + info.writePermission));
    }

    // Spoofed microG identities through the real PackageManager client.
    void checkMicroG(Context context) throws Exception {
        PackageManager manager = context.getPackageManager();
        int checked = 0;
        for (String name : new String[] {"com.google.android.gms", "com.android.vending"}) {
            if (system(name) == null) continue;
            Signature fake = new Signature(manager.getApplicationInfo(name, PackageManager.GET_META_DATA)
                    .metaData.getString("fake-signature"));
            PackageInfo legacy = manager.getPackageInfo(name, PackageManager.GET_SIGNATURES);
            PackageInfo current = manager.getPackageInfo(name, PackageManager.GET_SIGNING_CERTIFICATES);
            if (legacy.signatures == null || legacy.signatures.length != 1 || !fake.equals(legacy.signatures[0])
                    || current.signingInfo == null || current.signingInfo.hasMultipleSigners()
                    || !fake.equals(current.signingInfo.getApkContentsSigners()[0])
                    || legacy.signingInfo != null || current.signatures != null)
                throw new AssertionError("microG signature spoofing failed: " + name);
            if (manager.getPackageInfo(name, 0).signatures != null)
                throw new AssertionError("signatures returned without query flags: " + name);
            int uid = legacy.applicationInfo.uid;
            String[] owners = manager.getPackagesForUid(uid);
            if (owners == null || owners.length != 1 || !name.equals(owners[0]) || !name.equals(manager.getNameForUid(uid))
                    || (legacy.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0)
                throw new AssertionError("microG package identity failed: " + name);
            checked++;
        }
        if (checked != 2) throw new AssertionError("microG packages missing: " + checked);
        System.out.println("ACTIVITY_MICROG_PACKAGES_OK");
    }

    void checkPackageServices(Context context) throws Exception {
        PackageManager manager = context.getPackageManager();
        if (context.checkCallingOrSelfPermission("bifrost.missing.permission") != PackageManager.PERMISSION_DENIED
                || context.checkPermission("android.permission.INTERNET", Process.myPid(), Process.myUid() + 1)
                        != PackageManager.PERMISSION_DENIED)
            throw new AssertionError("permission check granted an unknown permission or foreign UID");
        for (String permission : grantedPermissions)
            if (context.checkCallingOrSelfPermission(permission) != PackageManager.PERMISSION_GRANTED)
                throw new AssertionError("normal install permission missing: " + permission);
        PackageInfo signing = manager.getPackageInfo(installed.packageName,
                PackageManager.GET_SIGNATURES | PackageManager.GET_SIGNING_CERTIFICATES);
        if (signing.signatures == null || signing.signatures.length == 0 || signing.signingInfo == null
                || !Arrays.equals(signing.signatures, installed.signatures)
                || !Arrays.equals(signing.signingInfo.getApkContentsSigners(),
                        installed.signingInfo.getApkContentsSigners()))
            throw new AssertionError("installed package signing certificates missing");
        PackageInfo plain = manager.getPackageInfo(installed.packageName, 0);
        if (plain.signatures != null || plain.signingInfo != null)
            throw new AssertionError("signing certificates returned without query flags");
        if (manager.getPackageInfo(installed.packageName, PackageManager.GET_SIGNATURES).signingInfo != null
                || manager.getPackageInfo(installed.packageName,
                        PackageManager.GET_SIGNING_CERTIFICATES).signatures != null)
            throw new AssertionError("legacy and current signing query flags conflated");
        signing.signatures[0] = null;
        if (manager.getPackageInfo(installed.packageName, PackageManager.GET_SIGNATURES).signatures[0] == null)
            throw new AssertionError("signing certificate array aliases package state");
        int flags = PackageManager.MATCH_DISABLED_COMPONENTS | PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        if (installed.activities != null) for (ActivityInfo parsed : installed.activities) {
            ComponentName component = new ComponentName(installed.packageName, parsed.name);
            ActivityInfo result = manager.getActivityInfo(component, flags | PackageManager.GET_META_DATA);
            if (!parsed.name.equals(result.name) || result.applicationInfo.uid != installed.applicationInfo.uid
                    || !java.util.Objects.equals(parsed.targetActivity, result.targetActivity)
                    || result.screenOrientation != parsed.screenOrientation
                    || (parsed.metaData != null && (result.metaData == null
                    || !parsed.metaData.keySet().equals(result.metaData.keySet()))))
                throw new AssertionError("activity differs from installed manifest: " + component);
            result = manager.getActivityInfo(component, flags);
            if (result.metaData != null || result.applicationInfo.metaData != null)
                throw new AssertionError("activity metadata returned without GET_META_DATA");
        }
        try {
            manager.getActivityInfo(new ComponentName(installed.packageName, "bifrost.missing.Activity"), flags);
            throw new AssertionError("unknown activity found");
        } catch (PackageManager.NameNotFoundException expected) {
            // The real framework translates a null activity reply.
        }
        int services = 0, metadata = 0;
        if (installed.services != null) for (ServiceInfo parsed : installed.services) {
            ComponentName component = new ComponentName(installed.packageName, parsed.name);
            ServiceInfo result = manager.getServiceInfo(component, flags | PackageManager.GET_META_DATA);
            if (!parsed.name.equals(result.name) || !installed.packageName.equals(result.packageName)
                    || !installed.applicationInfo.sourceDir.equals(result.applicationInfo.sourceDir)
                    || result.applicationInfo.uid != installed.applicationInfo.uid)
                throw new AssertionError("service identity differs from installed manifest: " + component);
            if (parsed.enabled && installed.applicationInfo.enabled) {
                // SDKs such as Firebase use GET_META_DATA without boot flags.
                ServiceInfo implicit = manager.getServiceInfo(component, PackageManager.GET_META_DATA);
                if (!parsed.name.equals(implicit.name)
                        || (parsed.metaData != null && (implicit.metaData == null
                        || !parsed.metaData.keySet().equals(implicit.metaData.keySet()))))
                    throw new AssertionError("implicit service query lost metadata: " + component);
            }
            if (parsed.metaData != null) {
                if (result.metaData == null || !parsed.metaData.keySet().equals(result.metaData.keySet()))
                    throw new AssertionError("missing service metadata: " + component);
                for (String key : parsed.metaData.keySet())
                    if (!java.util.Objects.equals(parsed.metaData.get(key), result.metaData.get(key)))
                        throw new AssertionError("changed service metadata: " + component + " " + key);
                metadata += parsed.metaData.size();
                result.metaData.clear();
                if (manager.getServiceInfo(component, flags | PackageManager.GET_META_DATA)
                        .metaData.size() != parsed.metaData.size())
                    throw new AssertionError("service metadata aliases package state");
            }
            result = manager.getServiceInfo(component, flags);
            if (result.metaData != null || result.applicationInfo.metaData != null)
                throw new AssertionError("service metadata returned without GET_META_DATA");
            services++;
        }
        for (ComponentName missing : new ComponentName[] {
                new ComponentName(installed.packageName, "bifrost.missing.Service"),
                new ComponentName("bifrost.missing.package", "bifrost.missing.Service")}) {
            try {
                manager.getServiceInfo(missing, flags | PackageManager.GET_META_DATA);
                throw new AssertionError("unknown service found: " + missing);
            } catch (PackageManager.NameNotFoundException expected) {
                // The real framework translates a null service reply.
            }
        }
        int providers = 0;
        if (installed.providers != null) for (ProviderInfo parsed : installed.providers) {
            if (parsed.authority == null) continue;
            for (String authority : parsed.authority.split(";")) {
                ProviderInfo result = manager.resolveContentProvider(authority,
                        flags | PackageManager.GET_META_DATA);
                if (result == null || !parsed.name.equals(result.name)
                        || result.applicationInfo.uid != installed.applicationInfo.uid
                        || !installed.applicationInfo.sourceDir.equals(result.applicationInfo.sourceDir))
                    throw new AssertionError("provider differs from installed manifest: " + authority);
                if (parsed.metaData != null && (result.metaData == null
                        || !parsed.metaData.keySet().equals(result.metaData.keySet())))
                    throw new AssertionError("provider metadata missing: " + authority);
                if (result.metaData != null) {
                    result.metaData.clear();
                    if (manager.resolveContentProvider(authority, flags | PackageManager.GET_META_DATA)
                            .metaData.size() != parsed.metaData.size())
                        throw new AssertionError("provider metadata aliases package state");
                }
                if (parsed.enabled && installed.applicationInfo.enabled) {
                    result = manager.resolveContentProvider(authority, 0);
                    if (result == null || result.metaData != null || result.applicationInfo.metaData != null)
                        throw new AssertionError("implicit provider query lost identity or leaked metadata");
                }
                int otherBoot = parsed.directBootAware ? PackageManager.MATCH_DIRECT_BOOT_UNAWARE
                        : PackageManager.MATCH_DIRECT_BOOT_AWARE;
                if (manager.resolveContentProvider(authority,
                        otherBoot | PackageManager.MATCH_DISABLED_COMPONENTS) != null)
                    throw new AssertionError("provider ignored explicit direct-boot filter");
                providers++;
            }
        }
        if (manager.resolveContentProvider("bifrost.missing.authority", 0) != null)
            throw new AssertionError("unknown content provider found");
        InstallSourceInfo source = manager.getInstallSourceInfo(installed.packageName);
        if (manager.getInstallerPackageName(installed.packageName) != null
                || source.getInstallingPackageName() != null || source.getInitiatingPackageName() != null
                || source.getOriginatingPackageName() != null || source.getUpdateOwnerPackageName() != null
                || source.getPackageSource() != android.content.pm.PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED)
            throw new AssertionError("host-installed APK reported an Android installer");
        try {
            manager.getInstallSourceInfo("bifrost.missing.package");
            throw new AssertionError("unknown install source found");
        } catch (PackageManager.NameNotFoundException expected) {
            // The framework translates the absent package's null reply.
        }
        System.out.println("APP_PACKAGE_SERVICES_OK services=" + services + " metadata=" + metadata
                + " providers=" + providers);
    }

}
