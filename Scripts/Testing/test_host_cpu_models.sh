#!/usr/bin/env bash
# The JIT's minimum host CPU (SSE4.1 and FMA3), emulated by qemu-x86_64: fixtures
# pass on the oldest supported model, and older models get a clear refusal.
#
#   Scripts/Testing/test_host_cpu_models.sh [EMULATOR]
#
# Host libraries must run on those CPUs too. A distribution that builds for
# x86-64-v3 needs a generic sysroot: QEMU_LD_PREFIX=/path/to/rootfs, with an
# emulator linked against it. A model the emulator cannot start on is reported
# as skipped; exit 77 when no model ran.
#
# LZCNT without BMI1 (AMD K10, early Bulldozer) is not covered: QEMU 11.1 runs
# LZCNT as BSR unless BMI1 is also enabled, so qemu's phenom and Opteron models
# give wrong answers for correct code.
set -u
ulimit -c 0  # A CPU model that cannot run a host library would leave a core per attempt.

EMU="${1:-${BIFROST_EMU:-./bifrost-emu}}"
FIXTURES=(jit_cls jit_bfext jit_block_split jit_fp_scalar jit_selfloop_writeback jit_round_away)
MODELS=(Haswell)
REFUSED=(SandyBridge Nehalem Conroe qemu64)  # no FMA3; Conroe and qemu64 no SSE4.1 either

if ! command -v qemu-x86_64 >/dev/null; then
    echo "test_host_cpu_models: skipped, qemu-x86_64 is not installed"
    exit 77
fi

ran=0
failed=0
for model in "${REFUSED[@]}"; do
    qemu-x86_64 -cpu "$model" "$EMU" --version >/dev/null 2>&1 || continue
    if timeout 60 qemu-x86_64 -cpu "$model" "$EMU" Tests/Unit/jit_cls.elf 2>&1 | grep -q 'the JIT needs a CPU'; then
        echo "PASS $model refused"
    else
        echo "FAIL $model: no refusal"
        failed=$((failed + 1))
    fi
done
for model in "${MODELS[@]}"; do
    if ! qemu-x86_64 -cpu "$model" "$EMU" --version >/dev/null 2>&1; then
        echo "SKIP $model: the emulator or a host library needs a newer CPU"
        continue
    fi
    ran=$((ran + 1))
    for fixture in "${FIXTURES[@]}"; do
        elf="Tests/Unit/$fixture.elf"
        if [ ! -f "$elf" ]; then
            echo "FAIL $model $fixture: $elf is missing"
            failed=$((failed + 1))
            continue
        fi
        if timeout 180 qemu-x86_64 -cpu "$model" "$EMU" "$elf" >/dev/null 2>&1; then
            echo "PASS $model $fixture"
        else
            echo "FAIL $model $fixture: exit $?"
            failed=$((failed + 1))
        fi
    done
done

if [ "$ran" = 0 ]; then
    echo "test_host_cpu_models: skipped, no CPU model could start the emulator"
    exit 77
fi
[ "$failed" = 0 ]
