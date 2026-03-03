# hammer-sim

`hammer-sim` builds and runs native host executables from a HammerBlade-style
application directory.

The expected app layout is:

- `main.cpp`
- `kernel.cpp`
- `tests.mk`
- `test_defs.mk`

The native runtime is built around the same host/device split as the HammerBlade
app, but it does not use the original HammerBlade make system.

The native build requires a C++20-capable compiler because the runtime uses
`std::atomic_ref` and `std::atomic::wait/notify` to model AMOs and sleeping
wait-word semantics without spin loops.

## Quick Start

Build all `hammer-sim` executables for an app:

```sh
make -C hammer-sim APP_DIR=../hammerblade
```

That will:

- build one executable per test
- print the executable paths
- generate `hammer-sim/bin/<app-name>/Makefile`
- do all of that inside the standalone `hammer-sim` git repository

Run one test:

```sh
make -C hammer-sim/bin/hammerblade run-seq-len_32__num-seq_64
```

That prints the full host-side output for the selected test.

Run all tests:

```sh
make -C hammer-sim/bin/hammerblade run-all
```

`run-all` prints a compact summary for CI-style use:

```text
hammerblade:
----------------------------------------------------------------------
seq-len_16__num-seq_64   PASSED
seq-len_32__num-seq_64   PASSED
seq-len_64__num-seq_64   PASSED
```

The status column is aligned and colorized:

- `PASSED` in green
- `FAILED` in red
- `DEADLOCK` in red

`run-all` suppresses each test's normal host-side log output. Use the
individual `run-<test-name>` targets when you want the full log for one test.

Clean all native build output:

```sh
make -C hammer-sim clean
```

`make -C hammer-sim clean` removes the entire `hammer-sim` `build/` and
`bin/` trees, not just one app's outputs.

Clean one app's bin directory from the generated runner:

```sh
make -C hammer-sim/bin/hammerblade clean
```

## App Contract

`hammer-sim` assumes the app directory contains `tests.mk` and `test_defs.mk`.

`tests.mk` defines the set of tests:

```make
TESTS += $(call test-name,32,64)
```

`test_defs.mk` defines:

- how test names are formed
- how to decode a test name
- which compile-time macros the native build should define
- which runtime argv should be passed when running a test

Example:

```make
test-name = seq-len_$(1)__num-seq_$(2)
get-num-seq = $(lastword $(subst _, ,$(filter num-seq_%,$(subst __, ,$(1)))))
get-seq-len = $(lastword $(subst _, ,$(filter seq-len_%,$(subst __, ,$(1)))))

native-defines-for-test = \
	-DNUM_SEQ=$(call get-num-seq,$(1)) \
	-DSEQ_LEN=$(call get-seq-len,$(1))

native-program-args-for-test = \
	hammer-sim-kernel \
	$(APP_DIR)/dna-query32.fasta \
	$(APP_DIR)/dna-reference32.fasta
```

`hammer-sim-kernel` is only a placeholder `argv[1]` for host code that expects
a device binary path. The native runtime does not load it.

## Atomics Helper

Native builds cannot compile raw RISC-V AMO inline assembly. To make a kernel
`hammer-sim` ready, replace raw AMO asm with the helper wrappers in:

- `helpers/hb_riscv_atomics.hpp`

Example:

```cpp
#include "hb_riscv_atomics.hpp"

int old = hb_amoadd_w(counter, 1);
```

Hardware builds emit the original RISC-V AMO instructions.
Native builds dispatch to the simulator runtime.

## Making An App Native-Sim Ready

For a new app, keep the adaptation surface small:

1. Put `main.cpp`, `kernel.cpp`, `tests.mk`, and `test_defs.mk` in the app
   directory.
2. Replace raw RISC-V AMO inline assembly in `kernel.cpp` with wrappers from
   `helpers/hb_riscv_atomics.hpp`.
3. Keep scratchpad objects that may be accessed remotely as file-scope
   declarations in `kernel.cpp`.
4. Define `native-defines-for-test` and `native-program-args-for-test` in
   `test_defs.mk`.

The simulator does not require scratchpad annotations in the kernel body, but
it does rely on those file-scope declarations being present in `kernel.cpp`.

## What The Generator Does

`scripts/generate_kernel.py` rewrites `kernel.cpp` into a `hammer-sim`-only generated
translation unit that:

- extracts file-scope kernel globals into a per-core scratchpad struct
- rewrites `volatile int` wait words used by `bsg_lr` / `bsg_lr_aq`
- registers the kernel entry point with the native runtime

This is how native scratchpad state is modeled without changing the kernel
source itself.

## Important Assumptions

These assumptions matter for correctness:

- Remote scratchpad accesses are only modeled for addresses derived from
  file-scope variables declared directly in `kernel.cpp`.
- Remote accesses to stack variables or other function-local objects are not
  supported.
- Remote accesses to file-scope scratchpad objects declared in some other
  translation unit are not supported. The generator only rewrites the current
  `kernel.cpp`.
- `bsg_lr` / `bsg_lr_aq` are expected to operate on file-scope scratchpad wait
  words, typically declared as `volatile int` in the original kernel source.
- Raw RISC-V AMO inline assembly must be replaced with helper wrappers from
  `helpers/hb_riscv_atomics.hpp`.
- The current runtime executes one host thread per simulated tile.
- Wait-word blocking is implemented with sleeping `std::atomic::wait/notify`
  semantics, not spin loops.
- Native AMOs on plain memory are implemented with `std::atomic_ref`.
- The current default tile group is `16x8`, but the makefile allows overriding
  `TILE_GROUP_DIM_X` and `TILE_GROUP_DIM_Y`.

One specific edge-case is tolerated:

- Some kernels compute out-of-bounds neighbor pointers speculatively on tile
  edges and never dereference them. The runtime permits that pattern.

## Build Output Controls

The native build is quiet by default.

Enable verbose command echoing:

```sh
make -C hammer-sim APP_DIR=../hammerblade VERBOSE=1
```

Enable warning-heavy compiler flags:

```sh
make -C hammer-sim APP_DIR=../hammerblade WARNINGS=1
```

Those flags also work with the generated bin-side makefile.

## Deadlock Detection

The runtime tracks whether every simulated tile is:

- running
- blocked on a wait word
- blocked on the tile-group barrier
- terminated

If not all tiles have terminated and no tile can make progress for the configured
blocked states, the runtime aborts the launch, prints a red deadlock message,
and the test exits with code `2`. A regular non-deadlock test failure exits
with code `1`.

There is no deadlock timeout. A deadlock is only reported when every
non-terminated core is blocked and none of the blocked conditions has become
runnable.

## Performance Status

For this Smith-Waterman kernel and test matrix, the current runtime completes
all five native tests quickly on the local machine. The big regression came
from an earlier runtime that used heavyweight condition-variable waits and
treated plain memory as `std::atomic<T>*`, which is not a sound way to model
raw AMOs. The current runtime fixes both of those issues.

Performance is still kernel-dependent because the simulator runs one host
thread per tile, but the wait path itself now sleeps efficiently and wakeups
from remote stores are handled explicitly through the wait-word version counter.
