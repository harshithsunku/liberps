#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that every internal link, asset and #anchor in the static docs site resolves.

usage: check_docs_links.py <site-dir>
External http(s) links are not fetched. Exits 1 and lists every broken reference.
"""
import os
import sys
from html.parser import HTMLParser
from urllib.parse import urlparse


class Collect(HTMLParser):
    def __init__(self):
        super().__init__()
        self.refs, self.ids = [], set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        for key in ("href", "src"):
            if a.get(key):
                self.refs.append(a[key])


def main() -> int:
    site = sys.argv[1]
    pages = {}
    for dirpath, _, files in os.walk(site):
        for f in files:
            if f.endswith(".html"):
                path = os.path.join(dirpath, f)
                c = Collect()
                with open(path, encoding="utf-8") as fh:
                    c.feed(fh.read())
                pages[os.path.normpath(path)] = c
    broken = []
    for page, c in pages.items():
        for ref in c.refs:
            u = urlparse(ref)
            if u.scheme in ("http", "https", "mailto") or ref.startswith("//"):
                continue
            target = page if not u.path else os.path.normpath(os.path.join(os.path.dirname(page), u.path))
            if u.path.endswith("/") or u.path in ("", "./") and not u.fragment:
                target = os.path.join(target if u.path else os.path.dirname(page), "index.html")
            if u.path in ("./",):
                target = os.path.join(os.path.dirname(page), "index.html")
            if not os.path.exists(target):
                broken.append(f"{os.path.relpath(page, site)}: {ref} (missing {os.path.relpath(target, site)})")
                continue
            if u.fragment and target.endswith(".html"):
                ids = pages.get(os.path.normpath(target))
                if ids is None or u.fragment not in ids.ids:
                    broken.append(f"{os.path.relpath(page, site)}: {ref} (no #{u.fragment})")
    for b in broken:
        print("broken:", b)
    print(f"checked {len(pages)} pages, {sum(len(c.refs) for c in pages.values())} references, "
          f"{len(broken)} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
