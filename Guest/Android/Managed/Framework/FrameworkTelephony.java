// TelephonyRegistry ("telephony.registry") for a device without a radio.
// SystemServer starts it on every device, and TelephonyManager's
// registerTelephonyCallback/listen reach it through TelephonyRegistryManager
// (ad SDKs register network detectors this way). The device has no modem:
// getActiveModemCount() is 0, so TelephonyRegistry.validatePhoneId never
// holds and listeners are recorded without any notification. Registration
// keeps TelephonyRegistry.listen's package check, checkListenerPermission
// and per-process limit (Android 15). Location is on, there is one user and
// app ops follow the manifest grants, so LocationAccessPolicy reduces to the
// granted permissions; no subscription carries carrier privileges.
import android.os.Build;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Process;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

final class FrameworkTelephony {
    private enum Registry { listenWithEventList }

    // TelephonyCallback.DEFAULT_PER_PID_REGISTRATION_LIMIT; its compat change
    // PHONE_STATE_LISTENER_LIMIT_CHANGE_ID is enabled for every app.
    private static final int REGISTRATION_LIMIT = 50;

    // TelephonyCallback.EVENT_* (@SystemApi), from the pinned framework.
    private static final int CELL_LOCATION = event("CELL_LOCATION_CHANGED"),
            CELL_INFO = event("CELL_INFO_CHANGED"),
            LEGACY_CALL_STATE = event("LEGACY_CALL_STATE_CHANGED"),
            CALL_STATE = event("CALL_STATE_CHANGED"),
            ACTIVE_DATA_SUBSCRIPTION = event("ACTIVE_DATA_SUBSCRIPTION_ID_CHANGED"),
            DISPLAY_INFO = event("DISPLAY_INFO_CHANGED");
    private static final Set<Integer> PHONE_STATE = events("CALL_FORWARDING_INDICATOR_CHANGED",
            "MESSAGE_WAITING_INDICATOR_CHANGED", "EMERGENCY_NUMBER_LIST_CHANGED");
    private static final Set<Integer> PRECISE_PHONE_STATE = events("PRECISE_DATA_CONNECTION_STATE_CHANGED",
            "DATA_CONNECTION_REAL_TIME_INFO_CHANGED", "PRECISE_CALL_STATE_CHANGED",
            "CALL_DISCONNECT_CAUSE_CHANGED", "CALL_ATTRIBUTES_CHANGED", "IMS_CALL_DISCONNECT_CAUSE_CHANGED",
            "REGISTRATION_FAILURE", "BARRING_INFO_CHANGED", "PHYSICAL_CHANNEL_CONFIG_CHANGED",
            "DATA_ENABLED_CHANGED", "LINK_CAPACITY_ESTIMATE_CHANGED", "MEDIA_QUALITY_STATUS_CHANGED");
    private static final Set<Integer> EMERGENCY_SESSION = events("OUTGOING_EMERGENCY_CALL",
            "OUTGOING_EMERGENCY_SMS");
    private static final Set<Integer> PRIVILEGED_PHONE_STATE = events("SRVCC_STATE_CHANGED",
            "VOICE_ACTIVATION_STATE_CHANGED", "RADIO_POWER_STATE_CHANGED", "ALLOWED_NETWORK_TYPE_LIST_CHANGED",
            "EMERGENCY_CALLBACK_MODE_CHANGED", "SIMULTANEOUS_CELLULAR_CALLING_SUBSCRIPTIONS_CHANGED");

    private static int event(String name) {
        try {
            return Class.forName("android.telephony.TelephonyCallback").getField("EVENT_" + name).getInt(null);
        } catch (ReflectiveOperationException error) {
            throw new ExceptionInInitializerError(error);
        }
    }

    private static Set<Integer> events(String... names) {
        Set<Integer> result = new HashSet<>();
        for (String name : names) result.add(event(name));
        return result;
    }

    private final FrameworkPackages packages;
    private final Map<IBinder, Set<Integer>> records = new HashMap<>();  // callback -> events

    FrameworkTelephony(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("telephony.registry", "com.android.internal.telephony.ITelephonyRegistry",
                new FrameworkServices.Call() {
            @Override public String[] methods() { return new String[] {Registry.listenWithEventList.name()}; }
            @Override public Object invoke(String method, Object[] a) throws Exception {
                // listenWithEventList(renounceFine, renounceCoarse, subId, callingPackage,
                // callingFeatureId, callback, events, notifyNow)
                listen((String) a[3], (IInterface) a[5], (int[]) a[6]);
                return null;
            }
        });
    }

    // TelephonyRegistry.listen without phones: unregister, check, record.
    private void listen(String callingPackage, IInterface callback, int[] list) {
        if (packages.packageUid(callingPackage) != Process.myUid())  // AppOpsManager.checkPackage
            throw new SecurityException("Specified package " + callingPackage + " under uid "
                    + Process.myUid() + " but it is not");
        Set<Integer> events = new HashSet<>();
        for (int event : list) events.add(event);
        IBinder binder = callback.asBinder();
        if (events.isEmpty()) {
            synchronized (records) { records.remove(binder); }
            return;
        }
        checkPermissions(events);
        synchronized (records) {
            if (!records.containsKey(binder) && records.size() >= REGISTRATION_LIMIT)
                throw new IllegalStateException("Pid " + Process.myPid() + " has exceeded the number of"
                        + " permissible registered listeners. Ignoring request to add.");
            records.put(binder, events);
        }
    }

    // TelephonyRegistry.checkListenerPermission. Its soft denials (app op
    // denied despite a grant) cannot occur here: app ops follow the grants.
    private void checkPermissions(Set<Integer> events) {
        int target = packages.targetSdk();
        boolean s = target >= Build.VERSION_CODES.S;  // the READ_PHONE_STATE compat changes
        if (events.contains(CELL_LOCATION) || events.contains(CELL_INFO)) {
            // Fine is enforced from Q, coarse always; a missing grant is DENIED_HARD.
            boolean fine = packages.granted("android.permission.ACCESS_FINE_LOCATION");
            if (!fine && (target >= Build.VERSION_CODES.Q
                    || !packages.granted("android.permission.ACCESS_COARSE_LOCATION")))
                throw new SecurityException("Unable to listen for events " + events
                        + " due to insufficient location permissions.");
        }
        boolean phoneState = !java.util.Collections.disjoint(events, PHONE_STATE)
                || (s && (events.contains(LEGACY_CALL_STATE) || events.contains(CALL_STATE)
                        || events.contains(ACTIVE_DATA_SUBSCRIPTION) || events.contains(CELL_INFO)))
                || (!s && events.contains(DISPLAY_INFO));
        // TelephonyPermissions.checkReadPhoneState: without carrier privileges
        // a missing READ_PHONE_STATE throws.
        if (phoneState) require("android.permission.READ_PRIVILEGED_PHONE_STATE", "android.permission.READ_PHONE_STATE");
        if (!java.util.Collections.disjoint(events, PRECISE_PHONE_STATE))
            require("android.permission.READ_PRECISE_PHONE_STATE");
        if (!java.util.Collections.disjoint(events, EMERGENCY_SESSION))
            require("android.permission.READ_ACTIVE_EMERGENCY_SESSION");
        if (!java.util.Collections.disjoint(events, PRIVILEGED_PHONE_STATE))
            require("android.permission.READ_PRIVILEGED_PHONE_STATE");
    }

    // Any one of `names` granted, else Context.enforceCallingOrSelfPermission's failure.
    private void require(String... names) {
        for (String name : names) if (packages.granted(name)) return;
        throw new SecurityException("uid " + Process.myUid() + " does not have " + names[names.length - 1] + ".");
    }
}
