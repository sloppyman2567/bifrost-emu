// Compile-time declaration of a hidden framework class (android-15.0.0_r1). The launcher build reads Stubs/ as a
// source path without generating classes from it (javac -implicit:none), so the device's own class runs.
package android.view;

public final class InputChannel {
    private InputChannel() {}
}
