// Legacy Android 15 MediaRouter clients, with no remote route providers.
// Local speaker routing is supplied by MediaRouter and FrameworkAudio.
import android.os.IBinder;
import android.os.IInterface;
import android.os.Process;
import android.os.RemoteException;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

final class FrameworkMediaRouter implements FrameworkServices.Call {
    private enum Contract {
        registerClientAsUser, unregisterClient, getState,
        isPlaybackActive, setBluetoothA2dpOn, setDiscoveryRequest, setSelectedRoute,
        requestSetVolume, requestUpdateVolume
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final FrameworkPackages packages;
    private final Map<IBinder, Client> clients = new HashMap<>();

    private final class Client implements IBinder.DeathRecipient {
        final IBinder token;
        final boolean trusted;
        String selectedRoute;
        int routeTypes;
        boolean activeScan;
        Client(IBinder token) {
            this.token = token;
            trusted = packages.granted("android.permission.CONFIGURE_WIFI_DISPLAY");
        }
        @Override public void binderDied() {
            synchronized (FrameworkMediaRouter.this) { clients.remove(token); }
        }
    }

    FrameworkMediaRouter(FrameworkPackages packages) { this.packages = packages; }

    void install(FrameworkServices services) throws Exception {
        services.register("media_router", "android.media.IMediaRouterService", this);
    }

    @Override public synchronized Object invoke(String method, Object[] a) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("media_router." + method); }
        if (contract == Contract.registerClientAsUser) {
            if (a[1] == null || packages.packageUid((String) a[1]) != Process.myUid())
                throw new SecurityException("packageName must match the calling uid");
            int user = (Integer) a[2];
            // USER_CURRENT / USER_CURRENT_OR_SELF resolve to this process's user.
            if (user != Process.myUid() / 100000 && user != -2 && user != -3)
                throw new SecurityException("Media router access to another user");
        }
        if (contract == Contract.requestSetVolume || contract == Contract.requestUpdateVolume)
            Objects.requireNonNull(a[1], "routeId must not be null");
        if (contract == Contract.setBluetoothA2dpOn && a[0] == null)
            throw new IllegalArgumentException("client must not be null");
        IBinder token = ((IInterface) Objects.requireNonNull(a[0], "client")).asBinder();
        Client client = clients.get(token);
        switch (contract) {
            case registerClientAsUser:
                if (client == null) {
                    client = new Client(token);
                    try { token.linkToDeath(client, 0); }
                    catch (RemoteException dead) { throw new IllegalStateException("Media router client died prematurely", dead); }
                    clients.put(token, client);
                }
                return null;
            case unregisterClient:
                if (client != null) { clients.remove(token); token.unlinkToDeath(client, 0); }
                return null;
            case getState:
                // AOSP hides remote routes from untrusted clients. Trusted
                // clients see a fresh empty provider-state snapshot.
                return client != null && client.trusted
                        ? Class.forName("android.media.MediaRouterClientState").getConstructor().newInstance() : null;
            case isPlaybackActive: return client != null && ManagedLauncher.hostAudioActive();
            case setDiscoveryRequest:
                if (client != null) {
                    client.routeTypes = (Integer) a[1] & (client.trusted ? -1 : ~4);
                    client.activeScan = (Boolean) a[2];
                }
                return null;
            case setSelectedRoute:
                if (client != null) client.selectedRoute = (String) a[1];
                return null;
            case setBluetoothA2dpOn:
            case requestSetVolume:
            case requestUpdateVolume:
                // No Bluetooth device or remote provider owns the requested
                // route. AOSP similarly has no route to configure in this case.
                return null;
            default: throw new UnsupportedOperationException("media_router." + method);
        }
    }
}
