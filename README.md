# hammer-sim

`hammer-sim` builds and runs native executables from a HammerBlade-style app
directory.

## Quick Start

Build all tests for an app:

```sh
make -C hammer-sim APP_DIR=/path/to/app
```

That creates one executable per test and a generated runner makefile at:

```text
hammer-sim/bin/<app-name>/Makefile
```

Run all tests with a compact summary:

```sh
make -C hammer-sim/bin/<app-name> run-all
```

Run one test with full output:

```sh
make -C hammer-sim/bin/<app-name> run-<test-name>
```

Clean generated output:

```sh
make -C hammer-sim clean
```

## Required App Files

The app directory must contain:

- `main.cpp`
- `kernel.cpp`
- `tests.mk`
- `test_defs.mk`

`tests.mk` defines the test list.

`test_defs.mk` defines:

- test naming
- how to decode a test name
- which `-D...` macros are set for each test
- which runtime arguments are passed for each test

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
	$(APP_DIR)/input_a \
	$(APP_DIR)/input_b
```

`hammer-sim-kernel` is only a placeholder `argv[1]` for host code that expects
a device binary path.

## Atomics

For AMOs, use `bsg_manycore_atomic.h`.

`hammer-sim` provides a native shim for:

- `bsg_amoswap`
- `bsg_amoswap_aq`
- `bsg_amoswap_rl`
- `bsg_amoswap_aqrl`
- `bsg_amoor`
- `bsg_amoor_aq`
- `bsg_amoor_rl`
- `bsg_amoor_aqrl`
- `bsg_amoadd`
- `bsg_amoadd_aq`
- `bsg_amoadd_rl`
- `bsg_amoadd_aqrl`

So kernels should include:

```cpp
#include <bsg_manycore_atomic.h>
```

and call those functions directly.

## What hammer-sim Models

- one host thread per simulated tile
- tile-group barriers
- `bsg_lr` / `bsg_lr_aq`
- remote scratchpad accesses derived from file-scope variables in `kernel.cpp`
- native AMOs on plain memory

## bsg_lr / bsg_lr_aq

`hammer-sim` supports `bsg_lr` / `bsg_lr_aq` on:

- file-scope globals in `kernel.cpp`
- fields inside file-scope globals
- array elements inside file-scope globals

For non-`int` objects, pass the real pointer type:

```cpp
bsg_lr(&flag_struct.ready);
bsg_lr(&mailbox_array[i]);
```

Do not cast everything to `int*` unless you only want to watch 4 bytes. The
watched byte range is `sizeof(*ptr)` for the pointer type you pass in.

`volatile int` wait words are still supported and remain the fastest path.

## Current Limitation

Remote scratchpad accesses are only modeled for addresses derived from
file-scope variables declared directly in `kernel.cpp`.

Remote accesses to stack locals or function-local objects are not supported.

## Deadlock Detection

If every non-terminated core is blocked and none of the blocked conditions can
become runnable, `hammer-sim` reports a deadlock and exits with code `2`.

Regular test failure exits with code `1`.

## Output

`run-all` prints a summary like:

```text
hammerblade:
----------------------------------------------------------------------
seq-len_16__num-seq_64   PASSED
seq-len_32__num-seq_64   PASSED
seq-len_64__num-seq_64   PASSED
```

- `PASSED` is green
- `FAILED` is red
- `DEADLOCK` is red

Use `run-<test-name>` if you want the full program output for one test.
