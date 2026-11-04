# Copyright (c) 2026 Ampere Computing. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

import os
import unittest
from unittest import mock

import pandas as pd
from base import ReportTest
from panthera.report import helpers
from panthera.report.__main__ import find_reports

DATES = ["2025-10-09", "2025-10-10", "2025-10-11"]
PRODUCTS = ["Siryn", "Banshee"]
DRIFT_COLUMNS = [
    f"{product} {suite}-gcc10 rate {rate}"
    for suite, rate in (("fp", 1), ("int", 1), ("int", "N"))
    for product in PRODUCTS
]

# The drift report compares against reference-machine scores for SPEC2017 fp
# rate 1 and int rate N, which are not distributed with Panthera. Stand in a
# flat score for each workload so the report's data handling can be exercised.
STANDIN_RATE_FILES = {
    "spec2017-gcc10_fp_rate_1.csv": ("spec2017-gcc10_fp_reference_data.csv", "Rate 1"),
    "spec2017-gcc10_int_rate_N.csv": ("spec2017-gcc10_int_reference_data.csv", "Rate N"),
}

real_read_data_file = helpers.read_data_file


def read_data_file_with_standins(filename: str) -> pd.DataFrame:
    try:
        return real_read_data_file(filename)
    except FileNotFoundError:
        if filename not in STANDIN_RATE_FILES:
            raise
    reference_file, configuration = STANDIN_RATE_FILES[filename]
    workloads = real_read_data_file(reference_file)["workload"].tolist() + ["Geomean"]
    return pd.DataFrame(
        {
            "Workload": workloads,
            "CPU": "Quicksilver",
            "Configuration": configuration,
            "Score": 5.0,
        }
    )


class TestDriftReport(ReportTest):
    @classmethod
    def rollup_args(cls):
        workloads = [
            workload
            for reference_file in ("spec2017-gcc10_int_reference_data.csv", "spec2017-gcc10_fp_reference_data.csv")
            for workload in real_read_data_file(reference_file)["workload"]
        ]
        return {
            "configs": PRODUCTS + [f"{product}-Rate-{rate}-gcc10" for product in PRODUCTS for rate in ("1", "N")],
            "workloads": workloads,
            "metrics": [
                "sys.soc_0.core_0.ipc",
                "sys.soc_0.core_0.gpc.cycle",
                "sys.soc_0.core_0.gpc.instruction_commit",
            ],
            "dates": DATES,
        }

    def test_weekly_drift_has_one_row_per_date(self):
        with mock.patch.object(helpers, "read_data_file", side_effect=read_data_file_with_standins):
            self.run_report(find_reports()["drift"])

        weekly = pd.read_csv(os.path.join(self.working_dir.name, "weekly_drift.workload.csv"))
        self.assertEqual(weekly["configuration"].tolist(), DATES)
        self.assertEqual(weekly.columns.tolist()[1:], DRIFT_COLUMNS)
        self.assertFalse(weekly.isna().any().any())


if __name__ == "__main__":
    unittest.main()
