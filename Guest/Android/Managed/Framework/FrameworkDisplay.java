// One virtual display; real DisplayManagerGlobal owns client listeners and dispatch.
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.Parcelable;
import java.lang.reflect.Array;
import android.content.res.Configuration;
import android.graphics.Rect;
import java.util.HashSet;
import java.util.Set;

final class FrameworkDisplay implements FrameworkServices.Call {
    private enum Contract {
        getDisplayInfo, getDisplayIds, registerCallback, registerCallbackWithEventMask,
        getPreferredWideGamutColorSpaceId, getOverlaySupport, getWifiDisplayStatus,
        disconnectWifiDisplay, connectWifiDisplay, renameWifiDisplay, forgetWifiDisplay,
        pauseWifiDisplay, resumeWifiDisplay, startWifiDisplayScan, stopWifiDisplayScan,
        getStableDisplaySize, getRefreshRateSwitchingType, getUserPreferredDisplayMode,
        getSystemPreferredDisplayMode, getDisplayDecorationSupport, areUserDisabledHdrTypesAllowed,
        getUserDisabledHdrTypes, getHdrConversionMode, getHdrConversionModeSetting,
        getSupportedHdrOutputTypes, isMinimalPostProcessingRequested
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private static final long CHANGED_MASK = 4; // DisplayManager.EVENT_FLAG_DISPLAY_CHANGED
    private static final int CHANGED_EVENT = 2; // DisplayManagerGlobal.EVENT_DISPLAY_CHANGED
    private Object info;
    private int width, height;
    private int rotation;
    private int maxWidth, maxHeight; // host display holding the window; 0 until play mode sizes it
    private int densityDpi = 160; // DisplayMetrics.DENSITY_DEFAULT until play mode sizes the display
    // DisplayManagerService records the default display's natural size at first boot and reports it
    // for good; here the first size play mode gives the display is that boot state.
    private int stableWidth, stableHeight;
    private boolean stableFromHost;
    private IInterface callback;
    private long eventsMask;
    private boolean wifiScanRequested;
    private FrameworkPackages packages;
    private Handler main;
    private Object applicationThread;
    private final Set<IBinder> windowContexts = new HashSet<>();

    synchronized void attachApplicationThread(Object thread) throws Exception {
        java.lang.reflect.Method method = thread.getClass().getDeclaredMethod("getApplicationThread");
        method.setAccessible(true);
        applicationThread = method.invoke(thread);
    }

    synchronized Object attachWindowContext(Object caller, IBinder token, int type, int displayId)
            throws Exception {
        if (caller == null || token == null) throw new NullPointerException("window context caller/token");
        if (caller != applicationThread || displayId != 0) return null;
        if (type < 1 || type > 2999) throw new IllegalArgumentException("Invalid window type " + type);
        Object info = windowContextInfo();
        windowContexts.add(token);
        return info;
    }

    synchronized void detachWindowContext(IBinder token) {
        if (token == null) throw new NullPointerException("window context token");
        windowContexts.remove(token);
    }

    synchronized boolean windowInsets(int displayId, Object out) throws Exception {
        if (displayId != 0) throw new android.view.WindowManager.InvalidDisplayException(
                "Display#" + displayId + " could not be found");
        if (out == null) throw new NullPointerException("outInsetsState");
        Class<?> state = Class.forName("android.view.InsetsState");
        Object value = state.getConstructor().newInstance();
        state.getMethod("setDisplayFrame", Rect.class).invoke(value, new Rect(0, 0, width, height));
        state.getMethod("set", state).invoke(out, value);
        // The owned host window has no Android status/navigation bars. IME
        // sources will be supplied by the input-method window when installed.
        return false;
    }

    synchronized Rect bounds() {
        return new Rect(0, 0, width, height);
    }

    // Pixel width, height and density dpi, as one snapshot.
    synchronized int[] metrics() {
        return new int[] {width, height, densityDpi};
    }

    synchronized int rotation() { return rotation; }

    void setRotation(int value) throws Exception {
        if (value < 0 || value > 3) throw new IllegalArgumentException("Invalid display rotation");
        synchronized (this) {
            if (rotation == value) return;
            info = displayInfo(width, height, densityDpi, value);
            rotation = value;
        }
        notifyChanged();
    }

    private Object windowContextInfo() throws Exception {
        Configuration config = globalConfiguration();
        setScreen(config);
        setWindowBounds(config);
        return Class.forName("android.window.WindowContextInfo")
                .getConstructor(Configuration.class, int.class).newInstance(config, 0);
    }

    // The Activity override configuration WMS supplies: its window bounds
    // and the screen size and density resolved from them (TaskFragment.
    // computeConfigResourceOverrides). ViewRootImpl measures new windows
    // (dialogs) and WindowMetrics reports sizes from these; other fields stay
    // undefined to inherit.
    synchronized Configuration activityOverride() throws Exception {
        Configuration config = new Configuration();
        Configuration.class.getMethod("unset").invoke(config);
        setScreen(config);
        setWindowBounds(config);
        return config;
    }

    // The process-wide configuration ResourcesManager applies: the system
    // configuration with this display's screen size, density and window
    // bounds, as WMS's global configuration carries them
    // (DisplayContent.computeScreenConfiguration). ViewConfiguration caches
    // its drawing-cache limit (4 * max bounds) per density from whichever
    // context asks first; empty bounds there made the limit 0, so software
    // layers, such as WebView ads in stacked Activities, never drew.
    synchronized Configuration processConfiguration() throws Exception {
        Configuration config = globalConfiguration();
        setScreen(config);
        setWindowBounds(config);
        return config;
    }

    // The host display's pixel size: windowConfiguration max bounds, the
    // largest the window can become (maximized or fullscreen).
    synchronized void setMaxBounds(int newWidth, int newHeight) {
        if (newWidth <= 0 || newHeight <= 0) throw new IllegalArgumentException("Invalid display bounds");
        maxWidth = newWidth;
        maxHeight = newHeight;
    }

    // The system configuration with the uiMode UiModeManagerService applies
    // to WMS's global configuration (normal type, the night mode of the desktop's colour scheme).
    private static Configuration globalConfiguration() {
        Configuration config = new Configuration(android.content.res.Resources.getSystem().getConfiguration());
        config.uiMode = FrameworkUiMode.configurationUiMode();
        // WMS derives these from the input devices' sources. The host window delivers a mouse and touch as fingers,
        // and the desktop keyboard is the virtual one: a touchscreen, and no physical keyboard or navigation device,
        // as on a phone. (The platform defaults leave all three undefined.)
        config.touchscreen = Configuration.TOUCHSCREEN_FINGER;
        config.keyboard = Configuration.KEYBOARD_NOKEYS;
        config.navigation = Configuration.NAVIGATION_NONAV;
        return config;
    }

    // ActivityTaskManagerService.getDeviceConfigurationInfo (android-15.0.0_r1): the global configuration's
    // touchscreen, keyboard and navigation, the input features those imply, and the property ro.opengles.version
    // (ConfigurationInfo.GL_ES_VERSION_UNDEFINED, 0, when it is not set).
    synchronized android.content.pm.ConfigurationInfo deviceConfigurationInfo() throws Exception {
        Configuration global = globalConfiguration();
        android.content.pm.ConfigurationInfo info = new android.content.pm.ConfigurationInfo();
        info.reqTouchScreen = global.touchscreen;
        info.reqKeyboardType = global.keyboard;
        info.reqNavigation = global.navigation;
        if (global.navigation == Configuration.NAVIGATION_DPAD || global.navigation == Configuration.NAVIGATION_TRACKBALL)
            info.reqInputFeatures |= android.content.pm.ConfigurationInfo.INPUT_FEATURE_FIVE_WAY_NAV;
        if (global.keyboard != Configuration.KEYBOARD_UNDEFINED && global.keyboard != Configuration.KEYBOARD_NOKEYS)
            info.reqInputFeatures |= android.content.pm.ConfigurationInfo.INPUT_FEATURE_HARD_KEYBOARD;
        info.reqGlEsVersion = (Integer) Class.forName("android.os.SystemProperties")
                .getMethod("getInt", String.class, int.class).invoke(null, "ro.opengles.version", 0);
        return info;
    }

    // TaskFragment rounds pixel sizes to dp as (int) (px / density + 0.5f).
    private void setScreen(Configuration config) {
        float density = densityDpi / 160f; // DisplayMetrics.DENSITY_DEFAULT
        config.densityDpi = densityDpi;
        config.screenWidthDp = (int) (width / density + 0.5f);
        config.screenHeightDp = (int) (height / density + 0.5f);
        config.smallestScreenWidthDp = Math.min(config.screenWidthDp, config.screenHeightDp);
        config.orientation = width > height ? Configuration.ORIENTATION_LANDSCAPE : Configuration.ORIENTATION_PORTRAIT;
    }

    private void setWindowBounds(Configuration config) throws Exception {
        Object window = Configuration.class.getField("windowConfiguration").get(config);
        Rect bounds = new Rect(0, 0, width, height);
        Rect max = new Rect(0, 0, Math.max(maxWidth, width), Math.max(maxHeight, height));
        window.getClass().getMethod("setBounds", Rect.class).invoke(window, bounds);
        window.getClass().getMethod("setMaxBounds", Rect.class).invoke(window, max);
        window.getClass().getMethod("setAppBounds", Rect.class).invoke(window, bounds);
        window.getClass().getMethod("setRotation", int.class).invoke(window, rotation);
        window.getClass().getMethod("setDisplayRotation", int.class).invoke(window, rotation);
    }

    private void dispatchWindowContexts() throws Exception {
        IBinder[] clients;
        Object config;
        synchronized (this) {
            clients = windowContexts.toArray(new IBinder[0]);
            config = windowContextInfo();
        }
        Class<?> controllerType = Class.forName("android.window.WindowTokenClientController");
        Object controller = controllerType.getMethod("getInstance").invoke(null);
        for (IBinder token : clients) {
            synchronized (this) { if (!windowContexts.contains(token)) continue; }
            // The single-process profile delivers the same client endpoint as
            // WindowContextInfoChangeItem, on the application's main looper.
            controllerType.getMethod("onWindowContextInfoChanged", IBinder.class, config.getClass())
                    .invoke(controller, token, config);
        }
    }

    void install(FrameworkServices services, FrameworkPackages packages) throws Exception {
        this.packages = packages;
        main = new Handler(Looper.getMainLooper());
        info = displayInfo(width = 480, height = 800, densityDpi, rotation);
        recordStableSize();
        services.register("display", "android.hardware.display.IDisplayManager", this);
    }

    @Override public Object invoke(String method, Object[] args) throws Exception {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("display." + method); }
        switch (contract) {
            case getDisplayInfo: return ((Integer) args[0]) == 0 ? snapshot() : null;
            case getDisplayIds: return new int[] {0};
            case registerCallback:
                register(args[0], 7L); // legacy ADDED | REMOVED | CHANGED
                return null;
            case registerCallbackWithEventMask:
                register(args[0], ((Number) args[1]).longValue());
                return null;
            case getPreferredWideGamutColorSpaceId: return 0;
            case getOverlaySupport:
                return Class.forName("android.hardware.OverlayProperties")
                        .getMethod("getDefault").invoke(null);
            case getWifiDisplayStatus:
                // DisplayManagerService without a WifiDisplayAdapter:
                // FEATURE_STATE_UNAVAILABLE, no scan, no active/remembered displays.
                return Class.forName("android.hardware.display.WifiDisplayStatus")
                        .getConstructor().newInstance();
            case disconnectWifiDisplay:
                // AOSP permits any app to disconnect; no adapter is present.
                return null;
            case connectWifiDisplay:
            case renameWifiDisplay:
            case forgetWifiDisplay:
                if (args[0] == null) throw new IllegalArgumentException("address must not be null");
                enforceWifiDisplay(packages);
                return null;
            case pauseWifiDisplay:
            case resumeWifiDisplay:
                enforceWifiDisplay(packages);
                return null;
            case startWifiDisplayScan:
            case stopWifiDisplayScan:
                enforceWifiDisplay(packages);
                synchronized (this) {
                    if (callback == null) throw new IllegalStateException(
                            "The calling process has not registered an IDisplayManagerCallback.");
                    // Requests are idempotent per process; without an adapter
                    // they do not change the advertised SCAN_STATE_NOT_SCANNING.
                    wifiScanRequested = contract == Contract.startWifiDisplayScan;
                }
                return null;
            // The remaining queries are answered as DisplayManagerService (android-15.0.0_r1) does on a
            // phone with one built-in panel: no HDR output control, no user or system preferred mode,
            // no decoration hardware, and the default mode-switching type.
            case getStableDisplaySize: {
                synchronized (this) { return new android.graphics.Point(stableWidth, stableHeight); }
            }
            case getRefreshRateSwitchingType:
                return constant("android.hardware.display.DisplayManager", "SWITCHING_TYPE_WITHIN_GROUPS");
            case getUserPreferredDisplayMode:
            case getSystemPreferredDisplayMode:
            case getDisplayDecorationSupport:
                return null;
            case areUserDisabledHdrTypesAllowed: return true;
            case getUserDisabledHdrTypes:
            case getSupportedHdrOutputTypes:
                return new int[0];
            case getHdrConversionMode:
            case getHdrConversionModeSetting:
                // The DeviceConfig flag behind mIsHdrOutputControlEnabled is unset here (default off), and the
                // panel advertises no HDR types: HDR_CONVERSION_MODE_UNSUPPORTED.
                return Class.forName("android.hardware.display.HdrConversionMode").getConstructor(int.class)
                        .newInstance(constant("android.hardware.display.HdrConversionMode", "HDR_CONVERSION_UNSUPPORTED"));
            case isMinimalPostProcessingRequested:
                // The logical display lookup is unchecked in AOSP, so an unknown id is a NullPointerException.
                if (((Integer) args[0]) != 0) throw new NullPointerException("no display " + args[0]);
                return false;
            default: throw new UnsupportedOperationException("display." + method);
        }
    }

    private static void enforceWifiDisplay(FrameworkPackages packages) {
        if (!packages.granted("android.permission.CONFIGURE_WIFI_DISPLAY"))
            throw new SecurityException("android.permission.CONFIGURE_WIFI_DISPLAY");
    }

    // The natural (rotation 0) size, as DisplayInfo.getNaturalWidth/Height define it.
    private void recordStableSize() throws Exception {
        Class<?> type = info.getClass();
        stableWidth = (Integer) type.getMethod("getNaturalWidth").invoke(info);
        stableHeight = (Integer) type.getMethod("getNaturalHeight").invoke(info);
    }

    private static int constant(String type, String name) throws Exception {
        return Class.forName(type).getField(name).getInt(null);
    }

    private synchronized Object snapshot() throws Exception {
        Parcel parcel = Parcel.obtain();
        try {
            ((Parcelable) info).writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return ((Parcelable.Creator<?>) info.getClass().getField("CREATOR").get(null)).createFromParcel(parcel);
        } finally { parcel.recycle(); }
    }

    private synchronized void register(Object listener, long mask) {
        if (listener == null) throw new IllegalArgumentException("listener must not be null");
        // AOSP retains the first callback per PID and updates its event mask.
        // Every client in this profile belongs to the same guest process.
        if (callback == null) {
            IInterface next = (IInterface) listener;
            IBinder token = next.asBinder();
            try {
                token.linkToDeath(() -> {
                    synchronized (FrameworkDisplay.this) {
                        if (callback != null && callback.asBinder() == token) {
                            callback = null;
                            eventsMask = 0;
                            wifiScanRequested = false;
                        }
                    }
                }, 0);
            } catch (android.os.RemoteException dead) { throw new IllegalStateException(dead); }
            callback = next;
        }
        eventsMask = mask;
    }

    void setSize(int newWidth, int newHeight) throws Exception {
        int density;
        synchronized (this) { density = densityDpi; }
        setMetrics(newWidth, newHeight, density);
    }

    // Pixel size and density of the host window's native buffer.
    void setMetrics(int newWidth, int newHeight, int newDensityDpi) throws Exception {
        if (newWidth <= 0 || newHeight <= 0 || newDensityDpi <= 0)
            throw new IllegalArgumentException("Invalid display metrics");
        synchronized (this) {
            if (width == newWidth && height == newHeight && densityDpi == newDensityDpi) return;
            int nextRotation = (rotation & 2) | (newWidth > newHeight ? 1 : 0);
            Object next = displayInfo(newWidth, newHeight, newDensityDpi, nextRotation);
            info = next;
            if (!stableFromHost) { stableFromHost = true; recordStableSize(); }
            width = newWidth;
            height = newHeight;
            densityDpi = newDensityDpi;
            rotation = nextRotation;
        }
        notifyChanged();
    }

    // The global configuration changed without the display changing (the night mode): window contexts take it.
    void uiModeChanged() {
        notifyChanged();
    }

    private void notifyChanged() {
        // Publish the new info before notifying, and never call clients under our lock.
        main.post(() -> {
            try {
                dispatchWindowContexts();
            } catch (Exception error) {
                throw new IllegalStateException("Window context configuration delivery failed", error);
            }
            IInterface listener;
            synchronized (FrameworkDisplay.this) {
                if ((eventsMask & CHANGED_MASK) == 0) return;
                listener = callback;
            }
            if (listener == null) return;
            long trace = FrameworkTrace.begin("display.callback", "onDisplayEvent", new Object[] {0, CHANGED_EVENT});
            try {
                Class.forName("android.hardware.display.IDisplayManagerCallback")
                        .getMethod("onDisplayEvent", int.class, int.class).invoke(listener, 0, CHANGED_EVENT);
                FrameworkTrace.end(trace, null, null);
            } catch (Exception error) {
                FrameworkTrace.end(trace, null, error);
                System.err.println("DISPLAY_CALLBACK_FAILED " + error.getClass().getName());
            }
        });
    }

    // One 60 Hz display with id 0: 480x800 at 160 dpi unless play mode sizes
    // it to the host window's native pixels and scale before the app first
    // queries it.
    private static Object displayInfo(int width, int height, int densityDpi, int rotation) throws Exception {
        Class<?> type = Class.forName("android.view.DisplayInfo");
        Object info = type.getConstructor().newInstance();
        type.getField("name").set(info, "Bifrost");
        type.getField("uniqueId").set(info, "bifrost:0");
        type.getField("rotation").setInt(info, rotation);
        for (String field : new String[] {"appWidth", "logicalWidth", "largestNominalAppWidth"})
            type.getField(field).setInt(info, width);
        for (String field : new String[] {"appHeight", "logicalHeight", "largestNominalAppHeight"})
            type.getField(field).setInt(info, height);
        type.getField("smallestNominalAppWidth").setInt(info, Math.min(width, height));
        type.getField("smallestNominalAppHeight").setInt(info, Math.min(width, height));
        type.getField("logicalDensityDpi").setInt(info, densityDpi);
        type.getField("physicalXDpi").setFloat(info, densityDpi);
        type.getField("physicalYDpi").setFloat(info, densityDpi);
        type.getField("state").setInt(info, 2);  // Display.STATE_ON
        type.getField("type").setInt(info, 1);   // Display.TYPE_INTERNAL
        Class<?> modeType = Class.forName("android.view.Display$Mode");
        Object mode = modeType.getConstructor(int.class, int.class, int.class, float.class)
                .newInstance(1, width, height, 60f);
        Object modes = Array.newInstance(modeType, 1);
        Array.set(modes, 0, mode);
        type.getField("supportedModes").set(info, modes);
        type.getField("appsSupportedModes").set(info, modes);
        type.getField("modeId").setInt(info, 1);
        type.getField("defaultModeId").setInt(info, 1);
        return info;
    }

}
