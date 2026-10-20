// Accounts, tokens, user data and visibility for the "account" service. Every
// guest process (the session, microG's service and Activity processes) runs
// its own AccountManagerService proxy, so the data lives in one JSON file
// under /data/system/users/0 that they update under a cross-process lock and
// publish atomically; Asgard reads and imports the same file on the host.
import android.accounts.Account;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONObject;

final class FrameworkAccountStore {
    static final String FILE = "/data/system/users/0/bifrost-accounts.json";
    private static final int VERSION = 1, MAX_BYTES = 8 * 1024 * 1024;
    // FileChannel.lock does not wait for overlapping locks held by another
    // channel in this JVM: serialize those before taking the process lock.
    private static final Object STORE_LOCK = new Object();

    static final class Entry {
        final String name, type;
        String password, previousName;
        long lastAuthenticated;
        final Map<String, String> data = new LinkedHashMap<>();
        final Map<String, String> tokens = new LinkedHashMap<>();
        final Map<String, Integer> visibility = new LinkedHashMap<>();
        final Set<String> grants = new HashSet<>();  // "uid:authTokenType"
        Entry(String name, String type) { this.name = name; this.type = type; }
        Account account() { return new Account(name, type); }
    }

    static final class State {
        final List<Entry> entries = new ArrayList<>();
        long revision;
        boolean dirty;

        Entry find(Account account) {
            for (Entry entry : entries)
                if (entry.name.equals(account.name) && entry.type.equals(account.type)) return entry;
            return null;
        }
        List<Entry> ofType(String type) {
            List<Entry> result = new ArrayList<>();
            for (Entry entry : entries) if (type == null || entry.type.equals(type)) result.add(entry);
            return result;
        }
        Entry add(Account account) {
            Entry entry = new Entry(account.name, account.type);
            entries.add(entry);
            dirty = true;
            return entry;
        }
        boolean remove(Account account) {
            for (Iterator<Entry> it = entries.iterator(); it.hasNext();) {
                Entry entry = it.next();
                if (entry.name.equals(account.name) && entry.type.equals(account.type)) {
                    it.remove();
                    dirty = true;
                    return true;
                }
            }
            return false;
        }
        void touch() { dirty = true; }
    }

    interface Mutation<T> { T apply(State state) throws Exception; }

    private final File file;
    private State cached;
    private long cachedStamp = -1, cachedSize = -1;

    FrameworkAccountStore(File file) { this.file = file; }

    long revision() {
        synchronized (STORE_LOCK) { return file.lastModified() * 31 + file.length(); }
    }

    // The latest published state. Writers replace the file atomically, so a
    // reader needs no lock; the parse is cached until the file changes.
    State read() throws Exception {
        synchronized (STORE_LOCK) {
            long stamp = file.lastModified(), size = file.length();
            if (cached == null || stamp != cachedStamp || size != cachedSize) {
                cached = load();
                cachedStamp = stamp;
                cachedSize = size;
            }
            return cached;
        }
    }

    <T> T update(Mutation<T> mutation) throws Exception {
        synchronized (STORE_LOCK) {
            File directory = file.getParentFile();
            if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Account storage unavailable");
            try (RandomAccessFile lock = new RandomAccessFile(new File(directory, file.getName() + ".lock"), "rw");
                    FileLock held = lock.getChannel().lock()) {
                State state = load();
                T result = mutation.apply(state);
                if (state.dirty) {
                    state.revision++;
                    save(state);
                    cached = null;
                }
                return result;
            }
        }
    }

    private State load() throws Exception {
        State state = new State();
        AtomicFile atomic = new AtomicFile(file);
        if (!file.exists() && !new File(file + ".bak").exists()) return state;
        byte[] bytes;
        try (java.io.FileInputStream input = atomic.openRead()) {
            java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            for (int count; (count = input.read(chunk)) > 0;) {
                buffer.write(chunk, 0, count);
                if (buffer.size() > MAX_BYTES) throw new IOException("Account storage is too large");
            }
            bytes = buffer.toByteArray();
        }
        JSONObject root = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
        if (root.getInt("version") != VERSION) throw new IOException("Unsupported account storage version");
        state.revision = root.optLong("revision");
        JSONArray array = root.getJSONArray("accounts");
        for (int i = 0; i < array.length(); i++) {
            JSONObject value = array.getJSONObject(i);
            Entry entry = new Entry(value.getString("name"), value.getString("type"));
            entry.password = value.isNull("password") ? null : value.optString("password", null);
            entry.previousName = value.isNull("previous") ? null : value.optString("previous", null);
            entry.lastAuthenticated = value.optLong("authenticated");
            strings(value.optJSONObject("data"), entry.data);
            strings(value.optJSONObject("tokens"), entry.tokens);
            JSONObject visible = value.optJSONObject("visibility");
            if (visible != null) for (Iterator<String> it = visible.keys(); it.hasNext();) {
                String key = it.next();
                entry.visibility.put(key, visible.getInt(key));
            }
            JSONArray grants = value.optJSONArray("grants");
            if (grants != null) for (int g = 0; g < grants.length(); g++) entry.grants.add(grants.getString(g));
            state.entries.add(entry);
        }
        return state;
    }

    private static void strings(JSONObject source, Map<String, String> target) throws Exception {
        if (source == null) return;
        for (Iterator<String> it = source.keys(); it.hasNext();) {
            String key = it.next();
            target.put(key, source.isNull(key) ? null : source.getString(key));
        }
    }

    private void save(State state) throws Exception {
        JSONArray array = new JSONArray();
        for (Entry entry : state.entries) {
            JSONObject value = new JSONObject().put("name", entry.name).put("type", entry.type)
                    .put("password", entry.password == null ? JSONObject.NULL : entry.password)
                    .put("previous", entry.previousName == null ? JSONObject.NULL : entry.previousName)
                    .put("authenticated", entry.lastAuthenticated)
                    .put("data", object(entry.data)).put("tokens", object(entry.tokens));
            JSONObject visible = new JSONObject();
            for (Map.Entry<String, Integer> each : entry.visibility.entrySet()) visible.put(each.getKey(), each.getValue());
            value.put("visibility", visible).put("grants", new JSONArray(entry.grants));
            array.put(value);
        }
        byte[] bytes = new JSONObject().put("version", VERSION).put("revision", state.revision)
                .put("accounts", array).toString().getBytes(StandardCharsets.UTF_8);
        AtomicFile atomic = new AtomicFile(file);
        FileOutputStream output = atomic.startWrite();
        try {
            output.write(bytes);
            atomic.finishWrite(output);
        } catch (IOException error) {
            atomic.failWrite(output);
            throw error;
        }
    }

    private static JSONObject object(Map<String, String> values) throws Exception {
        JSONObject result = new JSONObject();
        for (Map.Entry<String, String> each : values.entrySet())
            result.put(each.getKey(), each.getValue() == null ? JSONObject.NULL : each.getValue());
        return result;
    }
}
