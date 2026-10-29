#!/usr/bin/env python3
"""Regenerate the intent catalog in docs/intents.md from the `intents` blocks of apps/*/app.json.

    python3 tools/gen_intents_doc.py
"""
import glob, json, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "intents.md"
BEGIN, END = "<!-- catalog:begin -->", "<!-- catalog:end -->"


def fmt_ret(r):
    if isinstance(r, dict):
        return "record {" + ", ".join(f"{k}: {t}" for k, t in r.items()) + "}"
    return r


def main():
    out = []
    for f in sorted(glob.glob(str(ROOT / "apps" / "*" / "app.json"))):
        meta = json.load(open(f))
        intents = meta.get("intents")
        if not intents:
            continue
        out.append(f"### {meta['name']}\n")
        out.append("| Intent | What it does | Arguments | Returns |")
        out.append("|---|---|---|---|")
        for name, spec in intents.items():
            args = ", ".join(f"`{a}`: {t}" for a, t in spec.get("args", {}).items()) or "—"
            kind = " (launch)" if spec.get("launch") else ""
            out.append(f"| `{name}`{kind} | {spec['title']} | {args} | {fmt_ret(spec.get('returns', 'ok'))} |")
        out.append("")
    text = DOC.read_text()
    a, b = text.index(BEGIN) + len(BEGIN), text.index(END)
    DOC.write_text(text[:a] + "\n\n" + "\n".join(out) + "\n" + text[b:])
    print("catalog updated")


if __name__ == "__main__":
    main()
