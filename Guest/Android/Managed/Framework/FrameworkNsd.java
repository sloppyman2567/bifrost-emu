// Network service discovery on a network with no mDNS responder: NsdManager
// constructs, discovery starts and stops, and no service is ever found. The
// host has no mDNS daemon, so resolving and advertising fail instead of
// pretending to succeed. Mainline relocates the NSD classes, so every type is
// found by reflection from the provisioned INsdManager.
import java.lang.reflect.Method;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class FrameworkNsd implements FrameworkServices.Call {
    // NsdManager.FAILURE_INTERNAL_ERROR
    private static final int FAILURE_INTERNAL_ERROR = 0;

    private enum Contract { connect }

    private enum Connector {
        discoverServices, stopDiscovery, resolveService, stopResolution, registerService,
        unregisterService, registerServiceInfoCallback, unregisterServiceInfoCallback,
        startDaemon, registerOffloadEngine, unregisterOffloadEngine
    }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    @Override public String[] methods() { return names(Contract.values()); }

    private final FrameworkServices services;
    // NsdManager's callbacks are oneway: never run them inside the request.
    private final ExecutorService callbacks = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "NsdCallbacks");
        thread.setDaemon(true);
        return thread;
    });

    FrameworkNsd(FrameworkServices services) { this.services = services; }

    void install() throws Exception {
        services.register("servicediscovery", "android.net.nsd.INsdManager", this);
    }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        if (!method.equals("connect")) throw new UnsupportedOperationException("servicediscovery." + method);
        Object callback = a[0];
        Method connect = null;
        for (Method candidate : Class.forName("android.net.nsd.INsdManager").getMethods())
            if (candidate.getName().equals("connect")) connect = candidate;
        Class<?> callbackType = connect.getParameterTypes()[0];
        return services.local("servicediscovery.connector", connect.getReturnType().getName(),
                new FrameworkServices.Call() {
            @Override public String[] methods() { return names(Connector.values()); }

            @Override public Object invoke(String name, Object[] b) {
                switch (Connector.valueOf(name)) {
                    case discoverServices: deliver(callbackType, callback, "onDiscoverServicesStarted", b[0], b[1]); break;
                    case stopDiscovery: deliver(callbackType, callback, "onStopDiscoverySucceeded", b[0]); break;
                    case resolveService: deliver(callbackType, callback, "onResolveServiceFailed", b[0], FAILURE_INTERNAL_ERROR); break;
                    case stopResolution: deliver(callbackType, callback, "onStopResolutionSucceeded", b[0]); break;
                    case registerService: deliver(callbackType, callback, "onRegisterServiceFailed", b[0], FAILURE_INTERNAL_ERROR); break;
                    case unregisterService: deliver(callbackType, callback, "onUnregisterServiceFailed", b[0], FAILURE_INTERNAL_ERROR); break;
                    case unregisterServiceInfoCallback: deliver(callbackType, callback, "onServiceInfoCallbackUnregistered", b[0]); break;
                    default: break;
                }
                return null;
            }
        });
    }

    private void deliver(Class<?> type, Object callback, String name, Object... arguments) {
        callbacks.execute(() -> {
            try {
                for (Method method : type.getMethods())
                    if (method.getName().equals(name) && method.getParameterCount() == arguments.length) {
                        method.invoke(callback, arguments);
                        return;
                    }
                throw new NoSuchMethodException(name);
            } catch (Exception error) {
                System.out.println("NSD_CALLBACK_FAILED " + name + " " + error);
            }
        });
    }
}
