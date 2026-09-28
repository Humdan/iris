#!/usr/bin/env python3
"""Service heartbeat for the LCD: ping every service, publish who answered.

Every INTERVAL seconds each service in SERVICES gets one real probe -- an HTTP
request, a heartbeat-file freshness check, an ICMP ping, or a status command --
and the results land in /tmp/iris_services. A service is ACTIVE when its probe
passed on the latest beat; that is the only thing the LCD's SERVICES widget
lists. iris_fb (C) and the dashboard mirror both read this one file, so the
glass and the web page can't disagree about what's up.

Services are DYNAMIC (they do work: serve other things, run jobs, can be
triggered remotely, locally or by another service) or STATIC (they only answer
when asked -- a plain website). A dynamic service's probe tests the part that
does the work (its API, MCP, app route or publishing pipeline), not just that
a page loads. INTERNET is the network itself, shown in the widget header.

File format (rewritten atomically each beat):
    # ts=<epoch> interval=<s>
    NAME|OK|MS|KIND|DETAIL     OK 1/0, MS the probe's round trip, KIND D/S/N
A reader treats the whole file as stale once ts is older than 3 intervals:
a dead heartbeat must never leave a frozen "everything is fine" on screen.

Managed websites come from ~/.hermes/managed-sites.json, so a site added there
is pinged from the next beat with no edit here. Its "service" block says
whether it's dynamic and how to check the working part.
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


def probe_http(url, must_contain=None):
    req = urllib.request.Request(url, headers={"User-Agent": "iris-heartbeat"})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            ok = 200 <= r.status < 400
            if ok and must_contain:
                body = r.read(200_000).decode("utf-8", "replace")
                if must_contain.lower() not in body.lower():
                    return False, f"no '{must_contain[:12]}'"
            return ok, f"http {r.status}"
    except urllib.error.HTTPError as e:
        return False, f"http {e.code}"


def probe_json(url, key):
    """A dynamic service's data feed: must answer JSON that carries `key`."""
    req = urllib.request.Request(url, headers={"User-Agent": "iris-heartbeat"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        data = json.loads(r.read())
    return key in data, f"api {r.status}"


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


def probe_mcp(url):
    """MCP server: a real JSON-RPC initialize must come back with a result."""
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "iris-heartbeat", "version": "1"}}}).encode()
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "User-Agent": "iris-heartbeat", "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        text = r.read(65536).decode("utf-8", "replace")
    return '"result"' in text, "mcp init"


def _site_check():
    """site-check.py's Supabase reader, so freshness means the same thing here
    as in the nightly site report."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("site_check", HOME / ".hermes/scripts/site-check.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def probe_content_fresh(site):
    """Publishing pipeline alive: newest row younger than the site's budget."""
    from datetime import datetime, timezone
    newest = _site_check()._supabase_newest(site)
    if not newest:
        return False, "no data"
    ts = datetime.fromisoformat(newest[2].replace("Z", "+00:00"))
    age_h = (datetime.now(timezone.utc) - ts).total_seconds() / 3600
    budget = (site.get("freshness") or {}).get("max_age_hours", 30)
    return age_h <= budget, f"new {int(age_h)}h ago"


def probe_site(site):
    """A managed site. Static: the page loads and says what it should.
    Dynamic: that, plus the part that does the work -- its MCP, an app route,
    or fresh content from its pipeline (per service.check)."""
    svc = site.get("service") or {}
    must = ((site.get("expect") or {}).get("must_contain") or [None])[0]
    ok, detail = probe_http(site["url"], must)
    if not ok or svc.get("kind") != "dynamic":
        return ok, detail
    check = svc.get("check") or {}
    if check.get("mcp"):
        return probe_mcp(check["mcp"])
    if check.get("path"):
        return probe_http(site["url"].rstrip("/") + check["path"], check.get("must_contain"))
    if check.get("fresh"):
        return probe_content_fresh(site)
    return ok, "site only"


# kind: D dynamic (does work, serves others, can be triggered), S static
# (only answers when asked), N the network itself (drawn in the header).
# every: seconds between real probes; the last result is reused in between,
# so a slow check (a database query) doesn't run on every 15s beat.
def services():
    """(NAME, KIND, EVERY, probe). NAME is what the LCD prints: <= 10 chars."""
    svc = [
        ("DASHBOARD",  "D", 0, lambda: probe_json("http://127.0.0.1:8080/api/lcd/state", "clock")),
        ("GATEWAY",    "D", 0, probe_gateway),
        ("SESSIONS",   "D", 0, lambda: probe_fresh("/tmp/iris_sessions.json", 20)),
        ("TAILSCALE",  "D", 0, probe_tailscale),
        ("RPICONNECT", "D", 0, lambda: probe_cmd(["rpi-connect", "status"], "Signed in: yes")),
        ("INTERNET",   "N", 0, lambda: probe_icmp("1.1.1.1")),
    ]
    try:
        for s in json.loads(SITES.read_text()).get("sites", []):
            if not s.get("url"):
                continue
            if (s.get("service") or {}).get("kind") != "dynamic":
                continue   # static sites aren't shown; the nightly site-check covers them
            kind = "D"
            every = 600 if ((s.get("service") or {}).get("check") or {}).get("fresh") else 60
            svc.append((s.get("id", s["url"]).upper()[:10], kind, every, lambda s=s: probe_site(s)))
    except (OSError, ValueError):
        pass
    return svc


_last = {}   # name -> (monotonic time, result) for slow probes


def run(name, fn):
    t0 = time.monotonic()
    try:
        ok, detail = fn()
    except Exception as e:  # any failure to answer is a failed ping
        ok, detail = False, type(e).__name__
    ms = int((time.monotonic() - t0) * 1000)
    return name, bool(ok), ms, str(detail).replace("|", "/")[:40]


def cached_run(name, kind, every, fn):
    hit = _last.get(name)
    if hit and every and time.monotonic() - hit[0] < every:
        return hit[1] + (kind,)
    res = run(name, fn)
    _last[name] = (time.monotonic(), res)
    return res + (kind,)


def beat(pool):
    results = list(pool.map(lambda p: cached_run(*p), services()))
    lines = [f"# ts={int(time.time())} interval={INTERVAL}"]
    lines += [f"{n}|{int(ok)}|{ms}|{k}|{d}" for n, ok, ms, d, k in results]
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
