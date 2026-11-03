// Android Keystore for one device without secure hardware: the keystore2
// service ("android.system.keystore2.IKeystoreService/default") with a
// software KeyMint behind it, so the framework's own AndroidKeyStore provider
// (android.security.keystore2) generates, lists, loads and uses keys.
// Zygote normally installs that provider (ZygoteInit.warmUpJcaProviders);
// apps start here without Zygote, so install() does it too.
//
// Keys: AES (ECB, CBC, CTR, GCM; no padding or PKCS#7) and HMAC (MD5, SHA-1,
// SHA-2), generated or imported as raw material. Parameter checks and error
// codes follow the reference KeyMint TA (system/keymint, android15-release:
// common/src/tag.rs check_*_params, check_begin_params; crypto/aes.rs). RSA,
// EC and 3DES keys, attestation, grants and user-authentication-bound keys are
// not implemented and fail with KeyMint's own error codes.
//
// Key material is kept in software, in one file per uid under
// /data/misc/keystore (where keystore2 keeps its database), written atomically
// under a cross-process lock: every guest process runs its own instance.
// Authorizations carry SecurityLevel.SOFTWARE, so KeyInfo reports the keys as
// outside secure hardware.
import android.util.AtomicFile;
import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.lang.reflect.Array;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import javax.crypto.AEADBadTagException;
import javax.crypto.BadPaddingException;
import javax.crypto.Cipher;
import javax.crypto.IllegalBlockSizeException;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import org.json.JSONArray;
import org.json.JSONObject;

final class FrameworkKeystore {
    static final String SERVICE = "android.system.keystore2.IKeystoreService/default";
    static final String DIRECTORY = "/data/misc/keystore";
    private static final String PACKAGE = "android.system.keystore2.";
    private static final String KEYMINT = "android.hardware.security.keymint.";
    // Conscrypt, named so a cipher lookup never resolves to the AndroidKeyStore provider itself.
    private static final String ENGINE = "AndroidOpenSSL";
    private static final int VERSION = 1, MAX_BYTES = 4 * 1024 * 1024;
    // keystore2 operation.rs MAX_RECEIVE_DATA.
    private static final int MAX_RECEIVE_DATA = 0x8000;

    // Domain, ResponseCode (keystore2) and ErrorCode, SecurityLevel (KeyMint) values.
    private static final int DOMAIN_APP = 0, DOMAIN_GRANT = 1, DOMAIN_SELINUX = 2, DOMAIN_BLOB = 3, DOMAIN_KEY_ID = 4;
    private static final int SYSTEM_ERROR = 4, PERMISSION_DENIED = 6, KEY_NOT_FOUND = 7, INVALID_ARGUMENT = 20,
            TOO_MUCH_DATA = 21;
    private static final int UNSUPPORTED_PURPOSE = -2, INCOMPATIBLE_PURPOSE = -3, UNSUPPORTED_ALGORITHM = -4,
            UNSUPPORTED_KEY_SIZE = -6, UNSUPPORTED_BLOCK_MODE = -7, INCOMPATIBLE_BLOCK_MODE = -8,
            UNSUPPORTED_MAC_LENGTH = -9, UNSUPPORTED_PADDING_MODE = -10, INCOMPATIBLE_PADDING_MODE = -11,
            UNSUPPORTED_DIGEST = -12, INCOMPATIBLE_DIGEST = -13, UNSUPPORTED_KEY_FORMAT = -17,
            INVALID_INPUT_LENGTH = -21, KEY_NOT_YET_VALID = -24, KEY_EXPIRED = -25,
            KEY_USER_NOT_AUTHENTICATED = -26, INVALID_OPERATION_HANDLE = -28, VERIFICATION_FAILED = -30,
            INVALID_KEY_BLOB = -33, KM_INVALID_ARGUMENT = -38, INVALID_TAG = -40,
            IMPORT_PARAMETER_MISMATCH = -44, INVALID_NONCE = -52, MISSING_MAC_LENGTH = -53,
            CALLER_NONCE_PROHIBITED = -55, INVALID_MAC_LENGTH = -57, MISSING_MIN_MAC_LENGTH = -58,
            UNSUPPORTED_MIN_MAC_LENGTH = -59, HARDWARE_TYPE_UNAVAILABLE = -68, STORAGE_KEY_UNSUPPORTED = -77;
    private static final int LEVEL_SOFTWARE = 0, LEVEL_TRUSTED_ENVIRONMENT = 1, LEVEL_KEYSTORE = 100;

    // KeyMint Tag values (tag type in the top nibble).
    private static final int ALGORITHM = 0x10000002, BLOCK_MODE = 0x20000004, PADDING = 0x20000006,
            DIGEST = 0x20000005, PURPOSE = 0x20000001, KEY_SIZE = 0x30000003, MIN_MAC_LENGTH = 0x30000008,
            MAC_LENGTH = 0x300003EB, NONCE = 0x900003E9, CALLER_NONCE = 0x70000007,
            NO_AUTH_REQUIRED = 0x700001F7, USER_SECURE_ID = 0xA00001F6, USER_AUTH_TYPE = 0x100001F8,
            ORIGIN = 0x100002BE, CREATION_DATETIME = 0x600002BD, ACTIVE_DATETIME = 0x60000190,
            ORIGINATION_EXPIRE_DATETIME = 0x60000191, USAGE_EXPIRE_DATETIME = 0x60000192,
            USAGE_COUNT_LIMIT = 0x30000195, APPLICATION_ID = 0x90000259, APPLICATION_DATA = 0x900002BC,
            STORAGE_KEY = 0x700002D2, RSA_OAEP_MGF_DIGEST = 0x200000CB;
    // tag/info.rs: KeyMint-enforced and Keystore-enforced characteristics, and the tags
    // KeyMint adds itself (rejected as input).
    private static final int[] KEYMINT_ENFORCED = {ALGORITHM, 0x300001F9 /* AUTH_TIMEOUT */, BLOCK_MODE,
            0x7000012E /* BOOTLOADER_ONLY */, 0x300002CF /* BOOT_PATCHLEVEL */, CALLER_NONCE, DIGEST,
            0x70000131 /* EARLY_BOOT_ONLY */, 0x1000000A /* EC_CURVE */, KEY_SIZE,
            0x30000194 /* MAX_USES_PER_BOOT */, MIN_MAC_LENGTH, NO_AUTH_REQUIRED, ORIGIN,
            0x300002C2 /* OS_PATCHLEVEL */, 0x300002C1 /* OS_VERSION */, PADDING, PURPOSE,
            0x7000012F /* ROLLBACK_RESISTANCE */, RSA_OAEP_MGF_DIGEST, 0x500000C8 /* RSA_PUBLIC_EXPONENT */,
            STORAGE_KEY, 0x700001FC /* TRUSTED_CONFIRMATION_REQUIRED */,
            0x700001FB /* TRUSTED_USER_PRESENCE_REQUIRED */, USER_AUTH_TYPE, USER_SECURE_ID,
            0x300002CE /* VENDOR_PATCHLEVEL */};
    private static final int[] KEYSTORE_ENFORCED = {ACTIVE_DATETIME, 0x700001FA /* ALLOW_WHILE_ON_BODY */,
            CREATION_DATETIME, 0x300003F2 /* MAX_BOOT_LEVEL */, ORIGINATION_EXPIRE_DATETIME,
            0x700001FD /* UNLOCKED_DEVICE_REQUIRED */, USAGE_EXPIRE_DATETIME, 0x300001F5 /* USER_ID */,
            USAGE_COUNT_LIMIT};
    private static final int[] AUTO_ADDED = {ORIGIN, 0x300002C1, 0x300002C2, 0x300002CE, 0x300002CF};
    private static final int[] HIDDEN = {APPLICATION_ID, APPLICATION_DATA};

    private static final int AES = 32, HMAC = 128;
    private static final int ENCRYPT = 0, DECRYPT = 1, SIGN = 2, VERIFY = 3;
    private static final int ECB = 1, CBC = 2, CTR = 3, GCM = 32;
    private static final int PAD_NONE = 1, PAD_PKCS7 = 64;
    private static final int DIGEST_NONE = 0;
    private static final int ORIGIN_GENERATED = 0, ORIGIN_IMPORTED = 2;

    // KeyParameterValue union members, indexed by its tag (KeyParameterValue.Tag).
    private static final String[] VALUE_KINDS = {"invalid", "algorithm", "blockMode", "paddingMode", "digest",
            "ecCurve", "origin", "keyPurpose", "hardwareAuthenticatorType", "securityLevel", "boolValue",
            "integer", "longInteger", "dateTime", "blob"};
    private static final int KIND_BOOL = 10, KIND_LONG = 12, KIND_DATE = 13, KIND_BLOB = 14;

    private enum Service { getSecurityLevel, getKeyEntry, deleteKey, listEntries, listEntriesBatched,
        getNumberOfEntries, getInterfaceVersion, getInterfaceHash }
    private enum Level { generateKey, importKey, createOperation, getInterfaceVersion, getInterfaceHash }
    private enum Operation { update, updateAad, finish, abort, getInterfaceVersion, getInterfaceHash }

    // FileChannel.lock does not wait for overlapping locks held by another channel in this JVM.
    private static final Object STORE_LOCK = new Object();
    private final SecureRandom random = new SecureRandom();
    private FrameworkServices services;
    private Object securityLevel;

    // One KeyMint parameter, independent of the framework's hidden classes.
    static final class Param {
        final int tag, kind;
        final Object value;  // Integer, Boolean, Long or byte[] by kind
        Param(int tag, int kind, Object value) { this.tag = tag; this.kind = kind; this.value = value; }
        int integer() { return ((Number) value).intValue(); }
        long longValue() { return ((Number) value).longValue(); }
    }

    static final class Key {
        long id;
        String alias;
        byte[] material;
        long modified;
        final List<Param> keymint = new ArrayList<>(), keystore = new ArrayList<>(), hidden = new ArrayList<>();
        List<Param> characteristics() {
            List<Param> all = new ArrayList<>(keymint);
            all.addAll(keystore);
            return all;
        }
    }

    void install(FrameworkServices services) throws Exception {
        this.services = services;
        // ZygoteInit.warmUpJcaProviders: AndroidKeyStoreProvider.install().
        if (java.security.Security.getProvider("AndroidKeyStore") == null)
            Class.forName("android.security.keystore2.AndroidKeyStoreProvider").getMethod("install").invoke(null);
        securityLevel = services.local("keystore2.security_level", PACKAGE + "IKeystoreSecurityLevel",
                new FrameworkServices.Call() {
                    @Override public String[] methods() { return names(Level.values()); }
                    @Override public Object invoke(String method, Object[] a) throws Exception { return guard(() -> level(method, a)); }
                });
        services.register(SERVICE, PACKAGE + "IKeystoreService", new FrameworkServices.Call() {
            @Override public String[] methods() { return names(Service.values()); }
            @Override public Object invoke(String method, Object[] a) throws Exception { return guard(() -> service(method, a)); }
        });
    }

    private static String[] names(Enum<?>[] values) {
        String[] names = new String[values.length];
        for (int i = 0; i < values.length; i++) names[i] = values[i].name();
        return names;
    }

    interface Body { Object run() throws Exception; }

    // keystore2 answers internal failures with SYSTEM_ERROR; anything else would reach the
    // app as an exception the framework's KeyStore2 does not translate.
    private static Object guard(Body body) {
        try {
            return body.run();
        } catch (Exception failure) {
            if (failure.getClass() == SERVICE_SPECIFIC) throw (RuntimeException) failure;
            System.err.println("KEYSTORE_FAILURE " + failure);
            failure.printStackTrace();
            throw error(SYSTEM_ERROR, String.valueOf(failure));
        }
    }

    // android.os.ServiceSpecificException, hidden from the SDK this compiles against: what
    // keystore2's Binder errors become, and what KeyStore2 turns into KeyStoreException.
    private static final Class<?> SERVICE_SPECIFIC;
    private static final java.lang.reflect.Constructor<?> SERVICE_SPECIFIC_NEW;
    static {
        try {
            SERVICE_SPECIFIC = Class.forName("android.os.ServiceSpecificException");
            SERVICE_SPECIFIC_NEW = SERVICE_SPECIFIC.getConstructor(int.class, String.class);
        } catch (ReflectiveOperationException missing) {
            throw new ExceptionInInitializerError(missing);
        }
    }

    private static RuntimeException error(int code, String message) {
        try {
            return (RuntimeException) SERVICE_SPECIFIC_NEW.newInstance(code, message);
        } catch (ReflectiveOperationException failure) {
            throw new IllegalStateException(failure);
        }
    }

    // A call outside the contract: logged once, and a keystore error rather than an
    // exception type the framework's KeyStore2 does not translate.
    private static RuntimeException unsupported(String what) {
        FrameworkServices.unsupported("keystore2", what);
        return error(SYSTEM_ERROR, what + " is not implemented");
    }

    private static int version() throws Exception {
        return Class.forName(PACKAGE + "IKeystoreService").getField("VERSION").getInt(null);
    }

    private static String hash() throws Exception {
        return (String) Class.forName(PACKAGE + "IKeystoreService").getField("HASH").get(null);
    }

    private Object service(String method, Object[] a) throws Exception {
        Service contract;
        try { contract = Service.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw unsupported("IKeystoreService." + method); }
        int uid = android.os.Process.myUid();
        switch (contract) {
            case getSecurityLevel: {
                int level = (Integer) a[0];
                // TRUSTED_ENVIRONMENT is what the provider asks for; there is no StrongBox.
                if (level != LEVEL_TRUSTED_ENVIRONMENT)
                    throw error(HARDWARE_TYPE_UNAVAILABLE, "No KeyMint at security level " + level);
                return securityLevel;
            }
            case getKeyEntry: {
                Key key = find(uid, a[0]);
                Object response = Class.forName(PACKAGE + "KeyEntryResponse").newInstance();
                set(response, "iSecurityLevel", securityLevel);
                set(response, "metadata", metadata(key));
                return response;
            }
            case deleteKey: {
                Object descriptor = a[0];
                final Key key = find(uid, descriptor);
                store(uid).update(keys -> { keys.removeIf(k -> k.id == key.id); return null; });
                return null;
            }
            case listEntries:
            case listEntriesBatched: {
                int domain = (Integer) a[0];
                if (domain != DOMAIN_APP) throw error(PERMISSION_DENIED, "Only the app domain is listed");
                String after = contract == Service.listEntriesBatched ? (String) a[2] : null;
                List<String> aliases = new ArrayList<>();
                for (Key key : store(uid).read()) if (after == null || key.alias.compareTo(after) > 0) aliases.add(key.alias);
                Collections.sort(aliases);
                Class<?> type = Class.forName(PACKAGE + "KeyDescriptor");
                Object result = Array.newInstance(type, aliases.size());
                for (int i = 0; i < aliases.size(); i++)
                    Array.set(result, i, descriptor(DOMAIN_APP, uid, aliases.get(i)));
                return result;
            }
            case getNumberOfEntries:
                if ((Integer) a[0] != DOMAIN_APP) throw error(PERMISSION_DENIED, "Only the app domain is counted");
                return store(uid).read().size();
            case getInterfaceVersion: return version();
            case getInterfaceHash: return hash();
            default: throw unsupported("IKeystoreService." + method);
        }
    }

    private Object level(String method, Object[] a) throws Exception {
        Level contract;
        try { contract = Level.valueOf(method); }
        catch (IllegalArgumentException unknown) { throw unsupported("IKeystoreSecurityLevel." + method); }
        int uid = android.os.Process.myUid();
        switch (contract) {
            case generateKey: {
                if (a[4] != null) random.setSeed((byte[]) a[4]);  // addRngEntropy
                return create(uid, a[0], params(a[2]), null);
            }
            case importKey: {
                if (a[4] == null) throw error(INVALID_ARGUMENT, "No key material");
                return create(uid, a[0], params(a[2]), (byte[]) a[4]);
            }
            case createOperation: return begin(uid, a[0], params(a[1]));
            case getInterfaceVersion: return version();
            case getInterfaceHash: return hash();
            default: throw unsupported("IKeystoreSecurityLevel." + method);
        }
    }

    // ---- Key creation (generateKey/importKey) ----

    private Object create(int uid, Object descriptor, List<Param> params, byte[] imported) throws Exception {
        int domain = getInt(descriptor, "domain");
        String alias = (String) get(descriptor, "alias");
        if (domain == DOMAIN_SELINUX || domain == DOMAIN_BLOB)
            throw error(PERMISSION_DENIED, "An app may only create keys in its own domain");
        if (domain != DOMAIN_APP || alias == null) throw error(INVALID_ARGUMENT, "Keys need an app alias");
        for (Param p : params)
            if (contains(AUTO_ADDED, p.tag)) throw error(INVALID_TAG, "KeyMint-added tag in key parameters");
        int algorithm = single(params, ALGORITHM, UNSUPPORTED_ALGORITHM).integer();
        List<Param> deduced = new ArrayList<>();
        byte[] material;
        if (algorithm == AES) {
            int bits;
            if (imported == null) {
                bits = single(params, KEY_SIZE, UNSUPPORTED_KEY_SIZE).integer();
                if (bits != 128 && bits != 192 && bits != 256)
                    throw error(UNSUPPORTED_KEY_SIZE, "AES keys are 128, 192 or 256 bits");
            } else {
                bits = imported.length * 8;
                if (bits != 128 && bits != 192 && bits != 256)
                    throw error(UNSUPPORTED_KEY_SIZE, "AES keys must be 16, 24 or 32 bytes");
                deduced.addAll(matchingSize(params, bits));
            }
            if (has(params, BLOCK_MODE, GCM)) {
                int min = single(params, MIN_MAC_LENGTH, MISSING_MIN_MAC_LENGTH).integer();
                if (min % 8 != 0 || min < 96 || min > 128)
                    throw error(UNSUPPORTED_MIN_MAC_LENGTH, "Unsupported MIN_MAC_LENGTH " + min);
            }
            material = imported != null ? imported.clone() : bytes(bits / 8);
        } else if (algorithm == HMAC) {
            int bits = imported == null ? single(params, KEY_SIZE, UNSUPPORTED_KEY_SIZE).integer() : imported.length * 8;
            if (imported != null) deduced.addAll(matchingSize(params, bits));
            if (bits % 8 != 0 || bits < 64 || bits > 1024)
                throw error(UNSUPPORTED_KEY_SIZE, "Unsupported HMAC KEY_SIZE " + bits);
            int digest = single(params, DIGEST, UNSUPPORTED_DIGEST).integer();
            if (digest == DIGEST_NONE) throw error(UNSUPPORTED_DIGEST, "HMAC needs a digest");
            int min = single(params, MIN_MAC_LENGTH, MISSING_MIN_MAC_LENGTH).integer();
            if (min % 8 != 0 || min < 64 || min > 512)
                throw error(UNSUPPORTED_MIN_MAC_LENGTH, "Unsupported MIN_MAC_LENGTH " + min);
            material = imported != null ? imported.clone() : bytes(bits / 8);
        } else {
            FrameworkServices.unsupported("keystore2", "algorithm " + algorithm);
            throw error(UNSUPPORTED_ALGORITHM, "Only AES and HMAC keys are implemented");
        }
        Key key = new Key();
        key.alias = alias;
        key.material = material;
        key.modified = System.currentTimeMillis();
        List<Param> all = new ArrayList<>(params);
        all.addAll(deduced);
        all.add(new Param(ORIGIN, 6, imported != null ? ORIGIN_IMPORTED : ORIGIN_GENERATED));
        // keystore2 records the creation time when the caller has not.
        if (!present(all, CREATION_DATETIME)) all.add(new Param(CREATION_DATETIME, KIND_DATE, key.modified));
        for (Param p : all) {
            if (contains(KEYMINT_ENFORCED, p.tag)) key.keymint.add(p);
            else if (contains(KEYSTORE_ENFORCED, p.tag)) key.keystore.add(p);
            else if (contains(HIDDEN, p.tag)) key.hidden.add(p);
        }
        boolean noAuth = present(key.keymint, NO_AUTH_REQUIRED);
        if (noAuth && (present(key.keymint, USER_SECURE_ID) || present(key.keymint, USER_AUTH_TYPE)))
            throw error(INVALID_TAG, "NO_AUTH_REQUIRED with user authentication tags");
        if (present(key.keymint, USER_SECURE_ID) && !present(key.keymint, USER_AUTH_TYPE))
            throw error(INVALID_TAG, "USER_SECURE_ID without USER_AUTH_TYPE");
        // A new key under an existing alias replaces it, as in keystore2.
        store(uid).update(keys -> {
            keys.removeIf(k -> k.alias.equals(alias));
            long id;
            do { id = random.nextLong() & Long.MAX_VALUE; } while (id == 0 || idTaken(keys, id));
            key.id = id;
            keys.add(key);
            return null;
        });
        return metadata(key);
    }

    private static boolean idTaken(List<Key> keys, long id) {
        for (Key k : keys) if (k.id == id) return true;
        return false;
    }

    // check_*_import_params: KEY_SIZE matches the material, or is deduced from it.
    private static List<Param> matchingSize(List<Param> params, int bits) {
        Param size = optional(params, KEY_SIZE);
        if (size == null) return Collections.singletonList(new Param(KEY_SIZE, 11, bits));
        if (size.integer() != bits) throw error(IMPORT_PARAMETER_MISMATCH, "KEY_SIZE differs from the key material");
        return Collections.emptyList();
    }

    private byte[] bytes(int count) {
        byte[] result = new byte[count];
        random.nextBytes(result);
        return result;
    }

    // ---- Operations (createOperation and IKeystoreOperation) ----

    private Object begin(int uid, Object descriptor, List<Param> params) throws Exception {
        Key key = find(uid, descriptor);
        List<Param> chars = key.characteristics();
        int purpose = single(params, PURPOSE, UNSUPPORTED_PURPOSE).integer();
        int algorithm = single(chars, ALGORITHM, UNSUPPORTED_ALGORITHM).integer();
        boolean valid = algorithm == AES ? purpose == ENCRYPT || purpose == DECRYPT
                : algorithm == HMAC && (purpose == SIGN || purpose == VERIFY);
        if (!valid) throw error(UNSUPPORTED_PURPOSE, "Purpose " + purpose + " is invalid for algorithm " + algorithm);
        if (!has(chars, PURPOSE, purpose)) throw error(INCOMPATIBLE_PURPOSE, "Purpose not authorized for the key");
        if (present(chars, STORAGE_KEY)) throw error(STORAGE_KEY_UNSUPPORTED, "Storage keys cannot be used");
        for (int tag : HIDDEN) {
            Param stored = optional(key.hidden, tag), given = optional(params, tag);
            if (stored == null ? given != null : given == null || !Arrays.equals((byte[]) stored.value, (byte[]) given.value))
                throw error(INVALID_KEY_BLOB, "Application id or data does not match the key");
        }
        enforce(key, purpose);
        Param nonceParam = optional(params, NONCE);
        byte[] nonce = nonceParam == null ? null : (byte[]) nonceParam.value;
        if (nonce != null && purpose == ENCRYPT && !present(chars, CALLER_NONCE))
            throw error(CALLER_NONCE_PROHIBITED, "Caller nonce not allowed for encryption");
        Running running = algorithm == AES ? beginAes(key, chars, params, purpose, nonce)
                : beginHmac(key, chars, params, purpose);
        running.uid = uid;
        running.keyId = key.id;
        running.limited = optional(key.keystore, USAGE_COUNT_LIMIT) != null;
        Object operation = services.local("keystore2.operation", PACKAGE + "IKeystoreOperation",
                new FrameworkServices.Call() {
                    @Override public String[] methods() { return names(Operation.values()); }
                    @Override public Object invoke(String method, Object[] a) throws Exception {
                        return guard(() -> running.call(method, a));
                    }
                });
        Object response = Class.forName(PACKAGE + "CreateOperationResponse").newInstance();
        set(response, "iOperation", operation);
        if (running.generatedNonce != null) {
            Object parameters = Class.forName(PACKAGE + "KeyParameters").newInstance();
            Object array = Array.newInstance(Class.forName(KEYMINT + "KeyParameter"), 1);
            Array.set(array, 0, parameter(new Param(NONCE, KIND_BLOB, running.generatedNonce)));
            set(parameters, "keyParameter", array);
            set(response, "parameters", parameters);
        }
        return response;
    }

    // keystore2 enforcements.rs: validity dates; user authentication cannot be satisfied
    // on a device without a lock-screen credential or biometrics.
    private static void enforce(Key key, int purpose) {
        long now = System.currentTimeMillis();
        Param active = optional(key.keystore, ACTIVE_DATETIME);
        if (active != null && now < active.longValue()) throw error(KEY_NOT_YET_VALID, "Key is not yet valid");
        Param end = optional(key.keystore, purpose == ENCRYPT || purpose == SIGN
                ? ORIGINATION_EXPIRE_DATETIME : USAGE_EXPIRE_DATETIME);
        if (end != null && now > end.longValue()) throw error(KEY_EXPIRED, "Key has expired");
        if (present(key.keymint, USER_SECURE_ID))
            throw error(KEY_USER_NOT_AUTHENTICATED, "No authenticator is enrolled");
    }

    private Running beginAes(Key key, List<Param> chars, List<Param> params, int purpose, byte[] nonce)
            throws Exception {
        if (present(params, RSA_OAEP_MGF_DIGEST)) throw error(INVALID_TAG, "MGF digest not allowed for AES");
        Param digest = optional(params, DIGEST);
        if (digest != null && digest.integer() != DIGEST_NONE) throw error(INVALID_TAG, "Digest not allowed for AES");
        int mode = single(params, BLOCK_MODE, UNSUPPORTED_BLOCK_MODE).integer();
        int padding = single(params, PADDING, UNSUPPORTED_PADDING_MODE).integer();
        int macBits = 0;
        if (mode == GCM) {
            macBits = single(params, MAC_LENGTH, MISSING_MAC_LENGTH).integer();
            if (macBits % 8 != 0 || macBits > 128) throw error(UNSUPPORTED_MAC_LENGTH, "Invalid MAC length " + macBits);
            if (macBits < single(chars, MIN_MAC_LENGTH, MISSING_MIN_MAC_LENGTH).integer())
                throw error(INVALID_MAC_LENGTH, "MAC length below the key's minimum");
        }
        boolean streamMode = mode == GCM || mode == CTR;
        if (streamMode ? padding != PAD_NONE : padding != PAD_NONE && padding != PAD_PKCS7)
            throw error(INCOMPATIBLE_PADDING_MODE, "Padding " + padding + " is invalid for block mode " + mode);
        int nonceSize = mode == GCM ? 12 : mode == ECB ? 0 : 16;
        if (mode != ECB && mode != CBC && mode != CTR && mode != GCM)
            throw error(UNSUPPORTED_BLOCK_MODE, "Unsupported block mode " + mode);
        if (nonce != null && nonce.length != nonceSize) throw error(INVALID_NONCE, "Invalid nonce length");
        if (!has(chars, BLOCK_MODE, mode)) throw error(INCOMPATIBLE_BLOCK_MODE, "Block mode not authorized for the key");
        if (!has(chars, PADDING, padding)) throw error(INCOMPATIBLE_PADDING_MODE, "Padding not authorized for the key");
        // crypto/aes.rs Mode::new.
        if (mode == GCM && macBits < 96) throw error(INVALID_MAC_LENGTH, "GCM tags are 12 to 16 bytes");
        Running running = new Running();
        if (nonceSize > 0 && nonce == null) nonce = running.generatedNonce = bytes(nonceSize);
        String name = mode == ECB ? "ECB" : mode == CBC ? "CBC" : mode == CTR ? "CTR" : "GCM";
        Cipher cipher = Cipher.getInstance("AES/" + name + "/" + (padding == PAD_PKCS7 ? "PKCS5Padding" : "NoPadding"),
                ENGINE);
        int op = purpose == ENCRYPT ? Cipher.ENCRYPT_MODE : Cipher.DECRYPT_MODE;
        SecretKeySpec spec = new SecretKeySpec(key.material, "AES");
        if (mode == GCM) cipher.init(op, spec, new GCMParameterSpec(macBits, nonce));
        else if (mode == ECB) cipher.init(op, spec);
        else cipher.init(op, spec, new IvParameterSpec(nonce));
        running.cipher = cipher;
        running.aead = mode == GCM;
        return running;
    }

    private Running beginHmac(Key key, List<Param> chars, List<Param> params, int purpose) throws Exception {
        if (present(params, BLOCK_MODE) || present(params, RSA_OAEP_MGF_DIGEST))
            throw error(INVALID_TAG, "Block mode or MGF digest not allowed for HMAC");
        Param padding = optional(params, PADDING);
        if (padding != null && padding.integer() != PAD_NONE) throw error(INVALID_TAG, "Padding not allowed for HMAC");
        int digest = single(params, DIGEST, UNSUPPORTED_DIGEST).integer();
        int digestBits = digestBits(digest);
        int min = single(chars, MIN_MAC_LENGTH, MISSING_MIN_MAC_LENGTH).integer();
        Running running = new Running();
        if (purpose == SIGN) {
            int macBits = single(params, MAC_LENGTH, MISSING_MAC_LENGTH).integer();
            if (macBits % 8 != 0 || macBits > digestBits) throw error(UNSUPPORTED_MAC_LENGTH, "Invalid MAC length");
            if (macBits < min) throw error(INVALID_MAC_LENGTH, "MAC length below the key's minimum");
            running.macBytes = macBits / 8;
        }
        if (!has(chars, DIGEST, digest)) throw error(INCOMPATIBLE_DIGEST, "Digest not authorized for the key");
        String[] names = {null, "HmacMD5", "HmacSHA1", "HmacSHA224", "HmacSHA256", "HmacSHA384", "HmacSHA512"};
        Mac mac = Mac.getInstance(names[digest], ENGINE);
        mac.init(new SecretKeySpec(key.material, names[digest]));
        running.mac = mac;
        running.verify = purpose == VERIFY;
        running.minMacBytes = min / 8;
        return running;
    }

    private static int digestBits(int digest) {
        switch (digest) {
            case 1: return 128;
            case 2: return 160;
            case 3: return 224;
            case 4: return 256;
            case 5: return 384;
            case 6: return 512;
            default: throw error(INCOMPATIBLE_DIGEST, "Invalid digest " + digest);
        }
    }

    // One begun operation. Any failure ends it, as keystore2 drops a failed operation.
    private final class Running {
        Cipher cipher;
        Mac mac;
        boolean aead, verify, limited, dataSeen, done;
        int macBytes, minMacBytes, uid;
        long keyId;
        byte[] generatedNonce;

        synchronized Object call(String method, Object[] a) throws Exception {
            Operation contract;
            try { contract = Operation.valueOf(method); }
            catch (IllegalArgumentException unknown) { throw unsupported("IKeystoreOperation." + method); }
            if (contract == Operation.getInterfaceVersion) return version();
            if (contract == Operation.getInterfaceHash) return hash();
            if (done) throw error(INVALID_OPERATION_HANDLE, "Operation has ended");
            try {
                switch (contract) {
                    case updateAad: {
                        byte[] input = checked((byte[]) a[0]);
                        if (!aead || dataSeen) throw error(INVALID_TAG, "Associated data after input or without GCM");
                        cipher.updateAAD(input);
                        return null;
                    }
                    case update: {
                        byte[] input = checked((byte[]) a[0]);
                        if (input.length > 0) dataSeen = true;
                        if (cipher != null) return cipher.update(input);
                        mac.update(input);
                        return null;
                    }
                    case finish: {
                        byte[] input = a[0] == null ? new byte[0] : checked((byte[]) a[0]);
                        byte[] output = complete(input, (byte[]) a[1]);
                        done = true;
                        if (limited) used();
                        return output;
                    }
                    case abort:
                        done = true;
                        return null;
                    default: throw unsupported("IKeystoreOperation." + method);
                }
            } catch (RuntimeException | GeneralSecurityException | IOException failure) {
                done = true;
                if (failure.getClass() == SERVICE_SPECIFIC) throw (RuntimeException) failure;
                throw error(cipher != null ? KM_INVALID_ARGUMENT : SYSTEM_ERROR, String.valueOf(failure));
            }
        }

        private byte[] checked(byte[] input) {
            if (input == null) throw error(INVALID_ARGUMENT, "No input");
            if (input.length > MAX_RECEIVE_DATA) throw error(TOO_MUCH_DATA, "Input exceeds " + MAX_RECEIVE_DATA + " bytes");
            return input;
        }

        private byte[] complete(byte[] input, byte[] signature) throws Exception {
            if (cipher != null) {
                try { return cipher.doFinal(input); }
                catch (AEADBadTagException tag) { throw error(VERIFICATION_FAILED, "GCM tag mismatch"); }
                catch (IllegalBlockSizeException size) { throw error(INVALID_INPUT_LENGTH, "Input is not whole blocks"); }
                catch (BadPaddingException padding) { throw error(KM_INVALID_ARGUMENT, "Invalid padding"); }
            }
            mac.update(input);
            byte[] full = mac.doFinal();
            if (!verify) return Arrays.copyOf(full, macBytes);
            if (signature == null || signature.length < minMacBytes || signature.length > full.length)
                throw error(INVALID_MAC_LENGTH, "Invalid MAC length to verify");
            if (!MessageDigest.isEqual(Arrays.copyOf(full, signature.length), signature))
                throw error(VERIFICATION_FAILED, "MAC mismatch");
            return null;
        }

        // keystore2 counts a USAGE_COUNT_LIMIT key's successful uses and deletes it at zero.
        private void used() throws Exception {
            store(uid).update(keys -> {
                for (Key k : keys) {
                    if (k.id != keyId) continue;
                    for (int i = 0; i < k.keystore.size(); i++) {
                        Param p = k.keystore.get(i);
                        if (p.tag != USAGE_COUNT_LIMIT) continue;
                        if (p.integer() <= 1) keys.remove(k);
                        else k.keystore.set(i, new Param(USAGE_COUNT_LIMIT, p.kind, p.integer() - 1));
                        return null;
                    }
                }
                return null;
            });
        }
    }

    // ---- Descriptors and metadata ----

    // A key the caller may use: by its alias in the app domain, or by its key id.
    private Key find(int uid, Object descriptor) throws Exception {
        if (descriptor == null) throw error(INVALID_ARGUMENT, "No key descriptor");
        int domain = getInt(descriptor, "domain");
        if (domain == DOMAIN_SELINUX || domain == DOMAIN_BLOB)
            throw error(PERMISSION_DENIED, "An app may only use keys in its own domain");
        if (domain == DOMAIN_GRANT) throw error(KEY_NOT_FOUND, "No keys are granted");
        if (domain != DOMAIN_APP && domain != DOMAIN_KEY_ID) throw error(INVALID_ARGUMENT, "Unknown domain " + domain);
        String alias = (String) get(descriptor, "alias");
        long id = ((Number) get(descriptor, "nspace")).longValue();
        if (domain == DOMAIN_APP && alias == null) throw error(INVALID_ARGUMENT, "No alias");
        for (Key key : store(uid).read())
            if (domain == DOMAIN_APP ? key.alias.equals(alias) : key.id == id) return key;
        throw error(KEY_NOT_FOUND, "Key not found");
    }

    private static Object descriptor(int domain, long nspace, String alias) throws Exception {
        Object descriptor = Class.forName(PACKAGE + "KeyDescriptor").newInstance();
        set(descriptor, "domain", domain);
        set(descriptor, "nspace", nspace);
        set(descriptor, "alias", alias);
        return descriptor;
    }

    private static Object metadata(Key key) throws Exception {
        Object metadata = Class.forName(PACKAGE + "KeyMetadata").newInstance();
        set(metadata, "key", descriptor(DOMAIN_KEY_ID, key.id, null));
        set(metadata, "keySecurityLevel", LEVEL_SOFTWARE);
        Class<?> type = Class.forName(PACKAGE + "Authorization");
        List<Param> chars = key.characteristics();
        Object array = Array.newInstance(type, chars.size());
        for (int i = 0; i < chars.size(); i++) {
            Object authorization = type.newInstance();
            set(authorization, "keyParameter", parameter(chars.get(i)));
            set(authorization, "securityLevel", i < key.keymint.size() ? LEVEL_SOFTWARE : LEVEL_KEYSTORE);
            Array.set(array, i, authorization);
        }
        set(metadata, "authorizations", array);
        set(metadata, "modificationTimeMs", key.modified);
        return metadata;
    }

    // ---- KeyParameter conversion (framework classes are hidden from the SDK) ----

    private static List<Param> params(Object array) throws Exception {
        List<Param> result = new ArrayList<>();
        if (array == null) return result;
        for (int i = 0; i < Array.getLength(array); i++) {
            Object parameter = Array.get(array, i);
            Object value = get(parameter, "value");
            if (value == null) throw error(INVALID_ARGUMENT, "Key parameter without a value");
            int kind = (Integer) value.getClass().getMethod("getTag").invoke(value);
            if (kind <= 0 || kind >= VALUE_KINDS.length) throw error(INVALID_ARGUMENT, "Unknown parameter kind " + kind);
            String member = VALUE_KINDS[kind];
            Object v = value.getClass().getMethod("get" + Character.toUpperCase(member.charAt(0)) + member.substring(1))
                    .invoke(value);
            result.add(new Param(getInt(parameter, "tag"), kind, v));
        }
        return result;
    }

    private static Object parameter(Param p) throws Exception {
        Class<?> valueType = Class.forName(KEYMINT + "KeyParameterValue");
        Class<?> argument = p.kind == KIND_BOOL ? boolean.class : p.kind == KIND_LONG || p.kind == KIND_DATE
                ? long.class : p.kind == KIND_BLOB ? byte[].class : int.class;
        Object value = valueType.getMethod(VALUE_KINDS[p.kind], argument).invoke(null, p.value);
        Object parameter = Class.forName(KEYMINT + "KeyParameter").newInstance();
        set(parameter, "tag", p.tag);
        set(parameter, "value", value);
        return parameter;
    }

    // get_tag_value!: exactly one instance, else `missing`.
    private static Param single(List<Param> params, int tag, int missing) {
        Param found = null;
        for (Param p : params) {
            if (p.tag != tag) continue;
            if (found != null) throw error(missing, "Duplicate tag " + Integer.toHexString(tag));
            found = p;
        }
        if (found == null) throw error(missing, "Missing tag " + Integer.toHexString(tag));
        return found;
    }

    // get_opt_tag_value!: at most one instance.
    private static Param optional(List<Param> params, int tag) {
        Param found = null;
        for (Param p : params) {
            if (p.tag != tag) continue;
            if (found != null) throw error(INVALID_TAG, "Duplicate tag " + Integer.toHexString(tag));
            found = p;
        }
        return found;
    }

    private static boolean present(List<Param> params, int tag) {
        for (Param p : params) if (p.tag == tag) return true;
        return false;
    }

    private static boolean has(List<Param> params, int tag, int value) {
        for (Param p : params) if (p.tag == tag && p.value instanceof Integer && p.integer() == value) return true;
        return false;
    }

    private static boolean contains(int[] tags, int tag) {
        for (int t : tags) if (t == tag) return true;
        return false;
    }

    private static Object get(Object target, String field) throws Exception {
        return target.getClass().getField(field).get(target);
    }

    private static int getInt(Object target, String field) throws Exception {
        return target.getClass().getField(field).getInt(target);
    }

    private static void set(Object target, String field, Object value) throws Exception {
        target.getClass().getField(field).set(target, value);
    }

    // ---- Storage ----

    interface Mutation<T> { T apply(List<Key> keys) throws Exception; }

    private final java.util.Map<Integer, Store> stores = new java.util.HashMap<>();

    private synchronized Store store(int uid) {
        Store store = stores.get(uid);
        if (store == null) stores.put(uid, store = new Store(new File(DIRECTORY, "bifrost-" + uid + ".json")));
        return store;
    }

    static final class Store {
        private final File file;
        private List<Key> cached;
        private long cachedStamp = -1, cachedSize = -1;

        Store(File file) { this.file = file; }

        // The latest published keys. Writers replace the file atomically; the parse is
        // cached until the file changes.
        List<Key> read() throws Exception {
            synchronized (STORE_LOCK) {
                long stamp = file.lastModified(), size = file.length();
                if (cached == null || stamp != cachedStamp || size != cachedSize) {
                    cached = Collections.unmodifiableList(load());
                    cachedStamp = stamp;
                    cachedSize = size;
                }
                return cached;
            }
        }

        <T> T update(Mutation<T> mutation) throws Exception {
            synchronized (STORE_LOCK) {
                File directory = file.getParentFile();
                if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Keystore storage unavailable");
                try (RandomAccessFile lock = new RandomAccessFile(new File(directory, file.getName() + ".lock"), "rw");
                        FileLock held = lock.getChannel().lock()) {
                    List<Key> keys = load();
                    T result = mutation.apply(keys);
                    save(keys);
                    cached = null;
                    return result;
                }
            }
        }

        private List<Key> load() throws Exception {
            List<Key> keys = new ArrayList<>();
            if (!file.exists() && !new File(file + ".bak").exists()) return keys;
            byte[] bytes;
            try (FileInputStream input = new AtomicFile(file).openRead()) {
                ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                byte[] chunk = new byte[8192];
                for (int count; (count = input.read(chunk)) > 0;) {
                    buffer.write(chunk, 0, count);
                    if (buffer.size() > MAX_BYTES) throw new IOException("Keystore storage is too large");
                }
                bytes = buffer.toByteArray();
            }
            JSONObject root = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
            if (root.getInt("version") != VERSION) throw new IOException("Unsupported keystore storage version");
            JSONArray array = root.getJSONArray("keys");
            for (int i = 0; i < array.length(); i++) {
                JSONObject entry = array.getJSONObject(i);
                Key key = new Key();
                key.id = entry.getLong("id");
                key.alias = entry.getString("alias");
                key.material = Base64.decode(entry.getString("material"), Base64.NO_WRAP);
                key.modified = entry.getLong("modified");
                read(entry.getJSONArray("keymint"), key.keymint);
                read(entry.getJSONArray("keystore"), key.keystore);
                read(entry.getJSONArray("hidden"), key.hidden);
                keys.add(key);
            }
            return keys;
        }

        private static void read(JSONArray array, List<Param> into) throws Exception {
            for (int i = 0; i < array.length(); i++) {
                JSONArray p = array.getJSONArray(i);
                int kind = p.getInt(1);
                Object value = kind == KIND_BOOL ? (Object) p.getBoolean(2)
                        : kind == KIND_LONG || kind == KIND_DATE ? (Object) p.getLong(2)
                        : kind == KIND_BLOB ? Base64.decode(p.getString(2), Base64.NO_WRAP) : (Object) p.getInt(2);
                into.add(new Param(p.getInt(0), kind, value));
            }
        }

        private static JSONArray write(List<Param> params) throws Exception {
            JSONArray array = new JSONArray();
            for (Param p : params)
                array.put(new JSONArray().put(p.tag).put(p.kind)
                        .put(p.kind == KIND_BLOB ? Base64.encodeToString((byte[]) p.value, Base64.NO_WRAP) : p.value));
            return array;
        }

        private void save(List<Key> keys) throws Exception {
            List<Key> sorted = new ArrayList<>(keys);
            Collections.sort(sorted, Comparator.comparing((Key k) -> k.alias));
            JSONArray array = new JSONArray();
            for (Key key : sorted)
                array.put(new JSONObject().put("id", key.id).put("alias", key.alias)
                        .put("material", Base64.encodeToString(key.material, Base64.NO_WRAP))
                        .put("modified", key.modified).put("keymint", write(key.keymint))
                        .put("keystore", write(key.keystore)).put("hidden", write(key.hidden)));
            byte[] bytes = new JSONObject().put("version", VERSION).put("keys", array).toString()
                    .getBytes(StandardCharsets.UTF_8);
            AtomicFile atomic = new AtomicFile(file);
            FileOutputStream output = atomic.startWrite();
            try {
                output.write(bytes);
                atomic.finishWrite(output);
            } catch (IOException | RuntimeException failure) {
                atomic.failWrite(output);
                throw failure;
            }
            try { android.system.Os.chmod(file.getPath(), 0600); }
            catch (android.system.ErrnoException ignored) { /* the guest VFS may not keep modes */ }
        }
    }
}
