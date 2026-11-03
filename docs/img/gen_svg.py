#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the logo and the README figures (docs/img/*.svg).

The logo is the ring mark: eight record slots of a lock-free ring, the newest
record orange, earlier ones blue, free slots grey (64-unit grid, radius 22,
stroke 8, round ends, slots every 45 degrees; four thicker slots at favicon
size). docs/flyer.html animates the same geometry.

Each figure is written in a light and a dark variant; README.md picks one with
<picture> and prefers-color-scheme. Every number in the charts and tiles is read
from bench/results.txt, so rerun this script after bench/run.sh.
"""
import pathlib
import re
import sys
from html import escape

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]

SANS = "Inter, -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "'JetBrains Mono', ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"

# Same tokens as docs/flyer.html; the series pair passes the dataviz palette
# checks (lightness band, chroma, CVD and normal-vision separation, contrast)
# against each mode's surface.
THEMES = {
    "light": dict(surface="#fcfcfb", surface2="#efeee9", line="#e2e0d9", grid="#e6e4de",
                  ink="#0b0b0b", ink2="#52514e", ink3="#74736d", accent="#2a78d6",
                  s1="#2a78d6", s2="#eb6834", free="#dedcd4"),
    "dark": dict(surface="#1a1a19", surface2="#232321", line="#2f2f2c", grid="#2c2c2a",
                 ink="#f0efec", ink2="#c3c2b7", ink3="#9a998f", accent="#3987e5",
                 s1="#3987e5", s2="#d95926", free="#3a3a37"),
}
SERIES = (("rv32", "RV32 (rv32im_zce_zbb)", "s1"), ("armv7m", "Cortex-M4F", "s2"))


def parse(path):
    """{(arch, opt, caller): {row: (bytes, insns)}} from the markdown tables."""
    out, cur = {}, None
    for line in path.read_text().splitlines():
        m = re.match(r"## (\w+), clang -(\w+)(, EXTRACE_CFG_FTRACE_CALLER=1)?:", line)
        if m:
            cur = out.setdefault((m[1], m[2], bool(m[3])), {})
            continue
        m = re.match(r"\| ([^|]+?) \| (\d+) \| (\d+) \|", line)
        if m and cur is not None:
            cur[m[1]] = (int(m[2]), int(m[3]))
    return out


def svg(w, h, t, body, title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}" role="img" aria-label="{escape(title)}">\n'
            f'<title>{escape(title)}</title>\n'
            f'<rect x="0.5" y="0.5" width="{w - 1}" height="{h - 1}" rx="12" '
            f'fill="{t["surface"]}" stroke="{t["line"]}"/>\n{body}</svg>\n')


def text(x, y, s, fill, size=13, weight=400, anchor="start", font=SANS):
    return (f'<text x="{x}" y="{y}" fill="{fill}" font-family="{font}" font-size="{size}" '
            f'font-weight="{weight}" text-anchor="{anchor}">{escape(s)}</text>\n')


def bar(x, y, w, h, fill, r=4):
    """Bar anchored at x; only the data end (right) is rounded."""
    r = min(r, w, h / 2)
    return (f'<path d="M{x},{y} H{x + w - r} Q{x + w},{y} {x + w},{y + r} '
            f'V{y + h - r} Q{x + w},{y + h} {x + w - r},{y + h} H{x} Z" fill="{fill}"/>\n')


def legend(x, y, t):
    out = ""
    for _, name, key in SERIES:
        out += f'<rect x="{x}" y="{y - 9}" width="10" height="10" rx="2" fill="{t[key]}"/>\n'
        out += text(x + 16, y, name, t["ink2"], 12)
        x += 30 + 7 * len(name)
    return out


def hbars(t, title, sub, rows, vmax, step):
    """rows: [(label, note, {arch: value})] -> grouped horizontal bar chart."""
    w, left, right, top = 880, 210, 50, 92
    bh, gap, rgap = 11, 2, 16
    rowh = 2 * bh + gap + rgap
    h = top + len(rows) * rowh + 34
    pw = w - left - right
    sx = lambda v: left + pw * v / vmax
    b = text(24, 34, title, t["ink"], 16, 600)
    b += text(24, 55, sub, t["ink3"], 12)
    b += legend(24, 78, t)
    base = top + len(rows) * rowh - rgap + 6
    for v in range(0, vmax + 1, step):
        x = sx(v)
        b += f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{base}" stroke="{t["grid"]}"/>\n'
        b += text(round(x, 1), base + 16, str(v), t["ink3"], 11, anchor="middle")
    for i, (label, note, vals) in enumerate(rows):
        y = top + i * rowh
        b += text(left - 12, y + bh + 1, label, t["ink"], 12.5, 500, "end")
        if note:
            b += text(left - 12, y + 2 * bh + gap + 2, note, t["ink3"], 11, anchor="end")
        for j, (arch, _, key) in enumerate(SERIES):
            v = vals[arch]
            by = y + j * (bh + gap)
            b += bar(left, by, sx(v) - left, bh, t[key])
            b += text(round(sx(v) + 6, 1), by + bh - 1.5, str(v), t["ink2"], 11)
    b += f'<line x1="{left}" y1="{top - 6}" x2="{left}" y2="{base}" stroke="{t["ink3"]}"/>\n'
    return svg(w, h, t, b, title)


def cost_chart(t, d):
    rv, m4 = d[("rv32", "O2", False)], d[("armv7m", "O2", False)]
    spec = [("EXTRACE_MARK", "MARK"), ("EXTRACE (NONE)", "NONE"),
            ("EXTRACE_INTS, 1 arg", "INTS 1 arg"), ("ftrace MARK", "FTRACE MARK"),
            ("ftrace NONE", "FTRACE NONE"), ("ftrace ARGS(1)", "FTRACE ARGS 1 arg"),
            ("ftrace ARGS(4)", "FTRACE ARGS 4 args")]
    rows = [(lab, f"{rv[k][0]}-byte record", {"rv32": rv[k][1], "armv7m": m4[k][1]})
            for lab, k in spec]
    return hbars(t, "Instructions per trace event",
                 "Trace point to return, steady state. clang 18 -O2, library defaults. "
                 "Fewer is better.", rows, 300, 50)


def caller_chart(t, d):
    rv, m4 = d[("rv32", "O2", True)], d[("armv7m", "O2", True)]
    rows = []
    for lab, k in (("MARK", "FTRACE MARK"), ("NONE", "FTRACE NONE"), ("ARGS(1)", "FTRACE ARGS 1 arg")):
        for suffix, key in (("", k), (" | CALLER", k + ", caller")):
            rows.append((lab + suffix, f"{rv[key][0]}-byte record",
                         {"rv32": rv[key][1], "armv7m": m4[key][1]}))
    return hbars(t, "The cost of recording the caller",
                 "ftrace flavors without and with CALLER, EXTRACE_CFG_FTRACE_CALLER=1 build, "
                 "clang 18 -O2.", rows, 250, 50)


def tiles(t, d):
    rv, m4 = d[("rv32", "O2", False)], d[("armv7m", "O2", False)]
    items = [("Event switched off", f'{rv["MARK, event off"][1]}', f'/ {m4["MARK, event off"][1]} insns',
              "load, bit test, branch"),
             ("Traced function, unpatched", f'{rv["FTRACE not patched"][1]}',
              f'/ {m4["FTRACE not patched"][1]} insns', "only the NOPs in its entry pad"),
             ("Patched, FTRACE switch off", f'{rv["FTRACE patched, switch off"][1]}',
              f'/ {m4["FTRACE patched, switch off"][1]} insns', "RV32 / Cortex-M4"),
             ("Smallest function record", f'{rv["FTRACE MARK"][0]}', "bytes",
              "header and a 2-byte address")]
    w, h, pad, g = 880, 128, 16, 12
    tw = (w - 2 * pad - 3 * g) / 4
    b = ""
    for i, (lbl, big, small, note) in enumerate(items):
        x = pad + i * (tw + g)
        b += (f'<rect x="{x:.1f}" y="{pad}" width="{tw:.1f}" height="{h - 2 * pad}" rx="10" '
              f'fill="{t["surface2"]}"/>\n')
        b += text(round(x + 14, 1), pad + 24, lbl, t["ink2"], 12, 500)
        b += text(round(x + 14, 1), pad + 62, big, t["ink"], 34, 700)
        b += text(round(x + 18 + 21 * len(big), 1), pad + 62, small, t["ink3"], 13)
        b += text(round(x + 14, 1), pad + 84, note, t["ink3"], 11)
    return svg(w, h, t, b, "Cost of tracing when it is off, in instructions")


def record(t):
    cells = [("42", "hdr"), ("19", "tsd"), ("06", "len"), ("00", "sign"),
             ("C7 34", "addr"), ("93 18", "caller"), ("07", "a0")]
    w, h = 880, 150
    b = text(24, 34, "FTRACE ARGS(1) with caller: 9 bytes", t["ink"], 16, 600)
    x = 24
    for val, lab in cells:
        cw = 26 + 10 * len(val)
        fill = t["s1"] if lab in ("addr", "caller") else t["surface2"]
        ink = "#ffffff" if lab in ("addr", "caller") else t["ink"]
        b += f'<rect x="{x}" y="52" width="{cw}" height="40" rx="6" fill="{fill}"/>\n'
        b += text(x + cw / 2, 77, val, ink, 15, 500, "middle", MONO)
        b += text(x + cw / 2, 110, lab, t["ink3"], 11, anchor="middle", font=MONO)
        x += cw + 4
    b += text(24, 136, "Varint header, timestamp delta, then the payload. addr is the "
              "return address minus ftrace_base, carried once in the stream's INFO record.",
              t["ink3"], 12)
    return svg(w, h, t, b, "Example FTRACE record with caller, 9 bytes")


# Ring mark: slot colors, newest record at lower left, the head moving clockwise.
RING = ("s1", "s1", "s1", "s1", "s2", "free", "free", "free")
RING_SMALL = ("s1", "s1", "s2", "free")


def ring(t, x=0, y=0, size=64, small=False):
    """The ring mark on its 64-unit grid, placed at (x, y) and scaled to size px."""
    if small:
        attrs, r, slots, rot0, step = 'stroke-width="12" stroke-dasharray="13.4 112.26"', 20, RING_SMALL, -64.2, 90
    else:
        attrs, r, slots, rot0, step = 'stroke-width="8" stroke-dasharray="5.78 132.45"', 22, RING, -75, 45
    g = (f'<g transform="translate({x} {y}) scale({size / 64:g})" fill="none" '
         f'stroke-linecap="round" {attrs}>\n')
    for i, key in enumerate(slots):
        g += (f'  <circle cx="32" cy="32" r="{r}" stroke="{t[key]}" '
              f'transform="rotate({rot0 + step * i:g} 32 32)"/>\n')
    return g + "</g>\n"


def wordmark(t, x, y, size):
    return (f'<text x="{x}" y="{y}" font-family="{SANS}" font-size="{size}" font-weight="700" '
            f'letter-spacing="{-0.045 * size:.2f}" fill="{t["ink"]}">'
            f'<tspan fill="{t["accent"]}">ex</tspan>trace</text>\n')


def logo(t):
    """Lockup on a transparent ground: mark and wordmark."""
    w, h = 400, 112
    body = ring(t, 4, 8, 96) + wordmark(t, 108, 82, 88)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
            f'role="img" aria-label="extrace">\n<title>extrace</title>\n{body}</svg>\n')


def mark(t, small=False):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64" '
            'role="img" aria-label="extrace">\n<title>extrace</title>\n'
            f'{ring(t, small=small)}</svg>\n')


def favicon():
    """Four-slot mark; the free slot follows the browser's color scheme."""
    t = dict(THEMES["light"], free="var(--free)")
    style = ("<style>:root{--free:#d2d0c7}"
             "@media (prefers-color-scheme: dark){:root{--free:#4a4a46}}</style>\n")
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">\n'
            + style + ring(t, small=True).replace('stroke="var(--free)"', 'style="stroke:var(--free)"')
            + '</svg>\n')


def banner(t):
    w, h = 880, 268
    b = ring(t, 26, 20, 80) + wordmark(t, 114, 86, 72)
    lede = ["Tracing for heterogeneous multi-core SoCs, where every core runs its own",
            "firmware and the shared memory is not cache-coherent. Each core writes",
            "compact records into its own lock-free ring; one dispatcher reads them all."]
    for i, line in enumerate(lede):
        b += text(34, 120 + 22 * i, line, t["ink2"], 15)
    chips = ["2–8 byte records", "no locks between cores", "per-function ftrace flavors",
             "caller recording", "ARMv7-M · RV32", "freestanding C23"]
    x, y = 34, 196
    for c in chips:
        cw = 20 + 7.3 * len(c)
        if x + cw > w - 32:
            x, y = 34, y + 34
        b += (f'<rect x="{x:.1f}" y="{y}" width="{cw:.1f}" height="26" rx="13" '
              f'fill="{t["surface2"]}" stroke="{t["line"]}"/>\n')
        b += text(round(x + cw / 2, 1), y + 17.5, c, t["ink2"], 12, anchor="middle", font=MONO)
        x += cw + 8
    return svg(w, h, t, b, "extrace: tracing for heterogeneous multi-core SoCs")


def main():
    d = parse(ROOT / "bench/results.txt")
    figs = {"banner": lambda t: banner(t), "stats": lambda t: tiles(t, d),
            "cost": lambda t: cost_chart(t, d), "caller": lambda t: caller_chart(t, d),
            "record": lambda t: record(t)}
    figs.update({"logo": logo, "mark": mark})
    for name, fn in figs.items():
        for mode, t in THEMES.items():
            (HERE / f"{name}-{mode}.svg").write_text(fn(t))
    (HERE / "favicon.svg").write_text(favicon())
    return 0


if __name__ == "__main__":
    sys.exit(main())
