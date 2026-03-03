# hammer-sim Helpers

This directory is for small source-level helpers that make a HammerBlade kernel
buildable under `hammer-sim` without changing its algorithm.

## Files

- `hb_riscv_atomics.hpp`
  - Wraps RISC-V AMOs.
  - On hardware builds it emits the original inline assembly.
  - Under `HB_NATIVE_SIM` it dispatches to the native runtime implementation.

## Typical Use

```cpp
#include "hb_riscv_atomics.hpp"

int old = hb_amoadd_w(counter, 1);
```

For native builds, make sure the helper directory is on the include path.

## Test Mapping

`hammer-sim` does not inherently know what `SEQ_LEN` means. The app's
`test_defs.mk` defines both:

- how to parse the test name
- which compile-time macros should be defined for that test

The current app does that with:

```make
native-defines-for-test = \
	-DNUM_SEQ=$(call get-num-seq,$(1)) \
	-DSEQ_LEN=$(call get-seq-len,$(1))
```

For a different app, redefine `native-defines-for-test` in that app's own
`test_defs.mk`.

`test_defs.mk` can also define the runtime argv for each test:

```make
native-program-args-for-test = \
	hammer-sim-kernel \
	$(APP_DIR)/input_a \
	$(APP_DIR)/input_b
```

That is how a different app supplies its own input files to the built native
executables.

## Build Flow

Build all native executables for an app with:

```sh
make -C hammer-sim APP_DIR=../path/to/app
```

That will:

- build one executable per test
- print the executable paths
- generate `hammer-sim/bin/<app-name>/Makefile`

Then run from the generated bin directory with:

```sh
make -C hammer-sim/bin/<app-name> run-all
make -C hammer-sim/bin/<app-name> run-<test-name>
```
