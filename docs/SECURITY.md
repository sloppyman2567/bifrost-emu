# Security model

## What `BIFROST_ROOT` does

`BIFROST_ROOT` remaps and confines supported guest filesystem operations to
a selected root directory. The sandbox regression suite checks representative
read, write, `openat`, directory-fd, symlink, cwd, procfs, and namespace
operations. Use it to provide a controlled guest filesystem for tests.

It is a VFS path boundary inside the emulator. It is not an operating-system
security boundary and does not create Linux mount, PID, user, or network
namespaces. With `BIFROST_ROOT` unset, some guest paths intentionally pass
through to the host filesystem.

## Guest trust

Do not run hostile guest binaries on the assumption that `BIFROST_ROOT`
alone contains them. Guest code exercises the emulator's syscall, ELF loader,
JIT, and dynamic-linker implementations in the same host process. SDL, audio,
graphics thunks, and host drivers can expose additional host APIs. Emulator
bugs in any of these areas can cross the intended guest boundary.

For untrusted guests, run bifrost-emu under an independent host isolation
layer controlled by the operator: an unprivileged account, a container or
namespace sandbox, a restrictive syscall policy, resource limits, and a
sanitized environment. Do not pass through host devices, sockets, credentials,
or directories that the guest does not need. Treat GPU and audio passthrough
as additional trusted host interfaces.

## Regression coverage

`ctest/test_sandbox.c` validates selected BIFROST_ROOT path operations and
must remain in the strict integration suite. These checks are regression
coverage for the VFS behavior, not a proof that the emulator is safe against
malicious input. Security reports should include the guest ELF, host/kernel,
rootfs setup, relevant environment variables, and whether host thunks were
enabled.
