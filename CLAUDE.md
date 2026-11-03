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
  is reused outside it (or the reverse). For a host without Nix, see
  [Building without Nix](#building-without-nix).
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

## Building without Nix

This is not the supported setup. Library versions differ from the Nix pins,
so results may not match a Nix build. It has been tested on Ubuntu 25.10
arm64 (GCC 15.2, CMake 3.31, Boost 1.88, Python 3.13): every target builds,
and 35 of 38 unit tests pass. The gaps are listed at the end of this section.

Install the Ubuntu packages:

```bash
sudo apt install build-essential cmake ninja-build pkgconf git xz-utils parallel \
    libboost-all-dev python3-dev libprotobuf-dev protobuf-compiler libprotobuf-c-dev \
    libcapstone-dev libgtest-dev libgmock-dev nlohmann-json3-dev libimmer-dev \
    libglib2.0-dev python3-protobuf python3-capstone python3-sklearn python3-yaml \
    python3-openpyxl
```

Do not install `libfmt-dev`. Ubuntu ships fmt 10, Panthera needs fmt 11, and
having both installed produced `fmt::v10`/`fmt::v11` redefinition errors.

Build the remaining dependencies from source into a single prefix:

```bash
D=$HOME/.local/panthera-deps
# SystemC 2.3.3 (accellera.org). C++11, as in nix/pkgs/systemc, to match
# Panthera's -DSC_CPLUSPLUS=201103L.
cmake -S systemc-2.3.3 -B systemc-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=11 -DCMAKE_INSTALL_PREFIX=$D
ninja -C systemc-build install
# fmt 11.0.2 (fmtlib/fmt tag 11.0.2).
cmake -S fmt-11.0.2 -B fmt-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DFMT_TEST=OFF -DFMT_DOC=OFF -DCMAKE_INSTALL_PREFIX=$D
ninja -C fmt-build install
# keystone 0.9.2 (keystone-engine/keystone tag 0.9.2). Its bundled LLVM uses
# intptr_t without <cstdint>, which GCC 13 and later no longer include
# transitively.
cmake -S keystone-0.9.2 -B keystone-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON -DCMAKE_CXX_FLAGS="-include cstdint" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=$D
ninja -C keystone-build install
# espresso 1.1 (classabbyamp/espresso-logic tag 1.1). K&R C, which GCC 15's
# default of C23 rejects.
make -C espresso-logic-1.1/espresso-src CFLAGS="-std=gnu89 -O2 -w"
install -D -m 755 espresso-logic-1.1/bin/espresso $D/bin/espresso
# qemu-plugin.h only, from AmpereComputing/tracing-qemu at the revision pinned
# in nix/pkgs/qemu/default.nix.
curl -fsSL -o $D/include/qemu-plugin.h --create-dirs \
    https://raw.githubusercontent.com/AmpereComputing/tracing-qemu/39724d94fbdba2200df959955fd49aec2b9e730b/include/qemu/qemu-plugin.h
```

The SystemC build also registers its build tree in `~/.cmake/packages`
(under `SystemCLanguage/` and `SystemCTLM/`). Delete those two entries so
CMake finds the installed copy rather than a build directory that may later
be removed.

Configure and build:

```bash
export PATH=$D/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
make install JOBS=N "CMAKE_ARGS=-DCMAKE_PREFIX_PATH=$D -DQEMU_ROOT=$D -Dfmt_DIR=$D/lib/cmake/fmt"
```

- Use this minimal `PATH` so that per-user installs cannot shadow the system
  tools. Watch for two cases in particular. A `pkgconf` that searches only
  its own prefix hides glib. A per-user capstone 6 is not API-compatible: Nix
  pins capstone 4, and Ubuntu's 5.0.6 works.
- Pass the same `CMAKE_ARGS` to every `make` target (`unit-test`, `test`,
  `build-debug` and so on). The Makefile reconfigures on each call and adds
  `--fresh` when the `origin/main` merge-base moves, which drops the cached
  paths.
- `archx_raw` precompiles a header that pulls in `fmt/core.h`. Installing a
  library into an include directory that is already on the search path does
  not rebuild that PCH. After changing which fmt the build finds, run
  `ninja -C cmake-build-release -t clean` and then rebuild.
- A debug build uses about 55 GB of disk; a release build uses about 23 GB.

Gaps compared with the Nix environment:

- Three unit tests need tools that are missing from the list above.
  `panthera_catscan_python_test` needs `urwid`,
  `panthera_report_python_test` needs `seaborn`, and
  `cheetah_json_config_specifications` needs `check-jsonschema`. Ubuntu
  packages them as `python3-urwid` (2.6.16, the version Nix pins),
  `python3-seaborn` and `python3-check-jsonschema`, but installing them has
  not been tested yet. Several rollup and report packages listed in
  `nix/panthera.nix` (such as bokeh, holoviews, hvplot, panel and kaleido)
  have no Ubuntu package.
- `make check-format` needs clang-format 21 (Nix pins LLVM 21) and `ruff`.
  Ubuntu provides `clang-format-21`; the Makefile calls `clang-format`, so
  put `/usr/lib/llvm-21/bin` on `PATH`. `ruff` is not packaged, so install it
  with pip.

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
