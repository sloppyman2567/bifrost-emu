// Local Java Binder registration. Native wire services use their own Binder implementations.
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.Map;

final class FrameworkServices {
    boolean diagnostic;
    interface Call {
        Object invoke(String method, Object[] arguments) throws Exception;
    }

    // Registers a local implementation of `iface` under `name`.
    // ServiceManager.getService consults sCache before any Binder lookup.
    @SuppressWarnings("unchecked")
    Object register(String name, String iface, Call call) throws Exception {
        Class<?> type = Class.forName(iface);
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
                    long trace = FrameworkTrace.begin(name, method.getName(), arguments);
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
        Field cache = Class.forName("android.os.ServiceManager").getDeclaredField("sCache");
        cache.setAccessible(true);
        ((Map<String, IBinder>) cache.get(null)).put(name, binder);
        return proxy;
    }

}
