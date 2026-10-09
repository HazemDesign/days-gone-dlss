#!/usr/bin/env python3
"""Offline parser for Days Gone DLSS debug bundles (stdlib only, py3.8+).

Usage:
    python tools/parse-logs.py <bundle-dir-or-ReShade.log> [--frame frame.log]

Reads ReShade.log lines with prefixes DaysGone cDLSS/cSTATS/cSCAN/cVIEW/AUDIT
plus cap/capCS lines, and prints: slot table, MV producers, first-Draw
history, and decode verdict hints. Missing files -> warnings, exit 0.
"""
import argparse
import os
import re
import sys
from collections import Counter, OrderedDict

FIRST_DRAW_RE = re.compile(
    r"cDLSS first Draw ok=(\d+) reset=(\d+) render=(\d+)x(\d+) out=(\d+)x(\d+)"
    r".*?mvsrc=(\S+) mvfmt=(-?\d+) mvhash=([0-9A-Fa-f]+).*?msc=(\d+) dec=(\d+)"
    r".*?inv=(\d+).*?jit=([-\d.]+),([-\d.]+)"
)
CSTAT_MEAN_RE = re.compile(r"cSTATS (\S+) fmt=(-?\d+) (\d+)x(\d+).*?mean=([-\d.eE+]+)")
CSTAT_SIMPLE_RE = re.compile(r"cSTATS (\S+)\s+(.*)")
CACHE_RE = re.compile(r"cSTATS mvcache (FRESH|STALE)")
CAP_RE = re.compile(r"^cap (\d+) PS ([0-9A-Fa-f]+) (.*)")
CAPCS_RE = re.compile(r"^capCS (\d+) CS ([0-9A-Fa-f]+) (.*)")
AUDIT_RE = re.compile(r"AUDIT (\S+)")
INTERESTING = ("DaysGone", "cDLSS", "cSTATS", "cSCAN", "cVIEW", "AUDIT", "capCS", "FULL")
FULL_RE = re.compile(
    r"FULL full (\d+) build=(\S+) draws=(\d+) frozen=(\d+)"
    r" slot_att/slot_runs/slot_resets=(\d+)/(\d+)/(\d+)"
    r" mv=(real|zero|stale) mvhash=([0-9A-Fa-f]+) mvfmt=(-?\d+)"
    r" dlss_ok=(\d+) dlss_reset=(\d+) jit=([-\d.]+),([-\d.]+)"
    r"(?: jitraw=[-\d.,]+)?"
    r" cview=(\S+) freeze_master=(\d+)"
)
# templog appended fields (new builds only; absent on legacy lines).
FULL_X_RE = re.compile(
    r"view=(-?\d+)/(\d+) rejD\+(\d+) projrejD\+(\d+) p([-\d.]+),([-\d.]+)"
    r" rot=([-\d.]+) cfg=(\d+)\.(\d+)\.(\d+)\.(\d+) jm=(\d+)\.(\d+)\.(\d+)\.(\d+)"
    r" fov=([-\d.]+):([-\d.]+) fidx=(\d+)([!_=]) dage=(\d+) cc=(\d+)"
)
FULLAGG_RE = re.compile(
    r"FULLAGG presents=(\d+)-(\d+) jit_n=(\d+) mean=([-\d.]+),([-\d.]+)"
    r" quad00/01/10/11=(\d+)/(\d+)/(\d+)/(\d+)"
    r" rot_mean=([-\d.]+) rot_max=([-\d.]+) resets=(\d+) projrej=(\d+)"
    r" cfg=(\d+)\.(\d+)\.(\d+)\.(\d+)"
)


def warn(msg):
    print("WARNING: %s" % msg)


def load_lines(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError as e:
        warn("cannot read %s: %s" % (path, e))
        return []


def resolve_inputs(target, frame_arg):
    reshade = frame = full = None
    if target is None:
        return None, None, None
    if os.path.isdir(target):
        for cand in ("ReShade-filtered.log", "ReShade.log", "ReShade-tail.log"):
            p = os.path.join(target, cand)
            if os.path.isfile(p):
                reshade = p
                break
        for cand in ("Luma-DaysGone-frame.log", "frame-tail.log", "frame.log"):
            p = os.path.join(target, cand)
            if os.path.isfile(p):
                frame = p
                break
        for cand in ("Luma-DaysGone-full.log", "full-tail.log", "full.log"):
            p = os.path.join(target, cand)
            if os.path.isfile(p):
                full = p
                break
        if frame_arg and os.path.isfile(frame_arg):
            frame = frame_arg
    elif os.path.isfile(target):
        reshade = target
        if frame_arg and os.path.isfile(frame_arg):
            frame = frame_arg
    else:
        warn("input not found: %s" % target)
    return reshade, frame, full


def parse_reshade(lines):
    slots = Counter()
    per_frame = Counter()
    mv_prod = OrderedDict()  # hash -> dict(fmt, count)
    first_draws = []
    cache_flags = []
    cscan = Counter()
    cview = Counter()
    audit_tags = Counter()
    for ln in lines:
        if not any(k in ln for k in INTERESTING):
            continue
        m = re.search(r"\bPS ([0-9A-Fa-f]{4,8})\b", ln)
        if not m:
            m = re.search(r"\bCS ([0-9A-Fa-f]{4,8})\b", ln)
        if m:
            h = m.group(1).upper()
            slots[h] += 1
            fm = re.search(r"frame=(\d+)", ln)
            if fm:
                per_frame[(h, fm.group(1))] += 1
        mh = re.search(r"mvhash=([0-9A-Fa-f]+)", ln)
        mf = re.search(r"mvfmt=(-?\d+)", ln)
        if mh:
            h = mh.group(0).split("=")[1].upper()
            e = mv_prod.setdefault(h, {"fmt": "?", "count": 0})
            e["count"] += 1
            if mf:
                e["fmt"] = mf.group(1)
        fd = FIRST_DRAW_RE.search(ln)
        if fd:
            first_draws.append(ln.strip())
        cm = CACHE_RE.search(ln)
        if cm:
            cache_flags.append(cm.group(1))
        ms = re.search(r"cSCAN (\S+)", ln)
        if ms:
            cscan[ms.group(1)] += 1
        mv = re.search(r"cVIEW (\S+)", ln)
        if mv:
            cview[mv.group(1)] += 1
        am = AUDIT_RE.search(ln)
        if am:
            audit_tags[am.group(1)] += 1
    return slots, per_frame, mv_prod, first_draws, cache_flags, cscan, cview, audit_tags


def parse_means(lines):
    means = OrderedDict()
    for ln in lines:
        m = CSTAT_MEAN_RE.search(ln)
        if m:
            name = m.group(1)
            try:
                means[name] = float(m.group(5))
            except ValueError:
                pass
    return means


def parse_full(lines):
    """Parse DaysGone FULL mirror lines (and full.log lines) into row dicts."""
    rows = []
    for ln in lines:
        m = FULL_RE.search(ln)
        if not m:
            continue
        rows.append({
            "present": int(m.group(1)),
            "build": m.group(2),
            "draws": int(m.group(3)),
            "frozen": int(m.group(4)),
            "att": int(m.group(5)),
            "runs": int(m.group(6)),
            "resets": int(m.group(7)),
            "mv": m.group(8),
            "mvhash": m.group(9).upper(),
            "mvfmt": m.group(10),
            "ok": int(m.group(11)),
            "reset": int(m.group(12)),
            "jitx": m.group(13),
            "jity": m.group(14),
            "cview": m.group(15),
            "fmaster": int(m.group(16)),
            "raw": ln.strip(),
        })
        xm = FULL_X_RE.search(ln)
        if xm:
            rows[-1]["x"] = {
                "pick": int(xm.group(1)), "n": int(xm.group(2)),
                "drej": int(xm.group(3)), "dproj": int(xm.group(4)),
                "p00": float(xm.group(5)), "p11": float(xm.group(6)),
                "rot": float(xm.group(7)),
                "cfg": (xm.group(8), xm.group(9), xm.group(10), xm.group(11)),
                "jm": (xm.group(12), xm.group(13), xm.group(14), xm.group(15)),
                "fovsl": float(xm.group(16)), "fovtrue": float(xm.group(17)),
                "fidx": int(xm.group(18)), "bang": xm.group(19),
                "dage": int(xm.group(20)), "cc": int(xm.group(21)),
            }
        else:
            rows[-1]["x"] = None
    return rows


def parse_fullagg(lines):
    """Parse DaysGone FULLAGG window lines into dicts."""
    aggs = []
    for ln in lines:
        m = FULLAGG_RE.search(ln)
        if not m:
            continue
        aggs.append({
            "a": int(m.group(1)), "b": int(m.group(2)),
            "jit_n": int(m.group(3)),
            "meanx": float(m.group(4)), "meany": float(m.group(5)),
            "q00": int(m.group(6)), "q01": int(m.group(7)),
            "q10": int(m.group(8)), "q11": int(m.group(9)),
            "rot_mean": float(m.group(10)), "rot_max": float(m.group(11)),
            "resets": int(m.group(12)), "projrej": int(m.group(13)),
            "cfg": (m.group(14), m.group(15), m.group(16), m.group(17)),
            "raw": ln.strip(),
        })
    return aggs


def fullagg_verdicts(aggs, rows):
    """Verdicts from FULLAGG windows + new-field walk: biased-jitter /
    wrong-view-zone / history-nuke (+ rot coincidence)."""
    out = []
    if not aggs and not any(r.get("x") for r in rows):
        return ["no FULLAGG lines and no extended FULL fields: pre-templog build (or trace never started)."]
    for a in aggs:
        n = a["jit_n"]
        if n >= 60:
            qs = [("00", a["q00"]), ("01", a["q01"]), ("10", a["q10"]), ("11", a["q11"])]
            qname, qmax = max(qs, key=lambda t: t[1])
            if qmax * 1.0 / n >= 0.70:
                out.append("biased-jitter in presents %d-%d: quad%s holds %d/%d (%.0f%%), mean %.3f,%.3f - TAA sampling lopsided."
                           % (a["a"], a["b"], qname, qmax, n, 100.0 * qmax / n, a["meanx"], a["meany"]))
            if abs(a["meanx"]) > 0.15 or abs(a["meany"]) > 0.15:
                out.append("jit mean off-center in presents %d-%d: mean %.3f,%.3f over %d - drift, not a centered sequence."
                           % (a["a"], a["b"], a["meanx"], a["meany"], n))
        if a["resets"] > 0:
            out.append("history-nuke in presents %d-%d: %d NGX resets - history keeps nuking (resize/render-size churn?)."
                       % (a["a"], a["b"], a["resets"]))
        if a["rot_max"] > 0.05 and a["resets"] > 0:
            out.append("rot+reset coincidence in presents %d-%d: rot_max %.4f with %d resets - motion spike nuked history?"
                       % (a["a"], a["b"], a["rot_max"], a["resets"]))
        elif a["rot_max"] > 0.30:
            out.append("large rotation in presents %d-%d: rot_max %.4f (mean %.4f) - fast spin/cut zone."
                       % (a["a"], a["b"], a["rot_max"], a["rot_mean"]))
    xr = [r for r in rows if r.get("x")]
    if len(xr) >= 10:
        tp = sum(r["x"]["dproj"] for r in xr)
        p00s = [r["x"]["p00"] for r in xr]
        p11s = [r["x"]["p11"] for r in xr]
        if tp > 0 and (max(p00s) - min(p00s)) < 0.004 and (max(p11s) - min(p11s)) < 0.004:
            out.append("wrong-view-zone over %d FULL frames: %d projrej drops but elected p00/p11 frozen (%.3f,%.3f) - non-gameplay view locked."
                       % (len(xr), tp, p00s[-1], p11s[-1]))
    if not out:
        out.append("FULLAGG nominal: jitter centered, projection stable, no window resets.")
    return out


def full_verdicts(rows):
    out = []
    if not rows:
        return ["no DaysGone FULL lines: full-trace never started (or ReShade.log filter dropped them)."]
    runs0 = rows[0]["runs"]
    runs1 = rows[-1]["runs"]
    if runs1 == runs0:
        out.append("runs stall: slot_runs flat at %d across %d FULL frames - DLSS never executed (masters off / parked / Draw failing)." % (runs1, len(rows)))
    r0 = rows[0]["resets"]
    r1 = rows[-1]["resets"]
    if r1 > r0:
        out.append("resets climbing: %d -> %d over %d FULL frames - NGX history keeps nuking (resize/render-size churn?)." % (r0, r1, len(rows)))
    stale = sum(1 for r in rows if r["mv"] == "stale")
    zero = sum(1 for r in rows if r["mv"] == "zero")
    n = len(rows)
    if stale * 2 >= n:
        out.append("mv stale ratio %d/%d: last MV real but slot silent 30+ presents - feed not refreshing (static camera or wrong MV source)." % (stale, n))
    elif zero * 2 >= n:
        out.append("mv zero ratio %d/%d: DLSS running blind (zero fallback) - fix MV source/decode." % (zero, n))
    ok0 = sum(1 for r in rows if r["ok"])
    if ok0 == 0:
        out.append("dlss_ok never 1 across %d FULL frames: NGX Draw never succeeded - check attempts/runs + first-Draw FAIL lines." % n)
    if not out:
        out.append("FULL trace nominal: runs advance, resets flat, MV mostly real.")
    return out


def verdicts(first_draws, cache_flags, means):
    out = []
    resets = 0
    for ln in first_draws:
        m = FIRST_DRAW_RE.search(ln)
        if m and int(m.group(2)) > 0:
            resets += 1
    if resets > 1:
        out.append("resets>1 (%d): history-nuke warning - NGX keeps resetting, check resize/render-size churn." % resets)
    if cache_flags and all(c == "STALE" for c in cache_flags[-3:]):
        out.append("STALE mvcache + zero fallback warning: motion cache never refreshed; MV source likely wrong.")
    for name, mean in means.items():
        if abs(mean - 0.25) < 0.03:
            out.append("mean~0.25 on %s (%.4f): suggests 0.25-centered packed/encoded signal, not raw HDR." % (name, mean))
    if len(first_draws) >= 2:
        a = FIRST_DRAW_RE.search(first_draws[0])
        b = FIRST_DRAW_RE.search(first_draws[-1])
        if a and b and a.group(0) == b.group(0):
            out.append("first-Draw lines identical (X=Y suspicious): logger may be stuck / game вторит same feed.")
    if not first_draws:
        out.append("no cDLSS first-Draw lines: DLSS path never fired (attempts=0?) or log filter dropped them.")
    if not out:
        out.append("no strong verdict: feed looks nominal, compare mvfmt/mvhash across pan test.")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Parse Days Gone DLSS log bundle.")
    ap.add_argument("target", help="bundle dir or ReShade.log path")
    ap.add_argument("--frame", default=None, help="explicit frame.log path")
    ap.add_argument("--full", default=None, help="explicit full.log path")
    args = ap.parse_args(argv)
    reshade_path, frame_path, full_path = resolve_inputs(args.target, args.frame)
    if args.full and os.path.isfile(args.full):
        full_path = args.full
    rlines = load_lines(reshade_path) if reshade_path else []
    flines = load_lines(frame_path) if frame_path else []
    fulines = load_lines(full_path) if full_path else []
    if reshade_path is None:
        warn("no ReShade.log found; slot/MV analysis skipped.")
    if frame_path is None:
        warn("no frame.log found; cap/capCS analysis skipped.")
    if full_path is None:
        warn("no full.log found; FULL table uses ReShade.log mirror lines only.")
    slots, per_frame, mv_prod, fds, cache, cscan, cview, audits = parse_reshade(rlines)
    means = parse_means(rlines + flines)
    full_rows = parse_full(rlines + fulines)
    full_aggs = parse_fullagg(rlines + fulines)

    print("== slot table (hash, count, 1/frame?) ==")
    if not slots:
        print("  (none)")
    for h, c in slots.most_common(20):
        frames = len({f for (hh, f) in per_frame if hh == h})
        once = "yes" if frames and c // max(frames, 1) <= 1 else "no"
        print("  %s count=%d frames=%d 1/frame?%s" % (h, c, frames, once))
    print("== MV producers (hash, fmt, count) ==")
    if not mv_prod:
        print("  (none)")
    for h, e in list(mv_prod.items())[:20]:
        print("  %s fmt=%s count=%d" % (h, e["fmt"], e["count"]))
    if means:
        print("== cSTATS means ==")
        for k, v in means.items():
            print("  %s mean=%.5f" % (k, v))
    print("== cache flags: FRESH=%d STALE=%d ==" % (cache.count("FRESH"), cache.count("STALE")))
    if cscan:
        print("cSCAN: %s" % dict(cscan.most_common(10)))
    if cview:
        print("cVIEW: %s" % dict(cview.most_common(10)))
    if audits:
        print("AUDIT tags: %s" % dict(audits.most_common(10)))
    print("== first-Draw history (%d) ==" % len(fds))
    for ln in fds[-10:]:
        print("  " + ln)
    if flines:
        caps = sum(1 for l in flines if CAP_RE.match(l))
        ccs = sum(1 for l in flines if CAPCS_RE.match(l))
        print("== frame.log: cap=%d capCS=%d total=%d ==" % (caps, ccs, len(flines)))
    print("== FULL per-frame table (%d rows) ==" % len(full_rows))
    if not full_rows:
        print("  (none - start full-trace in game: Tools -> Start full-trace now)")
    else:
        n = len(full_rows)
        stale = sum(1 for r in full_rows if r["mv"] == "stale")
        zero = sum(1 for r in full_rows if r["mv"] == "zero")
        print("  builds: %s" % sorted(set(r["build"] for r in full_rows)))
        print("  runs %d->%d resets %d->%d mv stale %d/%d zero %d/%d" % (
            full_rows[0]["runs"], full_rows[-1]["runs"],
            full_rows[0]["resets"], full_rows[-1]["resets"],
            stale, n, zero, n))
        print("  present draws frozen att/runs/resets mv mvhash:mvfmt ok/rst cview")
        for r in full_rows[-20:]:
            print("  %d d=%d fz=%d %d/%d/%d %s %s:%s %d/%d %s" % (
                r["present"], r["draws"], r["frozen"],
                r["att"], r["runs"], r["resets"],
                r["mv"], r["mvhash"], r["mvfmt"],
                r["ok"], r["reset"], r["cview"]))
        print("== FULL verdict hints ==")
        for v in full_verdicts(full_rows):
            print("  - " + v)
    print("== FULL new-field walk (last 10, templog builds) ==")
    xr = [r for r in full_rows if r.get("x")]
    if not xr:
        print("  (none - pre-templog build or no extended FULL lines)")
    else:
        print("  present view rejD dproj p00,p11 rot cfg jm fov fidx dage cc")
        for r in xr[-10:]:
            x = r["x"]
            print("  %d %d/%d %d %d %.3f,%.3f %.4f %s.%s.%s.%s %s.%s.%s.%s %.3f:%.3f %d%s %d %d" % (
                r["present"], x["pick"], x["n"], x["drej"], x["dproj"],
                x["p00"], x["p11"], x["rot"],
                x["cfg"][0], x["cfg"][1], x["cfg"][2], x["cfg"][3],
                x["jm"][0], x["jm"][1], x["jm"][2], x["jm"][3],
                x["fovsl"], x["fovtrue"], x["fidx"], x["bang"],
                x["dage"], x["cc"]))
    print("== FULLAGG windows (%d) ==" % len(full_aggs))
    if not full_aggs:
        print("  (none - needs 600 presents on a templog build)")
    else:
        for a in full_aggs:
            print("  presents=%d-%d jit_n=%d mean=%.3f,%.3f quad=%d/%d/%d/%d rot=%.4f/%.4f resets=%d projrej=%d cfg=%s.%s.%s.%s" % (
                a["a"], a["b"], a["jit_n"], a["meanx"], a["meany"],
                a["q00"], a["q01"], a["q10"], a["q11"],
                a["rot_mean"], a["rot_max"], a["resets"], a["projrej"],
                a["cfg"][0], a["cfg"][1], a["cfg"][2], a["cfg"][3]))
        print("== FULLAGG verdicts ==")
        for v in fullagg_verdicts(full_aggs, full_rows):
            print("  - " + v)
    print("== decode verdict hints ==")
    for v in verdicts(fds, cache, means):
        print("  - " + v)
    return 0


if __name__ == "__main__":
    sys.exit(main())
