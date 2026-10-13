// Single virtual default network backed by the host sockets and resolver.
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkRequest;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Message;
import android.os.Messenger;
import java.lang.reflect.Method;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.Map;

final class FrameworkConnectivity {
    // The host launcher snapshots its default route and runs Android's HTTPS
    // validation probe. The network uses host sockets/DNS and is presented as
    // Wi-Fi: phone apps expect Wi-Fi or cellular, and some treat others as offline.
    private final boolean hostNetwork = "1".equals(System.getenv("BIFROST_ANDROID_HOST_NETWORK"));
    private final boolean validated = hostNetwork
            && "1".equals(System.getenv("BIFROST_ANDROID_NETWORK_VALIDATED"));
    private final String hostInterface = System.getenv("BIFROST_ANDROID_NETWORK_INTERFACE") != null
            ? System.getenv("BIFROST_ANDROID_NETWORK_INTERFACE") : "wlan0";
    private final AtomicInteger networkRequestId = new AtomicInteger(1);
    private final Map<NetworkRequest, Messenger> networkRequests = new ConcurrentHashMap<>();
    private Network defaultNetwork;
    private NetworkCapabilities hostCapabilities;
    private Handler networkHandler;

    boolean online() { return hostNetwork; }

    private void prepareNetwork() throws Exception {
        System.out.println("APP_NETWORK " + (!hostNetwork ? "offline"
                : "host-default-route " + hostInterface + (validated ? " validated" : " unvalidated")));
        if (!hostNetwork) return;
        defaultNetwork = Network.class.getConstructor(int.class).newInstance(100);
        // AOSP WifiInjector's regular Wi-Fi filter on top of the framework
        // defaults (NOT_RESTRICTED, TRUSTED, NOT_VPN, NOT_BANDWIDTH_CONSTRAINED).
        // Wi-Fi is unmetered unless a metered hint or user override says
        // otherwise (WifiConfiguration.isMetered); the host gives neither.
        // ConnectivityService adds FOREGROUND to non-background networks and
        // VALIDATED after NetworkMonitor's probe succeeds.
        hostCapabilities = new NetworkCapabilities();
        Method addTransport = NetworkCapabilities.class.getMethod("addTransportType", int.class);
        Method addCapability = NetworkCapabilities.class.getMethod("addCapability", int.class);
        addTransport.invoke(hostCapabilities, NetworkCapabilities.TRANSPORT_WIFI);
        for (String name : new String[] {"NET_CAPABILITY_INTERNET", "NET_CAPABILITY_NOT_METERED",
                "NET_CAPABILITY_NOT_ROAMING", "NET_CAPABILITY_NOT_CONGESTED", "NET_CAPABILITY_NOT_SUSPENDED",
                "NET_CAPABILITY_NOT_VCN_MANAGED", "NET_CAPABILITY_FOREGROUND"})
            addCapability.invoke(hostCapabilities, NetworkCapabilities.class.getField(name).getInt(null));
        if (validated) addCapability.invoke(hostCapabilities, NetworkCapabilities.NET_CAPABILITY_VALIDATED);
        for (String setter : new String[] {"setLinkUpstreamBandwidthKbps", "setLinkDownstreamBandwidthKbps"})
            NetworkCapabilities.class.getMethod(setter, int.class).invoke(hostCapabilities, 1024 * 1024);
    }

    private NetworkInfo hostNetworkInfo() throws Exception {
        if (!hostNetwork) return null;
        NetworkInfo info = new NetworkInfo(ConnectivityManager.TYPE_WIFI, 0, "WIFI", "");
        info.setDetailedState(NetworkInfo.DetailedState.CONNECTED, null, "host default network");
        NetworkInfo.class.getMethod("setIsAvailable", boolean.class).invoke(info, true);
        return info;
    }

    private LinkProperties hostLink() throws Exception {
        // Guest netlink reaches the host, so name the interface apps can enumerate.
        // Addresses, routes and DNS servers are not reported.
        LinkProperties link = new LinkProperties();
        LinkProperties.class.getMethod("setInterfaceName", String.class).invoke(link, hostInterface);
        return link;
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private NetworkRequest networkRequest(Object[] arguments, boolean listen) throws Exception {
        synchronized (networkRequests) {
            if (networkHandler == null) {
                HandlerThread thread = new HandlerThread("BifrostConnectivity");
                thread.start();
                networkHandler = new Handler(thread.getLooper());
            }
        }
        NetworkCapabilities need = (NetworkCapabilities) arguments[listen ? 0 : 1];
        Messenger messenger = (Messenger) arguments[listen ? 1 : 3];
        Class<? extends Enum> type = (Class<? extends Enum>) Class.forName("android.net.NetworkRequest$Type");
        Object requestType = listen ? Enum.valueOf(type, "LISTEN") : type.getEnumConstants()[(Integer) arguments[2]];
        NetworkCapabilities requested = need == null ? new NetworkCapabilities() : new NetworkCapabilities(need);
        NetworkRequest request = NetworkRequest.class.getConstructor(NetworkCapabilities.class,
                int.class, int.class, type).newInstance(requested, listen ? -1 : (Integer) arguments[6],
                networkRequestId.getAndIncrement(), requestType);
        networkRequests.put(request, messenger);
        boolean matches = hostNetwork && request.canBeSatisfiedBy(hostCapabilities);
        if (matches) {
            // AOSP's AVAILABLE message carries all three values; the real
            // ConnectivityManager dispatches available/capabilities/link callbacks.
            networkMessage(request, messenger, "CALLBACK_AVAILABLE");
        } else if (!listen && (Integer) arguments[4] > 0) {
            networkHandler.postDelayed(() -> {
                if (networkRequests.remove(request, messenger)) {
                    try { networkMessage(request, messenger, "CALLBACK_UNAVAIL"); }
                    catch (Exception error) { throw new IllegalStateException(error); }
                }
            }, (Integer) arguments[4]);
        }
        return request;
    }

    private void networkMessage(NetworkRequest request, Messenger messenger, String event) throws Exception {
        if (!event.equals("CALLBACK_UNAVAIL") && !networkRequests.containsKey(request)) return;
        Message message = Message.obtain();
        message.what = ConnectivityManager.class.getField(event).getInt(null);
        Bundle data = new Bundle();
        data.putParcelable("NetworkRequest", request);
        if (defaultNetwork != null) {
            data.putParcelable("Network", defaultNetwork);
            data.putParcelable("NetworkCapabilities", new NetworkCapabilities(hostCapabilities));
            data.putParcelable("LinkProperties", hostLink());
        }
        message.setData(data);
        messenger.send(message);
    }

    void install(FrameworkServices services) throws Exception {
        prepareNetwork();
        // One virtual default network backed by the host's sockets/resolver.
        // Unsupported operations remain explicit rather than returning success.
        services.register("connectivity", "android.net.IConnectivityManager", (method, a) -> {
            switch (method) {
                case "getActiveNetwork": return defaultNetwork;
                case "getActiveNetworkInfo":
                case "getActiveNetworkInfoForUid": return hostNetworkInfo();
                case "getNetworkInfoForUid":
                    // AOSP filters the requested Network, not the active one.
                    // This profile has no VPN or per-UID blocking policy.
                    return defaultNetwork != null && defaultNetwork.equals(a[0]) ? hostNetworkInfo() : null;
                case "getNetworkInfo": return (Integer) a[0] == ConnectivityManager.TYPE_WIFI
                        ? hostNetworkInfo() : null;
                case "getAllNetworkInfo": return defaultNetwork == null ? new NetworkInfo[0]
                        : new NetworkInfo[] {hostNetworkInfo()};
                case "getNetworkForType": return (Integer) a[0] == ConnectivityManager.TYPE_WIFI
                        ? defaultNetwork : null;
                case "getNetworkCapabilities": return defaultNetwork != null && defaultNetwork.equals(a[0])
                        ? new NetworkCapabilities(hostCapabilities) : null;
                case "getLinkProperties": return defaultNetwork != null && defaultNetwork.equals(a[0])
                        ? hostLink() : null;
                case "getAllNetworks": return defaultNetwork == null ? new Network[0] : new Network[] {defaultNetwork};
                case "isActiveNetworkMetered":
                    // ConnectivityService: no active network is conservatively metered.
                    return hostCapabilities == null
                            || !hostCapabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
                case "getRestrictBackgroundStatusByCaller":
                    // The host bridge applies no Android Data Saver or UID policy.
                    // Metering and route availability do not change this policy.
                    return ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED;
                case "requestNetwork": return networkRequest(a, false);
                case "listenForNetwork": return networkRequest(a, true);
                case "releaseNetworkRequest": networkRequests.remove((NetworkRequest) a[0]); return null;
                default: throw new UnsupportedOperationException("connectivity." + method);
            }
        });
    }

    void check(Context context) throws Exception {
        ConnectivityManager connectivity = (ConnectivityManager)
                context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (connectivity == null) throw new AssertionError("connectivity service unavailable");
        if (connectivity.getRestrictBackgroundStatus()
                != ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED)
            throw new AssertionError("host bridge unexpectedly restricts background networking");
        Network active = connectivity.getActiveNetwork();
        NetworkInfo info = connectivity.getActiveNetworkInfo();
        if (hostNetwork != (active != null) || hostNetwork != (info != null && info.isConnected()))
            throw new AssertionError("active network differs from host route snapshot");
        if (active != null) {
            NetworkInfo byNetwork = connectivity.getNetworkInfo(active);
            if (byNetwork == null || !byNetwork.isConnected()
                    || byNetwork.getType() != ConnectivityManager.TYPE_WIFI)
                throw new AssertionError("network-specific info differs from active network");
            byNetwork.setDetailedState(NetworkInfo.DetailedState.DISCONNECTED, null, null);
            if (!connectivity.getNetworkInfo(active).isConnected())
                throw new AssertionError("network info reply aliases service state");
            NetworkCapabilities caps = connectivity.getNetworkCapabilities(active);
            if (caps == null || !caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                    || caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
                    || caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)
                    || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                    || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
                    || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_FOREGROUND)
                    || caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED) != validated)
                throw new AssertionError("host network capabilities incorrect: " + caps);
            LinkProperties link = connectivity.getLinkProperties(active);
            if (link == null || !hostInterface.equals(link.getInterfaceName()))
                throw new AssertionError("host network link properties incorrect: " + link);
        }
        if (connectivity.isActiveNetworkMetered() != !hostNetwork)
            throw new AssertionError("active network metering differs from Wi-Fi defaults");
        Network unknown = Network.class.getConstructor(int.class).newInstance(101);
        if (connectivity.getNetworkInfo(unknown) != null
                || connectivity.getNetworkInfo(ConnectivityManager.TYPE_MOBILE) != null
                || connectivity.getNetworkInfo(ConnectivityManager.TYPE_ETHERNET) != null
                || (connectivity.getNetworkInfo(ConnectivityManager.TYPE_WIFI) != null) != hostNetwork
                || connectivity.getAllNetworkInfo().length != (hostNetwork ? 1 : 0))
            throw new AssertionError("legacy network lookup returned an unknown transport/network");
        CountDownLatch available = new CountDownLatch(1);
        CountDownLatch unavailable = new CountDownLatch(1);
        AtomicReference<Network> observed = new AtomicReference<>();
        ConnectivityManager.NetworkCallback callback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) { observed.set(network); }
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) available.countDown();
            }
            @Override public void onUnavailable() { unavailable.countDown(); }
        };
        connectivity.registerDefaultNetworkCallback(callback);
        if (hostNetwork && (!available.await(5, TimeUnit.SECONDS) || !active.equals(observed.get())))
            throw new AssertionError("default network callback missing");
        CountDownLatch listening = new CountDownLatch(1);
        ConnectivityManager.NetworkCallback listener = new ConnectivityManager.NetworkCallback() {
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) listening.countDown();
            }
        };
        NetworkRequest wifi = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build();
        if (hostNetwork && !wifi.canBeSatisfiedBy(hostCapabilities))
            throw new AssertionError("host capabilities do not satisfy Wi-Fi: " + wifi + " vs " + hostCapabilities);
        connectivity.registerNetworkCallback(wifi, listener);
        if (hostNetwork && !listening.await(5, TimeUnit.SECONDS))
            throw new AssertionError("matching network listener missing");
        if (!hostNetwork && listening.await(100, TimeUnit.MILLISECONDS))
            throw new AssertionError("offline network listener was satisfied");
        if (networkRequests.size() != 2) throw new AssertionError("network request IDs collided");
        connectivity.unregisterNetworkCallback(callback);
        if (networkRequests.size() != 1) throw new AssertionError("release removed another network listener");
        connectivity.unregisterNetworkCallback(listener);
        if (!networkRequests.isEmpty()) throw new AssertionError("released network request leaked");
        // This profile has no cellular transport, so do not satisfy that request
        // with the host network. Its explicit timeout must invoke unavailable.
        connectivity.requestNetwork(new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR).build(), callback, 100);
        if (!unavailable.await(5, TimeUnit.SECONDS) || !networkRequests.isEmpty())
            throw new AssertionError("unsatisfied network request did not time out");
        System.out.println("ACTIVITY_CONNECTIVITY_OK " + (!hostNetwork ? "offline"
                : validated ? "host-validated" : "host-unvalidated"));
    }
}
