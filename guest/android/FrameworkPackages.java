// Manifest-backed package service for the single installed-app profile.
import android.content.ComponentName;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.os.Process;
import android.os.Parcel;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.Map;

final class FrameworkPackages {
    private volatile PackageInfo installed;
    private final java.util.Set<String> grantedPermissions = ConcurrentHashMap.newKeySet();

    void install(Context system, PackageInfo pkg) {
        installPermissions(system, pkg);
        installed = copy(pkg, PackageInfo.CREATOR);
        componentStates.clear();
    }

    void register(FrameworkServices services) throws Exception {
        services.register("package", "android.content.pm.IPackageManager", (method, a) -> {
            switch (method) {
                case "hasSystemFeature": return false;
                case "isInstantApp": return false;  // This profile installs full APKs only.
                case "notifyPackageUse": return null;  // one-way usage report
                // (name, flags, user): null means "not installed", which the
                // client turns into NameNotFoundException.
                case "getPackageInfo":
                    return packageInfo((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getApplicationInfo":
                    return applicationInfo((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
                case "getInstalledPackages": {
                    PackageInfo info = packageInfo(installed == null ? null : installed.packageName,
                            ((Number) a[0]).longValue(), (Integer) a[1]);
                    return listSlice(info);
                }
                case "getInstalledApplications": {
                    ApplicationInfo info = applicationInfo(installed == null ? null : installed.packageName,
                            ((Number) a[0]).longValue(), (Integer) a[1]);
                    return listSlice(info);
                }
                case "getPackageUid":
                    return packageMatches((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2])
                            ? installed.applicationInfo.uid : -1;
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
                case "getServiceInfo":
                    return serviceInfo((ComponentName) a[0], ((Number) a[1]).longValue(),
                            (Integer) a[2]);
                case "resolveContentProvider":
                    return resolveProvider((String) a[0], ((Number) a[1]).longValue(), (Integer) a[2]);
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
                    return componentStates.getOrDefault(ownComponent(a[0], (Integer) a[1], false),
                            PackageManager.COMPONENT_ENABLED_STATE_DEFAULT);
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
        return installed != null && installed.packageName.equals(name)
                && userId == installed.applicationInfo.uid / 100000
                && ((flags & PackageManager.MATCH_SYSTEM_ONLY) == 0
                    || (installed.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0);
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
        ApplicationInfo result = copy(installed.applicationInfo, ApplicationInfo.CREATOR);
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
        PackageInfo pkg = installed;
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
        result.applicationInfo = applicationInfo(installed.packageName, flags,
                installed.applicationInfo.uid / 100000);
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
        PackageInfo result = copy(installed, PackageInfo.CREATOR);
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
        if (component == null || installed == null || installed.providers == null
                || !packageMatches(component.getPackageName(), flags, userId)) return null;
        for (ProviderInfo provider : installed.providers) {
            if (!provider.name.equals(component.getClassName())) continue;
            if (!componentMatches(provider, flags, userId)) return null;
            ProviderInfo result = copy(provider, ProviderInfo.CREATOR);
            installedComponentInfo(result, flags);
            return result;
        }
        return null;
    }

    private void installPermissions(Context system, PackageInfo pkg) {
        // The single-app profile grants manifest-requested normal permissions
        // at install time. Runtime and signature grants require services this
        // profile does not provide; unknown permission names stay denied.
        PackageInfo platform = system.getPackageManager().getPackageArchiveInfo(
                "/system/framework/framework-res.apk", PackageManager.GET_PERMISSIONS);
        if (platform == null || platform.permissions == null)
            throw new IllegalStateException("platform permission definitions unavailable");
        Map<String, PermissionInfo> definitions = new java.util.HashMap<>();
        for (PermissionInfo permission : platform.permissions) definitions.put(permission.name, permission);
        if (pkg.permissions != null)
            for (PermissionInfo permission : pkg.permissions) definitions.putIfAbsent(permission.name, permission);
        grantedPermissions.clear();
        if (pkg.requestedPermissions == null) return;
        for (int i = 0; i < pkg.requestedPermissions.length; i++) {
            String name = pkg.requestedPermissions[i];
            PermissionInfo definition = definitions.get(name);
            if (definition != null && (definition.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE)
                    == PermissionInfo.PROTECTION_NORMAL) {
                grantedPermissions.add(name);
                pkg.requestedPermissionsFlags[i] |= PackageInfo.REQUESTED_PERMISSION_GRANTED;
            }
        }
    }

    int checkPermission(String permission, int uid, int deviceId) {
        return permission != null && installed != null && uid == installed.applicationInfo.uid
                && deviceId == Context.DEVICE_ID_DEFAULT
                && grantedPermissions.contains(permission)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    private ServiceInfo serviceInfo(ComponentName component, long flags, int userId) {
        PackageInfo pkg = installed;
        if (pkg == null || !pkg.packageName.equals(component.getPackageName()) || pkg.services == null)
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
