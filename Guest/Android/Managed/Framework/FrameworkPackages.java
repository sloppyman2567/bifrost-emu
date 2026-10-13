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
    private java.util.Set<String> protectedBroadcasts;

    synchronized boolean protectedBroadcast(String action) throws Exception {
        if (protectedBroadcasts == null) {
            java.util.Set<String> names = new java.util.HashSet<>();
            for (Object name : (List<?>) invoke(parseArchive(new java.io.File("/system/framework/framework-res.apk")), "getProtectedBroadcasts"))
                names.add((String) name);
            protectedBroadcasts = names;
        }
        return protectedBroadcasts.contains(action);
    }

    void install(Context system, PackageInfo pkg) {
        installPermissions(system, pkg);
        installed = copy(pkg, PackageInfo.CREATOR);
        componentStates.clear();
        providerFilters.remove(pkg.packageName);
        receiverFilters.remove(pkg.packageName);
    }

    void installSystem(PackageInfo pkg) {
        if ((pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0)
            throw new IllegalArgumentException("Not a system package: " + pkg.packageName);
        systemPackages.put(pkg.packageName, copy(pkg, PackageInfo.CREATOR));
        providerFilters.remove(pkg.packageName);
    }

    PackageInfo systemPackage(String name) {
        PackageInfo pkg = systemPackages.get(name);
        return pkg == null ? null : copy(pkg, PackageInfo.CREATOR);
    }

    // Uid of the installed or a system package, or -1 when it does not exist.
    int packageUid(String name) {
        PackageInfo pkg = installed != null && installed.packageName.equals(name) ? installed : systemPackages.get(name);
        return pkg == null ? -1 : pkg.applicationInfo.uid;
    }

    PackageInfo processPackage(String name) {
        PackageInfo pkg = installed != null && installed.packageName.equals(name) ? installed : systemPackages.get(name);
        return pkg == null ? null : copy(pkg, PackageInfo.CREATOR);
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
        Map<ComponentName, List<IntentFilter>> filters = receiverFilters.get(pkg.packageName);
        if (filters == null) {
            filters = new java.util.HashMap<>();
            java.io.File base = new java.io.File(pkg.applicationInfo.sourceDir);
            java.io.File archive = pkg.applicationInfo.splitSourceDirs != null
                    && pkg.applicationInfo.splitSourceDirs.length != 0 ? base.getParentFile() : base;
            for (Object receiver : (List<?>) invoke(parseArchive(archive), "getReceivers")) {
                List<IntentFilter> entries = new ArrayList<>();
                for (Object entry : (List<?>) invoke(receiver, "getIntents"))
                    entries.add(new IntentFilter((IntentFilter) invoke(entry, "getIntentFilter")));
                filters.put(new ComponentName(pkg.packageName, (String) invoke(receiver, "getName")), entries);
            }
            receiverFilters.put(pkg.packageName, filters);
        }
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
                case "getPackagesForUid":
                    return installed != null && (Integer) a[0] == installed.applicationInfo.uid
                            ? new String[] {installed.packageName} : null;
                case "getNameForUid":
                    return installed != null && (Integer) a[0] == installed.applicationInfo.uid
                            ? installed.packageName : null;
                case "getReceiverInfo":
                    return activityInfo((ComponentName) a[0], ((Number) a[1]).longValue(),
                            (Integer) a[2], installed == null ? null : installed.receivers);
                case "getProviderInfo":
                    return providerInfo((ComponentName) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getActivityInfo":
                    return activityInfo((ComponentName) a[0], ((Number) a[1]).longValue(),
                            (Integer) a[2], installed == null ? null : installed.activities);
                case "resolveService":
                    return resolveService((android.content.Intent) a[0], ((Number) a[2]).longValue(), (Integer) a[3]);
                case "queryIntentServices":
                    return listSlice(resolveService((android.content.Intent) a[0],
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

    private boolean packageMatches(String name, long flags, int userId) {
        return findPackage(name, flags, userId) != null;
    }

    private PackageInfo findPackage(String name, long flags, int userId) {
        if (name == null) return null;
        PackageInfo pkg = installed != null && installed.packageName.equals(name)
                ? installed : systemPackages.get(name);
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

    private static Object listSlice(Object value) throws Exception {
        return Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class)
                .newInstance(value == null ? java.util.Collections.emptyList() : Arrays.asList(value));
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
        if ((flags & PackageManager.GET_SIGNATURES) == 0) result.signatures = null;
        if ((flags & PackageManager.GET_SIGNING_CERTIFICATES) == 0) result.signingInfo = null;
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
        Map<ComponentName, List<IntentFilter>> cached = providerFilters.get(pkg.packageName);
        if (cached != null) return cached;
        Map<ComponentName, List<IntentFilter>> filters = new java.util.HashMap<>();
        // PackageInfo omits intent filters. Reuse the image's manifest parser
        // for their full action/category/type/URI semantics rather than guess
        // from provider authorities. Parse the installed cluster, including splits.
        java.io.File base = new java.io.File(pkg.applicationInfo.sourceDir);
        if (base.isFile() && pkg.providers != null && pkg.providers.length != 0) {
            java.io.File archive = pkg.applicationInfo.splitSourceDirs != null
                    && pkg.applicationInfo.splitSourceDirs.length != 0 ? base.getParentFile() : base;
            for (Object provider : (List<?>) invoke(parseArchive(archive), "getProviders")) {
                List<IntentFilter> intents = new ArrayList<>();
                for (Object intent : (List<?>) invoke(provider, "getIntents"))
                    intents.add(new IntentFilter((IntentFilter) invoke(intent, "getIntentFilter")));
                filters.put(new ComponentName(pkg.packageName, (String) invoke(provider, "getName")), intents);
            }
        }
        Map<ComponentName, List<IntentFilter>> previous = providerFilters.putIfAbsent(pkg.packageName, filters);
        return previous == null ? filters : previous;
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
        List<PackageInfo> packages = new ArrayList<>(systemPackages.values());
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
        // grant. Other runtime/signature grants require unavailable services;
        // unknown permission names stay denied.
        // The runtime image is immutable. Retain its definitions independently
        // of app installation/grants, as PackageManager's permission registry
        // does; app permissions and target-dependent grants are rebuilt below.
        if (platformPermissions == null) {
            PackageInfo platform = system.getPackageManager().getPackageArchiveInfo(
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
                    == PermissionInfo.PROTECTION_NORMAL || legacySettings)) {
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

    int checkPermission(String permission, int uid, int deviceId) {
        return permission != null && installed != null && uid == installed.applicationInfo.uid
                && deviceId == Context.DEVICE_ID_DEFAULT
                && grantedPermissions.contains(permission)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    private android.content.pm.ResolveInfo resolveService(android.content.Intent intent, long flags, int userId) {
        if (intent != null && intent.getComponent() == null && intent.getPackage() != null
                && (installed == null || !intent.getPackage().equals(installed.packageName))) return null;
        if (intent == null || intent.getComponent() == null)
            throw new UnsupportedOperationException("Service intent-filter resolution is not implemented");
        ServiceInfo info = serviceInfo(intent.getComponent(), flags, userId);
        if (info == null) return null;
        android.content.pm.ResolveInfo result = new android.content.pm.ResolveInfo();
        result.serviceInfo = info;
        return result;
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

    // Whether the installed or a system package declares `authority`.
    boolean declaresProvider(String authority) {
        java.util.List<PackageInfo> all = new ArrayList<>(systemPackages.values());
        if (installed != null) all.add(installed);
        for (PackageInfo pkg : all) {
            if (pkg.providers == null) continue;
            for (ProviderInfo provider : pkg.providers)
                if (provider.authority != null
                        && Arrays.asList(provider.authority.split(";")).contains(authority)) return true;
        }
        return false;
    }

    private ProviderInfo resolveProvider(String authority, long flags, int userId) {
        if (installed == null || installed.providers == null || authority == null) return null;
        for (ProviderInfo provider : installed.providers) {
            if (provider.authority == null
                    || !Arrays.asList(provider.authority.split(";")).contains(authority)) continue;
            if (!componentMatches(provider, flags, userId)) return null;
            ProviderInfo result = copy(provider, ProviderInfo.CREATOR);
            installedComponentInfo(result, flags);
            return result;
        }
        return null;
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
