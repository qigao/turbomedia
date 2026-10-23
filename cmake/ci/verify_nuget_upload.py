#!/usr/bin/env python3
"""Exercise the actual Windows NuGet upload client with loopback-only fake keys.

No GitHub credentials or package writes are used. The fake V3 feed challenges
for Basic credentials and checks the API key on the multipart PUT, matching
the source-index / publish-endpoint split used by GitHub Packages.
"""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import xml.etree.ElementTree as ET
import zipfile

from github_packages import publish_config


def verify(nuget):
    token = "loopback-upload-fixture-only"
    auth = "Basic " + base64.b64encode(f"qigao:{token}".encode()).decode()
    evidence = set()
    failures = []

    class Feed(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def respond(self, code, body=b"", challenge=False):
            self.send_response(code)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Content-Type", "application/json")
            if challenge:
                self.send_header("WWW-Authenticate", 'Basic realm="upload-fixture"')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path != "/qigao/index.json":
                failures.append("unexpected service-index path")
                self.respond(404)
                return
            if self.headers.get("Authorization") != auth:
                self.respond(401, challenge=True)
                return
            evidence.add("basic-source-credentials")
            self.respond(200, json.dumps({"version": "3.0.0", "resources": [
                {"@id": feed + "/", "@type": "PackagePublish/2.0.0"}
            ]}).encode())

        def do_PUT(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            if self.path != "/qigao/":
                failures.append("unexpected publish endpoint")
            if self.headers.get("X-NuGet-ApiKey") != token:
                failures.append("missing or incorrect API key header")
            if ("multipart/form-data" not in self.headers.get("Content-Type", "") or
                    b"package.nupkg" not in body or b"PK\x03\x04" not in body):
                failures.append("invalid NuGet multipart package body")
            evidence.add("publish-put-api-key-multipart")
            self.respond(403 if failures else 201)

    with ThreadingHTTPServer(("127.0.0.1", 0), Feed) as server:
        feed = f"http://127.0.0.1:{server.server_port}/qigao"
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with tempfile.TemporaryDirectory(prefix="tm-upload-wire-") as temp:
                temp = Path(temp)
                config = temp / "publish.config"
                publish_config(config, token, feed)
                # Only this fake loopback feed permits HTTP; production is HTTPS.
                tree = ET.parse(config)
                tree.getroot().find("packageSources/add").set("allowInsecureConnections", "true")
                tree.write(config, encoding="utf-8", xml_declaration=True)
                package = temp / "upload.fixture.1.0.0.nupkg"
                with zipfile.ZipFile(package, "w") as archive:
                    archive.writestr("upload.fixture.nuspec", """<?xml version="1.0"?>
<package><metadata><id>upload.fixture</id><version>1.0.0</version>
<authors>upload fixture</authors><description>loopback only</description>
</metadata></package>""")
                    archive.writestr("fixture.txt", "loopback upload fixture")
                env = {k: v for k, v in os.environ.items()
                       if k.upper() not in {"GITHUB_TOKEN", "GH_TOKEN", "NUGET_API_KEY"}
                       and not k.upper().startswith("NUGETPACKAGESOURCECREDENTIALS_")}
                env["NUGET_API_KEY"] = token
                result = subprocess.run([
                    str(nuget), "push", "-ForceEnglishOutput", "-Verbosity", "detailed",
                    "-NonInteractive", str(package), "-Timeout", "15", "-ConfigFile", str(config),
                    "-AllowInsecureConnections", "-NoSymbols",
                ], env=env, cwd=temp, capture_output=True, text=True, timeout=30)
                if result.returncode or failures or len(evidence) != 2:
                    # The client sees only fake keys, never live credentials.
                    raise RuntimeError(f"NuGet loopback upload failed: {failures}; "
                                       f"evidence={sorted(evidence)}\n{result.stdout}\n{result.stderr}")
                print("Actual NuGet upload PASS: source Basic authentication, API-key header, "
                      "V3 publish endpoint and multipart package verified", flush=True)
        finally:
            server.shutdown()
            worker.join(timeout=2)


if __name__ == "__main__":
    if os.name != "nt":
        raise RuntimeError("native NuGet upload qualification is Windows-only")
    vcpkg = Path(os.environ["VCPKG_ROOT"]) / "vcpkg.exe"
    resolved = subprocess.check_output([str(vcpkg), "fetch", "nuget"], text=True)
    verify(Path(resolved.strip().splitlines()[-1].strip('"')))
