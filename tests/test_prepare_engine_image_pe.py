import importlib.util
import struct
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "prepare-engine-image.py"
spec = importlib.util.spec_from_file_location("prepare_engine_image", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture():
    data = bytearray(0x400)
    struct.pack_into("<H", data, 0, 0x5A4D)
    struct.pack_into("<I", data, 0x3C, 0x80)
    struct.pack_into("<IHHIIIHH", data, 0x80, 0x4550, 0x8664, 1, 0, 0, 0, 0xF0, 0)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<I", data, opt + 56, 0x3000)
    struct.pack_into("<I", data, opt + 60, 0x200)
    sec = opt + 0xF0
    struct.pack_into("<IIII", data, sec + 8, 0x100, 0x1000, 0x200, 0x200)
    data[0x200:0x400] = bytes(range(256)) * 2
    return data


class PreparePETests(unittest.TestCase):
    def test_maps_section_by_rva(self):
        image = module.prepare(fixture())
        self.assertEqual(len(image), 0x3000)
        self.assertEqual(image[0x1000:0x1200], bytes(range(256)) * 2)

    def test_rejects_truncated_headers(self):
        for data in (b"", fixture()[:0x100]):
            with self.subTest(size=len(data)), self.assertRaises(module.InvalidPE):
                module.prepare(data)

    def test_rejects_bad_signature_and_architecture(self):
        data = fixture()
        struct.pack_into("<I", data, 0x80, 0)
        with self.assertRaises(module.InvalidPE):
            module.prepare(data)
        data = fixture()
        struct.pack_into("<H", data, 0x84, 0x14C)
        with self.assertRaises(module.InvalidPE):
            module.prepare(data)

    def test_rejects_section_ranges(self):
        data = fixture()
        struct.pack_into("<I", data, 0x188 + 12, 0x2F00)
        with self.assertRaises(module.InvalidPE):
            module.prepare(data)
        data = fixture()
        struct.pack_into("<I", data, 0x188 + 20, 0x500)
        with self.assertRaises(module.InvalidPE):
            module.prepare(data)


if __name__ == "__main__":
    unittest.main()
