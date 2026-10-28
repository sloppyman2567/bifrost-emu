// Shared libraries (<uses-library>), as SystemConfig and PackageManagerService provide them. The image's
// /system/etc/permissions/*.xml declare each as <library name=... file=.../>. A manifest asks for some; install
// resolves them and records them in ApplicationInfo.sharedLibraryInfos, from which LoadedApk builds one class
// loader per library for the app's loader to consult (android-15.0.0_r1: sharedLibraryFiles alone is not enough,
// makePaths keeps only .apk entries from it). Android 9 removed the Apache HTTP legacy classes from the boot
// classpath, so an app that uses them declares org.apache.http.legacy (YouTube's obfuscated code does, as an
// optional library).
import android.content.pm.ApplicationInfo;
import android.content.pm.VersionedPackage;
import android.util.Xml;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

final class FrameworkSharedLibraries {
    static final String ORG_APACHE_HTTP_LEGACY = "org.apache.http.legacy";
    // Build.VERSION_CODES.P: an app targeting an older release gets org.apache.http.legacy without asking
    // (OrgApacheHttpLegacyUpdater and PackageSharedLibraryUpdater.prefixRequiredLibrary, android-15.0.0_r1).
    private static final int TARGET_WITHOUT_APACHE_HTTP_LEGACY = 28;

    private final Map<String, String> jars = new LinkedHashMap<>();  // library name -> jar path, declared and present

    // SystemConfig.readPermissions: every *.xml of the directory, in name order; a <library> whose file is
    // absent is ignored. The conditions SystemConfig also reads (min-device-sdk, on-bootclasspath-*) are not.
    FrameworkSharedLibraries(File permissions) {
        File[] files = permissions.listFiles((directory, name) -> name.endsWith(".xml"));
        if (files == null) return;
        Arrays.sort(files);
        for (File file : files) {
            try (FileInputStream input = new FileInputStream(file)) {
                XmlPullParser parser = Xml.newPullParser();
                parser.setInput(input, "UTF-8");
                for (int type; (type = parser.next()) != XmlPullParser.END_DOCUMENT;) {
                    if (type != XmlPullParser.START_TAG || !"library".equals(parser.getName())) continue;
                    String name = parser.getAttributeValue(null, "name"), path = parser.getAttributeValue(null, "file");
                    if (name != null && path != null && new File(path).isFile()) jars.putIfAbsent(name, path);
                }
            } catch (IOException | XmlPullParserException unreadable) {
                System.out.println("APP_SHARED_LIBRARIES_UNREADABLE " + file + ": " + unreadable);
            }
        }
    }

    static FrameworkSharedLibraries system() {
        return new FrameworkSharedLibraries(new File("/system/etc/permissions"));
    }

    boolean has(String name) { return jars.containsKey(name); }

    // The libraries an app gets, name -> jar path, required ones first: what PackageManagerService records. A
    // required library the image lacks fails the install there; here the app still starts (it ran before libraries
    // existed), with the gap named in the log.
    Map<String, String> resolve(int targetSdk, List<String> required, List<String> optional) {
        List<String> wanted = new ArrayList<>(required);
        if (targetSdk < TARGET_WITHOUT_APACHE_HTTP_LEGACY && !wanted.contains(ORG_APACHE_HTTP_LEGACY)
                && !optional.contains(ORG_APACHE_HTTP_LEGACY)) wanted.add(0, ORG_APACHE_HTTP_LEGACY);
        Map<String, String> libraries = new LinkedHashMap<>();
        for (boolean mandatory : new boolean[] {true, false}) {
            for (String name : mandatory ? wanted : optional) {
                String path = jars.get(name);
                if (path != null) libraries.putIfAbsent(name, path);
                else if (mandatory) System.out.println("APP_SHARED_LIBRARY_MISSING " + name);
            }
        }
        return libraries;
    }

    // ApplicationInfo as PackageManagerService hands it out: the jar paths (the public sharedLibraryFiles) and a
    // SharedLibraryInfo per library, built as SharedLibrariesImpl.addBuiltInSharedLibraryLPw does (the path set, no
    // package or code paths, version undefined, declared by the platform). Both are cleared for no libraries.
    static void apply(ApplicationInfo app, Map<String, String> libraries) throws ReflectiveOperationException {
        List<Object> infos = new ArrayList<>();
        if (!libraries.isEmpty()) {
            Class<?> type = Class.forName("android.content.pm.SharedLibraryInfo");
            java.lang.reflect.Constructor<?> constructor = type.getConstructor(String.class, String.class, List.class,
                    String.class, long.class, int.class, VersionedPackage.class, List.class, List.class, boolean.class);
            for (Map.Entry<String, String> library : libraries.entrySet())
                infos.add(constructor.newInstance(library.getValue(), null, null, library.getKey(),
                        type.getField("VERSION_UNDEFINED").getLong(null), type.getField("TYPE_BUILTIN").getInt(null),
                        new VersionedPackage("android", 0L), null, null, false));
        }
        app.sharedLibraryFiles = libraries.isEmpty() ? null : libraries.values().toArray(new String[0]);
        ApplicationInfo.class.getField("sharedLibraryInfos").set(app, infos.isEmpty() ? null : infos);
    }
}
