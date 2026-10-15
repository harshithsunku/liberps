#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the liberps documentation site (docs/*.html) from page fragments.

usage: python3 tools/docsite/build.py [--check]

Each page body lives in tools/docsite/pages/<name>.html. This script wraps it in the shared
head, top bar and footer and writes docs/<name>.html. The site is plain static HTML with no
generator at publish time: GitHub Pages serves docs/ as committed. --check exits 1 if the
committed pages differ from what the fragments would produce, so CI catches stale output.
The ring diagram on the home page is generated here from the ring geometry (to scale).
"""
import html
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAGES_DIR = os.path.join(ROOT, "tools", "docsite", "pages")
OUT_DIR = os.path.join(ROOT, "docs")
SITE = "https://harshithsunku.github.io/liberps/"
REPO = "https://github.com/harshithsunku/liberps"

# (file, nav label, <title>, meta description)
PAGES = [
    ("index", "Home", "liberps — G.8032 ring protection in C",
     "liberps implements ITU-T G.8032 Ethernet Ring Protection Switching for a single ring as a "
     "small, deterministic C11 library, with a command layer, a ring simulator and a Linux daemon."),
    ("getting-started", "Get started", "Get started — liberps",
     "Build liberps, run the tests and the ring simulator, embed the library, and run erpsd on Linux."),
    ("architecture", "Architecture", "Architecture — liberps",
     "How liberps is structured: the pure protocol core, the G.8032 control process data flow, "
     "platforms, and the design decisions behind them."),
    ("api", "C API", "C API reference — liberps",
     "Reference for the liberps C API: configuration, platform callbacks, inputs, timers, "
     "introspection, the instance manager and the R-APS codec."),
    ("cli", "Commands", "Command reference — liberps",
     "The embeddable ERPS command layer: configuration, operator commands, show views in text "
     "and JSON, running-config, and the erps-sim shell."),
    ("linux", "Linux", "Linux daemon — liberps",
     "Deploy erpsd and erpsctl on Linux bridges: dataplane model, configuration, control "
     "protocol and the end-to-end test."),
    ("conformance", "Conformance", "G.8032 conformance — liberps",
     "Clause-by-clause mapping of G.8032 (03/2020) to the liberps implementation and tests, "
     "with every interpretation and its reason."),
    ("quality", "Quality", "Testing and quality — liberps",
     "How liberps is verified: unit, scenario, chaos, fuzz and Linux end-to-end tests, "
     "coverage, and independent conformance reviews."),
]

HEAD = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{title}</title>
<meta name="description" content="{desc}">
<meta property="og:title" content="{title}">
<meta property="og:description" content="{desc}">
<meta property="og:type" content="website">
<meta property="og:url" content="{url}">
<meta name="theme-color" content="#0f161d">
<script>try{{var t=localStorage.getItem("liberps-theme");if(t==="dark"||t==="light")document.documentElement.setAttribute("data-theme",t)}}catch(e){{}}</script>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Semi+Condensed:wght@500;600;700&family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:ital,wght@0,400;0,500;0,600;1,400&display=swap">
<link rel="icon" type="image/svg+xml" href="assets/favicon.svg">
<link rel="stylesheet" href="assets/styles.css">
</head>
<body>
<header class="topbar">
  <div class="frame">
    <a class="brand" href="./">{mark}<span>liberps</span></a>
    <nav class="nav" aria-label="Documentation">
{nav}
    </nav>
    <div class="bar-right">
      <a class="gh" href="{repo}">GitHub</a>
      <button class="theme-btn" id="theme-toggle" type="button" aria-label="Toggle light or dark theme">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41"/></svg>
      </button>
    </div>
  </div>
</header>
<main class="frame">
"""

FOOT = """</main>
<footer class="site">
  <div class="frame">
    <span>liberps · Apache-2.0 · <a href="{repo}">source on GitHub</a></span>
    <span>G.8032/Y.1344 is © ITU. These pages are original documentation of this implementation.</span>
  </div>
</footer>
<script src="assets/docs.js"></script>
</body>
</html>
"""

# Brand mark: a ring of nodes with one blocked (amber) segment, the RPL.
MARK = ('<svg viewBox="0 0 26 26" aria-hidden="true"><circle cx="13" cy="13" r="9" fill="none" '
        'stroke="currentColor" stroke-width="2.2" stroke-dasharray="44 13" '
        'transform="rotate(-62 13 13)" style="color:var(--link)"/><path d="M8.6 4.6 L11.4 6.9" '
        'stroke="var(--block)" stroke-width="3" stroke-linecap="round"/></svg>')

FAVICON = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 26 26"><rect width="26" height="26" rx="6" fill="#0f161d"/><circle cx="13" cy="13" r="8" fill="none" stroke="#74a9d8" stroke-width="2.4" stroke-dasharray="39 11.3" transform="rotate(-62 13 13)"/><path d="M9.3 5.6 L11.8 7.6" stroke="#e8a23c" stroke-width="3" stroke-linecap="round"/></svg>
"""


def ring_svg() -> str:
    """Seven-node ring A..G (Appendix III); link i joins node i port 1 and node i+1 port 0."""
    names, ids = "ABCDEFG", [81, 26, 89, 62, 71, 31, 75]
    cx, cy, r, nr = 230, 215, 150, 22
    pos = []
    for i in range(7):
        th = math.radians(-90 + (i + 0.5) * 360 / 7)
        pos.append((cx + r * math.cos(th), cy + r * math.sin(th), th))
    out = ['<svg class="ringsvg" viewBox="-25 22 520 405" role="img" '
           'aria-label="Seven-node ring A to G; the RPL is the link between G and A">']
    for i in range(7):
        x1, y1, _ = pos[i]
        x2, y2, _ = pos[(i + 1) % 7]
        out.append(f'<line id="l{i}" class="lk" x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}"/>')
        mx, my = (x1 + x2) / 2, (y1 + y2) / 2
        out.append(f'<g id="x{i}" class="xm" visibility="hidden"><circle cx="{mx:.1f}" cy="{my:.1f}" r="9"/>'
                   f'<path d="M{mx-4:.1f} {my-4:.1f}L{mx+4:.1f} {my+4:.1f}M{mx+4:.1f} {my-4:.1f}L{mx-4:.1f} {my+4:.1f}"/></g>')
    x1, y1, _ = pos[6]
    x2, y2, _ = pos[0]
    out.append(f'<text class="rpl-l" x="{(x1+x2)/2:.1f}" y="{(y1+y2)/2-14:.1f}" text-anchor="middle">RPL</text>')
    for i in range(7):
        for port, j in ((1, (i + 1) % 7), (0, (i - 1) % 7)):
            x1, y1, _ = pos[i]
            x2, y2, _ = pos[j]
            dx, dy = x2 - x1, y2 - y1
            length = math.hypot(dx, dy)
            ux, uy = dx / length, dy / length
            px, py = x1 + ux * 33, y1 + uy * 33
            nx, ny = -uy * 10, ux * 10
            on = " on" if (i, port) in ((6, 1), (0, 0)) else ""  # resting state: Idle
            out.append(f'<line id="b{i}-{port}" class="blk{on}" x1="{px-nx:.1f}" y1="{py-ny:.1f}" '
                       f'x2="{px+nx:.1f}" y2="{py+ny:.1f}"/>')
    for i in range(7):
        x, y, th = pos[i]
        out.append(f'<g class="nd"><circle cx="{x:.1f}" cy="{y:.1f}" r="{nr}"/>'
                   f'<text class="nn" x="{x:.1f}" y="{y+5:.1f}" text-anchor="middle">{names[i]}</text></g>')
        lx, ly = cx + (r + 44) * math.cos(th), cy + (r + 44) * math.sin(th)
        anchor = "middle" if abs(math.cos(th)) < 0.3 else ("start" if math.cos(th) > 0 else "end")
        role = " · owner" if i == 6 else (" · neighbour" if i == 0 else "")
        out.append(f'<text class="nid" x="{lx:.1f}" y="{ly+4:.1f}" text-anchor="{anchor}">ID {ids[i]}{role}</text>')
    out.append("</svg>")
    return "\n".join(out)


# ---- a small Markdown subset, for sections rendered from docs/CONFORMANCE.md ----------------

def md_inline(text: str) -> str:
    text = html.escape(text, quote=False)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![*\w])\*([^*]+)\*(?![*\w])", r"<em>\1</em>", text)
    return text


def md_section(path: str, heading: str) -> list:
    """Lines of the '## heading' section of a Markdown file."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    out, inside = [], False
    for line in lines:
        if line.startswith("## "):
            inside = line[3:].strip() == heading
            continue
        if inside:
            out.append(line)
    if not out:
        raise SystemExit(f"section '{heading}' not found in {path}")
    return out


def md_table(lines: list) -> str:
    rows = [l for l in lines if l.startswith("|")]
    head = [c.strip() for c in rows[0].strip("|").split("|")]
    out = ['<div class="tw"><table><thead><tr>' + "".join(f"<th>{md_inline(c)}</th>" for c in head) +
           "</tr></thead><tbody>"]
    for r in rows[2:]:
        cells = [c.strip() for c in r.strip("|").split("|")]
        out.append("<tr>" + "".join(f'<td class="wrap">{md_inline(c)}</td>' for c in cells) + "</tr>")
    out.append("</tbody></table></div>")
    return "\n".join(out)


def md_list(lines: list) -> str:
    """Numbered or bulleted items with continuation lines and one level of '- ' sub-bullets."""
    items = []  # (text, [subitems])
    ordered = None
    for line in lines:
        if not line.strip():
            continue
        m = re.match(r"^(\d+)\.\s+(.*)$", line)
        b = re.match(r"^-\s+(.*)$", line)
        sub = re.match(r"^\s+-\s+(.*)$", line)
        if m or b:
            ordered = bool(m) if ordered is None else ordered
            items.append([(m or b).group(m and 2 or 1), []])
        elif sub and items:
            items[-1][1].append(sub.group(1))
        elif items:
            if items[-1][1]:
                items[-1][1][-1] += " " + line.strip()
            else:
                items[-1][0] += " " + line.strip()
    tag = "ol" if ordered else "ul"
    out = [f'<{tag} class="plain read">']
    for text, subs in items:
        inner = md_inline(text)
        if subs:
            inner += '<ul class="plain">' + "".join(f"<li>{md_inline(s)}</li>" for s in subs) + "</ul>"
        out.append(f"<li>{inner}</li>")
    out.append(f"</{tag}>")
    return "\n".join(out)


def conformance_parts() -> dict:
    path = os.path.join(ROOT, "docs", "CONFORMANCE.md")
    interp = [l for l in md_section(path, "Interpretations") if re.match(r"^(\d+\.|\s+)", l) or not l.strip()]
    return {
        "{{CONF_MATRIX}}": md_table(md_section(path, "Clause matrix")),
        "{{CONF_INTERP}}": md_list(interp),
        "{{CONF_LIMITS}}": md_list(md_section(path, "Known limitations")),
    }


def render(name: str, title: str, desc: str) -> str:
    with open(os.path.join(PAGES_DIR, name + ".html"), encoding="utf-8") as f:
        body = f.read()
    body = body.replace("{{RING_SVG}}", ring_svg())
    if "{{CONF_" in body:
        for key, value in conformance_parts().items():
            body = body.replace(key, value)
    nav = []
    for file, label, _, _ in PAGES:
        if file == "index":
            continue
        cur = ' aria-current="page"' if file == name else ""
        nav.append(f'      <a href="{file}.html"{cur}>{label}</a>')
    url = SITE + ("" if name == "index" else name + ".html")
    head = HEAD.format(title=title, desc=desc, url=url, mark=MARK, nav="\n".join(nav), repo=REPO)
    return head + body.rstrip("\n") + "\n" + FOOT.format(repo=REPO)


def main() -> int:
    check = "--check" in sys.argv[1:]
    outputs = {os.path.join(OUT_DIR, f + ".html"): render(f, t, d) for f, _, t, d in PAGES}
    outputs[os.path.join(OUT_DIR, "assets", "favicon.svg")] = FAVICON
    outputs[os.path.join(OUT_DIR, ".nojekyll")] = ""
    stale = []
    for path, text in outputs.items():
        current = None
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                current = f.read()
        if current != text:
            stale.append(os.path.relpath(path, ROOT))
            if not check:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
    if check and stale:
        print("docs site out of date; run: python3 tools/docsite/build.py")
        for p in stale:
            print("  " + p)
        return 1
    print(("checked " if check else "wrote ") + f"{len(outputs)} files" +
          (f" ({len(stale)} changed)" if not check else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
