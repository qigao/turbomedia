#!/usr/bin/env python3
"""Download GitHub Packages with curl; adapt vcpkg NuGet payloads to its files cache.

No NuGet resolver or managed runtime is used on the download path. vcpkg itself
computes package IDs/versions/ABIs via its dry-run packages.config output.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import queue
import re
import shutil
import signal
import stat
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FEED = "https://nuget.pkg.github.com/qigao"
IDENTITY = re.compile(r"[a-zA-Z0-9_.+-]+\Z")


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def run_producer(args):
    """vcpkg treats failed cache uploads as warnings; production must fail.

    Preserve the error's following diagnostics, then stop our process tree so a
    rejected upload cannot be hidden behind another hour of dependency builds.
    """
    lines = queue.Queue()
    with subprocess.Popen([str(a) for a in args], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                          errors="replace", bufsize=1, start_new_session=os.name != "nt") as process:
        def read_output():
            try:
                for line in process.stdout:
                    lines.put(line)
            finally:
                lines.put(None)

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        failed_at = None
        try:
            while True:
                if failed_at is not None and time.monotonic() >= failed_at + 1:
                    break
                timeout = None if failed_at is None else max(0, failed_at + 1 - time.monotonic())
                try:
                    line = lines.get(timeout=timeout)
                except queue.Empty:
                    break
                if line is None:
                    break
                print(line, end="", flush=True)
                lower = line.lower()
                if (failed_at is None and lower.startswith("error: ") and
                        "nuget.exe" in lower and " push " in lower and "failed with exit code" in lower):
                    failed_at = time.monotonic()
            if failed_at is not None:
                raise RuntimeError("NuGet cache publication failed; stopped dependency production")
            code = process.wait()
            if code:
                raise subprocess.CalledProcessError(code, args)
        finally:
            if process.poll() is None:
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                else:
                    os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=10)
            reader.join(timeout=2)


def download(package_id, version, output, *, allow_missing=False):
    """Only an explicit producer may treat HTTP 404 as a cache miss."""
    if not IDENTITY.fullmatch(package_id) or not IDENTITY.fullmatch(version):
        raise ValueError("invalid package identity")
    token = os.environ["GITHUB_TOKEN"]
    if not token:
        raise ValueError("GITHUB_TOKEN is empty")
    auth = base64.b64encode(f"qigao:{token}".encode()).decode()
    package_id, version = package_id.lower(), version.lower()
    url = f"{FEED}/download/{package_id}/{version}/{package_id}.{version}.nupkg"
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_suffix(".partial")
    try:
        # Config is supplied on stdin, never argv, logs or a credential file.
        # curl does not forward Authorization across redirect hosts.
        result = subprocess.run([
            "curl", "--disable", "--config", "-", "--fail", "--location",
            "--proto", "=https", "--proto-redir", "=https", "--silent", "--show-error",
            "--connect-timeout", "30", "--max-time", "1800",
            "--output", str(partial), "--write-out", "%{http_code}", url,
        ], input=f'header = "Authorization: Basic {auth}"\n',
            text=True, capture_output=True, check=False)
        status = result.stdout.strip()
        if result.returncode:
            if allow_missing and result.returncode == 22 and status == "404":
                return False
            # curl stderr can contain server responses; report only status/code.
            raise RuntimeError(f"curl download failed: {package_id}/{version} "
                               f"HTTP {status}, exit {result.returncode}")
        if status != "200":
            raise RuntimeError(f"unexpected package HTTP status: {status}")
        partial.replace(output)
        print(f"curl downloaded {package_id}/{version}", flush=True)
        return True
    finally:
        partial.unlink(missing_ok=True)


def members(archive):
    """Validate the entire ZIP before materializing any files, on every OS."""
    seen = set()
    for info in archive.infolist():
        # ZipInfo normalizes Windows backslashes and truncates NULs in filename;
        # validate the original archive name before either transformation.
        name = info.orig_filename
        path = PurePosixPath(name)
        mode = info.external_attr >> 16
        if ("\\" in name or "\0" in name or ":" in name or path.is_absolute() or
                not path.parts or any(p in ("", "..", ".") or p.endswith((" ", "."))
                                      for p in name.rstrip("/").split("/")) or
                stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in
                                      (0, stat.S_IFREG, stat.S_IFDIR))):
            raise ValueError(f"unsafe package member: {name}")
        key = name.rstrip("/").casefold()
        if key in seen:
            raise ValueError(f"duplicate package member: {name}")
        seen.add(key)
        yield info


def metadata(archive, package_id, version):
    specs = [n for n in archive.namelist() if "/" not in n and n.endswith(".nuspec")]
    if len(specs) != 1:
        raise ValueError("package must contain exactly one root nuspec")
    root = ET.fromstring(archive.read(specs[0]))
    values = {n.tag.rsplit("}", 1)[-1]: n.text for n in root.iter()}
    if (values.get("id", "").lower() != package_id.lower() or
            values.get("version", "").lower() != version.lower()):
        raise ValueError(f"package identity mismatch: {package_id}/{version}")
    return root


def version_key(value):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?", value)
    if not match:
        raise ValueError(f"unsupported NuGet version: {value}")
    prerelease = tuple((0, int(p)) if p.isdigit() else (1, p.lower())
                       for p in (match[4] or "").split("."))
    return (*(int(match[i]) for i in (1, 2, 3)), not bool(match[4]), prerelease)


def satisfies(version, requirement):
    actual = version_key(version)
    if requirement.startswith(("[", "(")):
        if not requirement.endswith(("]", ")")):
            raise ValueError("malformed NuGet version range")
        bounds = requirement[1:-1].split(",")
        if len(bounds) == 1 and requirement.startswith("[") and requirement.endswith("]"):
            return actual == version_key(bounds[0].strip())
        if len(bounds) != 2:
            raise ValueError("unsupported NuGet version range")
        low, high = [b.strip() for b in bounds]
        return ((not low or (actual >= version_key(low) if requirement[0] == "[" else actual > version_key(low))) and
                (not high or (actual <= version_key(high) if requirement[-1] == "]" else actual < version_key(high))))
    return actual >= version_key(requirement)


def check_dependencies(package, package_id, version, pins):
    with zipfile.ZipFile(package) as archive:
        root = metadata(archive, package_id, version)
        for node in root.iter():
            if node.tag.rsplit("}", 1)[-1] != "dependency":
                continue
            dep = node.attrib["id"].lower()
            if dep not in pins or not satisfies(pins[dep], node.attrib.get("version", "")):
                raise ValueError(f"dependency outside locked graph: {package_id} -> {dep}")


def extract(package, destination, package_id, version, digest=None):
    if digest and hashlib.sha256(Path(package).read_bytes()).hexdigest() != digest:
        raise ValueError(f"locked SHA256 mismatch: {package_id}/{version}")
    with zipfile.ZipFile(package) as archive:
        entries = list(members(archive))
        metadata(archive, package_id, version)
        for info in entries:
            target = Path(destination).joinpath(*PurePosixPath(info.filename).parts)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as source, target.open("wb") as out:
                    shutil.copyfileobj(source, out)
                # NuGet may omit Unix mode bits; SDK host tools live in bin/.
                if info.external_attr >> 16 & 0o111 or "bin" in target.parts:
                    target.chmod(target.stat().st_mode | 0o111)


def to_file_cache(package, cache, package_id, version):
    match = re.fullmatch(r".+-vcpkg([0-9a-f]{64})", version)
    if not match:
        raise ValueError("vcpkg plan lacks an exact ABI")
    abi = match[1]
    target = Path(cache) / abi[:2] / f"{abi}.zip"
    target.parent.mkdir(parents=True, exist_ok=True)
    pending = target.with_suffix(".partial")
    try:
        with zipfile.ZipFile(package) as source:
            entries = list(members(source))
            metadata(source, package_id, version)
            abi_files = [e for e in entries if re.fullmatch(r"share/[^/]+/vcpkg_abi_info.txt", e.filename)]
            if len(abi_files) != 1 or hashlib.sha256(source.read(abi_files[0])).hexdigest() != abi:
                raise ValueError(f"vcpkg payload ABI mismatch: {package_id}/{version}")
            with zipfile.ZipFile(pending, "w", zipfile.ZIP_DEFLATED) as out:
                for info in entries:
                    name = info.filename
                    # Strip only NuGet envelope files; retain CONTROL, BUILD_INFO
                    # and the original native package tree and attributes.
                    if (name in ("[Content_Types].xml", ".signature.p7s") or
                            name.startswith(("_rels/", "package/")) or
                            ("/" not in name and name.endswith(".nuspec"))):
                        continue
                    with source.open(info) as src, out.open(info, "w") as dst:
                        shutil.copyfileobj(src, dst)
        pending.replace(target)
    finally:
        pending.unlink(missing_ok=True)


def restore_sdks(rid):
    lock = json.loads((HERE / "m16-sdk-lock.json").read_text())
    packages = Path(os.environ["RUNNER_TEMP"]) / "turbomedia-platform-sdk-packages"
    shutil.rmtree(packages, ignore_errors=True)
    pins = {key: value for key, value in lock["packages"].items()
            if key != "RulesForge.Native" or rid in ("linux-x64", "windows-x64")}
    for package_id, pin in pins.items():
        root = packages / package_id.lower() / pin["version"]
        package = root / f'{package_id.lower()}.{pin["version"]}.nupkg'
        download(package_id, pin["version"], package)
        extract(package, root, package_id, pin["version"], pin["sha256"])
        check_dependencies(package, package_id, pin["version"],
                           {key.lower(): value["version"] for key, value in pins.items()})


def restore_re2c():
    version = json.loads((HERE / "m16-sdk-lock.json").read_text())["re2c_version"]
    host = {("Linux", "X64"): "linux-x64", ("Linux", "ARM64"): "linux-arm64",
            ("macOS", "X64"): "macos-x64", ("macOS", "ARM64"): "macos-arm64",
            ("Windows", "X64"): "windows-x64"}[(os.environ["RUNNER_OS"], os.environ["RUNNER_ARCH"])]
    root = Path(os.environ["RUNNER_TEMP"]) / "qigao-re2c-binary" / version
    shutil.rmtree(root, ignore_errors=True)
    package = root / f"qigao.re2c.binary.{version}.nupkg"
    download("Qigao.Re2c.Binary", version, package)
    extract(package, root, "Qigao.Re2c.Binary", version)
    root = root / "tools" / host
    exe = root / "bin" / ("re2c.exe" if os.name == "nt" else "re2c")
    for path in (exe, root / "share/re2c/stdlib/unicode_categories.re",
                 root / "share/re2c/stdlib/unicode_properties.re"):
        if not path.is_file():
            raise ValueError(f"missing re2c contract: {path}")
    run([exe, "--version"])
    export({"RE2C_ROOT": root, "RE2C_PACKAGE_VERSION": version})
    with open(os.environ["GITHUB_PATH"], "a", encoding="utf-8") as out:
        out.write(f"{root / 'bin'}\n")


def export(values):
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as out:
        for key, value in values.items():
            if "\n" in str(value) or "\r" in str(value):
                raise ValueError("invalid workflow environment value")
            out.write(f"{key}={value}\n")
    os.environ.update({key: str(value) for key, value in values.items()})


def publish_config(path, token, feed=FEED):
    """Use one matching source URL for push resolution and HTTP credentials."""
    root = ET.Element("configuration")
    settings = ET.SubElement(root, "config")
    ET.SubElement(settings, "add", key="defaultPushSource", value=f"{feed}/index.json")
    sources = ET.SubElement(root, "packageSources")
    ET.SubElement(sources, "clear")
    ET.SubElement(sources, "add", key="github", value=f"{feed}/index.json")
    creds = ET.SubElement(ET.SubElement(root, "packageSourceCredentials"), "github")
    ET.SubElement(creds, "add", key="Username", value="qigao")
    ET.SubElement(creds, "add", key="ClearTextPassword", value=token)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def producer_environment(config):
    # This metadata links the published package to the building repository.
    # A product GITHUB_TOKEN must not claim the shared infrastructure repo.
    os.environ["NUGET_API_KEY"] = os.environ["GITHUB_TOKEN"]
    os.environ["VCPKG_NUGET_REPOSITORY"] = "https://github.com/qigao/turbomedia"
    os.environ["VCPKG_BINARY_SOURCES"] += f";nugetconfig,{config},write"


def media(triplet, installed, producer=False):
    cache = Path(os.environ["VCPKG_BINARY_CACHE"])
    shutil.rmtree(installed, ignore_errors=True)
    # Always download this plan afresh, including the independent producer check.
    shutil.rmtree(cache, ignore_errors=True)
    cache.mkdir(parents=True)
    vcpkg = Path(os.environ["VCPKG_ROOT"]) / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    args = [vcpkg, "install", f"--x-manifest-root={REPO}",
            f"--x-install-root={installed}", f"--triplet={triplet}"]
    os.environ["VCPKG_BINARY_SOURCES"] = f"clear;files,{cache},readwrite"
    with tempfile.TemporaryDirectory(prefix="tm-media-", dir=os.environ["RUNNER_TEMP"]) as tmp:
        tmp = Path(tmp)
        plan = tmp / "packages.config"
        # This pinned tool's NuGet provider appends '_' to the configured prefix,
        # while packages.config generation takes the raw prefix unchanged.
        # Normalize for the dry-run only; publication keeps the original prefix.
        run([*args, "--dry-run", f"--x-write-nuget-packages-config={plan}"],
            env={**os.environ, "X_VCPKG_NUGET_ID_PREFIX": "vcpkg-cache_"})
        entries = ET.parse(plan).getroot().findall("package")
        if not entries:
            raise ValueError("empty vcpkg restore plan")
        for entry in entries:
            package_id, version = entry.attrib["id"], entry.attrib["version"]
            if not re.fullmatch(r"vcpkg-cache_[a-z0-9-]+_[a-z0-9-]+", package_id):
                raise ValueError(f"unexpected vcpkg cache namespace: {package_id}")
            package = tmp / f"{package_id}.{version}.nupkg"
            if download(package_id, version, package, allow_missing=producer):
                to_file_cache(package, cache, package_id, version)
        if producer:
            if os.name != "nt":
                raise ValueError("the explicit media producer is Windows-only")
            # The existing native Windows NuGet client is used ONLY to publish.
            # All reads use curl; Linux/macOS never invoke NuGet.exe or Mono.
            config = tmp / "publish.config"
            # vcpkg's nugetconfig provider passes -ConfigFile, not -Source.
            # packageSources alone does not select the publication destination.
            publish_config(config, os.environ["GITHUB_TOKEN"])
            # Pinned NuGet 7.9 reads NUGET_API_KEY directly. Do not write a
            # plaintext value into the encrypted-only <apikeys> config section.
            producer_environment(config)
        if producer:
            run_producer(args)
        else:
            run([*args, "--only-binarycaching"])
    print(f"Media closure: {len(entries)} exact ABI packages; producer={producer}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sdk = sub.add_parser("sdk")
    sdk.add_argument("--rid", required=True)
    sub.add_parser("re2c")
    native = sub.add_parser("media")
    native.add_argument("--triplet", required=True)
    native.add_argument("--installed", type=Path, required=True)
    native.add_argument("--produce", action="store_true")
    args = parser.parse_args()
    if args.command == "sdk":
        restore_sdks(args.rid)
    elif args.command == "re2c":
        restore_re2c()
    else:
        media(args.triplet, args.installed, args.produce)


if __name__ == "__main__":
    main()
