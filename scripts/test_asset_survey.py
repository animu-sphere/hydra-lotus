# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import tempfile
import unittest

import asset_survey as survey

PREFIX = "Warning: in Sync at line 1 of adapter.cpp -- "


class AssetSurveyTest(unittest.TestCase):
    def test_classifies_lotus_warnings(self):
        log = "\n".join([
            PREFIX + "Lotus uses the default material for /A: the material has no surface terminal",
            PREFIX + "Lotus uses the default material for /B: the surface shader is not UsdPreviewSurface",
            PREFIX + "Lotus uses the defaults of /C inputs whose shader graph it does not translate: diffuseColor, normal",
            PREFIX + "Lotus rejected mesh /D: face vertex index is out of range",
            PREFIX + "Lotus ignores the displayColor of /E: only constant interpolation is supported",
            PREFIX + "Lotus ignores the GeomSubset material bindings of /F: the mesh's own binding covers every face",
            "Warning: in _Read at line 2 of crateFile.cpp -- deprecated version",
            "Renderer plugin: HdLotusRendererPlugin",
        ])
        result = survey.classify_log(log)
        self.assertEqual(result["warnings"], {
            "no surface terminal": 1, "unsupported surface": 1,
            "untranslated connection": 1, "mesh rejected": 1,
            "displayColor ignored": 1, "GeomSubset bindings ignored": 1, "not Lotus": 1})
        self.assertEqual(result["details"]["input diffuseColor"], 1)
        self.assertEqual(result["details"]["input normal"], 1)
        self.assertEqual(result["details"]["mesh rejected: face vertex index is out of range"], 1)

    def test_records_paths_relative_to_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.assertEqual(survey.relative_name(root / "a" / "b.usda", root), "a/b.usda")
            self.assertEqual(survey.relative_name(Path(temp).parent / "c.usd", root), "c.usd")

    def test_reads_scalar_and_vector_values(self):
        self.assertEqual(survey.as_floats(0.5), [0.5])
        self.assertEqual(survey.as_floats((1, 0, 0)), [1.0, 0.0, 0.0])
        self.assertEqual(survey.as_floats("transparent"), [])
        self.assertEqual(survey.as_floats(None), [])

    def test_warnings_command_writes_one_entry_per_log(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / "stage.log"
            log.write_text(PREFIX + "Lotus rejected mesh /M: no points\n", encoding="utf-8")
            out = Path(temp) / "out.json"
            self.assertEqual(survey.main(["warnings", "--out", str(out), str(log)]), 0)
            data = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(data["stage"]["warnings"], {"mesh rejected": 1})


if __name__ == "__main__":
    unittest.main()
