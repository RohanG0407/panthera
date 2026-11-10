# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import io
import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from panthera.cheetah import local

SUCCESSFUL_LOG = "amperesys -cfg x.cfg\nCommand successful exit.\nAll commands exited successfully.\n"
FAILED_LOG = "amperesys -cfg x.cfg\nCommand error amperesys -cfg x.cfg exited with 1 status\n"


class TestOrderByWeight(unittest.TestCase):
    TRACES = [
        {"name": "a_1", "workload": "a", "weight": 0.2},
        {"name": "a_2", "workload": "a", "weight": 0.7},
        {"name": "a_3", "workload": "a", "weight": 0.1},
        {"name": "b.x_1", "workload": "b.x", "weight": 0.4},
        {"name": "b.x_2", "workload": "b.x", "weight": 0.6},
    ]

    def test_heaviest_trace_of_every_workload_first(self):
        jobs = [f"{config}/{trace['name']}.json" for config in ("base", "oracle") for trace in self.TRACES]
        self.assertEqual(
            local.order_by_weight(jobs, self.TRACES),
            [
                "base/a_2.json", "oracle/a_2.json", "base/b.x_2.json", "oracle/b.x_2.json",
                "base/a_1.json", "oracle/a_1.json", "base/b.x_1.json", "oracle/b.x_1.json",
                "base/a_3.json", "oracle/a_3.json",
            ],
        )  # fmt: skip

    def test_unknown_traces_run_last(self):
        jobs = ["base/zzz.json", "base/a_1.json", "base/other.json", "base/a_2.json"]
        self.assertEqual(
            local.order_by_weight(jobs, self.TRACES),
            ["base/a_2.json", "base/a_1.json", "base/other.json", "base/zzz.json"],
        )


class TestParseCpuList(unittest.TestCase):
    def test_ranges_and_single_cpus(self):
        self.assertEqual(local.parse_cpu_list("0-3,8,10-11\n"), {0, 1, 2, 3, 8, 10, 11})

    def test_empty(self):
        self.assertEqual(local.parse_cpu_list("\n"), set())


class TestStudyDirectory(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = TemporaryDirectory()
        self.study = Path(self.temporary_directory.name) / "study"
        (self.study / "base").mkdir(parents=True)
        self.jobs = ["base/done.json", "base/failed.json", "base/not-run.json"]
        (self.study / "jobs.txt").write_text("\n".join(self.jobs) + "\n")
        # As if the study was created somewhere else and then moved here.
        (self.study / "study.json").write_text(json.dumps({"job_list": "/elsewhere/study/jobs.txt"}))
        (self.study / "base" / "done.log").write_text(SUCCESSFUL_LOG)
        (self.study / "base" / "failed.log").write_text(FAILED_LOG)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_read_job_list_survives_a_moved_study(self):
        self.assertEqual(local.read_job_list(self.study), self.jobs)

    def test_only_a_successful_log_counts_as_finished(self):
        self.assertEqual([local.is_finished(self.study, job) for job in self.jobs], [True, False, False])

    def list_jobs(self, *options: str) -> list[str]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(local.main([str(self.study), "--list", *options]), 0)
        return output.getvalue().split()

    def test_list_skips_finished_jobs(self):
        self.assertEqual(self.list_jobs(), ["base/failed.json", "base/not-run.json"])

    def test_rerun_includes_finished_jobs(self):
        self.assertEqual(self.list_jobs("--rerun"), self.jobs)


class TestParallelCommand(unittest.TestCase):
    def test_runs_jobs_without_the_copy_step(self):
        command = local.parallel_command("jobs-local.txt", "study.joblog", 12)
        self.assertEqual(command[:7], ["parallel", "--joblog", "+study.joblog", "--jobs", "12", "-a", "jobs-local.txt"])
        self.assertTrue(command[-1].endswith("bin/panthera --without-nix run_job --chdir"))


if __name__ == "__main__":
    unittest.main()
