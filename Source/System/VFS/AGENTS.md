# Guest filesystem and devices

- BIFROST_ROOT confinement applies after resolving symlinks and dirfds, not only
  lexical `..` cleanup. Preserve the secure host-path resolution used by open,
  stat, access, readlink and mutation paths; do not replace it with string-prefix
  checks or host realpath followed by an unprotected open.
- `/proc` mappings and main-stack views describe real guest mappings without
  overlaps. Report real process/thread identity and retain the existing fd-node
  ownership/lifetime rules.
- `/dev/random` and `/dev/urandom` use host entropy, not deterministic counters.
  Synthetic `/sys` CPU topology shares the emulator's configured guest CPU count
  with sysconf/getcpu/affinity interfaces; do not hardcode the developer's host.
- Keep virtual nodes separate from host path translation. Preserve errno,
  symlink, directory iteration and host device behavior across that boundary.
- Run filesystem sandbox regressions and the affected proc/sys/device probes.
