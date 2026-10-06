mod native;
mod opentype;
mod thai;

use native::{Bitmap, Face, Metrics};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::{c_char, CStr, CString};
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Request {
    family: String,
    sizes: Vec<u8>,
    intervals: Vec<[u32; 2]>,
    thai: bool,
    autohint: bool,
    styles: Vec<Style>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Style {
    id: u8,
    path: String,
    #[serde(default)]
    fallbacks: Vec<String>,
}

struct Source {
    bytes: Vec<u8>,
    unicode: BTreeSet<u32>,
    sha256: String,
    variable: bool,
}

struct Glyph {
    cp: u32,
    width: u8,
    height: u8,
    advance: u16,
    left: i16,
    top: i16,
    length: u16,
    offset: u32,
}

struct Raster {
    id: u8,
    intervals: Vec<[u32; 2]>,
    glyphs: Vec<Glyph>,
    bitmap: Vec<u8>,
    metrics: Metrics,
    layout: opentype::LayoutTables,
    fallback_glyphs: Vec<usize>,
    missing: usize,
}

pub(crate) fn put16(bytes: &mut Vec<u8>, value: u16) {
    bytes.extend_from_slice(&value.to_le_bytes());
}
pub(crate) fn put32(bytes: &mut Vec<u8>, value: u32) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

fn hash(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

fn merged(mut intervals: Vec<[u32; 2]>) -> Vec<[u32; 2]> {
    intervals.sort_unstable();
    let mut result: Vec<[u32; 2]> = Vec::with_capacity(intervals.len());
    for interval in intervals {
        if let Some(previous) = result.last_mut() {
            if interval[0] <= previous[1] + 1 {
                previous[1] = previous[1].max(interval[1]);
                continue;
            }
        }
        result.push(interval);
    }
    result
}

fn validate(request: &mut Request) -> Result<(), String> {
    if request.family.is_empty()
        || request.family.len() > 120
        || request.family.trim() != request.family
        || request
            .family
            .chars()
            .any(|ch| !ch.is_alphanumeric() && !matches!(ch, ' ' | '_' | '-'))
    {
        return Err("Family must be 1–120 UTF-8 bytes of letters, digits, spaces, hyphens or underscores, without surrounding spaces".into());
    }
    if request.sizes.is_empty() || request.sizes.iter().any(|&size| size == 0) {
        return Err("Choose at least one point size, each in 1..255".into());
    }
    request.sizes.sort_unstable();
    request.sizes.dedup();
    if !(1..=4).contains(&request.styles.len()) {
        return Err("Choose between one and four styles".into());
    }
    let mut ids = BTreeSet::new();
    for style in &request.styles {
        if style.id > 3 || !ids.insert(style.id) {
            return Err(
                "Style IDs must be unique: 0 regular, 1 bold, 2 italic, 3 bold italic".into(),
            );
        }
        for path in std::iter::once(&style.path).chain(&style.fallbacks) {
            if !path.starts_with("/input/")
                || path.contains('\0')
                || path.contains('\\')
                || Path::new(path)
                    .components()
                    .any(|part| matches!(part, Component::ParentDir | Component::CurDir))
            {
                return Err(format!(
                    "Font path must be a file under /input without traversal: {path}"
                ));
            }
        }
    }
    request.styles.sort_by_key(|style| style.id);
    if request.intervals.is_empty() {
        return Err("Choose at least one Unicode interval".into());
    }
    for &[start, end] in &request.intervals {
        if start > end || end > 0x10ffff {
            return Err(format!("Invalid Unicode interval U+{start:X}–U+{end:X}; expected ascending values within U+0000..U+10FFFF"));
        }
    }
    request.intervals.push([0xfffd, 0xfffd]);
    request.intervals = merged(std::mem::take(&mut request.intervals));
    Ok(())
}

fn load_source(path: &str) -> Result<Source, String> {
    let bytes = fs::read(path).map_err(|e| format!("Cannot read font {path}: {e}"))?;
    let face = ttf_parser::Face::parse(&bytes, 0).map_err(|e| {
        format!("Invalid or unsupported font {path}: {e:?}. Use a TTF or OTF source.")
    })?;
    let variable = face.is_variable();
    let mut unicode = BTreeSet::new();
    if let Some(cmap) = face.tables().cmap {
        for subtable in cmap.subtables {
            if subtable.is_unicode() {
                subtable.codepoints(|cp| {
                    unicode.insert(cp);
                });
            }
        }
    }
    if unicode.is_empty() {
        return Err(format!("Font {path} has no supported Unicode cmap"));
    }
    let sha256 = hash(&bytes);
    Ok(Source {
        bytes,
        unicode,
        sha256,
        variable,
    })
}

fn pack_bitmap(
    bitmap: &Bitmap,
    cp: u32,
    offset: usize,
    output: &mut Vec<u8>,
) -> Result<Glyph, String> {
    let invalid = |field: &str| {
        format!("Glyph U+{cp:04X} {field} exceeds CPFont bounds; reduce the point size or exclude this glyph")
    };
    let width = u8::try_from(bitmap.width).map_err(|_| invalid("width (maximum 255)"))?;
    let height = u8::try_from(bitmap.height).map_err(|_| invalid("height (maximum 255)"))?;
    let advance = u16::try_from(bitmap.advance_fp).map_err(|_| invalid("12.4 advance"))?;
    let left = i16::try_from(bitmap.left).map_err(|_| invalid("left bearing"))?;
    let top = i16::try_from(bitmap.top).map_err(|_| invalid("top bearing"))?;
    let pixel_count = usize::from(width) * usize::from(height);
    let length = (pixel_count + 3) / 4;
    let offset = u32::try_from(offset).map_err(|_| "CPFont bitmap exceeds uint32 offset bounds")?;
    if pixel_count != 0 {
        let pitch = bitmap.pitch.unsigned_abs() as usize;
        if bitmap.pixels.is_null() || pitch < usize::from(width) {
            return Err(format!(
                "Glyph U+{cp:04X} has an invalid native bitmap buffer or pitch"
            ));
        }
        let buffer_len = pitch
            .checked_mul(usize::from(height))
            .ok_or("Bitmap stride overflow")?;
        if buffer_len > isize::MAX as usize {
            return Err("Bitmap buffer exceeds addressable bounds".into());
        }
        let pixels = unsafe { std::slice::from_raw_parts(bitmap.pixels, buffer_len) };
        let mut packed = 0u8;
        let mut count = 0usize;
        for y in 0..usize::from(height) {
            let row = if bitmap.pitch >= 0 {
                y
            } else {
                usize::from(height) - 1 - y
            };
            for x in 0..usize::from(width) {
                // Python's 8->4->2 bit thresholds are exactly the high two bits.
                // Packing is continuous across rows, with only the final byte padded.
                packed = (packed << 2) | (pixels[row * pitch + x] >> 6);
                count += 1;
                if count % 4 == 0 {
                    output.push(packed);
                    packed = 0;
                }
            }
        }
        if count % 4 != 0 {
            output.push(packed << ((4 - count % 4) * 2));
        }
    }
    Ok(Glyph {
        cp,
        width,
        height,
        advance,
        left,
        top,
        length: length as u16,
        offset,
    })
}

fn rasterize(
    style: &Style,
    source: &Source,
    size: u8,
    intervals: &[[u32; 2]],
    faces: &mut [Face],
    overrides: &BTreeMap<u32, u16>,
    alternates: &BTreeMap<u32, u32>,
    strict: bool,
    warnings: &mut Vec<String>,
) -> Result<Raster, String> {
    for &cp in alternates.keys() {
        if faces[0].glyph_index(cp) != 0 {
            return Err(format!(
                "Thai alternate would overwrite source cmap U+{cp:04X}"
            ));
        }
    }
    let mut glyphs = Vec::new();
    let mut bitmap = Vec::new();
    let mut present = BTreeSet::new();
    let mut fallback_glyphs = vec![0; faces.len() - 1];
    let mut missing = 0;
    for &[start, end] in intervals {
        for cp in start..=end {
            if (0xd800..=0xdfff).contains(&cp) {
                missing += 1;
                continue;
            }
            let primary_gid = faces[0].glyph_index(cp);
            let gid = if primary_gid != 0 {
                primary_gid
            } else if let Some(&gid) = alternates.get(&cp) {
                gid
            } else {
                u32::from(overrides.get(&cp).copied().unwrap_or(0))
            };
            let selected = if gid != 0 {
                Some((0, gid))
            } else {
                faces.iter().enumerate().skip(1).find_map(|(index, face)| {
                    let gid = face.glyph_index(cp);
                    (gid != 0).then_some((index, gid))
                })
            };
            let Some((face_index, gid)) = selected else {
                missing += 1;
                continue;
            };
            let glyph = faces[face_index]
                .raster(gid, |b| pack_bitmap(b, cp, bitmap.len(), &mut bitmap))
                .map_err(|e| format!("Style {} at {size} pt: {e}", style.id))?;
            if face_index != 0 {
                fallback_glyphs[face_index - 1] += 1;
            }
            present.insert(cp);
            glyphs.push(glyph);
        }
    }
    if glyphs.is_empty() {
        return Err(format!(
            "Style {} at {size} pt has no glyphs in the requested intervals",
            style.id
        ));
    }
    if !present.contains(&0xfffd) {
        warnings.push(format!(
            "Style {} at {size} pt: no source or fallback provides replacement character U+FFFD",
            style.id
        ));
    }
    if missing != 0 {
        warnings.push(format!("Style {} at {size} pt: omitted {missing} unavailable codepoints from the requested intervals", style.id));
    }
    let intervals = merged(present.iter().map(|&cp| [cp, cp]).collect());
    let mut layout = opentype::layout_tables(&source.bytes, &present, size)?;
    for warning in &layout.warnings {
        warnings.push(format!("Style {} at {size} pt: {warning}", style.id));
    }
    if layout.ligatures.len() > 255 {
        if strict {
            return Err(format!(
                "Thai paired CPFont style {} exceeds 255 ligature pairs",
                style.id
            ));
        }
        warnings.push(format!(
            "Style {} at {size} pt: {} ligature pairs exceed 255; keeping the first 255",
            style.id,
            layout.ligatures.len()
        ));
        layout.ligatures.truncate(255);
    }
    let raster = Raster {
        id: style.id,
        intervals,
        glyphs,
        bitmap,
        metrics: faces[0].metrics()?,
        layout,
        fallback_glyphs,
        missing,
    };
    raster.validate_metrics()?;
    Ok(raster)
}

impl Raster {
    fn validate_metrics(&self) -> Result<(), String> {
        u8::try_from(self.metrics.line_height).map_err(|_| {
            format!(
                "Style {} line advance {} exceeds CPFont 0..255; reduce the point size",
                self.id, self.metrics.line_height
            )
        })?;
        i16::try_from(self.metrics.ascender)
            .map_err(|_| "Ascender exceeds signed 16-bit bounds")?;
        i16::try_from(self.metrics.descender)
            .map_err(|_| "Descender exceeds signed 16-bit bounds")?;
        Ok(())
    }

    fn sections(&mut self) -> Result<Vec<Vec<u8>>, String> {
        let mut intervals = Vec::with_capacity(self.intervals.len() * 12);
        let mut offset = 0;
        for &[start, end] in &self.intervals {
            put32(&mut intervals, start);
            put32(&mut intervals, end);
            put32(&mut intervals, offset);
            offset += end - start + 1;
        }
        let mut glyphs = Vec::with_capacity(self.glyphs.len() * 16);
        for glyph in &self.glyphs {
            glyphs.push(glyph.width);
            glyphs.push(glyph.height);
            put16(&mut glyphs, glyph.advance);
            put16(&mut glyphs, glyph.left as u16);
            put16(&mut glyphs, glyph.top as u16);
            put16(&mut glyphs, glyph.length);
            put16(&mut glyphs, 0);
            put32(&mut glyphs, glyph.offset);
        }
        let classes = |entries: &[(u16, u8)], count: u8| -> Result<Vec<u8>, String> {
            u16::try_from(entries.len()).map_err(|_| "Kerning entries exceed uint16 bounds")?;
            let mut output = Vec::with_capacity(entries.len() * 3);
            for &(cp, class) in entries {
                if class == 0 || class > count {
                    return Err("Kerning class exceeds matrix bounds".into());
                }
                put16(&mut output, cp);
                output.push(class);
            }
            Ok(output)
        };
        let left = classes(&self.layout.left, self.layout.left_count)?;
        let right = classes(&self.layout.right, self.layout.right_count)?;
        if self.layout.matrix.len()
            != usize::from(self.layout.left_count) * usize::from(self.layout.right_count)
        {
            return Err("Kerning matrix dimensions do not match class counts".into());
        }
        let matrix = self
            .layout
            .matrix
            .iter()
            .map(|&value| value as u8)
            .collect();
        let mut ligatures = Vec::with_capacity(self.layout.ligatures.len() * 8);
        for &(pair, cp) in &self.layout.ligatures {
            if cp > 0xffff {
                return Err("Ligature output exceeds BMP bounds".into());
            }
            put32(&mut ligatures, pair);
            put32(&mut ligatures, cp);
        }
        Ok(vec![
            intervals,
            glyphs,
            left,
            right,
            matrix,
            ligatures,
            std::mem::take(&mut self.bitmap),
        ])
    }

    fn toc(&self, data_offset: usize, output: &mut Vec<u8>) -> Result<(), String> {
        self.validate_metrics()?;
        output.push(self.id);
        output.extend_from_slice(&[0; 3]);
        put32(
            output,
            u32::try_from(self.intervals.len()).map_err(|_| "Too many intervals")?,
        );
        put32(
            output,
            u32::try_from(self.glyphs.len()).map_err(|_| "Too many glyphs")?,
        );
        output.push(self.metrics.line_height as u8);
        put16(output, self.metrics.ascender as i16 as u16);
        put16(output, self.metrics.descender as i16 as u16);
        put16(
            output,
            u16::try_from(self.layout.left.len()).map_err(|_| "Too many left kerning entries")?,
        );
        put16(
            output,
            u16::try_from(self.layout.right.len()).map_err(|_| "Too many right kerning entries")?,
        );
        output.push(self.layout.left_count);
        output.push(self.layout.right_count);
        output.push(
            u8::try_from(self.layout.ligatures.len()).map_err(|_| "Too many ligature pairs")?,
        );
        put32(
            output,
            u32::try_from(data_offset).map_err(|_| "CPFont exceeds uint32 file offsets")?,
        );
        put32(output, 0);
        Ok(())
    }
}

#[derive(Serialize)]
struct OutputFile {
    path: String,
    kind: &'static str,
    size: usize,
}

struct Staging {
    path: PathBuf,
}
static NEXT_RUN: AtomicU32 = AtomicU32::new(0);

impl Staging {
    fn new() -> Result<Self, String> {
        let id = NEXT_RUN.fetch_add(1, Ordering::Relaxed);
        let path = PathBuf::from(format!("/output-pending-{id}"));
        fs::create_dir(&path)
            .map_err(|e| format!("Cannot create output staging directory: {e}"))?;
        Ok(Self { path })
    }

    fn write(
        &self,
        relative: &str,
        bytes: &[u8],
        kind: &'static str,
    ) -> Result<OutputFile, String> {
        let path = self.path.join(relative);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("Cannot create output folder: {e}"))?;
        }
        fs::write(path, bytes).map_err(|e| format!("Cannot stage {relative}: {e}"))?;
        Ok(OutputFile {
            path: format!("/output/{relative}"),
            kind,
            size: bytes.len(),
        })
    }

    fn publish(&self) -> Result<(), String> {
        let backup = self.path.with_extension("previous");
        let had_previous = Path::new("/output").exists();
        if had_previous {
            fs::rename("/output", &backup)
                .map_err(|e| format!("Cannot preserve previous output: {e}"))?;
        }
        if let Err(error) = fs::rename(&self.path, "/output") {
            if had_previous {
                fs::rename(&backup, "/output").map_err(|restore| format!("Publication failed: {error}; previous output remains at {} because restore failed: {restore}", backup.display()))?;
            }
            return Err(format!("Cannot publish output: {error}"));
        }
        // The generated output is already complete. A backup cleanup failure
        // must not turn successful publication into a reported failed pair.
        if had_previous {
            let _ = fs::remove_dir_all(backup);
        }
        Ok(())
    }
}

impl Drop for Staging {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn run(mut request: Request) -> Result<Value, String> {
    validate(&mut request)?;
    let mut sources = BTreeMap::new();
    let mut overrides = BTreeMap::new();
    for style in &request.styles {
        for path in std::iter::once(&style.path).chain(&style.fallbacks) {
            if !sources.contains_key(path) {
                sources.insert(path.clone(), load_source(path)?);
            }
        }
        let source = &sources[&style.path];
        if request.thai && source.variable {
            return Err(format!("Thai shaping requires a frozen/static primary font, not variable axes: {}. Export the desired instance as a static TTF/OTF first.", style.path));
        }
        overrides.insert(style.id, opentype::ligature_overrides(&source.bytes)?);
    }
    let staging = Staging::new()?;
    let mut files = Vec::new();
    let mut warnings = Vec::new();
    let (freetype, harfbuzz) = native::versions();
    for &size in &request.sizes {
        let warning_start = warnings.len();
        let mut rasters = Vec::new();
        let mut packed = Vec::new();
        let mut style_reports = BTreeMap::new();
        for style in &request.styles {
            let source = &sources[&style.path];
            let mut faces = std::iter::once(&style.path)
                .chain(&style.fallbacks)
                .map(|path| Face::open(path, size, request.autohint))
                .collect::<Result<Vec<_>, _>>()?;
            let mut model = if request.thai {
                let mut occupied = source.unicode.clone();
                for path in &style.fallbacks {
                    occupied.extend(&sources[path].unicode);
                }
                Some(
                    thai::bake(&mut faces[0], &source.unicode, &occupied)
                        .map_err(|e| format!("Style {} at {size} pt: {e}", style.id))?,
                )
            } else {
                None
            };
            let mut intervals = request.intervals.clone();
            if let Some(model) = &model {
                intervals.extend(model.mapping.values().map(|&cp| [cp, cp]));
                intervals = merged(intervals);
            }
            let empty = BTreeMap::new();
            let alternates = model.as_ref().map(|m| &m.alternates).unwrap_or(&empty);
            let mut raster = rasterize(
                style,
                source,
                size,
                &intervals,
                &mut faces,
                &overrides[&style.id],
                alternates,
                request.thai,
                &mut warnings,
            )?;
            if let Some(model) = &mut model {
                model.finish(&mut raster)?;
            }
            let mut report = json!({"source": style.path, "source_sha256": source.sha256,
                "fallbacks": style.fallbacks, "fallback_sha256s": style.fallbacks.iter().map(|p| &sources[p].sha256).collect::<Vec<_>>(),
                "fallback_glyph_counts": raster.fallback_glyphs, "glyph_count": raster.glyphs.len(),
                "interval_count": raster.intervals.len(), "bitmap_bytes": raster.bitmap.len(),
                "omitted_codepoints": raster.missing, "ascender": raster.metrics.ascender,
                "descender": raster.metrics.descender, "line_advance": raster.metrics.line_height,
                "kern_left_entries": raster.layout.left.len(), "kern_right_entries": raster.layout.right.len(),
                "kern_left_classes": raster.layout.left_count, "kern_right_classes": raster.layout.right_count,
                "ligature_pairs": raster.layout.ligatures.len()});
            if let Some(model) = &model {
                report["thai"] = model.counts.clone();
                report["glyph_mapping"] = json!(model.mapping_report());
            }
            style_reports.insert(style.id.to_string(), report);
            packed.push((style.id, raster.sections()?, model));
            rasters.push(raster);
        }
        let mut header_toc = Vec::with_capacity(32 + 32 * request.styles.len());
        header_toc.extend_from_slice(b"CPFONT\0\0");
        put16(&mut header_toc, 4);
        put16(&mut header_toc, 1);
        header_toc.push(request.styles.len() as u8);
        header_toc.extend_from_slice(&[0; 19]);
        let mut offset = 32 + 32 * request.styles.len();
        for (raster, (_, sections, _)) in rasters.iter().zip(&packed) {
            raster.toc(offset, &mut header_toc)?;
            for section in sections {
                offset = offset
                    .checked_add(section.len())
                    .ok_or("CPFont size overflow")?;
            }
        }
        u32::try_from(offset).map_err(|_| "CPFont file exceeds uint32 size bounds")?;
        let companion = if request.thai {
            Some(thai::companion(&header_toc, &packed)?)
        } else {
            None
        };
        let mut cpfont = Vec::with_capacity(offset);
        cpfont.extend_from_slice(&header_toc);
        for (_, sections, _) in &packed {
            for section in sections {
                cpfont.extend_from_slice(section);
            }
        }
        let basename = format!("{}_{}", request.family, size);
        let font_path = format!("fonts/{}/{}.cpfont", request.family, basename);
        let mut report = json!({"generator": "fonts_generator", "generator_version": env!("CARGO_PKG_VERSION"),
            "cpfont_version": 4, "cpshape_version": if request.thai { Some(1) } else { None },
            "dpi": 150, "point_size": size, "force_autohint": request.autohint,
            "settings": request, "versions": {"freetype": freetype, "harfbuzz": harfbuzz,
                "ttf_parser": "0.25.1", "rust": option_env!("FG_RUST_VERSION"),
                "emscripten": option_env!("FG_EMSCRIPTEN_VERSION")},
            "cpfont_bytes": cpfont.len(), "cpfont_sha256": hash(&cpfont),
            "styles": style_reports});
        files.push(staging.write(&font_path, &cpfont, "cpfont")?);
        if let Some(companion) = companion {
            report["companion_bytes"] = json!(companion.len());
            report["companion_sha256"] = json!(hash(&companion));
            report["metrics_crc32"] = json!(u32::from_le_bytes(
                companion[16..20]
                    .try_into()
                    .map_err(|_| "Invalid companion CRC field")?
            ));
            report["payload_crc32"] = json!(u32::from_le_bytes(
                companion[20..24]
                    .try_into()
                    .map_err(|_| "Invalid companion CRC field")?
            ));
            files.push(staging.write(
                &format!("fonts/{}/{}.cpshape", request.family, basename),
                &companion,
                "cpshape",
            )?);
        }
        report["warnings"] = json!(&warnings[warning_start..]);
        let mut report_bytes = serde_json::to_vec_pretty(&report)
            .map_err(|e| format!("Cannot serialize report: {e}"))?;
        report_bytes.push(b'\n');
        files.push(staging.write(&format!("reports/{basename}.json"), &report_bytes, "report")?);
    }
    let result = json!({"files": files, "warnings": warnings});
    staging.publish()?;
    Ok(result)
}

/// Convert a NUL-terminated UTF-8 JSON request. The returned string is owned by
/// this library and must be released exactly once with free_result.
///
/// # Safety
/// A non-null request must point to a readable NUL-terminated C string.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn convert(request: *const c_char) -> *mut c_char {
    let result = if request.is_null() {
        Err("Request pointer is null".into())
    } else {
        CStr::from_ptr(request)
            .to_str()
            .map_err(|e| format!("Request is not UTF-8: {e}"))
            .and_then(|text| {
                serde_json::from_str::<Request>(text)
                    .map_err(|e| format!("Invalid conversion request: {e}"))
            })
            .and_then(run)
    };
    let value = match result {
        Ok(value) => value,
        Err(error) => json!({"error": error}),
    };
    // JSON escapes embedded NUL characters, so this conversion cannot fail.
    match CString::new(value.to_string()) {
        Ok(string) => string.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Release a result produced by convert; a null pointer is permitted.
///
/// # Safety
/// A non-null pointer must be an unreleased return value from convert.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn free_result(result: *mut c_char) {
    if !result.is_null() {
        drop(CString::from_raw(result));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> Request {
        Request {
            family: "Test".into(),
            sizes: vec![16, 12, 16],
            intervals: vec![[32, 64], [65, 126]],
            thai: false,
            autohint: false,
            styles: vec![Style {
                id: 0,
                path: "/input/font.ttf".into(),
                fallbacks: vec![],
            }],
        }
    }

    #[test]
    fn request_merges_intervals_and_adds_replacement() {
        let mut request = request();
        validate(&mut request).unwrap();
        assert_eq!(request.intervals, vec![[32, 126], [0xfffd, 0xfffd]]);
        assert_eq!(request.sizes, vec![12, 16]);
    }

    #[test]
    fn request_rejects_bad_paths_ids_and_ranges() {
        let mut value = request();
        value.styles[0].path = "/input/../output/font.ttf".into();
        assert!(validate(&mut value).is_err());
        let mut value = request();
        value.styles.push(Style {
            id: 0,
            path: "/input/bold.ttf".into(),
            fallbacks: vec![],
        });
        assert!(validate(&mut value).is_err());
        let mut value = request();
        value.intervals = vec![[0, 0x110000]];
        assert!(validate(&mut value).is_err());
    }

    fn bitmap(pixels: &[u8], pitch: i32) -> Bitmap {
        Bitmap {
            width: 3,
            height: 2,
            left: -2,
            top: 5,
            advance_fp: 123,
            pitch,
            pixels: pixels.as_ptr(),
        }
    }

    #[test]
    fn two_bit_bitmap_crosses_rows_and_ignores_stride_padding() {
        let pixels = [0, 63, 64, 255, 127, 128, 255, 0];
        let mut packed = Vec::new();
        let glyph = pack_bitmap(&bitmap(&pixels, 4), 65, 7, &mut packed).unwrap();
        assert_eq!(packed, [0x05, 0xb0]);
        assert_eq!(
            (glyph.width, glyph.height, glyph.length, glyph.offset),
            (3, 2, 2, 7)
        );
        assert_eq!((glyph.advance, glyph.left, glyph.top), (123, -2, 5));
    }

    #[test]
    fn negative_pitch_has_identical_top_down_pixels() {
        let pixels = [127, 128, 255, 0, 0, 63, 64, 255];
        let mut packed = Vec::new();
        pack_bitmap(&bitmap(&pixels, -4), 65, 0, &mut packed).unwrap();
        assert_eq!(packed, [0x05, 0xb0]);
    }

    #[test]
    fn binary_sections_match_cpfont_struct_offsets() {
        let mut raster = Raster {
            id: 2,
            intervals: vec![[65, 65]],
            glyphs: vec![Glyph {
                cp: 65,
                width: 2,
                height: 3,
                advance: 0x1234,
                left: -2,
                top: 4,
                length: 2,
                offset: 0,
            }],
            bitmap: vec![0xaa, 0x50],
            metrics: Metrics {
                ascender: 20,
                descender: -5,
                line_height: 25,
                ..Metrics::default()
            },
            layout: opentype::LayoutTables {
                left: vec![(65, 1)],
                right: vec![(86, 1)],
                matrix: vec![-4],
                left_count: 1,
                right_count: 1,
                ..Default::default()
            },
            fallback_glyphs: vec![],
            missing: 0,
        };
        let sections = raster.sections().unwrap();
        assert_eq!(sections[0], [65, 0, 0, 0, 65, 0, 0, 0, 0, 0, 0, 0]);
        assert_eq!(
            sections[1],
            [2, 3, 0x34, 0x12, 0xfe, 0xff, 4, 0, 2, 0, 0, 0, 0, 0, 0, 0]
        );
        assert_eq!(sections[2], [65, 0, 1]);
        assert_eq!(sections[4], [0xfc]);
        let mut toc = Vec::new();
        raster.toc(64, &mut toc).unwrap();
        assert_eq!(toc.len(), 32);
        assert_eq!(toc[0], 2);
        assert_eq!(toc[12], 25);
        assert_eq!(&toc[24..28], &[64, 0, 0, 0]);
    }
}
