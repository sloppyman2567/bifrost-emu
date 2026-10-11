public final class Bootstrap {
    static { System.load("/data/local/tmp/libbifrost-jni.so"); }
    private static native int roundTrip(int[] values, String text);
    private static int callback(int value) { return value * 3; }
    public static void main(String[] args) {
        int[] values = {7, 11, 19};
        if (roundTrip(values, "JNI\u0000\u03bb") != 111 || values[1] != 42)
            throw new AssertionError("JNI round trip");
        System.out.println("android_jni_bootstrap: ALL PASS");
    }
}
