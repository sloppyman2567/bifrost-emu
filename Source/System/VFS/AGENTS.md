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
- Keep virtual nodes separate from host path translation. Preserve errno,
  symlink, directory iteration and host device behavior across that boundary.
- Run filesystem sandbox regressions and the affected proc/sys/device probes.
