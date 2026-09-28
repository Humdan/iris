#!/usr/bin/env python3
"""Service heartbeat for the LCD: ping every service, publish who answered.

Every INTERVAL seconds each service in SERVICES gets one real probe -- an HTTP
request, a heartbeat-file freshness check, an ICMP ping, or a status command --
and the results land in /tmp/iris_services. A service is ACTIVE when its probe
passed on the latest beat; that is the only thing the LCD's SERVICES widget
lists. iris_fb (C) and the dashboard mirror both read this one file, so the
glass and the web page can't disagree about what's up.

File format (rewritten atomically each beat):
    # ts=<epoch> interval=<s>
    NAME|OK|MS|DETAIL          OK is 1/0, MS the probe's round trip
A reader treats the whole file as stale once ts is older than 3 intervals:
a dead heartbeat must never leave a frozen "everything is fine" on screen.

Managed websites come from ~/.hermes/managed-sites.json, so a site added there
is pinged from the next beat with no edit here.
"""
import json
import os
import subprocess
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

INTERVAL = 15
TIMEOUT = 6
OUT = Path("/tmp/iris_services")
HOME = Path.home()
SITES = HOME / ".hermes" / "managed-sites.json"


def probe_http(url):
    req = urllib.request.Request(url, headers={"User-Agent": "iris-heartbeat"})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return 200 <= r.status < 400, f"http {r.status}"
    except urllib.error.HTTPError as e:
        return False, f"http {e.code}"


def probe_fresh(path, max_age):
    age = time.time() - Path(path).stat().st_mtime
    return age <= max_age, f"{int(age)}s old"


def probe_gateway():
    """Hermes gateway: heartbeat file fresh AND written by a live process."""
    hb = HOME / ".hermes" / "state" / "gateway.heartbeat"
    ok, detail = probe_fresh(hb, 90)
    pid = json.loads(hb.read_text()).get("pid")
    alive = bool(pid) and Path(f"/proc/{pid}").exists()
    return ok and alive, detail if alive else f"pid {pid} gone"


def probe_icmp(host):
    r = subprocess.run(["ping", "-c1", "-W2", host], capture_output=True, timeout=TIMEOUT)
    return r.returncode == 0, f"ping {host}"


def probe_cmd(argv, needle):
    r = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT)
    return r.returncode == 0 and needle in r.stdout, argv[0]


def probe_tailscale():
    r = subprocess.run(["tailscale", "status", "--json"], capture_output=True, text=True, timeout=TIMEOUT)
    state = json.loads(r.stdout).get("BackendState", "?") if r.returncode == 0 else "down"
    return state == "Running", state.lower()


def services():
    """(NAME, probe) pairs. NAME is what the LCD prints: keep it <= 10 chars."""
    svc = [
        ("DASHBOARD", lambda: probe_http("http://127.0.0.1:8080/")),
        ("GATEWAY",   probe_gateway),
        ("SESSIONS",  lambda: probe_fresh("/tmp/iris_sessions.json", 20)),
        ("INTERNET",  lambda: probe_icmp("1.1.1.1")),
        ("TAILSCALE", probe_tailscale),
        ("RPICONNECT", lambda: probe_cmd(["rpi-connect", "status"], "Signed in: yes")),
    ]
    try:
        for s in json.loads(SITES.read_text()).get("sites", []):
            url = s.get("url")
            if url:
                svc.append((s.get("id", url).upper()[:10], lambda u=url: probe_http(u)))
    except (OSError, ValueError):
        pass
    return svc


def run(name, fn):
    t0 = time.monotonic()
    try:
        ok, detail = fn()
    except Exception as e:  # any failure to answer is a failed ping
        ok, detail = False, type(e).__name__
    ms = int((time.monotonic() - t0) * 1000)
    return name, bool(ok), ms, str(detail).replace("|", "/")[:40]


def beat(pool):
    results = list(pool.map(lambda p: run(*p), services()))
    lines = [f"# ts={int(time.time())} interval={INTERVAL}"]
    lines += [f"{n}|{int(ok)}|{ms}|{d}" for n, ok, ms, d in results]
    tmp = OUT.with_suffix(".tmp")
    tmp.write_text("\n".join(lines) + "\n")
    os.chmod(tmp, 0o644)
    tmp.replace(OUT)


def main():
    with ThreadPoolExecutor(max_workers=12) as pool:
        while True:
            start = time.monotonic()
            beat(pool)
            time.sleep(max(1.0, INTERVAL - (time.monotonic() - start)))


if __name__ == "__main__":
    main()
