# zstd CLI regression fixture

`zstd-aarch64` is a stripped, statically linked AArch64 zstd 1.6.0 command-line
program built from clean upstream commit
`01b7154f1172432f8abe9b3bb9909e14a1176b7d` in the Meta/Facebook zstd
repository. It was built with `CFLAGS=-O2`, `LDFLAGS=-static`, and optional
zlib, LZMA, and LZ4 support disabled. Its SHA-256 is
`402981cd22c2d6006628a8de25971774ee2ada1305037e6b8dfe9a4953302b0a`.
Its BSD license is included in `zstd.LICENSE`.

Run `scripts/run_zstd_regression.sh` to compress a deterministic input under
both engines with default workers and `--single-thread`, then validate and
round-trip each output with the host zstd CLI as an independent oracle. Both
guest engines also decode a host-generated archive. Install the host `zstd`
package first. The guest fixture itself is checked in, so no cross compiler
or network access is needed to run the regression.
