# Running Panthera on a new machine

How to build Panthera from the fork, check that it works, download Ampere's
published traces, and run them.

Steps 1 to 5 were run as written on a 160-core aarch64 Linux host. The
timings below come from that host, which already had Nix installed. Step 6
(the study list) was run for one trace only; see the note there.

## What you need

- Linux with `sudo` access, to install Nix.
- Network access to github.com, nixos.org and zenodo.org.
- Disk space:

| Item | Space |
|---|---|
| Panthera checkout, release build and install | about 23 GB |
| Perf Check 500 traces | 1.7 GB download, 9.6 GB unpacked |
| Study list traces | 157 GB download plus 157 GB unpacked; 162 GB once you delete the archives |

## 1. Install Nix

Panthera's supported build environment is a Nix shell. Install Nix with the
official multi-user installer, then open a new terminal:

```bash
sh <(curl -L https://nixos.org/nix/install) --daemon
```

## 2. Clone and build

The commands below assume the checkout is `~/panthera`.

```bash
cd ~
git clone https://github.com/RohanG0407/panthera.git
cd panthera
nix-shell --run 'make install JOBS=32'
```

Set `JOBS` to about 80% of the machine's cores (`nproc`). The first
`nix-shell` downloads the whole toolchain, so it takes a while. On the test
host, with the Nix store already populated, the clone and build took 6
minutes.

To skip typing `nix-shell --run`, install
[direnv](https://direnv.net/) and run `direnv allow` once in the checkout.
After that, `make` works directly in that directory.

## 3. Check the build

Run the unit tests:

```bash
nix-shell --run 'make unit-test JOBS=32'
```

Expect `100% tests passed, 0 tests failed out of 39`. This took about 2
minutes.

Run one short simulation of the dhrystone trace that ships with the repo:

```bash
./install/bin/panthera sim install/bin/amperesys \
    -cfg install/config/model/sys/siryn.cfg \
    -sys.soc_0.core_0.instruction_stream_name src/ampere/core/test/dhrystone.is.xz \
    -sys.soc_0.core_0.gpc.max_instructions 200000
```

It takes under a minute and ends with a line like:

```
Simulator measurement :  424,046 cycles,  200,001 instructions,  0.472 IPC, ...
```

You don't need to be inside `nix-shell` to run `./install/bin/panthera`; it
enters the Nix environment itself.

## 4. Download traces

Ampere publishes the traces on Zenodo:

| Set | Zenodo record | What it is |
|---|---|---|
| Perf Check 500 | [10.5281/zenodo.23426212](https://doi.org/10.5281/zenodo.23426212) | 500 short tests, at most 200,000 instructions each, with cache and branch-predictor state loaded at the start |
| Study list | [10.5281/zenodo.23519872](https://doi.org/10.5281/zenodo.23519872) | 4,332 traces from 253 workloads, 20M measured instructions each, plus offline-warmup streams |

Zenodo limits each connection to about 1 MB/s, so a plain `curl` or `wget`
of the 157 GB study list takes about two days. Use `aria2c` with 8
connections. On the test host it ran at 13 MiB/s, which puts the study list
at 3 to 4 hours. `aria2c` checks the MD5 checksum when it finishes, and
`-c` makes a re-run continue an interrupted download.

Get `aria2c` from Nix (`nix-shell -p aria2`) or your distribution
(`sudo apt install aria2`). Pick a directory for the traces:

```bash
mkdir -p ~/traces && cd ~/traces
fetch() {  # fetch <record> <file> <md5>
    nix-shell -p aria2 --run "aria2c -c -x 8 -s 8 -k 64M --file-allocation=none \
        --checksum=md5=$3 -o $2 https://zenodo.org/api/records/$1/files/$2/content"
}
```

Perf Check 500:

```bash
fetch 23426212 perf_check_500.tar.xz 646df9b76eb839266dd9efa1b618751b
tar -xJf perf_check_500.tar.xz        # creates ampere_snippets/
```

Study list, plus its AUX archive. The AUX archive adds a `.mem` file per
trace (used for wrong-path execution), disassemblies and footprints:

```bash
fetch 23519872 2024-study-list.tar 2e3cb2fe9a883f2cf8d468cf56d08791
fetch 23519872 2024-study-list-AUX.tar.xz 8847196ca8c12ceaf492f03f5effc191
tar -xf 2024-study-list.tar           # creates ampere_traces/
chmod -R u+w ampere_traces            # see below
tar -xJf 2024-study-list-AUX.tar.xz   # adds to ampere_traces/
rm 2024-study-list.tar 2024-study-list-AUX.tar.xz   # optional, frees 158 GB
```

The first archive creates 95 of its directories read-only. Without the
`chmod`, the AUX archive cannot add its files to them and `tar` stops with
"Permission denied".

The trace lists are `ampere_snippets/perf_check_500.json` and
`ampere_traces/2024-study_list.json`.

## 5. Run Perf Check 500 traces

Make an 11-trace subset and run it:

```bash
cd ~/traces
P=~/panthera/install/bin/panthera
$P tracing.filter_study_list \
    --input-list ampere_snippets/perf_check_500.json \
    --output-list ampere_snippets/pc500-smoke.json \
    --match workload --include '^505-mcf' --include '^531-deepsjeng'
$P study ~/panthera/install/config/cheetah/python/rtl_snippets_study.py \
    --workloads "$PWD/ampere_snippets/pc500-smoke.json" \
    --name pc500-smoke --soc rogue --run serial
ls pc500-smoke/panthera/*.all.summary.csv | wc -l   # expect 11
```

This took about a minute. Each trace's results land in
`pc500-smoke/panthera/<trace>.all.summary.csv`.

Three settings in that command matter (see [Known problems](#known-problems)):

- `--soc rogue`: the Perf Check 500 cache preloads fit the Rogue core. On
  the default `domino`, 304 of the 500 tests fail.
- `"$PWD/..."`: give `--workloads` an absolute path.
- `--run serial`: `--run parallel` fails on a machine without Slurm.

To run all 500, drop the filter step and pass `perf_check_500.json`. Serial
runs take about 30 seconds per trace, so all 500 take a few hours. To go
faster, split the list and run one serial study per part at the same time:

```bash
python3 -c '
import json
d = json.load(open("ampere_snippets/perf_check_500.json"))
for i in range(8):
    json.dump(d[i::8], open(f"ampere_snippets/pc500-part{i}.json", "w"))'
for i in $(seq 0 7); do
    $P study ~/panthera/install/config/cheetah/python/rtl_snippets_study.py \
        --workloads "$PWD/ampere_snippets/pc500-part$i.json" \
        --name pc500-part$i --soc rogue --run serial > pc500-part$i.out 2>&1 &
done
wait
```

## 6. Run study-list traces

A study-list run uses the trace list's `.ms` files for cache warmup and its
`.bs` files for branch-predictor warmup, and measures 10 million
instructions per trace. Each trace takes tens of minutes, so run the traces
in parallel. `--run parallel` fails without Slurm (see
[Known problems](#known-problems)), so create the study with `--dryrun` and
run its jobs with `panthera cheetah.local`.

The example is the data-prefetcher headroom study,
`prefetch_headroom_study.py`. It runs every trace three times on the Domino
core: with the production prefetchers (`baseline`), with every data
prefetcher off (`no-prefetch`), and with oracle data prefetchers at L1D and
L2 (`oracle`). To run the standard core study from the
[Cheetah guide](cheetah.md) instead, use `core_study.py` in the same
directory.

Select the traces. This picks the four datacenter suites, 1,206 of the
4,332 traces:

```bash
cd ~/traces
$P tracing.filter_study_list \
    --input-list ampere_traces/2024-study_list.json \
    --output-list ampere_traces/datacenter-suites.json \
    --match suite --include '^(CloudInfra|WebServices|Database|SpecJBB)$'
```

`--match workload --include '^767.nest$'` selects one workload,
`--match trace` one trace, and `--min-weight 0.05` drops low-weight traces.
Write the output list next to the input list, because the trace paths in it
are relative to the list's directory.

Create the study directory without running anything, then run it:

```bash
$P study ~/panthera/install/config/cheetah/python/prefetch_headroom_study.py \
    --workloads "$PWD/ampere_traces/datacenter-suites.json" \
    --name pf-headroom-dc --run parallel --dryrun
$P cheetah.local pf-headroom-dc --jobs 64 \
    --workloads ampere_traces/datacenter-suites.json
```

`cheetah.local` options:

- `--jobs N`: simulations to run at once. The default is three quarters of
  the CPUs. Each simulation uses one CPU and at most about 1 GB of
  memory.
- `--workloads LIST`: run each workload's highest-weight trace first, so a
  study that is stopped early still covers every workload.
- `--numa-node N`: run only on that NUMA node's CPUs.
- `--list`: print the jobs that would run and exit.

A job is finished when its log, `<configuration>/<trace>.log`, ends with
"All commands exited successfully". Running `cheetah.local` again skips the
finished jobs, so the same command continues an interrupted study and
retries failed jobs. The study takes hours to days: start it inside `tmux`
or with `nohup`.

Each job writes `<configuration>/<trace>.summary.csv`. Combine them with
[rollup](../analysis/rollup.md):

```bash
$P rollup pf-headroom-dc/study.json \
    --formula ~/panthera/install/config/rollup/rollup_formula.py
```

This writes `rollup.by_trace.csv`, `rollup.by_workload.csv` and
`rollup.by_suite.csv` into the study directory, one row per configuration
and trace, workload or suite. The IPC column is `sys.soc_0.core_0.ipc`.

What was tested: one trace (`nodejs-misc-util_7992`) through all three
configurations and the rollup, with `--numa-node 0 --jobs 3`. Each job took
25 to 27 minutes on a Neoverse N1 core: 7 minutes of branch-predictor
warmup, then the simulation. The trace list's own estimate for that trace
is 17 minutes, so expect its estimates (about 824 CPU-hours per
configuration for the datacenter suites) to be low on a similar host. The
full datacenter study has not been run to completion.

## Known problems

These are in the fork as of commit `6b60a6a`. They were found while testing
this README and are not fixed yet:

- **`--run parallel` without Slurm.** Every batch job ends by running
  `panthera cheetah.cp --push`, which asks Slurm for Ampere's cluster layout.
  On a machine without Slurm it exits with "Could not establish a
  configuration source", so each job is marked failed even when its
  simulation finished. `--run serial` skips that step, and so does
  `panthera cheetah.local` (see [step 6](#6-run-study-list-traces)).
- **Prefetcher-disable configs on Siryn.** `pv/all_prefetchers_disabled.cfg`
  stops with "override-cfg did not override any config-scope" because the
  Furiosa core has no GRETCH prefetcher, and `prefetchers/disable_all.cfg`
  stops with "Not all parameters were matched". The first one loads on
  Domino, which is what the prefetcher headroom study uses.
- **Relative `--workloads` path.** The study resolves it from inside the
  study directory, so the simulator reports the trace as missing. Pass an
  absolute path.
- **Perf Check 500 needs `--soc rogue`.** On `domino` (the default), 304 of
  500 fail with "Unexpectedly evicted valid line during L2C preload". On
  `siryn` and `nebula`, every test fails with "Unrecognized magic number for
  protobuf stream". `banshee` also failed every test in an 11-trace sample.
