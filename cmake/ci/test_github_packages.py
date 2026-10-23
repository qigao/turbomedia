"""Transport/format regressions: no live credentials or package writes."""
import hashlib
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import github_packages as gp


class PackagesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.abi_info = b"portfile 123\ntriplet 456\n"
        self.abi = hashlib.sha256(self.abi_info).hexdigest()
        self.package_id = "vcpkg-cache_example_x64-linux"
        self.version = f"1.2.3-vcpkg{self.abi}"

    def package(self, extra=(), abi_info=None, dep=""):
        path = self.root / "fixture.nupkg"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("example.nuspec", f'<package><metadata><id>{self.package_id}</id>'
                             f'<version>{self.version}</version>{dep}</metadata></package>')
            archive.writestr("share/example/vcpkg_abi_info.txt", self.abi_info if abi_info is None else abi_info)
            archive.writestr("CONTROL", "Package: example\nArchitecture: x64-linux\n")
            archive.writestr("[Content_Types].xml", "envelope")
            archive.writestr("_rels/.rels", "envelope")
            tool = zipfile.ZipInfo("tools/example/bin/tool")
            tool.external_attr = (stat.S_IFREG | 0o755) << 16
            archive.writestr(tool, b"executable")
            for name, value in extra:
                archive.writestr(name, value)
        return path

    def test_native_cache_keeps_payload_and_executable_mode(self):
        gp.to_file_cache(self.package(), self.root / "cache", self.package_id, self.version)
        native = self.root / "cache" / self.abi[:2] / f"{self.abi}.zip"
        with zipfile.ZipFile(native) as archive:
            self.assertIn("CONTROL", archive.namelist())
            self.assertNotIn("example.nuspec", archive.namelist())
            self.assertNotIn("[Content_Types].xml", archive.namelist())
            self.assertNotIn("_rels/.rels", archive.namelist())
            self.assertEqual(archive.read("share/example/vcpkg_abi_info.txt"), self.abi_info)
            self.assertTrue(archive.getinfo("tools/example/bin/tool").external_attr >> 16 & 0o111)

    def test_wrong_abi_cannot_poison_native_cache(self):
        with self.assertRaisesRegex(ValueError, "ABI mismatch"):
            gp.to_file_cache(self.package(abi_info=b"wrong"), self.root / "cache", self.package_id, self.version)
        self.assertFalse(list((self.root / "cache").rglob("*.zip")))

    def test_identity_and_sha256_checked_before_extraction(self):
        package = self.package()
        for package_id, digest in (("other", None), (self.package_id, "0" * 64)):
            with self.subTest(package_id=package_id), self.assertRaises(ValueError):
                gp.extract(package, self.root / "sdk", package_id, self.version, digest)
        self.assertFalse((self.root / "sdk").exists())

    def test_archive_escape_alias_and_symlink_rejected_before_extraction(self):
        unsafe = ["../escape", "/absolute", "C:/absolute", "dir\\escape", "a/../escape",
                  "a//escape", "a./escape", "control", "dir/trailing "]
        for name in unsafe:
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    gp.extract(self.package([(name, b"bad")]), self.root / "sdk", self.package_id, self.version)
                self.assertFalse((self.root / "sdk").exists())
        info = zipfile.ZipInfo("link")
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with self.assertRaises(ValueError):
            gp.extract(self.package([(info, b"../escape")]), self.root / "sdk", self.package_id, self.version)

    def test_lock_graph_rejects_missing_dependency_and_incompatible_version(self):
        package = self.package(dep='<dependencies><dependency id="Salts.Native" version="[2.3.0-rc.15,)" /></dependencies>')
        for pins in ({}, {"salts.native": "2.3.0-rc.14"}):
            with self.assertRaisesRegex(ValueError, "outside locked graph"):
                gp.check_dependencies(package, self.package_id, self.version, pins)
        gp.check_dependencies(package, self.package_id, self.version, {"salts.native": "2.3.0-rc.15"})
        self.assertTrue(gp.satisfies("2.3.0-rc.15", "[2.3.0-rc.2,2.3.0)"))
        self.assertFalse(gp.satisfies("2.3.0", "[2.3.0-rc.2,2.3.0)"))

    def test_curl_uses_stdin_credentials_and_atomic_output(self):
        seen = []

        def fake_curl(args, **kwargs):
            seen.append((args, kwargs))
            Path(args[args.index("--output") + 1]).write_bytes(b"package")
            return subprocess.CompletedProcess(args, 0, "200", "")

        with patch.dict(os.environ, GITHUB_TOKEN="test-secret"), patch.object(gp.subprocess, "run", side_effect=fake_curl):
            output = self.root / "package.nupkg"
            self.assertTrue(gp.download("Example", "1.2.3", output))
            self.assertEqual(output.read_bytes(), b"package")
        args, kwargs = seen[0]
        self.assertNotIn("test-secret", " ".join(args))
        self.assertIn("Authorization: Basic", kwargs["input"])
        self.assertEqual(args[0], "curl")
        self.assertIn("https://nuget.pkg.github.com/qigao/download/example/1.2.3/example.1.2.3.nupkg", args)
        self.assertFalse(output.with_suffix(".partial").exists())

    def test_only_explicit_producer_can_accept_404(self):
        for status, code in (("404", 22), ("401", 22), ("403", 22), ("500", 22), ("000", 28)):
            for producer in (False, True):
                with self.subTest(status=status, producer=producer), patch.dict(os.environ, GITHUB_TOKEN="secret"):
                    with patch.object(gp.subprocess, "run", return_value=subprocess.CompletedProcess([], code, status, "")):
                        if producer and status == "404":
                            self.assertFalse(gp.download("example", "1.0.0", self.root / "missing", allow_missing=True))
                        else:
                            with self.assertRaises(RuntimeError):
                                gp.download("example", "1.0.0", self.root / "missing", allow_missing=producer)


if __name__ == "__main__":
    unittest.main()
