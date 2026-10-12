import csv
import io
import json
import os
import re
import time
from datetime import datetime, timezone

from django.conf import settings
from django.http import HttpResponseBadRequest, JsonResponse, StreamingHttpResponse
from django.shortcuts import render
from django.views.decorators.http import require_GET, require_POST

from . import control
from .models import Reading

FIELDS = [
    "id",
    "ts",
    "device",
    "uptime_s",
    "heap_free",
    "heap_min_free",
    "battery_mv",
    "d0",
    "d1",
    "d2",
]

SLEEP_MIN, SLEEP_MAX = 3, 86400  # the Pico spends ~2 s blinking before it sleeps


# ---------------------------------------------------------------- page
@require_GET
def index(request):
    return render(
        request, "index.html"
    )  # {{ csrf_token }} in the template sets the cookie


# ---------------------------------------------------------------- live data
@require_GET
def api_readings(request):
    """Rows newer than ?after=<id> (oldest first). Without it: the newest ?limit rows.
    ?devices=1 also returns the latest row of every Pico (used for the info cards)."""
    try:
        after = int(request.GET.get("after", 0))
        limit = min(max(int(request.GET.get("limit", 500)), 1), 5000)
    except ValueError:
        return HttpResponseBadRequest("bad parameters")

    if after > 0:
        rows = list(
            Reading.objects.filter(id__gt=after).order_by("id").values(*FIELDS)[:limit]
        )
    else:
        rows = list(Reading.objects.order_by("-id").values(*FIELDS)[:limit])[::-1]

    out = {"now": int(time.time()), "rows": rows}
    if request.GET.get("devices"):
        names = Reading.objects.order_by().values_list("device", flat=True).distinct()
        out["devices"] = [
            Reading.objects.filter(device=n)
            .order_by("-ts", "-id")
            .values(*FIELDS)
            .first()
            for n in names
        ]
    return JsonResponse(out)


# ---------------------------------------------------------------- server.log tail
def _tail(path, offset, max_bytes=65536):
    size = os.path.getsize(path)
    reset = offset is None or offset > size or size - offset > max_bytes
    start = max(0, size - max_bytes) if reset else offset
    with open(path, "rb") as f:
        f.seek(start)
        data = f.read(size - start)
    end = (
        data.rfind(b"\n") + 1
    )  # whole lines only; a half-written line waits for next poll
    lines = data[:end].decode("utf-8", "replace").splitlines()
    if reset and start > 0 and lines:  # landed in the middle of a line
        lines = lines[1:]
    return {"offset": start + end, "lines": lines[-500:], "reset": reset}


@require_GET
def api_log(request):
    path = getattr(settings, "PICO_SERVER_LOG", "")
    if not path:
        return JsonResponse(
            {"available": False, "error": "PICO_SERVER_LOG is not set in settings.py"}
        )
    try:
        offset = int(request.GET["offset"]) if "offset" in request.GET else None
    except ValueError:
        return HttpResponseBadRequest("bad offset")
    try:
        return JsonResponse({"available": True, **_tail(path, offset)})
    except OSError as e:
        return JsonResponse(
            {"available": False, "error": f"cannot read {path}: {e.strerror}"}
        )


# ---------------------------------------------------------------- download
RANGES = {"1h": 3600, "24h": 86400, "7d": 7 * 86400, "30d": 30 * 86400, "all": None}
FORMATS = {
    "csv": ("text/csv", "csv"),
    "json": ("application/json", "json"),
    "jsonl": ("application/x-ndjson", "jsonl"),
    "txt": ("text/plain", "txt"),
}


def _iso(ts):
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _num(v, nd=1):
    return "-" if v is None else (f"{v:.{nd}f}" if isinstance(v, float) else str(v))


def _iter_csv(rows):
    buf = io.StringIO()
    w = csv.writer(buf)

    def drain():
        s = buf.getvalue()
        buf.seek(0)
        buf.truncate(0)
        return s

    w.writerow(FIELDS + ["time_utc"])
    yield drain()
    for r in rows:
        w.writerow([r[f] for f in FIELDS] + [_iso(r["ts"])])
        yield drain()


def _iter_json(rows):
    yield "["
    first = True
    for r in rows:
        yield ("" if first else ",") + "\n" + json.dumps(
            {**r, "time_utc": _iso(r["ts"])}
        )
        first = False
    yield "\n]\n"


def _iter_jsonl(rows):
    for r in rows:
        yield json.dumps({**r, "time_utc": _iso(r["ts"])}) + "\n"


def _iter_txt(rows):
    for r in rows:
        yield (
            f"{_iso(r['ts'])}  #{r['id']}  {r['device']}  batt={_num(r['battery_mv'])}mV "
            f"up={_num(r['uptime_s'])}s heap={_num(r['heap_free'])}/{_num(r['heap_min_free'])} "
            f"d=[{_num(r['d0'])}, {_num(r['d1'])}, {_num(r['d2'])}]\n"
        )


ITERATORS = {
    "csv": _iter_csv,
    "json": _iter_json,
    "jsonl": _iter_jsonl,
    "txt": _iter_txt,
}


@require_GET
def download(request):
    fmt = request.GET.get("format", "csv")
    rng = request.GET.get("range", "24h")
    device = request.GET.get("device", "")
    if fmt not in FORMATS or rng not in RANGES:
        return HttpResponseBadRequest("bad format or range")

    qs = Reading.objects.order_by("id")
    if device:
        qs = qs.filter(device=device)
    if RANGES[rng]:
        qs = qs.filter(ts__gte=int(time.time()) - RANGES[rng])

    ctype, ext = FORMATS[fmt]
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", device) if device else "all"
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    resp = StreamingHttpResponse(
        ITERATORS[fmt](qs.values(*FIELDS).iterator(chunk_size=2000)),
        content_type=f"{ctype}; charset=utf-8",
    )
    resp["Content-Disposition"] = (
        f'attachment; filename="pico-{safe}-{rng}-{stamp}.{ext}"'
    )
    return resp


# ---------------------------------------------------------------- commands
def _command(pico_id, action, seconds=0):
    if not Reading.objects.filter(device=pico_id).exists():
        return JsonResponse({"ok": False, "error": "unknown pico id"}, status=404)
    ok, msg = control.send_command(action, pico_id, seconds)
    if ok:
        return JsonResponse({"ok": True, "message": msg})
    return JsonResponse({"ok": False, "error": msg}, status=502)


@require_POST
def reboot(request, pico_id):
    return _command(pico_id, "REBOOT")


@require_POST
def sleep_cmd(request, pico_id):
    try:
        seconds = int(request.POST.get("seconds", ""))
    except ValueError:
        return JsonResponse(
            {"ok": False, "error": "seconds must be a whole number"}, status=400
        )
    if not SLEEP_MIN <= seconds <= SLEEP_MAX:
        return JsonResponse(
            {"ok": False, "error": f"seconds must be {SLEEP_MIN}-{SLEEP_MAX}"},
            status=400,
        )
    return _command(pico_id, "SLEEP", seconds)
