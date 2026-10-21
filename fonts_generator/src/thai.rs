use crate::{
    native::{Face, ShapedGlyph},
    put16, put32, Raster,
};
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

// CPSHAPE v2 style: 12-byte header, KEY_COUNT 3-byte entries (base ID bits 0-6,
// mark count bits 7-9, first mark record bits 10-23), base records, mark records.
pub const KEY_COUNT: usize = 46 * 82;
pub const STYLE_LIMIT: usize = 96 * 1024;
pub const FAMILY_LIMIT: usize = 384 * 1024;
const STYLE_HEADER: usize = 12;
const ENTRY_BYTES: usize = 3;
const ABSENT_ENTRY: u32 = 0xff_ffff;
const MAX_BASES: usize = 127;
const MAX_MARKS: usize = 5;
const MAX_RECORDS: usize = 16383;
// CPFont v4 reserved header bytes that repeat the companion's metrics CRC.
pub const PAIR_TAG: std::ops::Range<usize> = 16..20;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq, Ord, PartialOrd)]
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
            "mark_records": read16(&self.payload, 2)?, "payload_bytes": self.payload.len(),
            "alternate_glyphs": self.alternates.len()});
        Ok(())
    }

    fn serialize(&self, raster: &Raster) -> Result<Vec<u8>, String> {
        if self.recipes.len() != KEY_COUNT {
            return Err("Incorrect Thai dense key count".into());
        }
        let mut base_ids = BTreeMap::new();
        let mut bases = Vec::new();
        let mut sequences: Vec<&[Record]> = Vec::new();
        let mut seen = HashSet::new();
        let mut entries = Vec::with_capacity(KEY_COUNT);
        for recipe in &self.recipes {
            if !(1..=MAX_MARKS + 1).contains(&recipe.len()) {
                return Err("Invalid Thai recipe glyph count".into());
            }
            let base = recipe[0];
            let marks = &recipe[1..];
            let bid = *base_ids.entry(base).or_insert_with(|| {
                bases.push(base);
                bases.len() - 1
            });
            if !marks.is_empty() && seen.insert(marks) {
                sequences.push(marks);
            }
            entries.push((bid, marks));
        }
        if bases.len() > MAX_BASES {
            return Err(format!(
                "Thai style has {} base records; CPSHAPE allows {MAX_BASES}",
                bases.len()
            ));
        }
        let (records, first) = pack_marks(&sequences);
        if records.len() > MAX_RECORDS {
            return Err(format!(
                "Thai style has {} mark records; CPSHAPE allows {MAX_RECORDS}",
                records.len()
            ));
        }
        let size = STYLE_HEADER + KEY_COUNT * ENTRY_BYTES + (bases.len() + records.len()) * 8;
        if size > STYLE_LIMIT {
            return Err(format!(
                "Thai style companion is {size} bytes; maximum is {STYLE_LIMIT}"
            ));
        }
        let mut output = Vec::with_capacity(size);
        put16(&mut output, bases.len() as u16);
        put16(&mut output, records.len() as u16);
        put16(&mut output, raster.metrics.ascender as i16 as u16);
        put16(&mut output, raster.metrics.descender as i16 as u16);
        put16(&mut output, raster.metrics.line_height as u16);
        put16(&mut output, 0);
        for (bid, marks) in entries {
            let start = if marks.is_empty() { 0 } else { first[marks] };
            let value = bid as u32 | (marks.len() as u32) << 7 | (start as u32) << 10;
            output.extend_from_slice(&value.to_le_bytes()[..ENTRY_BYTES]);
        }
        for record in bases.into_iter().chain(records) {
            record.encode(&mut output);
        }
        Ok(output)
    }

    // This independent reader is part of the conversion, not a best-effort check:
    // publication requires exact original-glyph-ID/position oracle restoration.
    fn roundtrip(&self, raster: &Raster) -> Result<(), String> {
        let p = &self.payload;
        let base = STYLE_HEADER + KEY_COUNT * ENTRY_BYTES;
        if p.len() < base || p.len() > STYLE_LIMIT {
            return Err("Invalid Thai style payload size".into());
        }
        let nb = usize::from(read16(p, 0)?);
        let nr = usize::from(read16(p, 2)?);
        let marks = base + nb * 8;
        if !(1..=MAX_BASES).contains(&nb)
            || nr > MAX_RECORDS
            || p.len() != marks + nr * 8
            || read16(p, 10)? != 0
            || read16(p, 4)? as i16 as i32 != raster.metrics.ascender
            || read16(p, 6)? as i16 as i32 != raster.metrics.descender
            || i32::from(read16(p, 8)?) != raster.metrics.line_height
        {
            return Err("Invalid Thai style section bounds or metrics".into());
        }
        let reverse: BTreeMap<_, _> = self
            .mapping
            .iter()
            .map(|(&gid, &cp)| (cp as u16, gid))
            .collect();
        for key in 0..KEY_COUNT {
            let at = STYLE_HEADER + key * ENTRY_BYTES;
            let value = u32::from(p[at]) | u32::from(p[at + 1]) << 8 | u32::from(p[at + 2]) << 16;
            if value == ABSENT_ENTRY {
                return Err(format!("Thai key {key} has no recipe"));
            }
            let (bid, count, start) = (
                (value & 0x7f) as usize,
                (value >> 7 & 7) as usize,
                (value >> 10) as usize,
            );
            if bid >= nb || count > MAX_MARKS || start + count > nr || (count == 0 && start != 0) {
                return Err("Invalid Thai recipe entry".into());
            }
            let restored = std::iter::once(base + bid * 8)
                .chain((start..start + count).map(|i| marks + i * 8))
                .map(|offset| {
                    let record = decode_record(p, offset)?;
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

/// Shared mark records and each sequence's first record.
type MarkPlacement<'a> = (Vec<Record>, HashMap<&'a [Record], usize>);

/// Place mark sequences longest first; one already contained in the records reuses
/// them. Ties keep first-appearance order, matching the Python baker byte for byte.
fn pack_marks<'a>(sequences: &[&'a [Record]]) -> MarkPlacement<'a> {
    let mut order: Vec<usize> = (0..sequences.len()).collect();
    order.sort_by_key(|&i| (std::cmp::Reverse(sequences[i].len()), i));
    let mut records = Vec::new();
    let mut first = HashMap::new();
    let mut windows: HashMap<Vec<Record>, usize> = HashMap::new();
    for i in order {
        let marks = sequences[i];
        let start = if let Some(&start) = windows.get(marks) {
            start
        } else {
            let start = records.len();
            records.extend_from_slice(marks);
            for end in start..records.len() {
                for begin in end.saturating_sub(MAX_MARKS - 1)..=end {
                    windows
                        .entry(records[begin..=end].to_vec())
                        .or_insert(begin);
                }
            }
            start
        };
        first.insert(marks, start);
    }
    (records, first)
}

fn read16(bytes: &[u8], offset: usize) -> Result<u16, String> {
    let data = bytes
        .get(offset..offset + 2)
        .ok_or("Truncated Thai uint16")?;
    Ok(u16::from_le_bytes([data[0], data[1]]))
}
#[cfg(test)]
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
    // The CRC, with the pair-tag field zeroed, is also the tag written into CPFont.
    let mut unpaired = header_toc.to_vec();
    unpaired
        .get_mut(PAIR_TAG)
        .ok_or("Truncated CPFont header")?
        .fill(0);
    let mut metrics_crc = crc32fast::Hasher::new();
    metrics_crc.update(&unpaired);
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
    put16(&mut output, 2);
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
        assert_eq!(model.payload.len(), 12 + 3772 * 3 + 8 + 8);
        model.payload[12] = 1;
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
        let mut paired = header;
        paired[PAIR_TAG].copy_from_slice(&first[16..20]);
        assert_eq!(companion(&paired, &styles).unwrap(), changed_metrics);
    }

    #[test]
    fn serialization_matches_python_baker_golden() {
        // test/thai_render/test_thai_shape_bake.py pins the same length and CRC.
        let (mut model, mut raster) = model_and_raster();
        model.recipes = (0..KEY_COUNT)
            .map(|key| {
                std::iter::once(Record {
                    cp: 0xe01 + (key / 82) as u16,
                    advance: 64,
                    x: 0,
                    y: 0,
                })
                .chain((0..key % 4).map(|j| Record {
                    cp: 0xe31 + ((key % 82 + j) % 5) as u16,
                    advance: 0,
                    x: -8 * j as i16,
                    y: 24,
                }))
                .collect()
            })
            .collect();
        (
            raster.metrics.ascender,
            raster.metrics.descender,
            raster.metrics.line_height,
        ) = (10, -3, 13);
        let payload = model.serialize(&raster).unwrap();
        assert_eq!(
            (payload.len(), crc32fast::hash(&payload)),
            (11816, 0xbb84_3c1d)
        );
    }

    #[test]
    fn contained_mark_sequences_share_records_longest_first() {
        let record = |cp| Record {
            cp,
            advance: 0,
            x: 0,
            y: 0,
        };
        let (a, b, c) = (record(1), record(2), record(3));
        let (bs, cs, abs, as_) = ([b], [c], [a, b], [a]);
        let sequences: Vec<&[Record]> = vec![&bs, &cs, &abs, &as_];
        let (records, first) = pack_marks(&sequences);
        assert_eq!(records, [a, b, c]);
        assert_eq!(
            sequences.iter().map(|s| first[s]).collect::<Vec<_>>(),
            [1, 2, 0, 0]
        );
    }
}
