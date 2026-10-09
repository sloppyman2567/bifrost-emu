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
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.Map;

final class FrameworkConnectivity {
    // The host launcher snapshots its default route. This virtual Ethernet
    // network uses host sockets/DNS; it is not a simulated Wi-Fi or modem.
    private final boolean hostNetwork = "1".equals(System.getenv("BIFROST_ANDROID_HOST_NETWORK"));
    private final AtomicInteger networkRequestId = new AtomicInteger(1);
    private final Map<NetworkRequest, Messenger> networkRequests = new ConcurrentHashMap<>();
    private Network defaultNetwork;
    private NetworkCapabilities hostCapabilities;
    private Handler networkHandler;

    private void prepareNetwork() throws Exception {
        System.out.println("APP_NETWORK " + (hostNetwork ? "host-default-route" : "offline"));
        if (!hostNetwork) return;
        defaultNetwork = Network.class.getConstructor(int.class).newInstance(100);
        hostCapabilities = new NetworkCapabilities();
        NetworkCapabilities.class.getMethod("addTransportType", int.class)
                .invoke(hostCapabilities, NetworkCapabilities.TRANSPORT_ETHERNET);
        NetworkCapabilities.class.getMethod("addCapability", int.class)
                .invoke(hostCapabilities, NetworkCapabilities.NET_CAPABILITY_INTERNET);
        NetworkCapabilities.class.getMethod("addCapability", int.class)
                .invoke(hostCapabilities, NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED);
        // Android adds this to ordinary app requests. The host bridge is not
        // an Android carrier Virtual Carrier Network, so it satisfies it.
        NetworkCapabilities.class.getMethod("addCapability", int.class).invoke(hostCapabilities,
                NetworkCapabilities.class.getField("NET_CAPABILITY_NOT_VCN_MANAGED").getInt(null));
        // Route availability does not establish validation, metering or bandwidth.
    }

    private NetworkInfo hostNetworkInfo() throws Exception {
        if (!hostNetwork) return null;
        NetworkInfo info = new NetworkInfo(ConnectivityManager.TYPE_ETHERNET, 0, "ETHERNET", "");
        info.setDetailedState(NetworkInfo.DetailedState.CONNECTED, null, "host default network");
        NetworkInfo.class.getMethod("setIsAvailable", boolean.class).invoke(info, true);
        return info;
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
            data.putParcelable("LinkProperties", new LinkProperties());
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
                case "getNetworkCapabilities": return defaultNetwork != null && defaultNetwork.equals(a[0])
                        ? new NetworkCapabilities(hostCapabilities) : null;
                case "getLinkProperties": return defaultNetwork != null && defaultNetwork.equals(a[0])
                        ? new LinkProperties() : null;
                case "getAllNetworks": return defaultNetwork == null ? new Network[0] : new Network[] {defaultNetwork};
                case "isActiveNetworkMetered": return true; // host billing policy is unknown
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
        Network active = connectivity.getActiveNetwork();
        NetworkInfo info = connectivity.getActiveNetworkInfo();
        if (hostNetwork != (active != null) || hostNetwork != (info != null && info.isConnected()))
            throw new AssertionError("active network differs from host route snapshot");
        if (active != null) {
            NetworkCapabilities caps = connectivity.getNetworkCapabilities(active);
            if (caps == null || !caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
                    || caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)
                    || caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED))
                throw new AssertionError("host network capabilities incorrect");
        }
        CountDownLatch available = new CountDownLatch(1);
        CountDownLatch unavailable = new CountDownLatch(1);
        AtomicReference<Network> observed = new AtomicReference<>();
        ConnectivityManager.NetworkCallback callback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) { observed.set(network); }
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) available.countDown();
            }
            @Override public void onUnavailable() { unavailable.countDown(); }
        };
        connectivity.registerDefaultNetworkCallback(callback);
        if (hostNetwork && (!available.await(5, TimeUnit.SECONDS) || !active.equals(observed.get())))
            throw new AssertionError("default network callback missing");
        CountDownLatch listening = new CountDownLatch(1);
        ConnectivityManager.NetworkCallback listener = new ConnectivityManager.NetworkCallback() {
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) listening.countDown();
            }
        };
        NetworkRequest ethernet = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET).build();
        if (hostNetwork && !ethernet.canBeSatisfiedBy(hostCapabilities))
            throw new AssertionError("host capabilities do not satisfy Ethernet: " + ethernet + " vs " + hostCapabilities);
        connectivity.registerNetworkCallback(ethernet, listener);
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
        System.out.println("ACTIVITY_CONNECTIVITY_OK " + (hostNetwork ? "host" : "offline"));
    }
}
