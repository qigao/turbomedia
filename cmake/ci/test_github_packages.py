"""Transport/format regressions: no live credentials or package writes."""
import contextlib
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch
import zipfile

import github_packages as gp
from verify_nuget_upload import request_body


class PackagesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        gp.package_base.cache_clear()
        self.addCleanup(gp.package_base.cache_clear)
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
                if isinstance(name, str):
                    raw = zipfile.ZipInfo(name)
                    raw.filename = name  # keep malformed separators on Windows
                    name = raw
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

    def test_upload_config_matches_source_and_escapes_credentials(self):
        config = self.root / "publish.config"
        gp.publish_config(config, 'fake<&"token')
        root = ET.parse(config).getroot()
        source = root.find("packageSources/add").attrib["value"]
        self.assertEqual(source, root.find("config/add").attrib["value"])
        self.assertEqual(source, gp.FEED + "/index.json")
        creds = root.find("packageSourceCredentials/github")
        self.assertEqual({x.attrib["key"]: x.attrib["value"] for x in creds},
                         {"Username": "qigao", "ClearTextPassword": 'fake<&"token'})
        self.assertIsNone(root.find("apikeys"))
        with patch.dict(os.environ, GITHUB_TOKEN="fake-token", VCPKG_BINARY_SOURCES="clear",
                        GITHUB_REPOSITORY="qigao/turbomedia", X_VCPKG_NUGET_ID_PREFIX="turbomedia-media"):
            gp.producer_environment(config)
            self.assertEqual(os.environ["NUGET_API_KEY"], "fake-token")
            self.assertEqual(os.environ["VCPKG_NUGET_REPOSITORY"], "https://github.com/qigao/turbomedia")
            self.assertEqual(os.environ["VCPKG_BINARY_SOURCES"], f"clear;nugetconfig,{config},write")

    def test_publication_rejects_other_repository_or_shared_namespace(self):
        for repo, prefix in (("qigao/vcpkg-cache", "turbomedia-media"),
                             ("qigao/turbomedia", "vcpkg-cache"),
                             ("", "turbomedia-media")):
            with self.subTest(repo=repo, prefix=prefix), patch.dict(
                    os.environ, GITHUB_REPOSITORY=repo, X_VCPKG_NUGET_ID_PREFIX=prefix,
                    GITHUB_TOKEN="fake-token", VCPKG_BINARY_SOURCES="clear"):
                with self.assertRaises(ValueError):
                    gp.producer_environment(self.root / "publish.config")
                self.assertEqual(os.environ["VCPKG_BINARY_SOURCES"], "clear")

    def test_native_nuget_chunked_upload_body(self):
        headers = {"Transfer-Encoding": "chunked"}
        encoded = b"4\r\nPK\x03\x04\r\n7;extension=ok\r\npayload\r\n0\r\n\r\n"
        self.assertEqual(request_body(io.BytesIO(encoded), headers), b"PK\x03\x04payload")
        for truncated in (b"4", b"4\r\nPK", b"4\r\nPK\x03\x04!!"):
            with self.assertRaises(ValueError):
                request_body(io.BytesIO(truncated), headers)
        with self.assertRaises(ValueError):
            request_body(io.BytesIO(b"100001\r\n"), headers)

    def test_rejected_upload_stops_a_running_producer_before_more_builds(self):
        marker = self.root / "unwanted-build"
        script = """
import sys, time
from pathlib import Path
print('error: "C:/tools/nuget.exe" push package.nupkg -ConfigFile publish.config failed with exit code 1', flush=True)
print('Source parameter was not specified.', flush=True)
time.sleep(30)
Path(sys.argv[1]).write_text('must not reach another build')
"""
        started = time.monotonic()
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            with self.assertRaisesRegex(RuntimeError, "publication failed"):
                gp.run_producer([sys.executable, "-c", script, str(marker)])
        self.assertLess(time.monotonic() - started, 10)
        self.assertFalse(marker.exists())
        self.assertIn("Source parameter was not specified.", captured.getvalue())

    def test_producer_preserves_native_success_and_compile_failure(self):
        gp.run_producer([sys.executable, "-c", "print('Stored package in cache')"])
        with self.assertRaises(subprocess.CalledProcessError) as caught:
            gp.run_producer([sys.executable, "-c", "raise SystemExit(7)"])
        self.assertEqual(caught.exception.returncode, 7)

    def test_vcpkg_plan_identity_download_and_cache_only_install(self):
        package = self.package()
        calls = []
        cache = self.root / "cache"

        def native(args, **kwargs):
            calls.append(args)
            if "--dry-run" in args:
                self.assertEqual(kwargs["env"]["X_VCPKG_NUGET_ID_PREFIX"], prefix + "_")
                plan = Path(next(a.split("=", 1)[1] for a in args if str(a).startswith("--x-write-nuget-packages-config=")))
                plan.write_text(f'<packages><package id="{self.package_id}" version="{self.version}" /></packages>')
            else:
                self.assertIn("--only-binarycaching", args)
                self.assertTrue((cache / self.abi[:2] / f"{self.abi}.zip").is_file())

        def fetch(package_id, version, destination, **kwargs):
            self.assertEqual((package_id, version), (self.package_id, self.version))
            self.assertFalse(kwargs["allow_missing"])
            shutil.copyfile(package, destination)
            return True

        for triplet, prefix in (("x64-linux", "vcpkg-cache"), ("x64-windows", "turbomedia-media")):
            self.package_id = f"{prefix}_example_{triplet}"
            package = self.package()
            with self.subTest(triplet=triplet), patch.dict(
                    os.environ, VCPKG_BINARY_CACHE=str(cache), VCPKG_ROOT=str(self.root), RUNNER_TEMP=str(self.root)):
                with patch.object(gp, "run", side_effect=native), patch.object(gp, "download", side_effect=fetch):
                    gp.media(triplet, self.root / "installed")
        self.assertEqual(len(calls), 4)

    def test_windows_producer_only_requests_and_publishes_product_packages(self):
        package_id = "turbomedia-media_example_x64-windows"

        def plan(args, **kwargs):
            self.assertIn("--dry-run", args)
            self.assertEqual(kwargs["env"]["X_VCPKG_NUGET_ID_PREFIX"], "turbomedia-media_")
            path = Path(next(str(a).split("=", 1)[1] for a in args
                             if str(a).startswith("--x-write-nuget-packages-config=")))
            path.write_text(f'<packages><package id="{package_id}" version="{self.version}" /></packages>')

        def produce(args):
            self.assertNotIn("--only-binarycaching", args)
            self.assertEqual(os.environ["X_VCPKG_NUGET_ID_PREFIX"], "turbomedia-media")
            self.assertEqual(os.environ["VCPKG_NUGET_REPOSITORY"], "https://github.com/qigao/turbomedia")

        with patch.dict(os.environ, VCPKG_BINARY_CACHE=str(self.root / "cache"),
                        VCPKG_ROOT=str(self.root), RUNNER_TEMP=str(self.root),
                        GITHUB_REPOSITORY="qigao/turbomedia", GITHUB_TOKEN="fake-token"):
            with patch.object(gp, "os", SimpleNamespace(name="nt", environ=os.environ)), patch.object(
                    gp, "run", side_effect=plan) as native, patch.object(
                    gp, "download", return_value=False) as fetch, patch.object(
                    gp, "run_producer", side_effect=produce) as producer:
                gp.media("x64-windows", self.root / "installed", producer=True)
                self.assertEqual(native.call_count, 1)
                producer.assert_called_once()
                fetch.assert_called_once()
                self.assertEqual(fetch.call_args.args[:2], (package_id, self.version))
                self.assertTrue(fetch.call_args.kwargs["allow_missing"])

    def test_windows_consumer_rejects_shared_plan_before_download(self):
        def plan(args, **_):
            path = Path(next(str(a).split("=", 1)[1] for a in args
                             if str(a).startswith("--x-write-nuget-packages-config=")))
            path.write_text(f'<packages><package id="vcpkg-cache_example_x64-windows" version="{self.version}" /></packages>')

        with patch.dict(os.environ, VCPKG_BINARY_CACHE=str(self.root / "cache"),
                        VCPKG_ROOT=str(self.root), RUNNER_TEMP=str(self.root)):
            with patch.object(gp, "run", side_effect=plan), patch.object(gp, "download") as fetch:
                with self.assertRaisesRegex(ValueError, "namespace"):
                    gp.media("x64-windows", self.root / "installed")
                fetch.assert_not_called()

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
            output = Path(args[args.index("--output") + 1])
            if args[-1] == gp.FEED + "/index.json":
                output.write_text(json.dumps({"resources": [
                    {"@type": "PackageBaseAddress/3.0.0", "@id": gp.FEED + "/content/"}
                ]}))
            else:
                output.write_bytes(b"package")
            return subprocess.CompletedProcess(args, 0, "200", "")

        with patch.dict(os.environ, GITHUB_TOKEN="test-secret", GITHUB_ACTOR="test-actor"), patch.object(gp.subprocess, "run", side_effect=fake_curl):
            output = self.root / "package.nupkg"
            self.assertTrue(gp.download("Example", "1.2.3", output))
            self.assertEqual(output.read_bytes(), b"package")
            self.assertTrue(gp.download("Other", "2.0.0", output))
        self.assertEqual(len(seen), 3)  # index is reused for the second package
        for args, kwargs in seen:
            self.assertNotIn("test-secret", " ".join(args))
            self.assertIn(base64.b64encode(b"test-actor:test-secret").decode(), kwargs["input"])
            self.assertEqual(args[0], "curl")
            self.assertIn("--retry-max-time", args)
        self.assertEqual(seen[1][0][-1], gp.FEED + "/content/example/1.2.3/example.1.2.3.nupkg")
        self.assertFalse(output.with_suffix(".partial").exists())

    def test_service_index_rejects_untrusted_or_ambiguous_content_endpoints(self):
        for bases in ([], [gp.FEED + "/a", gp.FEED + "/b"],
                      ["http://nuget.pkg.github.com/qigao/download/"],
                      ["https://example.com/qigao/download/"],
                      ["https://nuget.pkg.github.com/other/download/"],
                      [gp.FEED + "/../other/"], [gp.FEED + "/download/?token=x"]):
            with self.subTest(bases=bases):
                gp.package_base.cache_clear()

                def index_download(url, output, *_):
                    Path(output).write_text(json.dumps({"resources": [
                        {"@type": "PackageBaseAddress/3.0.0", "@id": base} for base in bases
                    ]}))
                    return True

                with patch.object(gp, "curl_get", side_effect=index_download):
                    with self.assertRaises(ValueError):
                        gp.package_base("fake-token", "fake-actor")

    def test_service_index_404_is_fatal_even_for_producer(self):
        with patch.dict(os.environ, GITHUB_TOKEN="secret"), patch.object(
                gp.subprocess, "run", return_value=subprocess.CompletedProcess([], 22, "404", "")):
            with self.assertRaisesRegex(RuntimeError, "HTTP 404"):
                gp.download("example", "1.0.0", self.root / "missing", allow_missing=True)

    def test_only_explicit_producer_can_accept_404(self):
        for status, code in (("404", 22), ("401", 22), ("403", 22), ("500", 22), ("000", 28)):
            for producer in (False, True):
                with self.subTest(status=status, producer=producer), patch.dict(os.environ, GITHUB_TOKEN="secret"):
                    with patch.object(gp, "package_base", return_value=gp.FEED + "/content"), patch.object(
                            gp.subprocess, "run", return_value=subprocess.CompletedProcess([], code, status, "")):
                        if producer and status == "404":
                            self.assertFalse(gp.download("example", "1.0.0", self.root / "missing", allow_missing=True))
                        else:
                            with self.assertRaises(RuntimeError):
                                gp.download("example", "1.0.0", self.root / "missing", allow_missing=producer)


if __name__ == "__main__":
    unittest.main()
