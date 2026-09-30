#!/usr/bin/env python3
"""Prepare explicit, hash-verified Thai CPFonts and desktop-only visual references.

Requires fonttools, freetype-py, uharfbuzz and Pillow (plus converter requirements).
No downloads or font discovery are performed by the offline regression runner.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
FALLBACK = ROOT / "lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf"
FALLBACK_SHA256 = "fe8c022f48d8dd29f17b744d16f9346f4357e16f7d4f7be58b000ae7c291b614"
CONVERTER = ROOT / "lib/EpdFont/scripts/fontconvert_sdcard.py"
SIZES = (12, 14, 16, 18)
INTERVALS = "ascii,punctuation,(0x0E00-0x0E7F)"
FONTS = (
    ("noto-sans-thai", "notosansthai", "NotoSansThai%5Bwdth%2Cwght%5D.ttf",
     "5a1c559bb539583c8a1fd99d1c5b9491e5e14478c9cd2bd0970d5c3096cc9ef8",
     "34b48ab6f74867dbfce19410a2f452abef34e3ff"),
    ("noto-serif-thai", "notoserifthai", "NotoSerifThai%5Bwdth%2Cwght%5D.ttf",
     "34a7ad11647c845303aabdde639059806c56b84719e5d2ceb28eb038711bdf53",
     "ef511270e21b250f7befe58b33bb3b44edf58369"),
    ("sarabun", "sarabun", "Sarabun-Regular.ttf",
     "226d4f368fbc0457990ddef2692679badfd2c1a4e89e5ac4d43c10ba7743b2f1",
     "161c9ac9a14e26dba0b9be14496fae451e5c2e24"),
)
PACK_FALLBACK_FONTS = (
    ("NotoSansSymbols", "notosanssymbols", "NotoSansSymbols%5Bwght%5D.ttf",
     "f7e7e04b4a24b6c78893d50cbfd2b2f6cae49617ab047bfef668d252adb128f7",
     "a0061bfbcbbec27bf2b280fc8c49ea42cf7cb7ab"),
    ("NotoSansSymbols2", "notosanssymbols2", "NotoSansSymbols2-Regular.ttf",
     "7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21",
     "caf89dd0e60e23ac39ce18da823095959d409437"),
    ("NotoSansMath", "notosansmath", "NotoSansMath-Regular.ttf",
     "3f495fe933c06786e4d5f6d86b8ee70b6753a68ee3b9d87528726de0f6e2c47d",
     "7e505c9bad286732f4d6daf0165e6b6cbbe57079"),
)
ARABIC_FALLBACK_SHA256 = "252629ca0e87b6233851249b8cbf7b43445211a8caf199f1b306a19202251508"
SHEET_TEXT = (
    "ก่ ก้ ก๊ ก๋", "กิ กี กึ กื", "กุ กู", "กี่ กุ่",
    "เก่ง น้ำ ตั้ง เรื่อง อ่าน", "ผู้หญิง ประเทศไทย โรงพยาบาล หนังสือ ภาษาไทย",
    "ป ฝ ฟ ปี่ ฝี่ ฟี่", "ญู ฐุ", "นํ้า", "EpubCraft เป็น EPUB Editor",
    "Version 4.2.0 รองรับภาษาไทย", "บทที่ 12 เวลา 02:40 น.",
    "“สวัสดีครับ” เขากล่าว", "คุณทำอะไรอยู่?", "ประเทศไทย...แล้วอย่างไรต่อ",
    "ประเทศไทยมีประชากรจำนวนมาก", "วันนี้ผมเดินทางไปโรงพยาบาลเพื่อพบคุณหมอ",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def dependencies() -> dict:
    versions = {name: importlib.metadata.version(name)
                for name in ("fonttools", "freetype-py", "uharfbuzz", "Pillow")}
    import freetype
    import uharfbuzz as hb
    versions.update(python=sys.version, freetype=".".join(map(str, freetype.version())),
                    harfbuzz=hb.version_string())
    return versions


def download(url: str) -> bytes:
    try:
        with urllib.request.urlopen(url, timeout=120) as response:
            return response.read()
    except (OSError, urllib.error.URLError) as error:
        raise RuntimeError(f"Download failed: {url}: {error}") from error


def fetch_verified(path: Path, family: str, filename: str, expected: str, blob: str) -> str:
    blob_url = f"https://api.github.com/repos/google/fonts/git/blobs/{blob}"
    if path.exists():
        if digest(path) == expected:
            return blob_url
        raise RuntimeError(f"Hash mismatch in cached source {path}; remove it explicitly before retrying")
    errors = []
    source_urls = (f"https://raw.githubusercontent.com/google/fonts/main/ofl/{family}/{filename}", blob_url)
    for url in source_urls:
        try:
            data = download(url)
            if url == blob_url:
                payload = json.loads(data)
                if payload.get("sha") != blob or payload.get("encoding") != "base64":
                    raise ValueError("Unexpected Git blob identity or encoding")
                data = base64.b64decode("".join(payload["content"].split()), validate=True)
            actual = hashlib.sha256(data).hexdigest()
            if actual != expected:
                raise ValueError(f"SHA-256 {actual}, expected {expected}")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            return url
        except (OSError, ValueError, KeyError, RuntimeError) as error:
            errors.append(f"{url}: {error}")
    raise RuntimeError("No verified source available:\n" + "\n".join(errors))


def fetch_license(path: Path, family: str) -> str:
    url = f"https://raw.githubusercontent.com/google/fonts/main/ofl/{family}/OFL.txt"
    if not path.exists():
        data = download(url)
        if b"SIL OPEN FONT LICENSE" not in data.upper() or b"1.1" not in data:
            raise RuntimeError(f"Unexpected OFL response: {url}")
        path.write_bytes(data)
    elif b"SIL OPEN FONT LICENSE" not in path.read_bytes().upper():
        raise RuntimeError(f"Invalid cached license: {path}")
    return url


def freeze(source: Path, target: Path) -> dict:
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont
    font = TTFont(source, recalcTimestamp=False)
    axes = {}
    if "fvar" in font:
        axes = {axis.axisTag: {"wght": 400, "wdth": 100}.get(axis.axisTag, axis.defaultValue)
                for axis in font["fvar"].axes}
        font = instantiateVariableFont(font, axes, inplace=True)
    font.recalcTimestamp = False
    font.save(target)
    font.close()
    return axes


def prepare_pack_fallbacks(output: Path) -> list[dict]:
    records = []
    for name, family, filename, expected, blob in (*PACK_FALLBACK_FONTS,
            ("NotoSansArabic", "NotoSansArabic", "NotoSansArabic-Regular.ttf",
             ARABIC_FALLBACK_SHA256, None)):
        directory = output / "sources" / family
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "original.ttf"
        license_path = directory / "OFL.txt"
        if blob:
            fetch_verified(source, family, filename, expected, blob)
            fetch_license(license_path, family)
        else:
            bundled = FALLBACK.parent.parent / family / filename
            if digest(bundled) != expected:
                raise RuntimeError(f"Checked-in fallback hash mismatch: {bundled}")
            shutil.copyfile(bundled, source)
            shutil.copyfile(bundled.parent / "OFL.txt", license_path)
        regular = directory / "regular.ttf"
        axes = freeze(source, regular)
        records.append({"name": name, "source": source.relative_to(output).as_posix(),
                        "source_sha256": expected, "source_git_blob": blob,
                        "path": regular.relative_to(output).as_posix(), "sha256": digest(regular),
                        "frozen_axes": axes, "license": license_path.relative_to(output).as_posix(),
                        "license_sha256": digest(license_path)})
    return records


def cmap_for(path: Path) -> set[int]:
    from fontTools.ttLib import TTFont
    with TTFont(path) as font:
        return set(font.getBestCmap())


def half_fp(value):
    fixed = math.floor(value * 16 + 0.5)
    return (-((-fixed + 1) // 2) if fixed < 0 else (fixed + 1) // 2) / 16


class ReferenceRenderer:
    """HarfBuzz positions + native-hinted FreeType bitmaps, never paragraph layout.

    The converter's two-stage 8→4→2-bit thresholds equal floor(coverage/64).
    FreeType uses precisely its FT_LOAD_RENDER flags and 150 DPI char size.
    Positions are shaped at the exact fractional ppem, snapped once at placement.
    Font fallback follows the converter's primary-cmap-then-explicit-fallback rule.
    """
    def __init__(self, font: Path, fallback: Path, point_size: int):
        import freetype
        import uharfbuzz as hb
        self.ft = freetype
        self.hb = hb
        self.faces = []
        self.fonts = []
        self.cmaps = []
        self.point_size = point_size
        for path in (font, fallback):
            face = freetype.Face(str(path))
            face.set_char_size(point_size << 6, point_size << 6, 150, 150)
            self.faces.append(face)
            hbfont = hb.Font(hb.Face(path.read_bytes()))
            hb.ot_font_set_funcs(hbfont)
            scale = round(point_size * 150 / 72 * 64)
            hbfont.scale = (scale, scale)
            hbfont.ppem = (face.size.x_ppem, face.size.y_ppem)
            self.fonts.append(hbfont)
            self.cmaps.append(cmap_for(path))
        self.ascender = math.ceil(self.faces[0].size.ascender / 64)
        self.line_height = math.ceil(self.faces[0].size.height / 64)

    def missing(self, text: str) -> list[str]:
        return sorted({f"U+{ord(ch):04X}" for ch in text if not ch.isspace()
                       and ord(ch) not in self.cmaps[0] and ord(ch) not in self.cmaps[1]
                       and ord(ch) not in (0x200B, 0x00AD)})

    def draw(self, image, text: str, x: int, y: int, *, tracking=0, word_spacing=100,
             rotated=False, ascender=None, clusters=None, scale_half=False) -> dict:
        from PIL import Image, ImageChops
        missing = self.missing(text)
        if missing:
            raise RuntimeError("Reference glyph coverage missing: " + ", ".join(missing))
        ascender = self.ascender if ascender is None else ascender
        outer = {}
        for span in clusters or ():
            if span["thai"] and span["valid"]:
                for scalar in range(span["begin"], span["end"]):
                    outer[scalar] = span["begin"]
        runs = []
        for index, ch in enumerate(text):
            face_id = 0 if ord(ch) in self.cmaps[0] else 1
            if runs and runs[-1][0] == face_id:
                runs[-1][2] += ch
            else:
                runs.append([face_id, index, ch])
        pen = 0.0
        records = []
        bounds = None
        previous_cluster, previous_thai = None, False
        for face_id, start, run in runs:
            buffer = self.hb.Buffer()
            buffer.add_str(run)
            buffer.guess_segment_properties()
            if any(0xE00 <= ord(ch) <= 0xE7F for ch in run):
                buffer.script, buffer.language, buffer.direction = "thai", "th", "ltr"
            self.hb.shape(self.fonts[face_id], buffer)
            infos, positions = buffer.glyph_infos, buffer.glyph_positions
            for index, (info, position) in enumerate(zip(infos, positions)):
                source_scalar = start + info.cluster
                current_thai = source_scalar in outer
                current_cluster = outer.get(source_scalar, source_scalar)
                if current_cluster != previous_cluster and (current_thai or previous_thai):
                    pen = math.floor(pen + 0.5)
                previous_cluster, previous_thai = current_cluster, current_thai
                x_offset = ((position.x_offset + 2) // 4) / 16 if current_thai else position.x_offset / 64
                y_offset = ((position.y_offset + 2) // 4) / 16 if current_thai else position.y_offset / 64
                advance = ((position.x_advance + 2) // 4) / 16 if current_thai else position.x_advance / 64
                if scale_half:
                    x_offset, y_offset, advance = map(half_fp, (x_offset, y_offset, advance))
                face = self.faces[face_id]
                face.load_glyph(info.codepoint, self.ft.FT_LOAD_RENDER)
                bitmap = face.glyph.bitmap
                raw = bitmap.buffer
                pixels = bytearray()
                stride = abs(bitmap.pitch)
                for row in range(bitmap.rows):
                    offset = (row if bitmap.pitch >= 0 else bitmap.rows - 1 - row) * stride
                    pixels.extend(255 - (raw[offset + col] // 64) * 85 for col in range(bitmap.width))
                bearing = math.trunc(face.glyph.bitmap_left / 2) if scale_half else face.glyph.bitmap_left
                glyph_top = math.trunc(face.glyph.bitmap_top / 2) if scale_half else face.glyph.bitmap_top
                left = math.floor(pen + x_offset + 0.5) + bearing
                top = ascender - glyph_top - math.floor(y_offset + 0.5)
                if bitmap.width and bitmap.rows:
                    glyph = Image.frombytes("L", (bitmap.width, bitmap.rows), bytes(pixels))
                    if scale_half:
                        reduced = Image.new("L", ((bitmap.width + 1) // 2, (bitmap.rows + 1) // 2), 255)
                        for dy in range(reduced.height):
                            for dx in range(reduced.width):
                                coverage = sum((255 - pixels[sy * bitmap.width + sx]) // 85
                                               for sy in range(dy * 2, min(dy * 2 + 2, bitmap.rows))
                                               for sx in range(dx * 2, min(dx * 2 + 2, bitmap.width)))
                                if coverage >= 2:
                                    reduced.putpixel((dx, dy), 0)
                        glyph = reduced
                    if rotated:
                        glyph = glyph.transpose(Image.Transpose.ROTATE_90)
                        gx, gy = x + top, y - left - glyph.height + 1
                    else:
                        gx, gy = x + left, y + top
                    box = (gx, gy, gx + glyph.width, gy + glyph.height)
                    # Minimum is deterministic dark-ink blending on the white framebuffer.
                    image.paste(ImageChops.darker(image.crop(box), glyph), (gx, gy))
                    bounds = (box if bounds is None else
                              (min(bounds[0], box[0]), min(bounds[1], box[1]),
                               max(bounds[2], box[2]), max(bounds[3], box[3])))
                records.append({"glyph_id": info.codepoint, "face": face_id,
                                "source_character": start + info.cluster, "pen": pen,
                                "x_offset": x_offset, "y_offset": y_offset,
                                "advance": advance})
                if info.cluster < len(run) and run[info.cluster] == " ":
                    advance *= word_spacing / 100
                pen += advance
                if index + 1 < len(infos):
                    current_scalar = start + info.cluster
                    next_scalar = start + infos[index + 1].cluster
                    if (outer.get(current_scalar, current_scalar) != outer.get(next_scalar, next_scalar)
                            and not text[current_scalar].isspace() and not text[next_scalar].isspace()):
                        pen += tracking
            if start + len(run) < len(text) and not run[-1].isspace() and not text[start + len(run)].isspace():
                pen += tracking
        if previous_thai:
            pen = math.floor(pen + 0.5)
        return {"text": text, "x": x, "y": y, "baseline": y + ascender,
                "advance": pen, "ink_bounds": list(bounds) if bounds else None,
                "glyphs": records}


def control_source(output: Path, name: str, font_url: str, license_url: str) -> Path:
    """Pin named release downloads in a local content lock, never discover system fonts."""
    directory = output / "sources" / name
    directory.mkdir(parents=True, exist_ok=True)
    lock_path = directory / "download-lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8")) if lock_path.exists() else {}
    for filename, url in (("regular.otf", font_url), ("OFL.txt", license_url)):
        path = directory / filename
        if not path.exists():
            data = download(url)
            if filename == "OFL.txt" and b"SIL OPEN FONT LICENSE" not in data.upper():
                raise RuntimeError(f"Not an OFL license: {url}")
            path.write_bytes(data)
        actual = digest(path)
        if filename in lock and lock[filename] != {"url": url, "sha256": actual}:
            raise RuntimeError(f"Control source differs from recorded release: {path}")
        lock[filename] = {"url": url, "sha256": actual}
    save_json(lock_path, lock)
    return directory / "regular.otf"


def prepare_controls(output: Path, corpus: dict, fallback: Path) -> list[dict]:
    import unicodedata
    cjk = control_source(output, "noto-sans-cjk-jp",
        "https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf",
        "https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/LICENSE")
    korean = control_source(output, "pretendard",
        "https://raw.githubusercontent.com/orioncactus/pretendard/v1.3.9/packages/pretendard/dist/public/static/Pretendard-Regular.otf",
        "https://raw.githubusercontent.com/orioncactus/pretendard/v1.3.9/LICENSE")
    local = {}
    for family, expected in (
        ("NotoSansArabic", "252629ca0e87b6233851249b8cbf7b43445211a8caf199f1b306a19202251508"),
        ("NotoSansHebrew", "671951828bd5c95db818e5bb12dcea2d0c0dda00311888522be061ee6835125e")):
        source = FALLBACK.parent.parent / family / f"{family}-Regular.ttf"
        if digest(source) != expected:
            raise RuntimeError(f"Checked-in control font hash mismatch: {source}")
        target = output / "sources" / family / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        shutil.copyfile(source.parent / "OFL.txt", target.parent / "OFL.txt")
        local[family] = target
    controls = []
    for case in corpus["cases"]:
        if case.get("category") != "control":
            continue
        text = case["input"]
        if any(0x600 <= ord(ch) <= 0x6FF for ch in text):
            source, substitute = local["NotoSansArabic"], local["NotoSansHebrew"]
            intervals = "ascii,punctuation,arabic,hebrew"
        elif any(0x590 <= ord(ch) <= 0x5FF for ch in text):
            source, substitute = local["NotoSansHebrew"], fallback
            intervals = "ascii,punctuation,hebrew"
        else:
            source = (korean if any(0xAC00 <= ord(ch) <= 0xD7AF for ch in text) else
                      cjk if any(0x3000 <= ord(ch) <= 0x9FFF for ch in text) else fallback)
            substitute = fallback
            # Exact fixture coverage, including NFC output; not a multi-megabyte full CJK export.
            scalars = sorted({ord(ch) for ch in text + unicodedata.normalize("NFC", text)})
            intervals = "ascii,punctuation," + ",".join(f"(0x{cp:X}-0x{cp:X})" for cp in scalars)
        missing = {ord(ch) for ch in text if not ch.isspace()} - cmap_for(source) - cmap_for(substitute)
        if missing:
            raise RuntimeError(f"Control {case['id']} missing glyphs: {sorted(missing)}")
        for size in SIZES:
            identifier = f"{case['id']}-{size}"
            cpfont = output / "cpfonts" / f"{identifier}.cpfont"
            subprocess.run([sys.executable, str(CONVERTER), "--regular", str(source),
                "--fallback-regular", str(substitute), "--intervals", intervals,
                "--size", str(size), "--output", str(cpfont)], cwd=ROOT, check=True)
            controls.append({"id": identifier, "case_id": case["id"], "point_size": size,
                "cpfont": cpfont.relative_to(output).as_posix(), "cpfont_sha256": digest(cpfont),
                "regular": source.relative_to(output).as_posix(), "regular_sha256": digest(source),
                "fallback": substitute.relative_to(output).as_posix(), "fallback_sha256": digest(substitute),
                "license": (source.parent / "OFL.txt").relative_to(output).as_posix(),
                "license_sha256": digest(source.parent / "OFL.txt"), "intervals": intervals})
    return controls


def sheet_clusters(probe: Path | None) -> dict:
    if probe is None:
        return {}
    with tempfile.TemporaryDirectory(prefix="thai-reference-") as directory:
        source = Path(directory) / "input.txt"
        source.write_text("\n".join(SHEET_TEXT), encoding="utf-8")
        result = subprocess.run([str(probe.resolve()), "--analyze-file", str(source)],
                                capture_output=True, text=True, encoding="utf-8", check=True)
    spans = json.loads(result.stdout)
    result = {}
    start = 0
    for text in SHEET_TEXT:
        result[text] = [{**span, "begin": span["begin"] - start, "end": span["end"] - start}
                        for span in spans if start <= span["begin"] and span["end"] <= start + len(text)]
        start += len(text) + 1
    return result


def prepare(output: Path, thai_shaping: bool = False, probe: Path | None = None) -> None:
    from PIL import Image, ImageDraw
    if thai_shaping and probe is None:
        raise RuntimeError("--thai-shaping reference preparation requires --probe for production cluster boundaries")
    clusters = sheet_clusters(probe)
    versions = dependencies()
    output.mkdir(parents=True, exist_ok=True)
    if digest(FALLBACK) != FALLBACK_SHA256:
        raise RuntimeError(f"Checked-in fallback hash mismatch: {FALLBACK}")
    fallback_dir = output / "sources/noto-sans-fallback"
    fallback_dir.mkdir(parents=True, exist_ok=True)
    fallback = fallback_dir / FALLBACK.name
    shutil.copyfile(FALLBACK, fallback)
    shutil.copyfile(FALLBACK.parent / "OFL.txt", fallback_dir / "OFL.txt")
    corpus_path = ROOT / "test/language/Thai/corpus.json"
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    # Controls need their established script fonts; do not turn absent glyphs into a shaping failure.
    requested = "".join(case.get("input", "") for case in corpus["cases"]
                        if case.get("category") != "control" and not case.get("analyzer_only"))
    required = {ord(ch) for ch in requested + "".join(SHEET_TEXT)
                if 0xE00 <= ord(ch) <= 0xE7F or 0x20 <= ord(ch) <= 0x7E or 0x2000 <= ord(ch) <= 0x206F}
    required -= {0x200B, 0x2028, 0x2029, 0x202F}
    manifest = {"schema_version": 1, "dpi": 150, "sizes": list(SIZES), "intervals": INTERVALS,
                "thai_shaping": thai_shaping,
                "versions": versions, "converter_sha256": digest(CONVERTER),
                "corpus_sha256": digest(corpus_path),
                "fallback": {"path": fallback.relative_to(output).as_posix(),
                             "sha256": FALLBACK_SHA256, "license": "sources/noto-sans-fallback/OFL.txt",
                             "license_sha256": digest(fallback_dir / "OFL.txt")},
                "pack_fallbacks": prepare_pack_fallbacks(output),
                "reference": {"load_flags": "FT_LOAD_RENDER (native hinting)",
                              "quantization": "coverage//64 -> {255,170,85,0}",
                              "positioning": ("HB 26.6 quantized to 12.4; completed Thai outer-cluster advance rounded once"
                                              if probe else "HarfBuzz 26.6; nearest-pixel placement"),
                              "baseline": "ceil(FreeType size ascender/64)",
                              "tracking": ("between production Thai orthographic clusters; HB graphemes elsewhere"
                                           if probe else "between HarfBuzz grapheme clusters"),
                              "cluster_probe_sha256": digest(probe) if probe else None},
                "fonts": []}
    for key, family, filename, expected, blob in FONTS:
        directory = output / "sources" / key
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "original.ttf"
        source_url = fetch_verified(source, family, filename, expected, blob)
        license_url = fetch_license(directory / "OFL.txt", family)
        regular = directory / "regular.ttf"
        axes = freeze(source, regular)
        coverage = cmap_for(regular) | cmap_for(fallback)
        missing = required - coverage
        if missing:
            raise RuntimeError(f"{key} lacks requested coverage (not shaping evidence): " +
                               ", ".join(f"U+{cp:04X}" for cp in sorted(missing)))
        control_coverage = {case["id"]: sorted({f"U+{ord(ch):04X}" for ch in case["input"]
                            if not ch.isspace() and ord(ch) not in coverage})
                            for case in corpus["cases"] if case.get("category") == "control"}
        for size in SIZES:
            identifier = f"{key}-{size}"
            cpfont = output / "cpfonts" / f"{identifier}.cpfont"
            cpfont.parent.mkdir(parents=True, exist_ok=True)
            command = [sys.executable, str(CONVERTER), "--regular", str(regular),
                       "--fallback-regular", str(fallback), "--intervals", INTERVALS,
                       "--size", str(size), "--output", str(cpfont)]
            if thai_shaping:
                command.append("--thai-shaping")
            subprocess.run(command, cwd=ROOT, check=True)
            renderer = ReferenceRenderer(regular, fallback, size)
            sheets = []
            for tracking in (0, 2):
                for compression in (1.0, 0.95):
                    row_height = math.ceil(renderer.line_height * compression) * 2
                    width, height = 1600, row_height * len(SHEET_TEXT) + 32
                    image = Image.new("L", (width, height), 255)
                    rows = [renderer.draw(image, text, 16, 16 + n * row_height, tracking=tracking,
                                          clusters=clusters.get(text)) for n, text in enumerate(SHEET_TEXT)]
                    stem = f"{identifier}-cs{tracking}-lc{compression:g}"
                    destination = output / "reference" / f"{stem}.png"
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    image.save(destination)
                    annotated = image.convert("RGB")
                    draw = ImageDraw.Draw(annotated)
                    for row in rows:
                        draw.line((0, row["baseline"], width, row["baseline"]), fill=(200, 120, 120))
                        if row["ink_bounds"]:
                            draw.rectangle(row["ink_bounds"], outline=(30, 130, 220))
                    annotated.resize((width * 2, height * 2), Image.Resampling.NEAREST).save(
                        destination.with_name(stem + "-annotated-2x.png"))
                    trace_path = destination.with_suffix(".json")
                    save_json(trace_path, {"schema_version": 1, "font_id": identifier,
                                          "character_spacing": tracking, "line_compression": compression,
                                          "width": width, "height": height, "rows": rows})
                    sheets.append(destination.relative_to(output).as_posix())
            manifest["fonts"].append({"id": identifier, "family": key, "point_size": size,
                "cpfont": cpfont.relative_to(output).as_posix(), "cpfont_sha256": digest(cpfont),
                "source": source.relative_to(output).as_posix(), "source_sha256": expected,
                "source_url": source_url, "source_git_blob": blob,
                "regular": regular.relative_to(output).as_posix(), "regular_sha256": digest(regular),
                "frozen_axes": axes, "license": (directory / "OFL.txt").relative_to(output).as_posix(),
                "license_url": license_url, "license_sha256": digest(directory / "OFL.txt"),
                "control_missing_glyphs": control_coverage, "reference_sheets": sheets})
            if thai_shaping:
                companion = cpfont.with_suffix(".cpshape")
                if not companion.is_file():
                    raise RuntimeError(f"Converter did not produce required companion: {companion}")
                manifest["fonts"][-1].update(shape=companion.relative_to(output).as_posix(),
                                             shape_sha256=digest(companion), shape_bytes=companion.stat().st_size)
    manifest["controls"] = prepare_controls(output, corpus, fallback)
    save_json(output / "manifest.json", manifest)
    print(f"Prepared {len(manifest['fonts'])} verified CPFonts: {output / 'manifest.json'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/thai/assets")
    parser.add_argument("--thai-shaping", action="store_true", help="Prepare opt-in paired CPFont/CPSHAPE assets")
    parser.add_argument("--probe", type=Path, help="Production probe for reference tracking cluster boundaries")
    args = parser.parse_args()
    try:
        prepare(args.output.resolve(), args.thai_shaping, args.probe)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError,
            importlib.metadata.PackageNotFoundError) as error:
        parser.exit(1, f"Asset preparation failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
