#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause


"""
Run the jobs of an existing study on this machine with GNU parallel.

`panthera study ... --run parallel` ends every job by pushing its outputs to
Ampere's Slurm clusters, so on any other machine every job is marked failed.
This command runs the same job files without that step. Create the study
directory first with `panthera study ... --run parallel --dryrun`, then:

    panthera cheetah.local STUDY_DIRECTORY --jobs 64

A job counts as finished when its log reports that all commands succeeded.
Finished jobs are skipped, so running the command again continues an
interrupted study and retries the jobs that failed.
"""

import argparse
import json
import logging
import math
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

from panthera import install_dir, setup_logging

from .logfile import LogFile

PENDING_JOBS_FILE = "jobs-local.txt"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("study_directory", type=Path, help="study directory created by `panthera study --dryrun`")
    parser.add_argument(
        "--jobs",
        type=int,
        help="simultaneous jobs (default: three quarters of the CPUs the jobs may use)",
    )
    parser.add_argument(
        "--workloads",
        type=Path,
        help="trace list the study was created from. Runs each workload's highest-weight trace first, "
        "so an interrupted study still covers every workload",
    )
    parser.add_argument(
        "--numa-node",
        type=int,
        help="run the jobs only on this NUMA node's CPUs. Their memory comes from the same node "
        "while it has free pages",
    )
    parser.add_argument("--rerun", action="store_true", help="also run the jobs that already finished")
    parser.add_argument("--list", action="store_true", help="print the jobs that would run, in order, and exit")
    parser.add_argument("--debug", action="store_true", help="enable debug logging")
    return parser.parse_args(argv)


def read_job_list(study_directory: Path) -> list[str]:
    """Job files of the study, relative to the study directory."""
    with open(study_directory / "study.json") as study_file:
        job_list = Path(json.load(study_file)["job_list"])
    # The recorded path is absolute, so it is stale once the study directory moves.
    if not job_list.is_file():
        job_list = study_directory / job_list.name
    return [line.strip() for line in job_list.read_text().splitlines() if line.strip()]


def order_by_weight(jobs: list[str], traces: list[dict]) -> list[str]:
    """
    Order jobs so each workload's highest-weight trace comes first.

    All workloads' heaviest traces run before any workload's second-heaviest
    trace, and so on. The jobs of one trace (one per configuration) stay
    together. Jobs whose trace is not in the trace list run last.
    """
    by_workload = defaultdict(list)
    for trace in traces:
        by_workload[trace["workload"]].append(trace)

    rank = {}
    for workload, workload_traces in by_workload.items():
        workload_traces.sort(key=lambda trace: -trace.get("weight", 0.0))
        for position, trace in enumerate(workload_traces):
            rank[trace["name"]] = (position, workload)

    def key(indexed_job: tuple[int, str]):
        index, job = indexed_job
        name = Path(job).stem
        position, workload = rank.get(name, (math.inf, ""))
        return (position, workload, name, index)

    return [job for _index, job in sorted(enumerate(jobs), key=key)]


def is_finished(study_directory: Path, job: str) -> bool:
    log = LogFile.from_file(str((study_directory / job).with_suffix(".log")))
    return log.complete and log.error is None


def parse_cpu_list(cpu_list: str) -> set[int]:
    """Parse the kernel's CPU list format, for example `0-3,8,10-11`."""
    cpus = set()
    for part in cpu_list.strip().split(","):
        if not part:
            continue
        first, _dash, last = part.partition("-")
        cpus.update(range(int(first), int(last or first) + 1))
    return cpus


def numa_node_cpus(node: int) -> set[int]:
    path = Path(f"/sys/devices/system/node/node{node}/cpulist")
    if not path.is_file():
        raise ValueError(f"NUMA node {node} does not exist on this machine")
    return parse_cpu_list(path.read_text())


def parallel_command(pending_jobs_file: str, joblog: str, jobs: int) -> list[str]:
    run_job = os.path.join(install_dir, "bin", "panthera") + " --without-nix run_job --chdir"
    # The "+" appends to the joblog, which keeps the record of earlier runs.
    return ["parallel", "--joblog", f"+{joblog}", "--jobs", str(jobs), "-a", pending_jobs_file, run_job]


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    setup_logging(debug=args.debug)
    study_directory = args.study_directory.resolve()

    jobs = read_job_list(study_directory)
    if args.workloads:
        with open(args.workloads) as workloads_file:
            jobs = order_by_weight(jobs, json.load(workloads_file))

    pending = jobs if args.rerun else [job for job in jobs if not is_finished(study_directory, job)]
    logging.info(f"{len(jobs)} jobs in the study, {len(jobs) - len(pending)} already finished, {len(pending)} to run")
    if args.list:
        print("\n".join(pending))
        return 0
    if not pending:
        return 0

    if args.numa_node is not None:
        os.sched_setaffinity(0, numa_node_cpus(args.numa_node))
    usable_cpus = len(os.sched_getaffinity(0))
    simultaneous = args.jobs or max(1, usable_cpus * 3 // 4)
    logging.info(f"Running {simultaneous} jobs at a time on {usable_cpus} CPUs")

    (study_directory / PENDING_JOBS_FILE).write_text("\n".join(pending) + "\n")
    command = parallel_command(PENDING_JOBS_FILE, f"{study_directory.name}.joblog", simultaneous)
    logging.debug(" ".join(command))
    subprocess.run(command, cwd=study_directory)

    unfinished = [job for job in pending if not is_finished(study_directory, job)]
    logging.info(f"{len(pending) - len(unfinished)} of {len(pending)} jobs finished successfully")
    for job in unfinished[:20]:
        logging.error(f"Not finished: {Path(job).with_suffix('.log')}")
    if len(unfinished) > 20:
        logging.error(f"... and {len(unfinished) - 20} more; `--list` prints them all")
    return 1 if unfinished else 0


if __name__ == "__main__":
    sys.exit(main())
