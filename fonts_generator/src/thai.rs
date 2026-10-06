use crate::{
    native::{Face, ShapedGlyph},
    put16, put32, Raster,
};
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};

pub const KEY_COUNT: usize = 46 * 82;
pub const STYLE_LIMIT: usize = 96 * 1024;
pub const FAMILY_LIMIT: usize = 384 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
struct Record {
    cp: u16,
    advance: u16,
    x: i16,
    y: i16,
}

impl Record {
    fn encode(self, output: &mut Vec<u8>) {
        put16(output, self.cp);
        put16(output, self.advance);
        put16(output, self.x as u16);
        put16(output, self.y as u16);
    }
}

pub struct BakedStyle {
    oracle: Vec<Vec<ShapedGlyph>>,
    recipes: Vec<Vec<Record>>,
    pub mapping: BTreeMap<u32, u32>,
    pub alternates: BTreeMap<u32, u32>,
    pub payload: Vec<u8>,
    pub counts: Value,
}

fn contexts() -> Vec<Vec<u32>> {
    let first = [
        0, 0xe31, 0xe34, 0xe35, 0xe36, 0xe37, 0xe38, 0xe39, 0xe3a, 0xe47, 0xe4d,
    ];
    let terminal = [0, 0xe48, 0xe49, 0xe4a, 0xe4b, 0xe4c, 0xe4e];
    let mut result = Vec::with_capacity(KEY_COUNT);
    for base in 0xe01..0xe2f {
        for a in first {
            for b in terminal {
                result.push([base, a, b].into_iter().filter(|&cp| cp != 0).collect());
            }
        }
        for b in [0, 0xe48, 0xe49, 0xe4a, 0xe4b] {
            result.push([base, b, 0xe33].into_iter().filter(|&cp| cp != 0).collect());
        }
    }
    result
}

pub fn bake(
    face: &mut Face,
    cmap_candidates: &BTreeSet<u32>,
    occupied: &BTreeSet<u32>,
) -> Result<BakedStyle, String> {
    let contexts = contexts();
    let required: BTreeSet<_> = contexts.iter().flatten().copied().collect();
    let missing: Vec<_> = required
        .into_iter()
        .filter(|&cp| face.glyph_index(cp) == 0)
        .map(|cp| format!("U+{cp:04X}"))
        .collect();
    if !missing.is_empty() {
        return Err(format!("Thai source face lacks {}. Thai shaping requires all context characters in the primary face, not a fallback.", missing.join(", ")));
    }
    let mut oracle = Vec::with_capacity(KEY_COUNT);
    let mut glyph_ids = BTreeSet::new();
    for (key, context) in contexts.iter().enumerate() {
        let glyphs = face
            .shape(context)
            .map_err(|e| format!("Thai key {key}: {e}"))?;
        for glyph in &glyphs {
            if glyph.glyph_id == 0 {
                return Err(format!("Thai key {key} produced missing glyph ID 0"));
            }
            if glyph.y_advance_fp != 0 {
                return Err(format!("Thai key {key} has nonzero y advance"));
            }
            u16::try_from(glyph.advance_fp).map_err(|_| {
                format!(
                    "Thai key {key} advance {} is outside 0..65535",
                    glyph.advance_fp
                )
            })?;
            i16::try_from(glyph.x_fp)
                .map_err(|_| format!("Thai key {key} x offset exceeds signed 16-bit bounds"))?;
            i16::try_from(glyph.y_fp)
                .map_err(|_| format!("Thai key {key} y offset exceeds signed 16-bit bounds"))?;
            glyph_ids.insert(glyph.glyph_id);
        }
        oracle.push(glyphs);
    }
    // All Unicode subtable keys are reserved, even aliases and fallback-only PUA.
    // Only the actual FreeType-selected cmap determines reusable glyph mappings.
    let mut by_glyph = BTreeMap::new();
    for &cp in cmap_candidates.range(..=0xffff) {
        if (0xd800..=0xdfff).contains(&cp) {
            continue;
        }
        let gid = face.glyph_index(cp);
        if gid != 0 {
            by_glyph.entry(gid).or_insert(cp);
        }
    }
    let (mapping, alternates) = allocate_codepoints(&glyph_ids, &by_glyph, occupied)?;
    let recipes = oracle
        .iter()
        .map(|glyphs| {
            glyphs
                .iter()
                .map(|g| Record {
                    cp: mapping[&g.glyph_id] as u16,
                    advance: g.advance_fp as u16,
                    x: g.x_fp as i16,
                    y: g.y_fp as i16,
                })
                .collect()
        })
        .collect();
    Ok(BakedStyle {
        oracle,
        recipes,
        mapping,
        alternates,
        payload: Vec::new(),
        counts: Value::Null,
    })
}

fn allocate_codepoints(
    glyph_ids: &BTreeSet<u32>,
    by_glyph: &BTreeMap<u32, u32>,
    occupied: &BTreeSet<u32>,
) -> Result<(BTreeMap<u32, u32>, BTreeMap<u32, u32>), String> {
    let mut available = (0xe000..0xf900).filter(|cp| !occupied.contains(cp));
    let mut mapping = BTreeMap::new();
    let mut alternates = BTreeMap::new();
    for &gid in glyph_ids {
        if gid == 0 {
            return Err("Thai shaping produced missing glyph ID 0".into());
        }
        let cp = if let Some(&cp) = by_glyph.get(&gid) {
            cp
        } else {
            let cp = available
                .next()
                .ok_or("Thai alternate glyphs exhaust unused BMP PUA")?;
            alternates.insert(cp, gid);
            cp
        };
        mapping.insert(gid, cp);
    }
    Ok((mapping, alternates))
}

impl BakedStyle {
    pub fn finish(&mut self, raster: &mut Raster) -> Result<(), String> {
        let glyphs: BTreeMap<_, _> = raster.glyphs.iter().map(|g| (g.cp, g)).collect();
        let (mut ink_top, mut ink_bottom) = (0, 0);
        for recipe in &self.recipes {
            for record in recipe {
                let glyph = glyphs.get(&u32::from(record.cp)).ok_or_else(|| {
                    format!(
                        "Thai recipe references absent CPFont glyph U+{:04X}",
                        record.cp
                    )
                })?;
                if glyph.width != 0 && glyph.height != 0 {
                    let top = i32::from(glyph.top) + (i32::from(record.y) + 8).div_euclid(16);
                    let bottom = top - i32::from(glyph.height);
                    raster.metrics.ascender = raster.metrics.ascender.max(top);
                    raster.metrics.descender = raster.metrics.descender.min(bottom);
                    ink_top = ink_top.max(top);
                    ink_bottom = ink_bottom.min(bottom);
                }
            }
        }
        raster.metrics.line_height = raster.metrics.line_height.max(ink_top - ink_bottom);
        raster.validate_metrics()?;
        self.payload = self.serialize(raster)?;
        self.roundtrip(raster)?;
        self.counts = json!({"dense_keys": KEY_COUNT, "base_records": read16(&self.payload, 0)?,
            "suffix_sequences": read16(&self.payload, 2)?, "payload_bytes": self.payload.len(),
            "alternate_glyphs": self.alternates.len()});
        Ok(())
    }

    fn serialize(&self, raster: &Raster) -> Result<Vec<u8>, String> {
        if self.recipes.len() != KEY_COUNT {
            return Err("Incorrect Thai dense key count".into());
        }
        let mut base_ids = BTreeMap::new();
        let mut suffix_ids = BTreeMap::new();
        let mut bases = Vec::new();
        let mut suffixes = Vec::new();
        let mut dense = Vec::with_capacity(KEY_COUNT * 4);
        for recipe in &self.recipes {
            if !(1..=6).contains(&recipe.len()) {
                return Err("Invalid Thai recipe glyph count".into());
            }
            let base = recipe[0];
            let suffix = &recipe[1..];
            let bid = if let Some(&id) = base_ids.get(&base) {
                id
            } else {
                let id = u16::try_from(bases.len()).map_err(|_| "Too many Thai base records")?;
                bases.push(base);
                base_ids.insert(base, id);
                id
            };
            let sid = if let Some(&id) = suffix_ids.get(suffix) {
                id
            } else {
                let id = u16::try_from(suffixes.len()).map_err(|_| "Too many Thai suffixes")?;
                suffixes.push(suffix);
                suffix_ids.insert(suffix, id);
                id
            };
            if bid == 0xffff || sid == 0xffff {
                return Err("Thai record IDs exceed 65534".into());
            }
            put16(&mut dense, bid);
            put16(&mut dense, sid);
        }
        let base_offset = 28 + dense.len();
        let table_offset = base_offset + bases.len() * 8;
        let data_offset = table_offset + (suffixes.len() + 1) * 4;
        let size = data_offset + suffixes.iter().map(|s| 1 + s.len() * 8).sum::<usize>();
        if size > STYLE_LIMIT {
            return Err(format!(
                "Thai style companion is {size} bytes; maximum is {STYLE_LIMIT}"
            ));
        }
        let mut output = Vec::with_capacity(size);
        put16(&mut output, bases.len() as u16);
        put16(&mut output, suffixes.len() as u16);
        for offset in [28, base_offset, table_offset, data_offset] {
            put32(&mut output, offset as u32);
        }
        put16(&mut output, raster.metrics.ascender as i16 as u16);
        put16(&mut output, raster.metrics.descender as i16 as u16);
        put16(&mut output, raster.metrics.line_height as u16);
        put16(&mut output, 0);
        output.extend_from_slice(&dense);
        for base in bases {
            base.encode(&mut output);
        }
        let mut offset = data_offset;
        for suffix in &suffixes {
            put32(&mut output, offset as u32);
            offset += 1 + suffix.len() * 8;
        }
        put32(&mut output, offset as u32);
        for suffix in suffixes {
            output.push(suffix.len() as u8);
            for &record in suffix {
                record.encode(&mut output);
            }
        }
        Ok(output)
    }

    // This independent reader is part of the conversion, not a best-effort check:
    // publication requires exact original-glyph-ID/position oracle restoration.
    fn roundtrip(&self, raster: &Raster) -> Result<(), String> {
        let p = &self.payload;
        if p.len() < 28 || p.len() > STYLE_LIMIT {
            return Err("Invalid Thai style payload size".into());
        }
        let nb = usize::from(read16(p, 0)?);
        let ns = usize::from(read16(p, 2)?);
        let dense = read32(p, 4)? as usize;
        let base = read32(p, 8)? as usize;
        let table = read32(p, 12)? as usize;
        let data = read32(p, 16)? as usize;
        if dense != 28
            || base != dense + KEY_COUNT * 4
            || table != base + nb * 8
            || data != table + (ns + 1) * 4
            || data > p.len()
            || read16(p, 26)? != 0
            || read16(p, 20)? as i16 as i32 != raster.metrics.ascender
            || read16(p, 22)? as i16 as i32 != raster.metrics.descender
            || i32::from(read16(p, 24)?) != raster.metrics.line_height
        {
            return Err("Invalid Thai style section bounds or metrics".into());
        }
        let offsets: Vec<_> = (0..=ns)
            .map(|i| read32(p, table + i * 4).map(|n| n as usize))
            .collect::<Result<_, _>>()?;
        if offsets.first() != Some(&data) || offsets.last() != Some(&p.len()) {
            return Err("Invalid Thai suffix endpoints".into());
        }
        let mut suffixes = Vec::with_capacity(ns);
        for pair in offsets.windows(2) {
            let (start, end) = (pair[0], pair[1]);
            if start < data || start >= end || end > p.len() {
                return Err("Invalid Thai suffix bounds".into());
            }
            let count = usize::from(p[start]);
            if count > 5 || end - start != 1 + count * 8 {
                return Err("Invalid Thai suffix count".into());
            }
            suffixes.push(
                (0..count)
                    .map(|i| decode_record(p, start + 1 + i * 8))
                    .collect::<Result<Vec<_>, _>>()?,
            );
        }
        let reverse: BTreeMap<_, _> = self
            .mapping
            .iter()
            .map(|(&gid, &cp)| (cp as u16, gid))
            .collect();
        for key in 0..KEY_COUNT {
            let bid = usize::from(read16(p, dense + key * 4)?);
            let sid = usize::from(read16(p, dense + key * 4 + 2)?);
            if bid >= nb || sid >= ns {
                return Err("Invalid Thai recipe index".into());
            }
            let first = decode_record(p, base + bid * 8)?;
            let restored = std::iter::once(&first)
                .chain(suffixes[sid].iter())
                .map(|record| {
                    let &gid = reverse
                        .get(&record.cp)
                        .ok_or("Unknown glyph in Thai roundtrip")?;
                    Ok(ShapedGlyph {
                        glyph_id: gid,
                        advance_fp: i32::from(record.advance),
                        x_fp: i32::from(record.x),
                        y_fp: i32::from(record.y),
                        y_advance_fp: 0,
                    })
                })
                .collect::<Result<Vec<_>, String>>()?;
            if restored != self.oracle[key] {
                return Err(format!(
                    "Thai key {key} serialization differs from HarfBuzz oracle"
                ));
            }
        }
        Ok(())
    }

    pub fn mapping_report(&self) -> Vec<Value> {
        self.mapping
            .iter()
            .map(|(&gid, &cp)| {
                json!({"glyph_id": gid, "codepoint": cp,
            "alternate": self.alternates.contains_key(&cp)})
            })
            .collect()
    }
}

fn read16(bytes: &[u8], offset: usize) -> Result<u16, String> {
    let data = bytes
        .get(offset..offset + 2)
        .ok_or("Truncated Thai uint16")?;
    Ok(u16::from_le_bytes([data[0], data[1]]))
}
fn read32(bytes: &[u8], offset: usize) -> Result<u32, String> {
    let data = bytes
        .get(offset..offset + 4)
        .ok_or("Truncated Thai uint32")?;
    Ok(u32::from_le_bytes([data[0], data[1], data[2], data[3]]))
}
fn decode_record(bytes: &[u8], offset: usize) -> Result<Record, String> {
    Ok(Record {
        cp: read16(bytes, offset)?,
        advance: read16(bytes, offset + 2)?,
        x: read16(bytes, offset + 4)? as i16,
        y: read16(bytes, offset + 6)? as i16,
    })
}

pub fn companion(
    header_toc: &[u8],
    styles: &[(u8, Vec<Vec<u8>>, Option<BakedStyle>)],
) -> Result<Vec<u8>, String> {
    let mut metrics_crc = crc32fast::Hasher::new();
    metrics_crc.update(header_toc);
    let mut cpfont_bytes = header_toc.len();
    let mut offset = 32 + 12 * styles.len();
    let mut payload = Vec::new();
    for (id, sections, model) in styles {
        let model = model.as_ref().ok_or("Missing Thai style model")?;
        for section in &sections[..sections.len() - 1] {
            metrics_crc.update(section);
        }
        cpfont_bytes = cpfont_bytes
            .checked_add(sections.iter().map(Vec::len).sum::<usize>())
            .ok_or("CPFont size overflow")?;
        payload.push(*id);
        payload.extend_from_slice(&[0; 3]);
        put32(&mut payload, offset as u32);
        put32(&mut payload, model.payload.len() as u32);
        offset += model.payload.len();
    }
    if offset > FAMILY_LIMIT {
        return Err(format!(
            "Thai family companion is {offset} bytes; maximum is {FAMILY_LIMIT}"
        ));
    }
    for (_, _, model) in styles {
        payload.extend_from_slice(&model.as_ref().ok_or("Missing Thai style model")?.payload);
    }
    let mut output = Vec::with_capacity(offset);
    output.extend_from_slice(b"CPSHAPE\0");
    put16(&mut output, 1);
    put16(&mut output, 32);
    put32(
        &mut output,
        u32::try_from(cpfont_bytes).map_err(|_| "CPFont exceeds uint32 file-size bounds")?,
    );
    put32(&mut output, metrics_crc.finalize());
    put32(&mut output, crc32fast::hash(&payload));
    put32(&mut output, offset as u32);
    put32(&mut output, styles.len() as u32);
    output.extend_from_slice(&payload);
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{native::Metrics, opentype::LayoutTables, Glyph};

    #[test]
    fn context_order_matches_runtime_dense_keys() {
        let contexts = contexts();
        assert_eq!(contexts.len(), 3772);
        assert_eq!(contexts[0], [0xe01]);
        assert_eq!(contexts[1], [0xe01, 0xe48]);
        assert_eq!(contexts[7], [0xe01, 0xe31]);
        assert_eq!(contexts[76], [0xe01, 0xe4d, 0xe4e]);
        assert_eq!(contexts[77], [0xe01, 0xe33]);
        assert_eq!(contexts[81], [0xe01, 0xe4b, 0xe33]);
        assert_eq!(contexts[82], [0xe02]);
        assert_eq!(contexts[3771], [0xe2e, 0xe4b, 0xe33]);
    }

    #[test]
    fn pua_allocation_is_sorted_and_excludes_fallback_cmaps() {
        let gids = BTreeSet::from([30, 5, 20]);
        let known = BTreeMap::from([(20, 0xe01)]);
        let occupied = BTreeSet::from([0xe000, 0xe002, 0xe01]);
        let (mapping, alternates) = allocate_codepoints(&gids, &known, &occupied).unwrap();
        assert_eq!(
            mapping,
            BTreeMap::from([(5, 0xe001), (20, 0xe01), (30, 0xe003)])
        );
        assert_eq!(alternates, BTreeMap::from([(0xe001, 5), (0xe003, 30)]));
        assert!(allocate_codepoints(&BTreeSet::from([0]), &known, &occupied).is_err());
        let full = (0xe000..0xf900).collect();
        assert!(allocate_codepoints(&gids, &known, &full).is_err());
    }

    fn model_and_raster() -> (BakedStyle, Raster) {
        let records = vec![
            Record {
                cp: 0xe01,
                advance: 64,
                x: 0,
                y: 0,
            },
            Record {
                cp: 0xe31,
                advance: 0,
                x: -8,
                y: 24,
            },
        ];
        let oracle = vec![
            ShapedGlyph {
                glyph_id: 10,
                advance_fp: 64,
                x_fp: 0,
                y_fp: 0,
                y_advance_fp: 0,
            },
            ShapedGlyph {
                glyph_id: 11,
                advance_fp: 0,
                x_fp: -8,
                y_fp: 24,
                y_advance_fp: 0,
            },
        ];
        let model = BakedStyle {
            oracle: vec![oracle; KEY_COUNT],
            recipes: vec![records; KEY_COUNT],
            mapping: BTreeMap::from([(10, 0xe01), (11, 0xe31)]),
            alternates: BTreeMap::new(),
            payload: vec![],
            counts: Value::Null,
        };
        let raster = Raster {
            id: 0,
            intervals: vec![[0xe01, 0xe01], [0xe31, 0xe31]],
            glyphs: vec![
                Glyph {
                    cp: 0xe01,
                    width: 2,
                    height: 8,
                    advance: 64,
                    left: 0,
                    top: 5,
                    length: 4,
                    offset: 0,
                },
                Glyph {
                    cp: 0xe31,
                    width: 2,
                    height: 3,
                    advance: 0,
                    left: 0,
                    top: 8,
                    length: 2,
                    offset: 4,
                },
            ],
            bitmap: vec![0; 6],
            metrics: Metrics {
                ascender: 5,
                descender: -2,
                line_height: 10,
                ..Metrics::default()
            },
            layout: LayoutTables::default(),
            fallback_glyphs: vec![],
            missing: 0,
        };
        (model, raster)
    }

    #[test]
    fn thai_serialization_expands_metrics_and_restores_original_glyph_ids() {
        let (mut model, mut raster) = model_and_raster();
        model.finish(&mut raster).unwrap();
        assert_eq!(
            (
                raster.metrics.ascender,
                raster.metrics.descender,
                raster.metrics.line_height
            ),
            (10, -3, 13)
        );
        assert_eq!(
            (
                read16(&model.payload, 0).unwrap(),
                read16(&model.payload, 2).unwrap()
            ),
            (1, 1)
        );
        assert_eq!(model.payload.len(), 28 + 3772 * 4 + 8 + 8 + 9);
        model.payload[28] = 1;
        assert!(model.roundtrip(&raster).is_err());
    }

    #[test]
    fn companion_crc_binds_metadata_but_excludes_bitmap_bytes() {
        let (mut model, mut raster) = model_and_raster();
        model.finish(&mut raster).unwrap();
        let header = [0u8; 64];
        let sections = raster.sections().unwrap();
        let mut styles = vec![(0, sections, Some(model))];
        let first = companion(&header, &styles).unwrap();
        styles[0].1[6][0] = 255;
        let changed_bitmap = companion(&header, &styles).unwrap();
        assert_eq!(first, changed_bitmap);
        styles[0].1[1][0] ^= 1;
        let changed_metrics = companion(&header, &styles).unwrap();
        assert_ne!(&first[16..20], &changed_metrics[16..20]);
        assert_eq!(read32(&first, 20).unwrap(), crc32fast::hash(&first[32..]));
        assert_eq!(read32(&first, 24).unwrap() as usize, first.len());
    }
}
