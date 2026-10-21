// Web pages for links an app hands to the browser. Plain Java: no framework
// classes, so the host JVM can run its test (Tests/Host/FrameworkLinksProbe.java).
import java.io.UnsupportedEncodingException;
import java.net.URI;
import java.net.URISyntaxException;
import java.net.URLDecoder;
import java.net.URLEncoder;
import java.util.regex.Pattern;

final class FrameworkLinks {
    private static final String STORE = "https://play.google.com/store/apps/details?id=";
    private static final Pattern PACKAGE = Pattern.compile("[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)*");

    private FrameworkLinks() {}

    // The Play Store's public page for an application id.
    static String storePage(String id) {
        try {
            return STORE + URLEncoder.encode(id, "UTF-8");
        } catch (UnsupportedEncodingException impossible) {
            throw new AssertionError(impossible);
        }
    }

    // An http(s) link for the host browser. Google's ad install links are
    // https://play.google.com/d?id=<package>&inline=true&enifd=...: a deep link
    // only the Play Store app understands, which the web answers with 404. They
    // open the public store page of the same application instead; other links,
    // and a /d link without a valid id, are returned as they are.
    static String webLink(String url) {
        try {
            URI uri = new URI(url);
            String path = uri.getRawPath();
            if (uri.getHost() == null || !uri.getHost().equalsIgnoreCase("play.google.com")
                    || !("/d".equals(path) || "/d/".equals(path))) return url;
            String id = queryParameter(uri.getRawQuery(), "id");
            return id != null && PACKAGE.matcher(id).matches() ? storePage(id) : url;
        } catch (URISyntaxException malformed) {
            return url;
        }
    }

    private static String queryParameter(String query, String name) {
        if (query == null) return null;
        for (String pair : query.split("&")) {
            int equals = pair.indexOf('=');
            try {
                if (equals > 0 && URLDecoder.decode(pair.substring(0, equals), "UTF-8").equals(name))
                    return URLDecoder.decode(pair.substring(equals + 1), "UTF-8");
            } catch (UnsupportedEncodingException | IllegalArgumentException undecodable) {
                return null;
            }
        }
        return null;
    }
}
