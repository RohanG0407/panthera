# Copyright (c) 2025 Ampere Computing. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

import contextlib
import io
import os
import unittest

from panthera.cheetah.xml_report import XMLReport

FIXTURE_PATH = os.path.join(os.path.dirname(os.path.dirname(__file__)), "fixtures")


class TestXMLReport(unittest.TestCase):
    def fixture(self, name):
        return os.path.join(FIXTURE_PATH, "xml_reports", f"{name}.xml")


class TestAllPassingXMLReport(TestXMLReport):
    def setUp(self) -> None:
        self.report = XMLReport.from_xml(self.fixture("all_passed"))

    def test_summary(self):
        self.assertEqual(
            self.report.summary(),
            {
                "test": {
                    "jobs": 3,
                    "errors": 0,
                    "incompletes": 0,
                }
            },
        )

    def test_errors(self):
        self.assertEqual(self.report.errors(), {})

    def test_print_summary(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.report.print_summary()
        self.assertEqual(
            output.getvalue(),
            "Report for: test\nNumber of jobs: 3\nNumber of errors: 0\nNumber of incompletes: 0\n",
        )


class TestSomeIncompleteXMLReport(TestXMLReport):
    def setUp(self) -> None:
        self.report = XMLReport.from_xml(self.fixture("some_incomplete"))

    def test_summary(self):
        self.assertEqual(
            self.report.summary(),
            {
                "test": {
                    "jobs": 3,
                    "errors": 0,
                    "incompletes": 1,
                }
            },
        )

    def test_errors(self):
        self.assertEqual(self.report.errors(), {})


class TestSomeFailingXMLReport(TestXMLReport):
    def setUp(self) -> None:
        self.report = XMLReport.from_xml(self.fixture("failures"))

    def test_summary(self):
        self.assertEqual(
            self.report.summary(),
            {
                "test": {
                    "jobs": 3,
                    "errors": 2,
                    "incompletes": 0,
                }
            },
        )

    def test_errors(self):
        self.assertEqual(
            self.report.errors(),
            {
                "test/config_0/dhrystone": "some error",
                "test/config_2/dhrystone": "some warning",
            },
        )
