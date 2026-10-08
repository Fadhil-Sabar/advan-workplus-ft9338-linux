import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "extract-firmware.py"
spec = importlib.util.spec_from_file_location("extract_firmware", SCRIPT)
extract = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extract
spec.loader.exec_module(extract)


class ExtractFirmwareTests(unittest.TestCase):
    def make_elf(self, directory: Path) -> Path:
        path = directory / "fixture.so"
        data = bytearray(512)
        data[:16] = b"\x7fELF" + bytes((2, 1, 1)) + bytes(9)
        struct.pack_into("<HHI", data, 16, 3, 62, 1)  # ET_DYN, x86-64
        struct.pack_into("<Q", data, 40, 64)  # e_shoff
        struct.pack_into("<HHHHHH", data, 52, 64, 56, 0, 64, 3, 0)
        # Two valid file-backed sections. Section 1 contains the symbol at VA
        # 0x1000 + 0x20, backed at file offset 256.
        struct.pack_into("<IIQQQQIIQQ", data, 64 + 64,
                         0, 1, 0, 0x1000, 256, 64, 0, 0, 1, 0)
        struct.pack_into("<IIQQQQIIQQ", data, 64 + 128,
                         0, 1, 0, 0x2000, 320, 32, 0, 0, 1, 0)
        data[288:296] = b"firmware"
        path.write_bytes(data)
        return path

    @staticmethod
    def fake_nm(value="1020", size="8", status=0, stdout=None, stderr=""):
        return subprocess.CompletedProcess(["nm"], status,
                                            stdout if stdout is not None else f"{value} {size} R {extract.SYM}\n",
                                            stderr)

    def test_extracts_correct_bytes_and_writes_atomic_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            elf = self.make_elf(root)
            output = root / "out" / "firmware.h"
            with mock.patch.object(extract.subprocess, "run", return_value=self.fake_nm()):
                firmware = extract.read_firmware(elf)
                self.assertEqual(firmware, b"firmware")
                extract.write_header(firmware, output)
            header = output.read_text()
            self.assertIn("0x66, 0x69, 0x72, 0x6d", header)
            self.assertIn("ft9201_fw_size = 8U", header)
            self.assertEqual(list(output.parent.iterdir()), [output])

    def test_missing_ambiguous_bad_nm_and_nm_execution_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            elf = self.make_elf(Path(tmp))
            for result, message in (
                (self.fake_nm(stdout=""), "not found"),
                (self.fake_nm(stdout=f"1020 8 R {extract.SYM}\n1020 8 R {extract.SYM}\n"), "ambiguous"),
                (self.fake_nm(status=2, stderr="bad object"), "nm failed"),
            ):
                with self.subTest(message=message), mock.patch.object(extract.subprocess, "run", return_value=result):
                    with self.assertRaisesRegex(extract.ExtractionError, message):
                        extract.read_firmware(elf)
            with mock.patch.object(extract.subprocess, "run", side_effect=FileNotFoundError("nm missing")):
                with self.assertRaisesRegex(extract.ExtractionError, "could not run nm"):
                    extract.read_firmware(elf)

    def test_rejects_malformed_elf_and_symbol_outside_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad = root / "bad.so"
            bad.write_bytes(b"not elf")
            with self.assertRaisesRegex(extract.ExtractionError, "not a complete ELF"):
                extract.read_firmware(bad)
            elf = self.make_elf(root)
            with mock.patch.object(extract.subprocess, "run", return_value=self.fake_nm(value="103c", size="8")):
                with self.assertRaisesRegex(extract.ExtractionError, "extends beyond"):
                    extract.read_firmware(elf)

    def test_failed_replace_preserves_existing_output_and_removes_temp(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "firmware.h"
            output.write_text("old header")
            with mock.patch.object(extract.os, "replace", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"):
                    extract.write_header(b"data", output)
            self.assertEqual(output.read_text(), "old header")
            self.assertEqual(list(output.parent.iterdir()), [output])


if __name__ == "__main__":
    unittest.main()
