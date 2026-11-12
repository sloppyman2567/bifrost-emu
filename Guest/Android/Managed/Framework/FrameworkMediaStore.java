// The "media" provider's images collection (MediaStore.Images, content://media/{external,external_primary}/images/media)
// for the app's own process, as Android 15's MediaProvider answers an app that saves a picture: insert creates the file
// under DCIM/ or Pictures/ in the app's shared storage (/storage/emulated/0), named as FileUtils.splitFileName and
// buildUniqueFile name it, and returns the item's Uri; the app writes it through openAssetFile/openFile; an item is
// published when its IS_PENDING is cleared, or, inserted without IS_PENDING, when a writable descriptor of it closes.
// Publishing asks the supervisor (`publish_media`) to copy the finished file into the host's Pictures folder
// (~/Pictures/Bifrost/<app>/); the guest keeps its copy. Rows live as long as the process: query, update, delete and
// getType answer for items it inserted.
//
// Everything else (other collections, collection queries, selections, extras, unknown columns, renames, call, batch
// operations) is answered exactly as ContentResolver answers when no "media" provider exists, which is what every app
// saw before this provider: ad SDKs ask for other collections and survive a missing provider, so a strict refusal here
// would end them. Those requests log SERVICE_CALL_UNSUPPORTED media <call>.
import android.content.AttributionSource;
import android.content.ClipDescription;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.res.AssetFileDescriptor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.provider.MediaStore;
import android.webkit.MimeTypeMap;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.json.JSONObject;

final class FrameworkMediaStore implements FrameworkServices.Call {
    private enum Contract {
        insert, update, delete, query, getType, getTypeAsync, getTypeAnonymousAsync, canonicalizeAsync,
        uncanonicalizeAsync, openFile, openAssetFile, openTypedAssetFile, createCancellationSignal
    }

    static final String AUTHORITY = "media";
    private static final File STORAGE = new File("/storage/emulated/0");
    // MediaProvider's allowed primary directories for images; the first is where an insert without RELATIVE_PATH goes.
    private static final List<String> IMAGE_DIRECTORIES = Arrays.asList("Pictures", "DCIM");
    private static final Set<String> VOLUMES = new HashSet<>(Arrays.asList(MediaStore.VOLUME_EXTERNAL,
            MediaStore.VOLUME_EXTERNAL_PRIMARY));
    // Provider-owned columns: an app's value is replaced, as MediaProvider recomputes them.
    private static final Set<String> OWNED = new HashSet<>(Arrays.asList(MediaStore.MediaColumns._ID,
            MediaStore.MediaColumns.SIZE, MediaStore.MediaColumns.DATE_ADDED, MediaStore.MediaColumns.DATE_MODIFIED,
            MediaStore.MediaColumns.VOLUME_NAME, MediaStore.MediaColumns.OWNER_PACKAGE_NAME));
    // Descriptive image columns an app may set; they are kept and returned, and change nothing on disk.
    private static final Set<String> METADATA = metadataColumns();

    private final Map<Long, Item> items = new HashMap<>();
    private long nextId = 1;

    private static final class Item {
        final long id;
        final String volume, displayName, mimeType, relativePath, owner;
        final File file;
        final long added = System.currentTimeMillis() / 1000;
        final Map<String, Object> metadata = new HashMap<>();
        boolean pending, published;
        Long expires;
        Item(long id, String volume, File file, String displayName, String mimeType, String relativePath,
                String owner) {
            this.id = id; this.volume = volume; this.file = file; this.displayName = displayName;
            this.mimeType = mimeType; this.relativePath = relativePath; this.owner = owner;
        }
    }

    // A request this provider does not model; invoke answers it as a missing provider would.
    private static final class Unmodelled extends RuntimeException {
        Unmodelled(String detail) { super(detail, null, false, false); }
    }

    @Override public String[] methods() { return names(Contract.values()); }

    @Override public Object invoke(String method, Object[] a) throws Exception {
        Object[] arguments = a == null ? new Object[0] : a;
        Uri found = null;
        for (Object argument : arguments) if (argument instanceof Uri) { found = (Uri) argument; break; }
        final Uri uri = found;
        Contract call = null;
        try { call = Contract.valueOf(method); } catch (IllegalArgumentException other) { }
        try {
            if (call == null) throw new Unmodelled(method);
            if (call == Contract.createCancellationSignal)
                return Class.forName("android.os.CancellationSignal").getMethod("createTransport").invoke(null);
            if (call == Contract.getTypeAnonymousAsync) return reply(arguments[1], true, () -> type(uri));
            // The deprecated overloads without an AttributionSource come here too; ContentResolver does not use them.
            if (!(arguments[0] instanceof AttributionSource) || uri == null || !isImages(uri))
                throw new Unmodelled(method + (uri == null ? "" : " " + uri.getPath()));
            AttributionSource caller = (AttributionSource) arguments[0];
            if (caller.getUid() != Process.myUid()) throw new SecurityException("Foreign media caller");
            switch (call) {
                case insert: return insert(uri, (ContentValues) arguments[2], (Bundle) arguments[3],
                        caller.getPackageName());
                case update: return update(uri, (ContentValues) arguments[2], (Bundle) arguments[3]);
                case delete: return delete(uri, (Bundle) arguments[2]);
                case query: return query(uri, (String[]) arguments[2], (Bundle) arguments[3]);
                case getType: return type(uri);
                case getTypeAsync: return reply(arguments[2], true, () -> type(uri));
                // MediaProvider has canonical forms; without one, ContentResolver.canonicalize answers null.
                case canonicalizeAsync: case uncanonicalizeAsync:
                    return reply(arguments[2], false, () -> null);
                case openFile: return open(uri, (String) arguments[2]);
                case openAssetFile:
                    return new AssetFileDescriptor(open(uri, (String) arguments[2]), 0, AssetFileDescriptor.UNKNOWN_LENGTH);
                case openTypedAssetFile: {
                    Item item;
                    synchronized (this) { item = item(uri, "openTypedAssetFile"); }
                    if (!ClipDescription.compareMimeTypes(item.mimeType, (String) arguments[2]))
                        throw new FileNotFoundException("Can't open " + uri + " as type " + arguments[2]);
                    return new AssetFileDescriptor(open(uri, "r"), 0, AssetFileDescriptor.UNKNOWN_LENGTH);
                }
                default: throw new Unmodelled(method);
            }
        } catch (Unmodelled unmodelled) {
            return absent(method, uri, arguments, unmodelled.getMessage());
        }
    }

    // ContentResolver's answer when acquireProvider finds no provider for the authority (Android 15).
    private static Object absent(String method, Uri uri, Object[] arguments, String detail) throws Exception {
        FrameworkServices.unsupported(AUTHORITY, detail);
        switch (method) {
            case "query": case "getType": case "canonicalize": case "uncanonicalize": case "getStreamTypes":
                return null;
            case "getTypeAsync": case "getTypeAnonymousAsync": case "canonicalizeAsync": case "uncanonicalizeAsync":
                return reply(arguments[arguments.length - 1], method.startsWith("getType"), () -> null);
            case "refresh": return false;
            case "openFile": case "openAssetFile": case "openTypedAssetFile":
                throw new FileNotFoundException("No content provider: " + uri);
            case "call": case "applyBatch": throw new IllegalArgumentException("Unknown authority " + AUTHORITY);
            default: throw new IllegalArgumentException("Unknown URL " + uri);
        }
    }

    private interface Answer { Object get() throws Exception; }

    // ContentProvider.Transport's async replies: the result, or the exception, in one Bundle. RemoteCallback and
    // ParcelableException are hidden, so they are reached by reflection.
    private static Object reply(Object callback, boolean string, Answer answer) throws Exception {
        Bundle result = new Bundle();
        try {
            Object value = answer.get();
            if (string) result.putString(callbackKey("REMOTE_CALLBACK_RESULT"), (String) value);
            else result.putParcelable(callbackKey("REMOTE_CALLBACK_RESULT"), (Uri) value);
        } catch (Unmodelled unmodelled) {
            throw unmodelled;
        } catch (Exception error) {
            result.putParcelable(callbackKey("REMOTE_CALLBACK_ERROR"), (android.os.Parcelable) Class
                    .forName("android.os.ParcelableException").getConstructor(Throwable.class).newInstance(error));
        }
        Class.forName("android.os.RemoteCallback").getMethod("sendResult", Bundle.class).invoke(callback, result);
        return null;
    }

    private static String callbackKey(String name) throws ReflectiveOperationException {
        return (String) ContentResolver.class.getField(name).get(null);
    }

    private synchronized Uri insert(Uri uri, ContentValues values, Bundle extras, String owner) throws IOException {
        if (!isCollection(uri)) throw new Unmodelled("insert " + uri.getPath());
        if (extras != null && !extras.isEmpty()) throw new Unmodelled("insert extras");
        ContentValues own = values == null ? new ContentValues() : values;
        Map<String, Object> metadata = new HashMap<>();
        for (String key : own.keySet()) {
            if (METADATA.contains(key)) metadata.put(key, own.get(key));
            else if (!OWNED.contains(key) && !isInsertColumn(key)) throw new Unmodelled("insert column " + key);
        }
        String displayName = own.getAsString(MediaStore.MediaColumns.DISPLAY_NAME);
        if (displayName == null || displayName.isEmpty()) displayName = String.valueOf(System.currentTimeMillis());
        displayName = displayName.replace('/', '_').replace('\0', '_');
        String mimeType = own.getAsString(MediaStore.MediaColumns.MIME_TYPE);
        if (mimeType == null) mimeType = mimeFromName(displayName);
        if (mimeType == null || !mimeType.startsWith("image/"))
            throw new IllegalArgumentException("MIME type " + mimeType + " cannot be inserted into " + uri
                    + "; expected MIME type under image/*");
        String relativePath = relativePath(own.getAsString(MediaStore.MediaColumns.RELATIVE_PATH), uri);
        File directory = new File(STORAGE, relativePath);
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IllegalStateException("Failed to create directory " + directory);
        File file = uniqueFile(directory, displayName, mimeType);
        String volume = uri.getPathSegments().get(0);
        Item item = new Item(nextId++, volume, file, file.getName(), mimeType, relativePath, owner);
        Integer pending = own.getAsInteger(MediaStore.MediaColumns.IS_PENDING);
        item.pending = pending != null && pending != 0;
        item.expires = own.getAsLong(MediaStore.MediaColumns.DATE_EXPIRES);
        item.metadata.putAll(metadata);
        items.put(item.id, item);
        return itemUri(volume, item.id);
    }

    private int update(Uri uri, ContentValues values, Bundle extras) {
        Item item;
        boolean publish = false;
        synchronized (this) {
            item = item(uri, "update");
            if (extras != null && !extras.isEmpty()) throw new Unmodelled("update extras");
            for (String key : values.keySet())
                if (!key.equals(MediaStore.MediaColumns.IS_PENDING) && !key.equals(MediaStore.MediaColumns.DATE_EXPIRES)
                        && !METADATA.contains(key) && !OWNED.contains(key))
                    throw new Unmodelled("update column " + key);
            for (String key : values.keySet()) {
                if (key.equals(MediaStore.MediaColumns.IS_PENDING)) {
                    Integer pending = values.getAsInteger(key);
                    boolean now = pending != null && pending != 0;
                    publish = item.pending && !now;
                    item.pending = now;
                } else if (key.equals(MediaStore.MediaColumns.DATE_EXPIRES)) {
                    item.expires = values.getAsLong(key);
                } else if (METADATA.contains(key)) {
                    item.metadata.put(key, values.get(key));
                }
            }
        }
        if (publish) publish(item);
        return 1;
    }

    private synchronized int delete(Uri uri, Bundle extras) {
        Item item = item(uri, "delete");
        if (extras != null && !extras.isEmpty()) throw new Unmodelled("delete extras");
        items.remove(item.id);
        item.file.delete();
        return 1;
    }

    private MatrixCursor query(Uri uri, String[] projection, Bundle queryArgs) {
        Item item;
        synchronized (this) {
            item = item(uri, "query");
            if (queryArgs != null && !queryArgs.isEmpty()) throw new Unmodelled("query arguments");
        }
        String[] columns = projection != null ? projection : new String[] {
                MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME, MediaStore.MediaColumns.MIME_TYPE,
                MediaStore.MediaColumns.RELATIVE_PATH, MediaStore.MediaColumns.IS_PENDING,
                MediaStore.MediaColumns.SIZE, MediaStore.MediaColumns.DATE_ADDED,
                MediaStore.MediaColumns.DATE_MODIFIED, MediaStore.MediaColumns.VOLUME_NAME };
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        Object[] row = new Object[columns.length];
        synchronized (this) {
            for (int i = 0; i < columns.length; i++) row[i] = column(item, columns[i]);
        }
        cursor.addRow(row);
        return cursor;
    }

    private Object column(Item item, String name) {
        switch (name) {
            case MediaStore.MediaColumns._ID: return item.id;
            case MediaStore.MediaColumns.DISPLAY_NAME: return item.displayName;
            case MediaStore.MediaColumns.MIME_TYPE: return item.mimeType;
            case MediaStore.MediaColumns.RELATIVE_PATH: return item.relativePath;
            case MediaStore.MediaColumns.IS_PENDING: return item.pending ? 1 : 0;
            case MediaStore.MediaColumns.DATE_EXPIRES: return item.expires;
            case MediaStore.MediaColumns.SIZE: return item.file.length();
            case MediaStore.MediaColumns.DATE_ADDED: return item.added;
            case MediaStore.MediaColumns.DATE_MODIFIED: return item.file.lastModified() / 1000;
            case MediaStore.MediaColumns.VOLUME_NAME: return MediaStore.VOLUME_EXTERNAL_PRIMARY;
            case MediaStore.MediaColumns.OWNER_PACKAGE_NAME: return item.owner;
            case MediaStore.MediaColumns.DATA: return item.file.getPath();
        }
        if (METADATA.contains(name)) return item.metadata.get(name);
        throw new IllegalArgumentException("Invalid column " + name);
    }

    private synchronized String type(Uri uri) {
        if (uri == null || !isImages(uri)) throw new Unmodelled("getType " + (uri == null ? "" : uri.getPath()));
        if (isCollection(uri)) return "vnd.android.cursor.dir/image";
        Item item = find(uri);
        return item == null ? null : item.mimeType;
    }

    private ParcelFileDescriptor open(Uri uri, String mode) throws FileNotFoundException {
        Item item;
        synchronized (this) { item = item(uri, "open"); }
        int flags = ParcelFileDescriptor.parseMode(mode);
        ParcelFileDescriptor descriptor = ParcelFileDescriptor.open(item.file, flags);
        // MODE_READ_WRITE includes the write bit.
        if ((flags & ParcelFileDescriptor.MODE_WRITE_ONLY) == 0) return descriptor;
        // The app's process holds this very object (ContentResolver wraps it, and a wrapper's close closes what it
        // wraps), so its close runs here, on the app's thread, before the app's own close returns.
        return new ParcelFileDescriptor(descriptor) {
            @Override public void close() throws IOException {
                try { super.close(); } finally { closed(item); }
            }
        };
    }

    // A writable descriptor closed: an item inserted without IS_PENDING is finished now.
    private void closed(Item item) {
        synchronized (this) {
            if (item.pending || item.published || !items.containsKey(item.id)) return;
        }
        publish(item);
    }

    private void publish(Item item) {
        synchronized (this) {
            if (item.published || item.file.length() == 0) return;
            item.published = true;
        }
        try {
            JSONObject response = FrameworkProcessRuntime.host(new JSONObject().put("operation", "publish_media")
                    .put("path", item.file.getPath()));
            System.out.println("APP_MEDIA_PUBLISHED " + response.optString("name"));
        } catch (Exception error) {
            // The item stays in the app's storage; only the host copy failed.
            System.out.println("APP_MEDIA_PUBLISH_FAILED " + item.displayName + ": " + error.getMessage());
        }
    }

    // RELATIVE_PATH as MediaProvider accepts it for images: a primary directory of DCIM or Pictures, then plain
    // segments, with a trailing slash; null places the item in Pictures/.
    private static String relativePath(String requested, Uri uri) {
        if (requested == null || requested.isEmpty()) return IMAGE_DIRECTORIES.get(0) + "/";
        String path = requested.endsWith("/") ? requested : requested + "/";
        String[] segments = path.substring(0, path.length() - 1).split("/", -1);
        for (String segment : segments)
            if (segment.isEmpty() || segment.equals(".") || segment.equals("..") || segment.indexOf('\0') >= 0)
                throw new IllegalArgumentException("Invalid path " + requested);
        if (!IMAGE_DIRECTORIES.contains(segments[0]))
            throw new IllegalArgumentException("Primary directory " + segments[0] + " not allowed for " + uri
                    + "; allowed directories are [DCIM, Pictures]");
        return path;
    }

    // FileUtils.splitFileName keeps the display name's extension only if it maps to the MIME type, else appends the
    // type's own; buildUniqueFile then tries "name (1).ext" .. "name (31).ext".
    private static File uniqueFile(File directory, String displayName, String mimeType) throws IOException {
        String extension = MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType);
        String base = displayName, suffix = "";
        int dot = displayName.lastIndexOf('.');
        String own = dot > 0 ? displayName.substring(dot + 1) : null;
        if (own != null && mimeType.equals(MimeTypeMap.getSingleton().getMimeTypeFromExtension(own.toLowerCase()))) {
            base = displayName.substring(0, dot); suffix = "." + own;
        } else if (extension != null) {
            suffix = "." + extension;
        }
        for (int i = 0; i < 32; i++) {
            File candidate = new File(directory, i == 0 ? base + suffix : base + " (" + i + ")" + suffix);
            if (candidate.createNewFile()) return candidate;
        }
        throw new FileNotFoundException("Failed to create unique file");
    }

    private static String mimeFromName(String name) {
        int dot = name.lastIndexOf('.');
        return dot < 0 ? null : MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase());
    }

    private static boolean isInsertColumn(String key) {
        return key.equals(MediaStore.MediaColumns.DISPLAY_NAME) || key.equals(MediaStore.MediaColumns.MIME_TYPE)
                || key.equals(MediaStore.MediaColumns.RELATIVE_PATH) || key.equals(MediaStore.MediaColumns.IS_PENDING)
                || key.equals(MediaStore.MediaColumns.DATE_EXPIRES);
    }

    // content://media/<external|external_primary>/images/media[/<id>]
    private static boolean isImages(Uri uri) {
        List<String> path = uri.getPathSegments();
        return AUTHORITY.equals(uri.getAuthority()) && (path.size() == 3 || path.size() == 4)
                && VOLUMES.contains(path.get(0)) && path.get(1).equals("images") && path.get(2).equals("media");
    }

    private static boolean isCollection(Uri uri) {
        return isImages(uri) && uri.getPathSegments().size() == 3;
    }

    private Item find(Uri uri) {
        if (!isImages(uri) || isCollection(uri)) return null;
        try {
            return items.get(Long.parseLong(uri.getPathSegments().get(3)));
        } catch (NumberFormatException error) {
            return null;
        }
    }

    // An item this process inserted. Another id may be another app's item or one from an earlier process, which
    // this provider does not know: it is answered as a missing provider would answer it.
    private Item item(Uri uri, String call) {
        Item item = find(uri);
        if (item == null) throw new Unmodelled(call + " " + uri.getPath());
        return item;
    }

    private static Uri itemUri(String volume, long id) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY)
                .appendPath(volume).appendPath("images").appendPath("media").appendPath(String.valueOf(id)).build();
    }

    // The String constants of MediaColumns and ImageColumns that describe an image, minus the ones handled above.
    private static Set<String> metadataColumns() {
        Set<String> columns = new HashSet<>();
        for (Class<?> type : new Class<?>[] { MediaStore.MediaColumns.class, MediaStore.Images.ImageColumns.class })
            for (Field field : type.getFields())
                if (field.getType() == String.class && Modifier.isStatic(field.getModifiers()))
                    try { columns.add((String) field.get(null)); } catch (IllegalAccessException ignored) { }
        columns.removeAll(OWNED);
        columns.removeAll(Arrays.asList(MediaStore.MediaColumns.DISPLAY_NAME, MediaStore.MediaColumns.MIME_TYPE,
                MediaStore.MediaColumns.RELATIVE_PATH, MediaStore.MediaColumns.IS_PENDING,
                MediaStore.MediaColumns.DATE_EXPIRES, MediaStore.MediaColumns.DATA));
        return columns;
    }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }
}
