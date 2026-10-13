// LocationManagerService ("location") for a device without GNSS hardware or a
// network/fused location provider (no Google Play services): only the passive
// provider exists, and the location setting has its AOSP default (on,
// def_location_mode = 3). LocationManager is built from
// ServiceManager.getServiceOrThrow("location"). Location data requires
// ACCESS_COARSE/FINE_LOCATION; without a grant AOSP throws SecurityException.
// With a grant there is still no location source, which stays unsupported.
import android.os.Process;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class FrameworkLocation implements FrameworkServices.Call {
    private static final String PASSIVE = "passive";
    private static final int USER_CURRENT_OR_SELF = -3, USER_CURRENT = -2;

    private enum Contract {
        getAllProviders,
        getProviders,
        getBestProvider,
        hasProvider,
        isProviderEnabledForUser,
        isLocationEnabledForUser,
        getGnssHardwareModelName,
        getGnssYearOfHardware,
        getLastLocation,
        getCurrentLocation,
        registerLocationListener,
        registerLocationPendingIntent,
        unregisterLocationListener
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;

    FrameworkLocation(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("location", "android.location.ILocationManager", this);
    }

    private boolean permitted() {
        return packages.granted("android.permission.ACCESS_COARSE_LOCATION")
                || packages.granted("android.permission.ACCESS_FINE_LOCATION");
    }

    // LocationPermissions.enforceLocationPermission.
    private void requireLocation(String method) {
        if (!permitted())
            throw new SecurityException("uid " + Process.myUid() + " does not have android.permission.ACCESS_COARSE_LOCATION"
                    + " or android.permission.ACCESS_FINE_LOCATION.");
        throw new UnsupportedOperationException("location." + method + ": no location source");
    }

    private void user(int userId) {
        if (userId != 0 && userId != USER_CURRENT && userId != USER_CURRENT_OR_SELF
                && !packages.granted("android.permission.INTERACT_ACROSS_USERS"))
            throw new SecurityException("Requires INTERACT_ACROSS_USERS");
    }

    @Override public Object invoke(String method, Object[] a) {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("location." + method); }
        switch (contract) {
            case getAllProviders: return new ArrayList<>(Collections.singletonList(PASSIVE));
            // Filtered queries return nothing without a location permission;
            // getBestProvider skips passive and finds no fused/gps/network.
            case getProviders: return permitted() ? new ArrayList<>(Collections.singletonList(PASSIVE)) : new ArrayList<String>();
            case getBestProvider: return null;
            case hasProvider: return PASSIVE.equals(a[0]);
            case isProviderEnabledForUser: user((Integer) a[1]); return PASSIVE.equals(a[0]);
            case isLocationEnabledForUser: user((Integer) a[0]); return true;
            case getGnssHardwareModelName: return null;
            case getGnssYearOfHardware: return 0;
            case getLastLocation:
            case getCurrentLocation:
            case registerLocationListener:
            case registerLocationPendingIntent: requireLocation(method); return null;
            // Nothing can have been registered.
            case unregisterLocationListener: return null;
            default: throw new UnsupportedOperationException("location." + method);
        }
    }
}
