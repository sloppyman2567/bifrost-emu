// PackageManager.requestChecksums (IPackageManager.requestPackageChecksums),
// following Android 15's PackageManagerService.requestChecksumsInternal and
// ApkChecksums.getChecksums on the pinned framework's own APK classes. Per
// file (base, then splits when requested), optional types are returned only
// when readily available: the APK Signature Scheme v2/v3 content digests
// (partial Merkle roots). Bifrost files carry no fs-verity hash, nothing is
// installed through IncFS, and installs have no installer-provided digests.
// Required types still missing are computed: whole-file MD5/SHA-1/SHA-256/
// SHA-512, the fs-verity 4K Merkle root, and the 1 MiB partial Merkle roots.
// The listener is called once, from a background thread, as in AOSP.
import android.content.pm.ApplicationInfo;
import android.content.pm.Checksum;
import java.io.File;
import java.io.FileInputStream;
import java.io.RandomAccessFile;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class FrameworkPackageChecksums {
    // ApkSigningBlockUtils.CONTENT_DIGEST_CHUNKED_SHA256/SHA512.
    private static final int CHUNKED_SHA256 = 1, CHUNKED_SHA512 = 2;

    private final ExecutorService background = Executors.newSingleThreadExecutor(work -> {
        Thread thread = new Thread(work, "PackageChecksums");
        thread.setDaemon(true);
        return thread;
    });

    void request(ApplicationInfo info, boolean includeSplits, int optional, int required, Object listener) {
        if (listener == null) throw new NullPointerException("onChecksumsReadyListener");
        List<String> splits = new ArrayList<>();
        List<File> files = new ArrayList<>();
        splits.add(null);
        files.add(new File(info.sourceDir));
        if (includeSplits && info.splitNames != null) {
            for (int i = 0; i < info.splitNames.length; i++) {
                splits.add(info.splitNames[i]);
                files.add(new File(info.splitSourceDirs[i]));
            }
        }
        background.execute(() -> {
            List<Object> all = new ArrayList<>();
            for (int i = 0; i < files.size(); i++) {
                Map<Integer, Object> checksums = new LinkedHashMap<>();
                try {
                    available(splits.get(i), files.get(i), optional | required, checksums);
                } catch (Throwable error) {
                    System.err.println("PackageChecksums: preferred checksum calculation error: " + error);
                }
                try {
                    required(splits.get(i), files.get(i), required, checksums);
                } catch (Throwable error) {
                    System.err.println("PackageChecksums: required checksum calculation error: " + error);
                }
                all.addAll(checksums.values());
            }
            try {
                Method ready = Class.forName("android.content.pm.IOnChecksumsReadyListener")
                        .getMethod("onChecksumsReady", List.class);
                ready.invoke(listener, all);
            } catch (ReflectiveOperationException error) {
                System.err.println("PackageChecksums: listener failed: " + error);
            }
        });
    }

    private static boolean wanted(int type, int types, Map<Integer, Object> checksums) {
        return (types & type) != 0 && !checksums.containsKey(type);
    }

    // ApkChecksums.getAvailableApkChecksums without fs-verity or installer digests.
    private static void available(String split, File file, int types, Map<Integer, Object> checksums)
            throws Exception {
        if (!file.exists()) return;
        if (!wanted(Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA256, types, checksums)
                && !wanted(Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA512, types, checksums)) return;
        Class<?> verifier = Class.forName("android.util.apk.ApkSignatureVerifier");
        Class<?> input = Class.forName("android.content.pm.parsing.result.ParseInput");
        Object parse = Class.forName("android.content.pm.parsing.result.ParseTypeImpl")
                .getMethod("forDefaultParsing").invoke(null);
        Method verify = verifier.getDeclaredMethod("verifySignaturesInternal", input, String.class, int.class,
                boolean.class);
        verify.setAccessible(true);
        // SignatureSchemeVersion.SIGNING_BLOCK_V2, without full verification.
        Object result = verify.invoke(null, parse, file.getAbsolutePath(), 2, false);
        if ((Boolean) result.getClass().getMethod("isError").invoke(result)) return;
        Object details = result.getClass().getMethod("getResult").invoke(result);
        java.lang.reflect.Field field = details.getClass().getDeclaredField("contentDigests");
        field.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<Integer, byte[]> digests = (Map<Integer, byte[]>) field.get(details);
        if (digests == null) return;
        if ((types & Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA256) != 0 && digests.get(CHUNKED_SHA256) != null)
            put(checksums, split, Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA256, digests.get(CHUNKED_SHA256));
        if ((types & Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA512) != 0 && digests.get(CHUNKED_SHA512) != null)
            put(checksums, split, Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA512, digests.get(CHUNKED_SHA512));
    }

    // ApkChecksums.getRequiredApkChecksums.
    private static void required(String split, File file, int types, Map<Integer, Object> checksums)
            throws Exception {
        if (wanted(Checksum.TYPE_WHOLE_MERKLE_ROOT_4K_SHA256, types, checksums)) {
            Class<?> factory = Class.forName("android.util.apk.ByteBufferFactory");
            Object buffers = java.lang.reflect.Proxy.newProxyInstance(factory.getClassLoader(),
                    new Class<?>[] {factory}, (self, method, a) -> {
                        if (method.getName().equals("create")) return ByteBuffer.allocate((Integer) a[0]);
                        throw new UnsupportedOperationException(method.getName());
                    });
            Method root = Class.forName("android.util.apk.VerityBuilder").getMethod("generateFsVerityRootHash",
                    String.class, byte[].class, factory);
            byte[] hash = (byte[]) root.invoke(null, file.getAbsolutePath(), null, buffers);
            put(checksums, split, Checksum.TYPE_WHOLE_MERKLE_ROOT_4K_SHA256, verityHash(file, hash));
        }
        whole(checksums, split, file, types, Checksum.TYPE_WHOLE_MD5, "MD5");
        whole(checksums, split, file, types, Checksum.TYPE_WHOLE_SHA1, "SHA1");
        whole(checksums, split, file, types, Checksum.TYPE_WHOLE_SHA256, "SHA256");
        whole(checksums, split, file, types, Checksum.TYPE_WHOLE_SHA512, "SHA512");
        partial(checksums, split, file, types);
    }

    private static void whole(Map<Integer, Object> checksums, String split, File file, int types, int type,
            String algorithm) throws Exception {
        if (!wanted(type, types, checksums)) return;
        MessageDigest digest = MessageDigest.getInstance(algorithm);
        byte[] buffer = new byte[(int) Math.max(4096, Math.min(128 * 1024, file.length()))];
        try (FileInputStream in = new FileInputStream(file)) {
            for (int read; (read = in.read(buffer)) != -1; ) digest.update(buffer, 0, read);
        }
        put(checksums, split, type, digest.digest());
    }

    // ApkChecksums.calculatePartialChecksumsIfRequested: V3, else V2 signing block.
    private static void partial(Map<Integer, Object> checksums, String split, File file, int types)
            throws Exception {
        boolean sha256 = wanted(Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA256, types, checksums);
        boolean sha512 = wanted(Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA512, types, checksums);
        if (!sha256 && !sha512) return;
        try (RandomAccessFile apk = new RandomAccessFile(file, "r")) {
            Object signature = null;
            for (String scheme : new String[] {"ApkSignatureSchemeV3Verifier", "ApkSignatureSchemeV2Verifier"}) {
                Method find = Class.forName("android.util.apk." + scheme).getDeclaredMethod("findSignature",
                        RandomAccessFile.class);
                find.setAccessible(true);
                try {
                    signature = find.invoke(null, apk);
                    break;
                } catch (java.lang.reflect.InvocationTargetException missing) {
                    // SignatureNotFoundException: try the older scheme.
                }
            }
            if (signature == null) return;  // AOSP logs "V2/V3 signatures not found"
            int[] algorithms = sha256 && sha512 ? new int[] {CHUNKED_SHA256, CHUNKED_SHA512}
                    : new int[] {sha256 ? CHUNKED_SHA256 : CHUNKED_SHA512};
            Class<?> utils = Class.forName("android.util.apk.ApkSigningBlockUtils");
            Method compute = utils.getDeclaredMethod("computeContentDigestsPer1MbChunk", int[].class,
                    java.io.FileDescriptor.class, Class.forName("android.util.apk.SignatureInfo"));
            compute.setAccessible(true);
            byte[][] digests = (byte[][]) compute.invoke(null, algorithms, apk.getFD(), signature);
            for (int i = 0; i < algorithms.length; i++)
                put(checksums, split, algorithms[i] == CHUNKED_SHA256 ? Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA256
                        : Checksum.TYPE_PARTIAL_MERKLE_ROOT_1M_SHA512, digests[i]);
        }
    }

    // ApkChecksums.verityHashForFile: SHA-256 of the fsverity_descriptor.
    private static byte[] verityHash(File file, byte[] rootHash) throws Exception {
        ByteBuffer descriptor = ByteBuffer.allocate(256).order(ByteOrder.LITTLE_ENDIAN);
        descriptor.put((byte) 1).put((byte) 1).put((byte) 12).put((byte) 0).putInt(0).putLong(file.length());
        descriptor.put(rootHash);
        for (int i = 0; i < 32 + 32 + 144; i++) descriptor.put((byte) 0);
        descriptor.flip();
        MessageDigest digest = MessageDigest.getInstance("SHA256");
        digest.update(descriptor);
        return digest.digest();
    }

    private static Constructor<?> apkChecksum;

    private static void put(Map<Integer, Object> checksums, String split, int type, byte[] value) throws Exception {
        if (apkChecksum == null) {
            Constructor<?> constructor = android.content.pm.ApkChecksum.class
                    .getDeclaredConstructor(String.class, int.class, byte[].class);
            constructor.setAccessible(true);
            apkChecksum = constructor;
        }
        checksums.put(type, apkChecksum.newInstance(split, type, value));
    }
}
