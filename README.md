# Assignment 2

This package contains:
- `unifiedpass.cpp`: LLVM plugin with
  - reusable fixed-point dataflow engine
  - set-print helper utilities
  - bitvector-based Available Expressions, Liveness, and Reaching Definitions passes
  - a map-based Constant Propagation starter using a 3-point lattice
    (`TOP`, `Const`, `NAC`)
  - pass registration for
  - `available`
  - `liveness`
  - `reaching`
  - `constantprop`
- `Makefile`: build + run targets
- `tests/`: 4 provided test inputs (`*.bc`)

## Build

```bash
make
```

This builds `build/unifiedpass.so`.

## Run all tests

```bash
make tests
```

This generates:
- `build/tests/*-m2r.ll` (disassembled inputs)
- `build/tests/*-opt.ll` (outputs after running each pass)

## Run one pass manually

```bash
opt -bugpoint-enable-legacy-pm=1 \
  -load-pass-plugin=build/unifiedpass.so \
  -passes='available' tests/available-test-m2r.bc -o /tmp/out.bc
```

Replace `available` with one of: `liveness`, `reaching`, `constantprop`.


