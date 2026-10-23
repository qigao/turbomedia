#!/usr/bin/env python3
"""Bootstrap the locked shared vcpkg contract without its Mono/NuGet action."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from github_packages import HERE, REPO, export, run


def output(args):
    return subprocess.check_output([str(a) for a in args], text=True).strip()


def setup(infrastructure):
    infrastructure = Path(infrastructure).resolve()
    lock = json.loads((HERE / "m16-sdk-lock.json").read_text())
    expected = lock["actions"]["qigao/vcpkg-cache/.github/actions/setup-vcpkg-cache"]
    if output(["git", "-C", infrastructure, "rev-parse", "HEAD"]) != expected:
        raise ValueError("shared infrastructure source differs from the lock")
    scripts = (infrastructure / "vcpkg-scripts-revision.txt").read_text().strip()
    tool = (infrastructure / "vcpkg-tool-version.txt").read_text().strip()
    if scripts != lock["vcpkg_scripts_revision"] or tool != lock["vcpkg_tool_version"]:
        raise ValueError("shared vcpkg tool/scripts lock mismatch")
    temp = Path(os.environ["RUNNER_TEMP"])
    root = temp / f"qigao-vcpkg-{scripts}"
    # A fresh, full-history checkout retains historical versioned port trees.
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    run(["git", "-C", root, "init"])
    run(["git", "-C", root, "remote", "add", "origin", "https://github.com/microsoft/vcpkg.git"])
    run(["git", "-c", "protocol.version=2", "-C", root, "fetch", "--no-tags", "origin", scripts])
    run(["git", "-C", root, "checkout", "--detach", "FETCH_HEAD"])
    baselines = output([sys.executable, infrastructure / "scripts/vcpkg-required-commits.py",
                        infrastructure, REPO]).splitlines()
    for baseline in baselines:
        exists = subprocess.run(["git", "-C", str(root), "cat-file", "-e", f"{baseline}^{{commit}}"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode:
            run(["git", "-C", root, "fetch", "--no-tags", "origin", baseline])
        run(["git", "-C", root, "cat-file", "-e", f"{baseline}:versions/baseline.json"])
    if (output(["git", "-C", root, "rev-parse", "HEAD"]) != scripts or
            output(["git", "-C", root, "rev-parse", "--is-shallow-repository"]) != "false"):
        raise ValueError("vcpkg checkout identity/history mismatch")
    for option in ("remote.origin.promisor", "remote.origin.partialclonefilter"):
        result = subprocess.run(["git", "-C", str(root), "config", "--get", option],
                                capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip():
            raise ValueError(f"partial vcpkg checkout: {option}")
    if os.name == "nt":
        run(["cmd.exe", "/d", "/c", root / "bootstrap-vcpkg.bat", "-disableMetrics"], cwd=root)
    else:
        run(["sh", root / "bootstrap-vcpkg.sh", "-disableMetrics"], cwd=root)
    exe = root / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    if output([exe, "version"]).splitlines()[0].removeprefix("vcpkg package management program version ") != tool:
        raise ValueError("bootstrapped vcpkg executable differs from the lock")
    revision = output([sys.executable, infrastructure / "scripts/cache-contract-revision.py", infrastructure])
    version = (infrastructure / "cache-contract-version.txt").read_text().strip()
    # Use the unchanged shared snapshot/hash AND the same absolute overlay paths.
    # Editing the old action before hashing it would invalidate cached ABIs.
    contract = temp / f"qigao-vcpkg-contract-v{version}-{revision}"
    shutil.rmtree(contract, ignore_errors=True)
    for directory in ("ports", "triplets"):
        shutil.copytree(infrastructure / directory, contract / directory)
    (contract / "cmake").mkdir()
    toolchain = contract / "cmake/QigaoVcpkgToolchain.cmake"
    shutil.copy2(infrastructure / "cmake/QigaoVcpkgToolchain.cmake", toolchain)
    (contract / ".qigao-cache-revision").write_text(revision + "\n")
    cache = REPO / "build/vcpkg-binary-cache"
    cache.mkdir(parents=True, exist_ok=True)
    export({"VCPKG_ROOT": root, "VCPKG_TOOL_REVISION": tool,
            "VCPKG_SCRIPTS_REVISION": scripts, "VCPKG_CACHE_CONTRACT_VERSION": version,
            "VCPKG_CACHE_REVISION": revision, "VCPKG_CACHE_REPOSITORY_ROOT": contract,
            "VCPKG_OVERLAY_PORTS": contract / "ports", "VCPKG_OVERLAY_TRIPLETS": contract / "triplets",
            "QIGAO_VCPKG_WINDOWS_TRIPLET": "qigao-x64-windows-v145",
            "QIGAO_VCPKG_WINDOWS_HOST_TRIPLET": "qigao-x64-windows-v145",
            "QIGAO_VCPKG_TOOLCHAIN_FILE": toolchain, "VCPKG_BINARY_CACHE": cache,
            "X_VCPKG_NUGET_ID_PREFIX": "vcpkg-cache",
            "VCPKG_BINARY_SOURCES": f"clear;files,{cache},readwrite"})
    print(f"Locked vcpkg contract v{version}: {revision}", flush=True)


if __name__ == "__main__":
    setup(sys.argv[1])
