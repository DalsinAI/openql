"""openql-fetch-roms: the catalogue is whole, and a ROM is kept only when the
download and the ROM inside it are the ones roms.json names."""
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
loader = importlib.machinery.SourceFileLoader("fetch", str(ROOT / "tools/openql-fetch-roms"))
spec = importlib.util.spec_from_loader("fetch", loader)
fetch = importlib.util.module_from_spec(spec)
loader.exec_module(fetch)


def zip_of(name: str, data: bytes) -> bytes:
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr("x/" + name, data)
    return b.getvalue()


class Catalogue(unittest.TestCase):
    def test_every_rom_is_whole(self):
        roms = json.loads((ROOT / "roms.json").read_text())["roms"]
        self.assertEqual({r["id"] for r in roms}, {"minerva", "smsqe"})
        for r in roms:
            for k in ("machine", "file", "url", "sha256", "member", "romSha256", "romBytes", "licence"):
                self.assertIn(k, r)
            self.assertTrue(r["url"].startswith("https://"))
            self.assertEqual(len(r["sha256"]), 64)
            self.assertEqual(len(r["romSha256"]), 64)

    def test_no_rom_is_in_the_repository(self):
        names = {r["member"] for r in fetch.catalogue()["roms"]} | {r["file"] for r in fetch.catalogue()["roms"]}
        found = [p for p in ROOT.rglob("*") if p.name in names or p.suffix.lower() in (".rom", ".bin")]
        self.assertEqual(found, [])


class Extract(unittest.TestCase):
    def rom(self, data: bytes, archive: bytes) -> dict:
        return {"id": "t", "member": "T.rom", "romBytes": len(data),
                "romSha256": hashlib.sha256(data).hexdigest(), "sha256": hashlib.sha256(archive).hexdigest()}

    def test_a_matching_download_gives_its_rom(self):
        data = b"\x00\x01" * 100
        archive = zip_of("T.rom", data)
        self.assertEqual(fetch.extract(self.rom(data, archive), archive), data)

    def test_another_download_is_refused(self):
        data = b"\x00\x01" * 100
        archive = zip_of("T.rom", data)
        rom = self.rom(data, archive)
        with self.assertRaises(ValueError):
            fetch.extract(rom, zip_of("T.rom", data + b"x"))

    def test_another_rom_inside_is_refused(self):
        data = b"\x00\x01" * 100
        archive = zip_of("T.rom", data + b"!")
        rom = self.rom(data, archive)
        with self.assertRaises(ValueError):
            fetch.extract(rom, archive)

    def test_a_refused_rom_leaves_nothing_behind(self):
        with tempfile.TemporaryDirectory() as d:
            bad = Path(d) / "bad.zip"
            bad.write_bytes(zip_of("Minerva_1.98j4.bin", b"not minerva"))
            self.assertEqual(fetch.main(["--dir", d, "--zip", str(bad), "minerva"]), 1)
            self.assertEqual(sorted(p.name for p in Path(d).iterdir()), ["bad.zip"])


if __name__ == "__main__":
    unittest.main()
