# Guest filesystem and devices

- BIFROST_ROOT confinement applies after resolving symlinks and dirfds, not only
  lexical `..` cleanup. Preserve the secure host-path resolution used by open,
  stat, access, readlink and mutation paths; do not replace it with string-prefix
  checks or host realpath followed by an unprotected open.
- A path syscall must never hand a remapped path to a host call that resolves
  it: `remap_path` is lexical only and host symlinks would escape. Name-level
  operations (create, unlink, rename, link) use `open_parent`; calls that act
  on the resolved file (stat, access, readlink, chmod, chown, utimens, truncate,
  statfs, inotify, followed linkat) use `path_confined` + `open_confined_path`
  and operate on the O_PATH fd (`AT_EMPTY_PATH`) or its `proc_fd_path`.
  Unsandboxed paths keep their direct host call. `test_sandbox` and the
  host-side canary check in `run_tests.sh` cover each call.
- Relative `AT_FDCWD` lookups follow the host cwd. A guest whose executable
  lies inside the root starts with the host cwd inside it
  (`enter_root_cwd`: the in-root launch directory, else the root), and
  `BIFROST_ROOT` is made absolute first. Host paths the emulator itself
  writes after loading must be absolute by then (`DebugFlags`, `Main.cpp`).
  Only an executable outside the root keeps the legacy host cwd.
- Task names belong to the Emulator: `/proc/self/comm` is the leader's,
  `/proc/self/task/<tid>/comm` (and `/proc/thread-self/comm`, rewritten in
  `open_guest_at`) reads and renames one guest TID with `comm_write`
  semantics. Guest TIDs are not host TIDs; never pass those through.
- `/proc` mappings and main-stack views describe real guest mappings without
  overlaps. `/proc/self/maps` follows `fs/proc/task_mmu.c` `show_map_vma`:
  real permissions, `s`/`p`, file offset, `major:minor` and inode of the file,
  a header ending in a space and names padded to column 73. File names use the
  guest view of the resolved path (`guest_view_of_host_path`, as fd readlink
  does). Report real process/thread identity and retain the existing fd-node
  ownership/lifetime rules.
- `/dev/random` and `/dev/urandom` use host entropy, not deterministic counters.
  Synthetic `/sys` CPU topology shares the emulator's configured guest CPU count
  with sysconf/getcpu/affinity interfaces; do not hardcode the developer's host.
- Node ioctls marshal fixed-layout structs through guest Memory
  (`Nodes/TerminalIoctls.hpp`, `Nodes/InterfaceIoctls.hpp`); never hand a guest
  pointer to a host ioctl. Unknown requests stay `-ENOTTY`.
- Keep virtual nodes separate from host path translation. Preserve errno,
  symlink, directory iteration and host device behavior across that boundary.
- Run filesystem sandbox regressions and the affected proc/sys/device probes.

- Raw TCGETS/TCSETS use the 36-byte kernel termios layout, never host libc struct termios (which can be larger). Preserve guest buffer bounds on both real and synthetic TTY paths.
