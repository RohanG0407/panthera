# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` is the canonical repository guide: build, test, format and lint
commands, test registration, style rules and change hygiene. It is imported
here so there is a single source of truth; this file adds the cross-cutting
architecture that takes several files to piece together.

@AGENTS.md

## Environment and build gotchas

- Configure and build inside the Nix shell (`direnv allow` once per checkout).
  CMake caches `USE_NIX` and fails if a build directory configured inside Nix
  is reused outside it (or the reverse).
- The Makefile records the merge-base with `origin/main` in each build
  directory and passes `--fresh` to CMake when it changes, so the first build
  after a rebase or fetch reconfigures from scratch. `CMAKE_ALWAYS_FRESH=0`
  suppresses that.
- `JOBS=N` sets build parallelism; `CTEST_NPROC=N` sets CTest parallelism,
  which otherwise falls back to `JOBS` and then to 8.
- `make test TEST=<name>` builds only that C++ test target. Python and
  Panthera (Cheetah) tests have to be selected with `TESTS=<regex>`.
- Simulations read configs from `install/config/`, which is copied from
  `src/ampere/config/` at install time. Run `make install` after editing a
  `.cfg` before re-running a simulation.

Running a single simulation (from the User Guide):

```bash
./install/bin/panthera sim install/bin/amperesys -cfg install/config/model/sys/siryn.cfg \
    -sys.soc_0.core_0.instruction_stream_name src/ampere/core/test/dhrystone.is.xz   # add --es for an event stream
./install/bin/amperesys -cfg install/config/model/sys/domino.cfg -h               # list every parameter and its value
```

`install/bin/panthera <command>` is a bash launcher (`src/ampere/panthera`)
for the Python package in `src/ampere/python/panthera/`. Its commands include
`sim`, `cheetah` (batch studies, Slurm support), `rollup`, `report`,
`catscan`, `events.*` and `tracing.*`. Running it with no arguments lists
them.

## Architecture

Panthera is a trace-driven, cycle-level simulator. A trace (instruction
stream `*.is.xz`, optionally with branch or memory streams for offline warmup)
drives a SystemC model of a full SoC: cores on a CHI mesh with home nodes,
SLC and memory. The main executable is `amperesys` (`src/ampere/sys/`).

### Simulation framework (`src/ampere/sc/`)

Every model component builds on this layer rather than on raw SystemC:

- `Module` is an `sc_module` that is also a `Runnable`. Each cycle the `Clock`
  calls `run()` on scheduled runnables. Returning `true` reschedules the
  module for the next cycle. Returning `false` leaves it idle until one of its
  input connections schedules it again when data arrives.
- Modules talk through `In<T>`/`Out<T>` ports joined with
  `connect(out, in).name("...")`. Named connections expose `latency`,
  `bandwidth` and `capacity` as parameters, so pipeline timing lives in
  config, not code. `src/ampere/example/5stage/5stage.cc` is the smallest
  complete example.
- `Param<T> x{"name", "description", default}` members declare hierarchical
  parameters named by the module path (for example
  `sys.soc_0.core_0.gpc.uop_retire_width`). `Event` members record into the
  protobuf event stream that `es_viewer`, `catscan` and `events.tracing`
  consume.
- `poly` is a registry that instantiates modules by type name from config
  (`implements<T, Iface>`; see `MeshDevice` in `doc/user-guide/model-soc.md`).
  Registration runs in static initializers, which is why most libraries use
  the `*_raw` plus whole-archive link pattern in `doc/development/linking.md`.
  If the linker drops those initializers, nothing fails at link time; the
  failure only appears at runtime when a registered type or decoder is
  missing.
- Test helpers: `TestModule` (`sc/testing/test_module`) and
  `MockPutter`/`MockTaker` (`sc/testing/test_interfaces.h`). Simulation-based
  C++ tests use `add_cpp_test(... SYSTEMC)` to get an `sc_main` harness.

### Model layers

- `archx/`: architectural layer. Instruction representation, trace drivers
  (`driver_is`, `driver_bs`), emulation and architectural state. It also
  builds every per-CPU decoder.
- `decoders/<cpu>/`: instruction-to-uop translation for each core generation
  (`xlat/*.xlat`, plus `yaml/*_uops.yaml` per execution unit). The `xlat` tool
  in `python/xlat/` turns these into C++ at build time through
  `build_xlat_decoder(...)` in `archx/CMakeLists.txt`. Each CPU keeps a full
  set of files; `PARENT_CPU` lets a variant inherit from another. A new CPU
  decoder needs both a directory and an entry there.
- `core/`: the out-of-order core. Its unit names follow the Furiosa
  microarchitecture: ICF (fetch and IL1, with the BPU: BTB, TAGE and others),
  IDR (decode, rename, dispatch), IXU (integer execution and schedulers), FSU
  (FP/SIMD), LSU, MMU, GPC (in-order commit and flush control) and `l2/`.
  `doc/design/furiosa.md` explains the pipeline stages these modules model.
- `mesh/`: the CHI interconnect. CHI packet types and opcodes are in
  `mesh/chi/`, mesh plumbing is `MeshModule`/`MeshDevice`/`MeshPort`, and
  `HomeNode` handles coherence, the snoop filter and the SLC.
  `memory/` is the `Memory` endpoint, which uses a loaded-latency model.
- `soc/` builds the mesh and attaches devices from the `devices` JSON
  parameter, and handles warmup. `sys/` is the top-level `amperesys`
  executable that wraps the SoC (`sys.soc_0`).
- Trace and stream formats live in `instruction_stream/`, `branch_stream/`,
  `memory_stream/` and `event_stream/`. Standalone analysis binaries
  (`is_*`, `es_*`, `bs_*`) are in `tools/`.

### Configuration layering (`src/ampere/config/model/`)

Configs are `.cfg` parameter files composed hierarchically. The syntax is in
`doc/user-guide/parameters.md`: `scope:cfg`, `override-cfg`, `append-cfg`,
`~regex`, `+param` JSON merge and `$(prefix)`.

- `sys/<name>.cfg` selects a topology (`sys.soc_0:cfg ../soc/<topology>.cfg`)
  and applies study-level overrides.
- `soc/<topology>.cfg` sets the mesh size and the `devices` list, then loads
  the core (`core_0:cfg ../core/<core>/<core>.cfg`). Device instances are
  named as the lowercased type plus an index (`core_0`, `homenode_3`,
  `memory_1`).
- `core/<core>/` configs are deltas on the previous generation. For example,
  `ripley/ripley.cfg` loads `../furiosa/furiosa.cfg` and then overlays its own
  `frontend.cfg`, `backend.cfg` and `decode.cfg`. Changing a Furiosa default
  therefore changes every later core that does not override it.

Configs use engineering code names:

| Product          | SoC     | Core    |
|------------------|---------|---------|
| AmpereOne        | Siryn   | Furiosa |
| AmpereOne X      | Banshee | Ripley  |
| AmpereOne MX     | Rogue   | Trinity |
| AmpereOne Aurora | Domino  | Connor  |
| AmpereOne Next   | Nebula  | Alita   |

Config prefixes follow the SoC name: `sn-` Siryn, `bn-` Banshee, `rg-` Rogue,
`dn-` Domino.

### Generated code

Several sources are inputs to generators that run at build time. Edit the
input, never the output in `cmake-build-*`:

- `*.tab` files (such as `core/ixu.tab` and `mesh/chi/opcodes.tab`) go
  through `panthera.tables` (`tab2enum` in `cmake/tables.cmake`) and produce
  C++ enums, helpers and the Python bindings used by event-stream tools.
- `archx/*_patterns` files are compiled into decode functions by the `dectab`
  DFA builder (`src/ampere/dectab/`).
- `decoders/**/*.xlat` and `*.yaml` produce the per-CPU decoders described
  above.
- `adis/` generates an AArch64 disassembler from the vendored AARCHMRS
  instruction data (`adis/pysrc/genadis.py`).

## Upstream contribution scope

Per `CONTRIBUTING.md`, upstream Panthera is a reference implementation. It
accepts bug fixes, documentation and build-integration improvements. It does
not accept new microarchitectural features, changes that would affect the
reproducibility of published results, or new dependencies without prior
discussion. Changes to catscan or the perf-streams tools belong in their own
repositories. Open an issue before any larger change.
