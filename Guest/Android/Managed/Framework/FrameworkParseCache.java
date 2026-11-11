// Parsed APK manifests of immutable archives (the WebView provider, the
// platform resources, microG, the installed app). Parsing one costs 0.2 to 3
// seconds in a cold runtime, on every process start; the same archive yields
// the same result, so the first result is kept under /data/local/tmp and read
// back by later processes and launches: the PackageInfo, and what
// FrameworkPackages derives from a full parse (intent filters, the platform's
// protected broadcasts). An entry is keyed by the archive's path, size and
// modification time (every APK of a split directory), the request and the
// platform resources' identity; any unreadable entry is a miss. The host keeps
// the directory when it rebuilds an app's runtime while this class and
// FrameworkPackages are unchanged (android_managed.PARSE_CACHE_SOURCES).
import android.content.ComponentName;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Parcel;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;

final class FrameworkParseCache {
    private static final String DIRECTORY = "/data/local/tmp/package-cache";
    private static final int MAGIC = 0x50494332, MAX_BYTES = 32 * 1024 * 1024;

    private FrameworkParseCache() { }

    // PackageManager.getPackageArchiveInfo(apk, flags), from the cache when possible.
    static PackageInfo archiveInfo(PackageManager manager, String apk, int flags) {
        File archive = new File(apk);
        String set = "info-" + flags;
        Parcel cached = read(archive, set);
        if (cached != null) {
            try {
                PackageInfo info = cached.readTypedObject(PackageInfo.CREATOR);
                if (info != null && cached.dataAvail() == 0)
                    return FrameworkPackages.scanProviderAuthorities(info);
            } catch (RuntimeException unreadable) {
                // Parsed again below.
            } finally { cached.recycle(); }
        }
        PackageInfo parsed = FrameworkPackages.scanProviderAuthorities(manager.getPackageArchiveInfo(apk, flags));
        if (parsed != null) {
            Parcel parcel = Parcel.obtain();
            try { parcel.writeTypedObject(parsed, 0); write(archive, set, parcel); }
            finally { parcel.recycle(); }
        }
        return parsed;
    }

    // Intent filters of named component groups, as FrameworkPackages parses them.
    static Map<String, Map<ComponentName, List<IntentFilter>>> filters(File archive, String set,
            Callable<Map<String, Map<ComponentName, List<IntentFilter>>>> parse) throws Exception {
        Parcel cached = read(archive, set);
        if (cached != null) {
            try {
                Map<String, Map<ComponentName, List<IntentFilter>>> groups = new HashMap<>();
                for (int g = cached.readInt(); g > 0; g--) {
                    String name = cached.readString();
                    Map<ComponentName, List<IntentFilter>> components = new HashMap<>();
                    for (int c = cached.readInt(); c > 0; c--) {
                        ComponentName component = cached.readTypedObject(ComponentName.CREATOR);
                        List<IntentFilter> list = new ArrayList<>();
                        for (int f = cached.readInt(); f > 0; f--) list.add(cached.readTypedObject(IntentFilter.CREATOR));
                        components.put(component, list);
                    }
                    groups.put(name, components);
                }
                if (cached.dataAvail() == 0) return groups;
            } catch (RuntimeException unreadable) {
                // Parsed again below.
            } finally { cached.recycle(); }
        }
        Map<String, Map<ComponentName, List<IntentFilter>>> parsed = parse.call();
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(parsed.size());
            for (Map.Entry<String, Map<ComponentName, List<IntentFilter>>> group : parsed.entrySet()) {
                parcel.writeString(group.getKey());
                parcel.writeInt(group.getValue().size());
                for (Map.Entry<ComponentName, List<IntentFilter>> component : group.getValue().entrySet()) {
                    parcel.writeTypedObject(component.getKey(), 0);
                    parcel.writeInt(component.getValue().size());
                    for (IntentFilter filter : component.getValue()) parcel.writeTypedObject(filter, 0);
                }
            }
            write(archive, set, parcel);
        } finally { parcel.recycle(); }
        return parsed;
    }

    // A list of strings derived from an archive (the platform's protected broadcasts).
    static List<String> strings(File archive, String set, Callable<List<String>> parse) throws Exception {
        Parcel cached = read(archive, set);
        if (cached != null) {
            try {
                List<String> list = cached.createStringArrayList();
                if (list != null && cached.dataAvail() == 0) return list;
            } catch (RuntimeException unreadable) {
                // Parsed again below.
            } finally { cached.recycle(); }
        }
        List<String> parsed = parse.call();
        Parcel parcel = Parcel.obtain();
        try { parcel.writeStringList(parsed); write(archive, set, parcel); }
        finally { parcel.recycle(); }
        return parsed;
    }

    // The archive's identity: path, size and modification time of it and, for a
    // directory of splits, of every APK in it (its oat and lib directories change
    // with each compile and are not parsed); plus the platform resources.
    private static String identity(File archive, String set) {
        File platform = new File("/system/framework/framework-res.apk");
        StringBuilder text = new StringBuilder(set).append('|').append(archive.getPath());
        File[] files = archive.isDirectory()
                ? archive.listFiles(file -> file.isFile() && file.getName().endsWith(".apk"))
                : new File[] {archive};
        if (files != null) {
            Arrays.sort(files);
            for (File file : files) text.append('|').append(file.getName()).append(':').append(file.length())
                    .append(':').append(file.lastModified());
        }
        return text.append('|').append(platform.length()).append(':').append(platform.lastModified())
                .append('|').append(android.os.Build.FINGERPRINT).toString();
    }

    private static String prefix(File archive, String set) {
        return (archive.getName() + "-" + set).replaceAll("[^A-Za-z0-9._-]", "_") + "-";
    }

    private static File entry(File archive, String set) {
        try {
            byte[] hash = MessageDigest.getInstance("SHA-256")
                    .digest(identity(archive, set).getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < 12; i++) hex.append(String.format("%02x", hash[i]));
            return new File(DIRECTORY, prefix(archive, set) + hex + ".bin");
        } catch (java.security.GeneralSecurityException error) {
            throw new IllegalStateException(error);
        }
    }

    // The payload of a valid entry, or null.
    private static Parcel read(File archive, String set) {
        File entry = entry(archive, set);
        if (!entry.isFile() || entry.length() > MAX_BYTES) return null;
        Parcel parcel = Parcel.obtain();
        try (FileInputStream input = new FileInputStream(entry)) {
            ByteArrayOutputStream buffer = new ByteArrayOutputStream((int) entry.length());
            byte[] chunk = new byte[65536];
            for (int count; (count = input.read(chunk)) > 0;) buffer.write(chunk, 0, count);
            byte[] bytes = buffer.toByteArray();
            parcel.unmarshall(bytes, 0, bytes.length);
            parcel.setDataPosition(0);
            if (parcel.readInt() != MAGIC) throw new IllegalStateException("bad magic");
            return parcel;
        } catch (Exception unreadable) {
            parcel.recycle();
            entry.delete();
            return null;
        }
    }

    // Best effort: a failed write only costs the next process a parse.
    private static void write(File archive, String set, Parcel payload) {
        File entry = entry(archive, set), pending = null;
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeInt(MAGIC);
            parcel.appendFrom(payload, 0, payload.dataSize());
            File directory = entry.getParentFile();
            if (!directory.isDirectory()) {
                directory.mkdirs();
                directory.setReadable(true, false); directory.setWritable(true, false); directory.setExecutable(true, false);
            }
            // Entries for an earlier version of this archive are obsolete.
            File[] old = directory.listFiles();
            String prefix = prefix(archive, set);
            if (old != null) for (File other : old)
                if (other.getName().startsWith(prefix) && other.getName().endsWith(".bin") && !other.equals(entry)) other.delete();
            pending = new File(directory, "." + entry.getName() + "." + android.os.Process.myPid() + "." + System.nanoTime());
            try (FileOutputStream output = new FileOutputStream(pending)) { output.write(parcel.marshall()); }
            pending.setReadable(true, false);
            if (!pending.renameTo(entry)) pending.delete();
        } catch (Exception failure) {
            if (pending != null) pending.delete();
        } finally {
            parcel.recycle();
        }
    }
}
