// Diagnostic compatibility entry point; the managed launcher owns the runtime.
public final class ActivityProbe {
    public static void main(String[] args) {
        ManagedLauncher.probe(args);
    }
}
