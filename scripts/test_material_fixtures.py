# SPDX-License-Identifier: Apache-2.0
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import material_fixtures as fixtures


class MaterialFixturesTest(unittest.TestCase):
    def test_committed_files_are_generated(self):
        for name, generate in fixtures.FIXTURES.items():
            committed = (fixtures.DIRECTORY / name).read_text(encoding="utf-8")
            self.assertEqual(committed, generate(), f"{name} is stale")

    def test_check_reports_stale_files(self):
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(fixtures, "DIRECTORY", Path(temp)):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    self.assertEqual(fixtures.main(["check"]), 1)
                self.assertIn("material-basics-v1.usda", stderr.getvalue())
                self.assertEqual(fixtures.main(["write"]), 0)
                self.assertEqual(fixtures.main(["check"]), 0)

    def test_fixed_spelling(self):
        self.assertEqual(fixtures.fixed(0.5), "0.500000")
        self.assertEqual(fixtures.fixed(-1e-9), "0.000000")
        self.assertEqual(fixtures.fixed(-0.1234567), "-0.123457")
        self.assertEqual(fixtures.fixed(1.0), "1.000000")

    def test_sphere_faces_wind_outwards(self):
        points, counts, indices = fixtures.sphere()
        self.assertEqual(len(points), 2 + (fixtures.RINGS - 1) * fixtures.SEGMENTS)
        self.assertEqual(sum(counts), len(indices))
        first = 0
        for count in counts:
            face = [points[i] for i in indices[first:first + count]]
            first += count
            a, b, c = face[0], face[1], face[2]
            u = [b[k] - a[k] for k in range(3)]
            v = [c[k] - a[k] for k in range(3)]
            normal = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                      u[0] * v[1] - u[1] * v[0])
            centre = [sum(p[k] for p in face) / count for k in range(3)]
            self.assertGreater(sum(n * x for n, x in zip(normal, centre)), 0)


if __name__ == "__main__":
    unittest.main()
