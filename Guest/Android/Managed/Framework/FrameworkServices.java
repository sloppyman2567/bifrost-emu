// Local Java Binder registration. Native wire services use their own Binder implementations.
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.Map;
import java.util.LinkedHashMap;
import java.util.Arrays;
import org.json.JSONArray;
import org.json.JSONObject;

final class FrameworkServices {
    boolean diagnostic;
    private final Map<String, String> interfaces = new LinkedHashMap<>();
    private final Map<String, Call> handlers = new LinkedHashMap<>();
    interface Call {
        Object invoke(String method, Object[] arguments) throws Exception;
        // null means coverage has not yet been classified, not that all
        // methods are supported. The same enum drives dispatch and inventory.
        default String[] methods() { return null; }
    }

    synchronized String inventory() throws Exception {
        JSONArray result = new JSONArray();
        for (String name : interfaces.keySet()) {
            String iface = interfaces.get(name);
            String[] handled = handlers.get(name).methods();
            java.lang.reflect.Method[] methods = Class.forName(iface).getDeclaredMethods();
            Arrays.sort(methods, java.util.Comparator.comparing(java.lang.reflect.Method::toString));
            JSONArray contracts = new JSONArray();
            for (java.lang.reflect.Method method : methods) {
                if (method.getDeclaringClass() == Object.class || method.getName().equals("asBinder")) continue;
                contracts.put(new JSONObject().put("signature", method.toString()).put("coverage",
                        handled == null ? "unclassified" : Arrays.asList(handled).contains(method.getName())
                                ? "implemented" : "unsupported"));
            }
            result.put(new JSONObject().put("service", name).put("interface", iface).put("methods", contracts));
        }
        return new JSONObject().put("services", result).toString();
    }

    // Registers a local implementation of `iface` under `name`.
    // ServiceManager.getService consults sCache before any Binder lookup.
    @SuppressWarnings("unchecked")
    synchronized Object register(String name, String iface, Call call) throws Exception {
        String[] covered = call.methods();
        if (covered != null) {
            java.util.Set<String> declared = new java.util.HashSet<>();
            for (java.lang.reflect.Method method : Class.forName(iface).getMethods()) declared.add(method.getName());
            for (String method : covered) if (!declared.contains(method))
                throw new IllegalStateException(name + "." + method + " is absent from the framework interface");
        }
        Object proxy = local(name, iface, call);
        Field cache = Class.forName("android.os.ServiceManager").getDeclaredField("sCache");
        cache.setAccessible(true);
        ((Map<String, IBinder>) cache.get(null)).put(name, ((IInterface) proxy).asBinder());
        interfaces.put(name, iface); handlers.put(name, call);
        return proxy;
    }

    // Child Binder interfaces (for example IImeTracker) are returned by their
    // parent service rather than published in ServiceManager.
    Object local(String name, String iface, Call call) throws Exception {
        Class<?> type = Class.forName(iface);
        FrameworkTrace.catalog(name, type, call.methods());
        Binder binder = new Binder();
        Object proxy = Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type},
                (self, method, arguments) -> {
                    if (method.getName().equals("asBinder")) return binder;
                    if (method.getDeclaringClass() == Object.class) {
                        switch (method.getName()) {
                            case "toString": return "Bifrost:" + name;
                            case "hashCode": return System.identityHashCode(self);
                            case "equals": return self == arguments[0];
                            default: throw new AssertionError(method);
                        }
                    }
                    if (diagnostic) System.out.println("LOCAL_SERVICE " + name + "." + method.getName());
                    long trace = FrameworkTrace.begin(name, method, arguments);
                    try {
                        Object result = call.invoke(method.getName(), arguments);
                        FrameworkTrace.end(trace, result, null);
                        return result;
                    } catch (Throwable error) {
                        FrameworkTrace.end(trace, null, error);
                        throw error;
                    }
                });
        binder.attachInterface((IInterface) proxy, iface);
        return proxy;
    }

}
