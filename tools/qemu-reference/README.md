# QEMU reference signal-mask fix

`ctest/test_sigreturn_context.c` deliberately adds `SIGKILL` and `SIGSTOP`
to the handler's saved mask. Linux removes both on `rt_sigreturn`:
[AArch64 restore_sigframe](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/signal.c)
calls [set_current_blocked](https://github.com/torvalds/linux/blob/master/kernel/signal.c),
which clears the two unblockable signals.

The installed QEMU 11.1.1 reports both bits as blocked after returning.
The same issue exists in upstream revision
`81ce3a87737aa50716c42db8886082d12783e0a1` (11.1.50):
`linux-user/signal.c:set_sigmask` copies the mask without clearing those bits.
The adjacent `do_sigprocmask` path already clears them. The included patch
applies the same normalization to `set_sigmask`, covering signal return too.

This patch is for the optional QEMU reference runner. Bifrost already clears
the bits, and the regression keeps both assertions strict. No installed
system QEMU binary is changed.

## Build and reproduce

Run from the Bifrost repository root. A C toolchain, Python, Meson/Ninja,
and GLib development files are required; QEMU configure reports missing
dependencies. This builds only the AArch64 Linux user-mode executable.

```bash
bifrost_repo="$(pwd)"
git clone --depth 1 https://github.com/qemu/qemu.git /tmp/bifrost-qemu-reference
git -C /tmp/bifrost-qemu-reference fetch --depth 1 origin 81ce3a87737aa50716c42db8886082d12783e0a1
git -C /tmp/bifrost-qemu-reference checkout --detach 81ce3a87737aa50716c42db8886082d12783e0a1
git -C /tmp/bifrost-qemu-reference apply "$bifrost_repo/tools/qemu-reference/sigreturn-unblockable-mask.patch"
mkdir /tmp/bifrost-qemu-reference/build
cd /tmp/bifrost-qemu-reference/build
../configure --target-list=aarch64-linux-user --disable-system --disable-docs --disable-tools --disable-guest-agent --disable-werror
ninja -j4 qemu-aarch64
cd "$bifrost_repo"
make cross SRC=ctest/test_memory_permissions.c OUT=ctest/test_memory_permissions.elf
make cross SRC=ctest/test_sigreturn_context.c OUT=ctest/test_sigreturn_context.elf
timeout -k 1s 20s /tmp/bifrost-qemu-reference/build/qemu-aarch64 -cpu max ctest/test_memory_permissions.elf
timeout -k 1s 20s /tmp/bifrost-qemu-reference/build/qemu-aarch64 -cpu max ctest/test_sigreturn_context.elf
```

Expected: memory **106 checks, 0 failures**, signal context **48 checks,
0 failures**. Before the patch, signal context fails specifically on the
`SIGKILL` and `SIGSTOP` assertions. The memory test also supports CPUs without
LSE: `-cpu cortex-a53` reports that LSE is unavailable and passes 102 checks.
