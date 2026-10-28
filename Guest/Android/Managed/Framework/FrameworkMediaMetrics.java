// MediaMetricsManagerService ("media_metrics", android.media.metrics.IMediaMetricsManager). Sessions get random
// 96-bit ids (URL-safe Base64, no padding), as the service makes them. Its reports go to statsd, which this device
// does not run, so they are accepted and dropped, as when logging is blocked.
import java.security.SecureRandom;

final class FrameworkMediaMetrics implements FrameworkServices.Call {
    private enum Contract {
        reportPlaybackMetrics, getPlaybackSessionId, getRecordingSessionId, reportNetworkEvent, reportPlaybackErrorEvent,
        reportPlaybackStateEvent, reportTrackChangeEvent, reportEditingEndedEvent, getTranscodingSessionId,
        getEditingSessionId, getBundleSessionId, reportBundleMetrics, releaseSessionId
    }

    @Override public String[] methods() {
        Contract[] values = Contract.values();
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    private final SecureRandom random = new SecureRandom();

    void install(FrameworkServices services) throws Exception {
        services.register("media_metrics", "android.media.metrics.IMediaMetricsManager", this);
    }

    private String sessionId() {
        byte[] id = new byte[12];
        random.nextBytes(id);
        return android.util.Base64.encodeToString(id, android.util.Base64.NO_PADDING | android.util.Base64.NO_WRAP | android.util.Base64.URL_SAFE);
    }

    @Override public Object invoke(String method, Object[] a) {
        Contract contract;
        try { contract = Contract.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw new UnsupportedOperationException("media_metrics." + method); }
        switch (contract) {
            case getPlaybackSessionId:
            case getRecordingSessionId:
            case getTranscodingSessionId:
            case getEditingSessionId:
            case getBundleSessionId:
                return sessionId();
            default: return null;  // reports and releaseSessionId: nothing consumes them here
        }
    }
}
