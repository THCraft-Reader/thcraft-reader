#!/usr/bin/env python3
"""Embed the conversion engine and publish a dependency-free static HTML page."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def encoded(path: Path) -> str:
    return base64.b64encode(path.read_bytes()).decode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site-dir", type=Path, help="Existing static THCraft site root")
    args = parser.parse_args()
    template = (ROOT / "web/index.html").read_text(encoding="utf-8")
    inputs = {
        "ENGINE": ROOT / "build/engine.js",
        "WASM": ROOT / "build/engine.wasm",
        "WORKER": ROOT / "web/worker.js",
        "LICENSES": ROOT / "dist/LICENSES.txt",
        "MASCOT": ROOT / "web/thcraft-mascot.webp",
    }
    for name, path in inputs.items():
        if not path.is_file():
            raise SystemExit(f"Missing {path}; run python build.py first.")
        if name not in ("WASM", "LICENSES"):
            template = template.replace(f"@@{name}@@", encoded(path))
    app = (ROOT / "web/app.js").read_text(encoding="utf-8")
    if "</script" in app.lower():
        raise SystemExit("Application JavaScript must not contain an HTML script end tag.")
    template = template.replace("@@APP@@", app).replace("@@HOME@@", "../")
    standalone = (template.replace("@@WASM@@", encoded(inputs["WASM"]))
                  .replace("@@LICENSES@@", encoded(inputs["LICENSES"]))
                  .replace("@@LICENSE_NOTICE@@", '<pre id="notices"></pre>')
                  .replace("@@WASM_URL@@", "").replace("@@CONNECT@@", "'none'"))
    if "@@" in standalone:
        raise SystemExit("Unresolved template placeholder.")
    destination = ROOT / "dist/fonts-generator.html"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(standalone, encoding="utf-8", newline="\n")
    manifest = {"files": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in [*inputs.values(), ROOT / "web/app.js", ROOT / "web/index.html"]},
                "html_sha256": hashlib.sha256(destination.read_bytes()).hexdigest()}
    (ROOT / "dist/build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Built {destination} ({destination.stat().st_size:,} bytes)")
    if args.site_dir:
        site = args.site_dir.resolve()
        if not (site / "index.html").is_file():
            raise SystemExit(f"Not an existing static site: {site}")
        output = site / "font-generator/index.html"
        output.parent.mkdir(parents=True, exist_ok=True)
        wasm_bytes = inputs["WASM"].read_bytes()
        wasm_name = f"engine.{hashlib.sha256(wasm_bytes).hexdigest()[:16]}.wasm"
        (output.parent / wasm_name).write_bytes(wasm_bytes)
        (output.parent / "LICENSES.txt").write_text(
            inputs["LICENSES"].read_text(encoding="utf-8"), encoding="utf-8-sig", newline="\n")
        website = (template.replace("@@WASM@@", "").replace("@@WASM_URL@@", f"./{wasm_name}")
                   .replace("@@LICENSES@@", "")
                   .replace("@@LICENSE_NOTICE@@", '<a href="./LICENSES.txt">View software licenses</a>')
                   .replace("@@CONNECT@@", "'self'"))
        output.write_text(website, encoding="utf-8", newline="\n")
        print(f"Published local site page: {output}")


if __name__ == "__main__":
    main()
