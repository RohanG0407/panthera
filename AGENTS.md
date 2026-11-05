# Panthera repository guide

## Scope and orientation

This file applies at the repository root and to all subdirectories unless a
more-local `AGENTS.md` is added later. Panthera is an Ampere SoC performance
simulator: its primary implementation is C++23/SystemC, with Python tools and
tests. The normal build is CMake, but the top-level `Makefile` is the supported
developer interface; use its targets for configuring, building, testing,
formatting, and generating documentation whenever one exists.

Important locations:

* `src/ampere/` contains Panthera modules, simulator models, tools, Python
  package, configurations, and regressions. Each module generally owns a
  `CMakeLists.txt` and may have `test/` and `testing/` children.
* `src/{elfio,effolkronium}/` are bundled or third-party components. Avoid
  broad refactors there; preserve their local conventions and change them only
  when the task requires it.
* `infrastructure/` holds hooks, formatting checks, and developer scripts.
* `nix/`, `shell.nix`, and `clang.nix` define reproducible GCC and Clang
  development environments. On supported cluster hosts, enter/enable the Nix
  environment (normally via `direnv allow`) before configuring.
* `doc/` is the MkDocs source. Its sections are `user-guide/`, `analysis/`,
  `micros/`, `tracing/`, and `development/`. `mkdocs.yml`
  is the canonical navigation and site configuration; add an entry there when
  adding a user-facing page. `doc/index.md` is the overview and
  `doc/UserGuide.md` is the main guide.

## Build and test

Run commands from the repository root. Builds default to `release` and use
`cmake-build-<type>/`; installation goes to `install/`.

```bash
make install             # configure, build, and install the default release build
make install-debug       # configure, build, and install a debug build
make build               # build without installing
make test                # build/install and run the default test selection
make unit-test           # exclude integration, regression, benchmark, and veryslow tests
make integration         # run integration and regression tests
make list-tests          # list CTest tests for the current build directory
make test TEST=name      # build and run one C++ test by its exact CTest name
make test TESTS='regex'  # run tests matching a CTest regex (including Python/Panthera tests)
```

Use a matching build-type suffix when needed, for example `make test-debug` or
`make unit-test-asan`. Pass `CTEST_ARGS=-VV` to see a test's full command.
Prefer the smallest relevant test target while iterating; run `make unit-test`
for ordinary implementation changes and add integration coverage when behavior
crosses component boundaries. `make all-tests`, `make benchmark`,
`make asan-unit-test`, `make valgrind-unit-test`, and `make coverage` are more
expensive, targeted checks. For expected performance-neutral changes, consider
`make check-drift`; it requires the project regression environment and can be
long-running.

Do not manually edit generated build, installation, coverage, or MkDocs output
(`cmake-build-*`, `install*`, `coverage/`, `public/`, or
`compile_commands.json`). Use `make reset-cache`, `make clean`, or
`make clean-<type>` only when the affected build directory is the intended
target. The build automatically initializes submodules and installs the
pre-commit hook during CMake configuration.

### Environment and build notes

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

### Running a simulation

From the User Guide:

```bash
./install/bin/panthera sim install/bin/amperesys -cfg install/config/model/sys/siryn.cfg \
    -sys.soc_0.core_0.instruction_stream_name src/ampere/core/test/dhrystone.is.xz \
    -sys.soc_0.core_0.gpc.max_instructions 200000   # add --es for an event stream
./install/bin/amperesys -cfg install/config/model/sys/domino.cfg -h   # list every parameter and its value
```

Always set `max_instructions` below the length of the trace. Without it, the
simulator runs until the instruction stream is exhausted and exits with
`E549: No instructions remaining in stream`.

`install/bin/panthera <command>` is a bash launcher (`src/ampere/panthera`)
for the Python package in `src/ampere/python/panthera/`. Its commands include
`sim`, `cheetah` (batch studies, Slurm support), `rollup`, `report`,
`catscan`, `events.*` and `tracing.*`. Running it with no arguments lists
them.

## Building without Nix

This is not the supported setup. Library versions differ from the Nix pins,
so results may not match a Nix build. It has been tested on Ubuntu 25.10
arm64 (GCC 15.2, CMake 3.31, Boost 1.88, Python 3.13): every target builds
and all 38 unit tests pass. The remaining gaps are listed at the end of this
section.

Install the Ubuntu packages:

```bash
sudo apt install build-essential cmake ninja-build pkgconf git xz-utils parallel \
    libboost-all-dev python3-dev libprotobuf-dev protobuf-compiler libprotobuf-c-dev \
    libcapstone-dev libgtest-dev libgmock-dev nlohmann-json3-dev libimmer-dev \
    libglib2.0-dev python3-protobuf python3-capstone python3-sklearn python3-yaml \
    python3-openpyxl python3-urwid python3-seaborn
```

`python3-urwid` is 2.6.16, the version Nix pins. It and `python3-seaborn`
are needed by the catscan and report Python tests.

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
# check-jsonschema, for cheetah_json_config_specifications. Ubuntu's
# python3-check-jsonschema (0.34.0) fails on import: it needs the `regress`
# module, which Ubuntu does not package.
python3 -m venv $D/venvs/check-jsonschema
$D/venvs/check-jsonschema/bin/pip install check-jsonschema
ln -s $D/venvs/check-jsonschema/bin/check-jsonschema $D/bin/
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

- Several rollup and report packages listed in `nix/panthera.nix` (such as
  bokeh, holoviews, hvplot, panel and kaleido) have no Ubuntu package. No
  unit test needs them.
- `make check-format` needs clang-format 21 (Nix pins LLVM 21) and `ruff`.
  Ubuntu provides `clang-format-21`; the Makefile calls `clang-format`, so
  put `/usr/lib/llvm-21/bin` on `PATH`. `ruff` is not packaged, so install it
  with pip.

## Implementation and tests

* Keep changes narrowly scoped to the owning module. When adding a source,
  executable, library, generated artifact, or test, update that directory's
  `CMakeLists.txt`; files are not discovered automatically.
* Put behavior tests in `**/test/` and name them `test_*.(cc|py)`. Put reusable
  fixtures, builders, fakes, and mocks in `**/testing/`; C++ support libraries
  should normally be header-only or end in `_testing` and use an
  `ampere::*::testing` namespace.
* Register C++ tests with `add_cpp_test`, Python tests with `add_python_test`,
  and study/cheetah tests with `add_panthera_test`. Label integration tests
  `integration` (and use appropriate timeout/optional labels) so Makefile
  selectors continue to work.
* The root CMake project uses C++23 (with GNU C++ extensions disabled) and
  C23, and exports `compile_commands.json`. Preserve CMake target
  relationships rather than adding global include paths or flags.
* Some libraries depend on registration before `main()`. Follow the
  `*_raw`/whole-archive pattern described in `doc/development/linking.md` when
  adding to such a library. Add sources to the raw target and never link
  consumers against `LIBNAME_raw` directly.

## Style and quality checks

Follow `doc/development/conventions.md` and let the configured formatters make
layout decisions.

* C++: `PascalCase` types/enums, `snake_case` functions/methods/variables, and
  `UPPER_CASE` constants/macros. Prefer `auto`, `const` member functions,
  `enum class`, references where null is invalid, named structs over tuples,
  and the standard library. Avoid `using namespace`, and use braces when an
  `if` condition or body spans multiple lines.
* Python: write idiomatic code, prefer type annotations and comprehensions
  where clear, use underscore-prefixed unused variables, and follow Ruff's
  120-column, four-space configuration in `pyproject.toml`.
* Markdown, configuration, and source text must end in a newline; avoid tabs
  in the checked text formats.

Check changes with the Makefile:

```bash
make check-format         # text/newline, clang-format, and Ruff format checks
make python-lint          # Ruff lint for tracked Panthera Python files
make doc                  # build the MkDocs site after documentation changes
```

`make format`, `make clang-format`, and `make python-format` intentionally
require a clean worktree and rewrite all matching files. Do not use them over
someone else's uncommitted work. For an already-dirty tree, format only the
files you own with the configured formatter, or use the `*-changed` Makefile
targets after verifying their diff scope. Run `make check-format` before
hand-off; if unrelated pre-existing changes make a repository-wide check fail,
report that distinction clearly.

## Documentation and change hygiene

Keep documentation close to the audience it serves and use existing Markdown
heading, link, fenced-code, and relative-link conventions. Update `mkdocs.yml`
when adding a new page that should appear in the published navigation, then
validate with `make doc`. Update documentation together with any changed
command, test workflow, configuration, or user-visible behavior.

Before making changes, review `git status` and preserve unrelated work in a
dirty worktree. Do not use `git reset`, broad cleanup, or destructive
regeneration to discard or overwrite existing work. Before handing off, review
`git diff` and `git status`, keep generated and unrelated files out of the
change, and state the exact build/test/format/doc commands run (or why one was
not run). When committing, make each commit small, standalone, and atomic:
stage only one coherent change and ensure every commit builds and passes its
relevant checks so history is easy to review and can be automatically
git-bisected. Write commit messages that explain the rationale as well as the
material implementation details, so a reviewer or future code archaeologist
can understand both why and how the change was made. Wrap commit-message body
lines at 72 columns. For a performance-impacting
change, record expected drift in the review using the repository's
`Expected Perf Drift: <value or negligible>` convention.

## Upstream contribution scope

Per `CONTRIBUTING.md`, upstream Panthera is a reference implementation. It
accepts bug fixes, documentation and build-integration improvements. It does
not accept new microarchitectural features, changes that would affect the
reproducibility of published results, or new dependencies without prior
discussion. Changes to catscan or the perf-streams tools belong in their own
repositories. Open an issue before any larger change.

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
