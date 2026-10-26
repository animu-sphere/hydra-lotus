# SPDX-License-Identifier: Apache-2.0
import contextlib
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
from unittest.mock import patch

import material_fixtures as fixtures


class MaterialFixturesTest(unittest.TestCase):
    def test_committed_files_are_generated(self):
        for name, generate in fixtures.FIXTURES.items():
            content = generate()
            path = fixtures.DIRECTORY / name
            committed = (path.read_text(encoding="utf-8") if isinstance(content, str)
                         else path.read_bytes())
            self.assertEqual(committed, content, f"{name} is stale")

    def test_check_reports_stale_files(self):
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(fixtures, "DIRECTORY", Path(temp)):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    self.assertEqual(fixtures.main(["check"]), 1)
                self.assertIn("material-basics-v1.usda", stderr.getvalue())
                self.assertEqual(fixtures.main(["write"]), 0)
                self.assertEqual(fixtures.main(["check"]), 0)

    def test_images_decode_to_their_texels(self):
        for name, (width, height, channels, texel) in fixtures.IMAGES.items():
            data = fixtures.png(width, height, channels, texel)
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            chunks, offset = {}, 8
            while offset < len(data):
                (length,) = struct.unpack(">I", data[offset:offset + 4])
                kind = data[offset + 4:offset + 8]
                body = data[offset + 8:offset + 8 + length]
                (crc,) = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])
                self.assertEqual(crc, zlib.crc32(kind + body), name)
                chunks[kind] = body
                offset += 12 + length
            self.assertEqual(list(chunks), [b"IHDR", b"IDAT", b"IEND"], name)
            self.assertEqual(struct.unpack(">IIBB", chunks[b"IHDR"][:10]),
                             (width, height, 8, {1: 0, 2: 4, 3: 2, 4: 6}[channels]))
            rows = zlib.decompress(chunks[b"IDAT"])
            stride = 1 + width * channels
            for y in range(height):
                row = rows[y * stride:(y + 1) * stride]
                self.assertEqual(row[0], 0, name)
                for x in range(width):
                    self.assertEqual(tuple(row[1 + x * channels:1 + (x + 1) * channels]),
                                     texel(x, y), f"{name} at {x},{y}")

    def test_texture_cards_connect_to_their_nodes(self):
        text = fixtures.textures()
        for row in fixtures.TEXTURES:
            for name, material, _ in row:
                for authored in [*material["surface"].values(),
                                 *(v for _, inputs in material["nodes"].values()
                                   for v in inputs.values())]:
                    if isinstance(authored, tuple) and authored and authored[0] == "connect":
                        self.assertIn(authored[1], material["nodes"], name)
                        self.assertIn(f'def Shader "{authored[1]}"', text)
        self.assertEqual(len(fixtures.TEXTURES), len(fixtures.CARD_ROWS))
        for row in fixtures.TEXTURES:
            self.assertEqual(len(row), len(fixtures.CARD_COLUMNS))

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
