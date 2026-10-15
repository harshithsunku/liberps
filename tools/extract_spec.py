#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the local (gitignored) G.8032 reference material from the ITU-T PDF.

usage: python3 tools/extract_spec.py <T-REC-G.8032-202003-I!!PDF-E.pdf> [out-dir]

Writes <out-dir>/G.8032-2020-03.txt (full text, page-delimited) and <out-dir>/figures/*.png
(the figures the implementation notes refer to). Requires PyMuPDF (pip install pymupdf).
The output is (c) ITU and must not be committed or redistributed; .gitignore excludes it.
"""
import os
import sys

FIGURES = {
    14: "fig9-1_normal", 15: "fig9-2_sf", 17: "fig9-4_node_model",
    28: "fig10-1_erp_control_process", 32: "table10-2_p1", 33: "table10-2_p2",
    34: "table10-2_p3", 35: "table10-2_p4", 36: "table10-2_p5", 37: "table10-2_p6",
    38: "table10-2_p7", 49: "fig10-2_raps_pdu", 50: "fig10-3_raps_info",
    56: "figIII-1_link_failure", 57: "figIII-2_revertive_recovery",
    58: "figIII-3_nonrevertive", 59: "figIII-4_unidir", 60: "figIII-5_unidir_recovery",
    61: "figIII-6_rpl_failure", 62: "figIII-7_rpl_recovery", 64: "figIII-8_multiple_failure",
    67: "figIV-1_guard",
}


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    import fitz  # PyMuPDF

    pdf = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join("docs", "spec")
    os.makedirs(os.path.join(out, "figures"), exist_ok=True)
    doc = fitz.open(pdf)
    with open(os.path.join(out, "G.8032-2020-03.txt"), "w", encoding="utf-8") as f:
        f.write("ITU-T G.8032/Y.1344 (03/2020) Ethernet ring protection switching -- text "
                "extraction (PyMuPDF)\nLocal reference copy only; (c) ITU. Not for "
                "redistribution.\n\n")
        for i, page in enumerate(doc):
            f.write(f"\n===== PDF page {i + 1} =====\n")
            f.write(page.get_text())
    for page, name in FIGURES.items():
        if page <= doc.page_count:
            doc[page - 1].get_pixmap(dpi=110).save(
                os.path.join(out, "figures", f"p{page:02d}_{name}.png"))
    print(f"wrote {out}/G.8032-2020-03.txt and {len(FIGURES)} figure pages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
