# Copyright (c) 2026 Ampere Computing. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

import unittest

import pandas as pd
from panthera.matrix.annotations import ooc_to_annotation


def colormap(value, bytes=False):
    return (10, 20, 30, 255)


class TestOocToAnnotation(unittest.TestCase):
    def ooc(self, links):
        index = pd.MultiIndex.from_tuples([(0, link) for link in links], names=["die", "link"])
        return pd.DataFrame({"req": [12.5] * len(links)}, index=index)

    def test_device_links_are_labelled_without_color(self):
        ann = ooc_to_annotation(self.ooc(["x0y0.p1.to_dev3", "x0y0.p1.from_dev3"]), "req", colormap)
        for direction in ("to_dev3", "from_dev3"):
            link = ann.die0.x0y0.p1[direction]
            self.assertEqual(link.label, "12")
            self.assertNotIn("color", link)

    def test_mesh_links_are_labelled_and_colored(self):
        ann = ooc_to_annotation(self.ooc(["x0y0.south"]), "req", colormap)
        link = ann.die0.x0y0.south
        self.assertEqual(link.label, "12")
        self.assertEqual(link.color, "rgb(10 20 30)")


if __name__ == "__main__":
    unittest.main()
