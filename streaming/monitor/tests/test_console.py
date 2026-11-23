import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for(processes, predicate, description, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if any(process.poll() is not None for process in processes):
            raise AssertionError("A service exited while waiting for " + description)
        try:
            if result := predicate():
                return result
        except (URLError, TimeoutError):
            pass
        time.sleep(0.1)
    raise AssertionError("Timed out waiting for " + description)


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def verify(console, fixture, web, work):
    control = work / "pause-camera"
    finish = work / "stop-fixture"
    archive_root = work / "录像"
    archive_root.mkdir()
    rtsp_port, http_port = free_port(), free_port()
    base = f"http://127.0.0.1:{http_port}"

    def request(path, **kwargs):
        return urlopen(Request(base + path, **kwargs), timeout=2)

    def status():
        with request("/api/status") as response:
            return json.load(response)

    server = monitor = None
    with (work / "rtsp.log").open("w+b") as rtsp_log, (work / "monitor.log").open("w+b") as monitor_log:
        try:
            server = subprocess.Popen([fixture, str(rtsp_port), str(control), str(finish)],
                                      stdout=rtsp_log, stderr=subprocess.STDOUT)
            wait_for([server], lambda: b"ready" in (work / "rtsp.log").read_bytes(),
                     "the RTSP fixture")
            args = [console, "--archive", str(archive_root), "--web", web,
                    "--port", str(http_port), "--run-seconds", "32"]
            names = ["入口", "仓库", "通道", "月台"]
            for index, name in enumerate(names, 1):
                args += ["--source", name,
                         f"rtsp://127.0.0.1:{rtsp_port}/live/camera-{index}"]
            monitor = subprocess.Popen(args, stdout=monitor_log, stderr=subprocess.STDOUT)
            processes = [server, monitor]

            def all_live():
                sources = status()
                return sources if len(sources) == 4 and all(
                    s["signal"] == "live" and s["preview"] > 0 for s in sources
                ) else None

            online = wait_for(processes, all_live, "four live JPEG previews")
            assert [source["name"] for source in online] == names
            with request("/api/preview/4") as response:
                assert response.headers.get_content_type() == "image/jpeg"
                assert response.read(2) == b"\xff\xd8"
            with request("/") as response:
                assert b"/monitor.js" in response.read()
            time.sleep(1.5)
            control.touch()

            def find_incident():
                sources = status()
                return sources if sources[0]["signal"] == "offline" and sources[0]["clip"] == "ready" else None

            failed = wait_for(processes, find_incident, "the offline alert and archived video")
            assert all(s["signal"] == "live" and s["preview"] > online[i]["preview"]
                       for i, s in enumerate(failed) if i != 0), failed
            incident = failed[0]["incident"]
            assert incident > 0 and not failed[0]["acknowledged"]

            recording = f"/api/recording/1/{incident}"
            with request(recording, headers={"Range": "bytes=0-15"}) as response:
                assert response.status == 206 and b"ftyp" in response.read()
            clip = work / "download.mp4"
            with request(recording) as response:
                assert response.headers.get_content_type() == "video/mp4"
                clip.write_bytes(response.read())
            subprocess.run([fixture, "--probe", str(clip)], timeout=8, check=True)

            ack = f"/api/ack/1/{incident}"
            try:
                with request(ack, method="POST"):
                    raise AssertionError("Confirmation without the action header succeeded")
            except HTTPError as error:
                assert error.code == 403
            with request(ack, method="POST", headers={"X-Monitor-Action": "acknowledge"}) as response:
                assert response.status == 200
            assert status()[0]["acknowledged"]

            control.unlink()
            recovered = wait_for(processes, all_live, "the recovering camera")
            assert recovered[0]["incident"] == incident
            assert recovered[0]["acknowledged"] and recovered[0]["recovered"] > failed[0]["opened"]
            assert recovered[0]["preview"] > failed[0]["preview"]

            archive, = archive_root.glob("monitor-*")
            (archive / f"camera-1-{incident + 1}.mp4.part").mkdir()
            time.sleep(1.2)
            control.touch()

            def archive_failed():
                sources = status()
                first = sources[0]
                return sources if (first["signal"] == "offline" and first["incident"] > incident
                                   and first["clip"] == "unavailable") else None

            degraded = wait_for(processes, archive_failed, "an archive write failure")
            assert all(s["signal"] == "live" and s["preview"] > recovered[i]["preview"]
                       for i, s in enumerate(degraded) if i != 0), degraded
            try:
                with request(ack, method="POST", headers={"X-Monitor-Action": "acknowledge"}):
                    raise AssertionError("A stale acknowledgement succeeded")
            except HTTPError as error:
                assert error.code == 409
            control.unlink()
            assert monitor.wait(timeout=25) == 0
            finish.touch()
            assert server.wait(timeout=5) == 0
        except Exception:
            for log in ("rtsp.log", "monitor.log"):
                print(log, (work / log).read_bytes().decode(errors="replace"), file=sys.stderr)
            raise
        finally:
            stop(monitor)
            stop(server)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="turbo-monitor-") as folder:
        verify(*sys.argv[1:], Path(folder))
