//! CPFont's deliberately restricted OpenType projection. Keep the same additive
//! kerning and liga/rlig policy as fontconvert_sdcard.py, not shaping-engine rules.
use std::collections::{BTreeMap, BTreeSet};
use ttf_parser::gpos::{PairAdjustment, PositioningSubtable, ValueRecord};
use ttf_parser::gsub::SubstitutionSubtable;
use ttf_parser::opentype_layout::{LayoutTable, LookupSubtable};
use ttf_parser::{Face, GlyphId, Tag};

#[derive(Debug, Default)]
pub struct LayoutTables {
    pub left: Vec<(u16, u8)>,
    pub right: Vec<(u16, u8)>,
    pub matrix: Vec<i8>,
    pub left_count: u8,
    pub right_count: u8,
    pub ligatures: Vec<(u32, u32)>,
    pub warnings: Vec<String>,
}

type Cmap = BTreeMap<u32, u16>;
type Aliases = BTreeMap<u16, Vec<u16>>;
type RawKern = BTreeMap<(u16, u16), i64>;

fn warn(warnings: &mut Vec<String>, message: impl Into<String>) {
    let message = message.into();
    if !warnings.contains(&message) {
        warnings.push(message);
    }
}

fn parse_face(bytes: &[u8]) -> Result<Face<'_>, String> {
    Face::parse(bytes, 0).map_err(|e| format!("Cannot parse OpenType font: {e:?}"))
}

// fontTools getBestCmap preference, rather than merging incompatible subtables.
// Iterate the entire chosen cmap: a glyph may have several Unicode aliases.
fn best_cmap(face: &Face<'_>) -> Cmap {
    let mut result = BTreeMap::new();
    let Some(cmap) = face.tables().cmap else {
        return result;
    };
    for (platform, encoding) in [
        (3, 10),
        (0, 6),
        (0, 4),
        (3, 1),
        (0, 3),
        (0, 2),
        (0, 1),
        (0, 0),
    ] {
        let selected = cmap.subtables.into_iter().find(|s| {
            s.platform_id as u16 == platform
                && s.encoding_id == encoding
                && !matches!(
                    s.format,
                    ttf_parser::cmap::Format::UnicodeVariationSequences(_)
                        | ttf_parser::cmap::Format::MixedCoverage
                )
        });
        if let Some(subtable) = selected {
            subtable.codepoints(|cp| {
                if char::from_u32(cp).is_some() {
                    if let Some(gid) = subtable.glyph_index(cp) {
                        if gid.0 < face.number_of_glyphs() {
                            result.insert(cp, gid.0);
                        }
                    }
                }
            });
            break;
        }
    }
    result
}

fn canonical_codepoints(cmap: &Cmap) -> BTreeMap<u16, u32> {
    // Python's reversed cmap dict keeps the last (largest) codepoint per glyph.
    cmap.iter().map(|(&cp, &gid)| (gid, cp)).collect()
}

fn standard_ligature(sequence: &[u32]) -> Option<u32> {
    match sequence {
        [0x66, 0x66] => Some(0xfb00),
        [0x66, 0x69] => Some(0xfb01),
        [0x66, 0x6c] => Some(0xfb02),
        [0x66, 0x66, 0x69] => Some(0xfb03),
        [0x66, 0x66, 0x6c] => Some(0xfb04),
        [0x17f, 0x74] => Some(0xfb05),
        [0x73, 0x74] => Some(0xfb06),
        _ => None,
    }
}

fn lookup_indices(
    table: LayoutTable<'_>,
    tags: &[Tag],
    warnings: &mut Vec<String>,
) -> BTreeSet<u16> {
    let mut indices = BTreeSet::new();
    for index in 0..table.features.len() {
        let Some(feature) = table.features.get(index) else {
            warn(warnings, "Malformed OpenType feature record was skipped.");
            continue;
        };
        if tags.contains(&feature.tag) {
            indices.extend(feature.lookup_indices);
        }
    }
    indices
}

// ttf-parser's PairSet exposes lookup but not iteration. Retain the raw bytes
// through its public lookup visitor so Format 1 visits encoded records instead
// of probing every possible glyph pair. Bounds checks also cover extensions.
struct RawSubtable<'a> {
    kind: u16,
    data: &'a [u8],
}
impl<'a> LookupSubtable<'a> for RawSubtable<'a> {
    fn parse(data: &'a [u8], kind: u16) -> Option<Self> {
        Some(Self { kind, data })
    }
}
impl<'a> RawSubtable<'a> {
    fn unwrap(self, extension_kind: u16) -> Option<Self> {
        if self.kind != extension_kind {
            return Some(self);
        }
        if be16(self.data, 0)? != 1 {
            return None;
        }
        let kind = be16(self.data, 2)?;
        if kind == extension_kind {
            return None; // Nested extensions are forbidden by OpenType.
        }
        let offset = usize::try_from(be32(self.data, 4)?).ok()?;
        if offset < 8 {
            return None;
        }
        Some(Self {
            kind,
            data: self.data.get(offset..)?,
        })
    }
}
fn be16(data: &[u8], offset: usize) -> Option<u16> {
    let bytes = data.get(offset..offset.checked_add(2)?)?;
    Some(u16::from_be_bytes([bytes[0], bytes[1]]))
}
fn be32(data: &[u8], offset: usize) -> Option<u32> {
    let bytes = data.get(offset..offset.checked_add(4)?)?;
    Some(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

fn visit_ligatures(
    face: &Face<'_>,
    cmap: &Cmap,
    warnings: &mut Vec<String>,
    mut visit: impl FnMut(Vec<u32>, u16),
) {
    let Some(table) = face.tables().gsub else {
        if face.raw_face().table(Tag::from_bytes(b"GSUB")).is_some() {
            warn(
                warnings,
                "GSUB could not be parsed; ligatures were dropped.",
            );
        }
        return;
    };
    let canonical = canonical_codepoints(cmap);
    for index in lookup_indices(
        table,
        &[Tag::from_bytes(b"liga"), Tag::from_bytes(b"rlig")],
        warnings,
    ) {
        let Some(lookup) = table.lookups.get(index) else {
            warn(
                warnings,
                format!("GSUB liga/rlig lookup {index} is malformed and was skipped."),
            );
            continue;
        };
        if lookup.flags.0 != 0 {
            warn(warnings, "GSUB lookup filtering flags cannot be stored in CPFont; ligatures use the reference converter's unfiltered pair semantics.");
        }
        for sub_index in 0..lookup.subtables.len() {
            let Some(raw) = lookup
                .subtables
                .get::<RawSubtable<'_>>(sub_index)
                .and_then(|s| s.unwrap(7))
            else {
                warn(
                    warnings,
                    format!("Malformed GSUB subtable in lookup {index} was skipped."),
                );
                continue;
            };
            if raw.kind != 4 {
                warn(warnings, format!("Unsupported GSUB liga/rlig lookup type {} (lookup {index}); only ligature substitutions fit CPFont pair rules.", raw.kind));
                continue;
            }
            let Some(SubstitutionSubtable::Ligature(subtable)) =
                SubstitutionSubtable::parse(raw.data, raw.kind)
            else {
                warn(
                    warnings,
                    format!("Malformed GSUB ligature subtable in lookup {index} was skipped."),
                );
                continue;
            };
            for (&first_gid, &first_cp) in &canonical {
                let Some(coverage_index) = subtable.coverage.get(GlyphId(first_gid)) else {
                    continue;
                };
                let Some(set) = subtable.ligature_sets.get(coverage_index) else {
                    warn(warnings, "Malformed GSUB ligature set was skipped.");
                    continue;
                };
                for lig_index in 0..set.len() {
                    let Some(ligature) = set.get(lig_index) else {
                        warn(warnings, "Malformed GSUB ligature record was skipped.");
                        continue;
                    };
                    if ligature.glyph.0 >= face.number_of_glyphs() {
                        warn(
                            warnings,
                            "GSUB ligature with an out-of-range target glyph was skipped.",
                        );
                        continue;
                    }
                    let mut sequence =
                        Vec::with_capacity(usize::from(ligature.components.len()) + 1);
                    sequence.push(first_cp);
                    let mut valid = true;
                    for component in ligature.components {
                        if let Some(&cp) = canonical.get(&component.0) {
                            sequence.push(cp);
                        } else {
                            valid = false;
                            break;
                        }
                    }
                    if valid && sequence.len() >= 2 {
                        visit(sequence, ligature.glyph.0);
                    } else {
                        warn(warnings, "GSUB ligature with unencoded components or fewer than two inputs cannot be represented and was skipped.");
                    }
                }
            }
        }
    }
}

pub fn ligature_overrides(bytes: &[u8]) -> Result<BTreeMap<u32, u16>, String> {
    let face = parse_face(bytes)?;
    let cmap = best_cmap(&face);
    let mut overrides = BTreeMap::new();
    visit_ligatures(&face, &cmap, &mut Vec::new(), |sequence, gid| {
        if let Some(cp) = standard_ligature(&sequence) {
            if !cmap.contains_key(&cp) {
                overrides.insert(cp, gid);
            }
        }
    });
    Ok(overrides)
}

fn extract_ligatures(
    face: &Face<'_>,
    cmap: &Cmap,
    codepoints: &BTreeSet<u32>,
    warnings: &mut Vec<String>,
) -> Vec<(u32, u32)> {
    let canonical = canonical_codepoints(cmap);
    let mut rules = BTreeMap::new();
    let mut unencoded = false;
    visit_ligatures(face, cmap, warnings, |sequence, gid| {
        if let Some(cp) = canonical
            .get(&gid)
            .copied()
            .or_else(|| standard_ligature(&sequence))
        {
            rules.insert(sequence, cp);
        } else {
            unencoded = true;
        }
    });
    if unencoded {
        warn(warnings, "Dropped GSUB ligatures whose output has no cmap entry and no standard Unicode ligature codepoint.");
    }
    let mut supplementary = false;
    rules.retain(|sequence, cp| {
        if *cp > 0xffff || sequence.iter().any(|c| *c > 0xffff) {
            supplementary = true;
            return false;
        }
        codepoints.contains(cp) && sequence.iter().all(|c| codepoints.contains(c))
    });
    if supplementary {
        warn(
            warnings,
            "Dropped supplementary-plane ligature rules: CPFont pair components are uint16.",
        );
    }
    let mut pairs = BTreeMap::new();
    // As in the reference, emit two-character rules before longer chains.
    let mut ordered: Vec<_> = rules.iter().collect();
    ordered.sort_by(|(a, _), (b, _)| a.len().cmp(&b.len()).then_with(|| a.cmp(b)));
    for (sequence, &target) in ordered {
        let left = if sequence.len() == 2 {
            Some(sequence[0])
        } else {
            let prefix = &sequence[..sequence.len() - 1];
            rules.get(prefix).copied()
        };
        let Some(left) = left else {
            warn(warnings, "Dropped multi-character ligatures without a generated prefix ligature for chaining.");
            continue;
        };
        let key = (left << 16) | sequence[sequence.len() - 1];
        if let Some(&previous) = pairs.get(&key) {
            if previous != target {
                warn(warnings, format!("Conflicting ligature pair U+{left:04X} + U+{:04X}: kept U+{previous:04X}, dropped U+{target:04X}.", sequence[sequence.len() - 1]));
                continue;
            }
        }
        pairs.insert(key, target);
    }
    pairs.into_iter().collect()
}

fn add_kern(raw: &mut RawKern, left: u16, right: u16, value: i16) {
    if value != 0 {
        *raw.entry((left, right)).or_default() += i64::from(value);
    }
}

fn extract_legacy_kern(
    face: &Face<'_>,
    aliases: &Aliases,
    raw: &mut RawKern,
    warnings: &mut Vec<String>,
) {
    let Some(table) = face.tables().kern else {
        if face.raw_face().table(Tag::from_bytes(b"kern")).is_some() {
            warn(
                warnings,
                "Legacy kern table could not be parsed and was dropped.",
            );
        }
        return;
    };
    let mut visited = 0;
    for subtable in table.subtables {
        visited += 1;
        if let ttf_parser::kern::Format::Format0(pairs) = subtable.format {
            // fontTools sums all format-0 subtables, including coverage flags.
            if !subtable.horizontal || subtable.has_cross_stream || subtable.variable {
                warn(warnings, "Legacy kern contains vertical/cross-stream/variable flags; applying raw values additively as the reference converter does.");
            }
            for pair in pairs.pairs {
                let (left, right) = (pair.left().0, pair.right().0);
                if aliases.contains_key(&left) && aliases.contains_key(&right) {
                    add_kern(raw, left, right, pair.value);
                }
            }
        } else {
            warn(warnings, "Unsupported non-format-0 legacy kern subtable was skipped, matching fontTools kernTable extraction.");
        }
    }
    if visited != table.subtables.len() {
        warn(warnings, "Malformed or unsupported legacy kern subtables prevented reading the remainder of that table.");
    }
}

fn value_has_extra(value: ValueRecord<'_>, allow_x_advance: bool) -> bool {
    value.x_placement != 0
        || value.y_placement != 0
        || value.y_advance != 0
        || (!allow_x_advance && value.x_advance != 0)
        || value.x_placement_device.is_some()
        || value.y_placement_device.is_some()
        || value.x_advance_device.is_some()
        || value.y_advance_device.is_some()
}

fn pair_format1(
    data: &[u8],
    coverage: ttf_parser::opentype_layout::Coverage<'_>,
    aliases: &Aliases,
    raw: &mut RawKern,
    warnings: &mut Vec<String>,
) -> Option<()> {
    let first_format = be16(data, 4)?;
    let second_format = be16(data, 6)?;
    if (first_format | second_format) & 0xff00 != 0 {
        return None;
    }
    let first_len = first_format.count_ones() as usize * 2;
    let second_len = second_format.count_ones() as usize * 2;
    let record_len = 2 + first_len + second_len;
    let set_count = be16(data, 8)?;
    if first_format & !0x0004 != 0 || second_format != 0 {
        warn(warnings, "GPOS pair values contain placement, second-glyph, or device adjustments; only first-glyph XAdvance is retained, matching the reference converter.");
    }
    for &left in aliases.keys() {
        let Some(index) = coverage.get(GlyphId(left)) else {
            continue;
        };
        if index >= set_count {
            return None;
        }
        let offset = usize::from(be16(data, 10 + usize::from(index) * 2)?);
        let set = data.get(offset..)?;
        let pair_count = usize::from(be16(set, 0)?);
        set.get(..2 + pair_count * record_len)?;
        if first_format & 0x0004 == 0 {
            continue;
        }
        let x_offset = 2 + (first_format & 0x0003).count_ones() as usize * 2;
        for index in 0..pair_count {
            let start = 2 + index * record_len;
            let right = be16(set, start)?;
            if aliases.contains_key(&right) {
                let value = be16(set, start + x_offset)? as i16;
                add_kern(raw, left, right, value);
            }
        }
    }
    Some(())
}

fn extract_gpos(face: &Face<'_>, aliases: &Aliases, raw: &mut RawKern, warnings: &mut Vec<String>) {
    let Some(table) = face.tables().gpos else {
        if face.raw_face().table(Tag::from_bytes(b"GPOS")).is_some() {
            warn(
                warnings,
                "GPOS could not be parsed; GPOS kerning was dropped.",
            );
        }
        return;
    };
    for index in lookup_indices(table, &[Tag::from_bytes(b"kern")], warnings) {
        let Some(lookup) = table.lookups.get(index) else {
            warn(
                warnings,
                format!("Malformed GPOS kern lookup {index} was skipped."),
            );
            continue;
        };
        if lookup.flags.0 != 0 {
            warn(warnings, "GPOS kern lookup filtering flags cannot be stored in CPFont; pairs use the reference converter's unfiltered semantics.");
        }
        for sub_index in 0..lookup.subtables.len() {
            let Some(sub) = lookup
                .subtables
                .get::<RawSubtable<'_>>(sub_index)
                .and_then(|s| s.unwrap(9))
            else {
                warn(
                    warnings,
                    format!("Malformed GPOS extension/subtable in lookup {index} was skipped."),
                );
                continue;
            };
            if sub.kind != 2 {
                warn(warnings, format!("Unsupported GPOS kern lookup type {} (lookup {index}); contextual/attachment positioning cannot be stored as CPFont pair kerning.", sub.kind));
                continue;
            }
            let Some(PositioningSubtable::Pair(pair)) =
                PositioningSubtable::parse(sub.data, sub.kind)
            else {
                warn(warnings, format!("Malformed or unsupported GPOS PairPos subtable in lookup {index} was skipped."));
                continue;
            };
            match pair {
                PairAdjustment::Format1 { coverage, .. } => {
                    if pair_format1(sub.data, coverage, aliases, raw, warnings).is_none() {
                        warn(warnings, format!("Malformed GPOS PairPos format 1 records in lookup {index}; extraction stopped at the invalid record."));
                    }
                }
                PairAdjustment::Format2 {
                    coverage,
                    classes,
                    matrix,
                } => {
                    let mut left_classes: BTreeMap<u16, Vec<u16>> = BTreeMap::new();
                    let mut right_classes: BTreeMap<u16, Vec<u16>> = BTreeMap::new();
                    for &gid in aliases.keys() {
                        if coverage.contains(GlyphId(gid)) {
                            left_classes
                                .entry(classes.0.get(GlyphId(gid)))
                                .or_default()
                                .push(gid);
                        }
                        right_classes
                            .entry(classes.1.get(GlyphId(gid)))
                            .or_default()
                            .push(gid);
                    }
                    // Only visit populated classes and only expand NONZERO
                    // values. In particular, a huge implicit class zero does
                    // not cause a quadratic scan of CJK glyphs for zero kern.
                    for (&left_class, left_glyphs) in &left_classes {
                        for (&right_class, right_glyphs) in &right_classes {
                            let Some((first, second)) = matrix.get((left_class, right_class))
                            else {
                                warn(warnings, "GPOS PairPos class index or matrix value is invalid and was skipped.");
                                continue;
                            };
                            if value_has_extra(first, true) || value_has_extra(second, false) {
                                warn(warnings, "GPOS pair values contain placement, second-glyph, or device adjustments; only first-glyph XAdvance is retained, matching the reference converter.");
                            }
                            if first.x_advance == 0 {
                                continue;
                            }
                            for &left in left_glyphs {
                                for &right in right_glyphs {
                                    add_kern(raw, left, right, first.x_advance);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

fn scaled_adjustment(design_units: i64, point_size: u8, units_per_em: u16) -> i8 {
    // Preserve Python's floating-point operation order and round-to-even.
    let ppem = f64::from(point_size) * 150.0 / 72.0;
    let value = design_units as f64 * (ppem / f64::from(units_per_em)) * 16.0;
    value.round_ties_even().clamp(-128.0, 127.0) as i8
}

type Profiles = BTreeMap<u16, Vec<(u16, i8)>>;
fn assign_classes(
    profiles: &Profiles,
    aliases: &Aliases,
) -> Option<(Vec<(u16, u8)>, BTreeMap<u16, u8>, u8)> {
    let mut ordered: Vec<_> = profiles.keys().copied().collect();
    ordered.sort_by_key(|gid| aliases[gid][0]);
    // Sparse nonzero rows/columns are equivalent exactly when the dense
    // profiles are equivalent. Aliases need not be expanded into pair maps.
    let mut profile_classes: BTreeMap<&[(u16, i8)], u8> = BTreeMap::new();
    let mut glyph_classes = BTreeMap::new();
    let mut entries = Vec::new();
    for gid in ordered {
        let profile = profiles[&gid].as_slice();
        let class = if let Some(&class) = profile_classes.get(profile) {
            class
        } else {
            let next = u8::try_from(profile_classes.len() + 1).ok()?;
            profile_classes.insert(profile, next);
            next
        };
        glyph_classes.insert(gid, class);
        entries.extend(aliases[&gid].iter().map(|&cp| (cp, class)));
    }
    entries.sort_unstable();
    Some((entries, glyph_classes, profile_classes.len() as u8))
}

fn derive_classes(
    raw: RawKern,
    aliases: &Aliases,
    point_size: u8,
    units_per_em: u16,
    output: &mut LayoutTables,
) {
    let mut rows: Profiles = BTreeMap::new();
    let mut columns: Profiles = BTreeMap::new();
    for ((left, right), design_units) in raw {
        let value = scaled_adjustment(design_units, point_size, units_per_em);
        if value != 0 {
            rows.entry(left).or_default().push((right, value));
            columns.entry(right).or_default().push((left, value));
        }
    }
    // BTreeMap pair traversal yields sorted sparse row and column profiles.
    let Some((left, left_classes, left_count)) = assign_classes(&rows, aliases) else {
        warn(
            &mut output.warnings,
            "Kerning requires more than 255 left classes; dropped all kerning for this style.",
        );
        return;
    };
    let Some((right, right_classes, right_count)) = assign_classes(&columns, aliases) else {
        warn(
            &mut output.warnings,
            "Kerning requires more than 255 right classes; dropped all kerning for this style.",
        );
        return;
    };
    let mut matrix = vec![0; usize::from(left_count) * usize::from(right_count)];
    for (left_gid, row) in rows {
        let row_start = usize::from(left_classes[&left_gid] - 1) * usize::from(right_count);
        for (right_gid, value) in row {
            matrix[row_start + usize::from(right_classes[&right_gid] - 1)] = value;
        }
    }
    output.left = left;
    output.right = right;
    output.matrix = matrix;
    output.left_count = left_count;
    output.right_count = right_count;
}

pub fn layout_tables(
    bytes: &[u8],
    codepoints: &BTreeSet<u32>,
    point_size: u8,
) -> Result<LayoutTables, String> {
    let face = parse_face(bytes)?;
    let cmap = best_cmap(&face);
    let mut output = LayoutTables::default();
    if cmap.is_empty() {
        warn(
            &mut output.warnings,
            "No supported Unicode cmap: no kerning or ligature rules can be extracted.",
        );
    }
    let mut aliases: Aliases = BTreeMap::new();
    for &cp in codepoints {
        if let Some(&gid) = cmap.get(&cp) {
            if let Ok(cp) = u16::try_from(cp) {
                aliases.entry(gid).or_default().push(cp);
            } else {
                warn(&mut output.warnings, "Supplementary-plane codepoints are excluded from kerning: CPFont class entries are uint16.");
            }
        }
    }
    let mut raw = BTreeMap::new();
    extract_legacy_kern(&face, &aliases, &mut raw, &mut output.warnings);
    extract_gpos(&face, &aliases, &mut raw, &mut output.warnings);
    derive_classes(raw, &aliases, point_size, face.units_per_em(), &mut output);
    output.ligatures = extract_ligatures(&face, &cmap, codepoints, &mut output.warnings);
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scales_rounds_even_and_clamps() {
        // 3pt = 6.25px; at 200 UPEM one design unit is exactly half fp4.
        assert_eq!(scaled_adjustment(1, 3, 200), 0);
        assert_eq!(scaled_adjustment(3, 3, 200), 2);
        assert_eq!(scaled_adjustment(5, 3, 200), 2);
        assert_eq!(scaled_adjustment(-3, 3, 200), -2);
        assert_eq!(scaled_adjustment(-5, 3, 200), -2);
        assert_eq!(scaled_adjustment(1000, 3, 200), 127);
        assert_eq!(scaled_adjustment(-1000, 3, 200), -128);
    }

    #[test]
    fn sparse_classes_preserve_aliases_and_codepoint_order() {
        let aliases = BTreeMap::from([
            (9, vec![65, 193]),
            (2, vec![66]),
            (7, vec![86]),
            (3, vec![87]),
        ]);
        let raw = BTreeMap::from([((9, 7), -8), ((9, 3), -8), ((2, 7), -8), ((2, 3), -8)]);
        let mut output = LayoutTables::default();
        derive_classes(raw, &aliases, 3, 200, &mut output);
        assert_eq!(output.left, vec![(65, 1), (66, 1), (193, 1)]);
        assert_eq!(output.right, vec![(86, 1), (87, 1)]);
        assert_eq!((output.left_count, output.right_count), (1, 1));
        assert_eq!(output.matrix, vec![-4]);
    }

    #[test]
    fn class_ids_follow_codepoints_not_glyph_ids() {
        let aliases = BTreeMap::from([(9, vec![65]), (2, vec![66]), (7, vec![86])]);
        let raw = BTreeMap::from([((9, 7), -8), ((2, 7), -4)]);
        let mut output = LayoutTables::default();
        derive_classes(raw, &aliases, 3, 200, &mut output);
        assert_eq!(output.left, vec![(65, 1), (66, 2)]);
        assert_eq!(output.matrix, vec![-4, -2]);
    }

    #[test]
    fn additive_cancellation_produces_no_classes() {
        let aliases = BTreeMap::from([(1, vec![65]), (2, vec![86])]);
        let mut raw = BTreeMap::new();
        add_kern(&mut raw, 1, 2, 20);
        add_kern(&mut raw, 1, 2, -20);
        let mut output = LayoutTables::default();
        derive_classes(raw, &aliases, 3, 200, &mut output);
        assert_eq!((output.left_count, output.right_count), (0, 0));
        assert!(output.matrix.is_empty());
    }

    #[test]
    fn excessive_class_count_drops_complete_table() {
        let aliases: Aliases = (1..=256).map(|gid| (gid, vec![gid])).collect();
        let raw = (1..=256).map(|gid| ((gid, gid), -4)).collect();
        let mut output = LayoutTables::default();
        derive_classes(raw, &aliases, 3, 200, &mut output);
        assert!(output.left.is_empty() && output.right.is_empty() && output.matrix.is_empty());
        assert_eq!((output.left_count, output.right_count), (0, 0));
        assert!(output.warnings.iter().any(|w| w.contains("255")));
    }

    #[test]
    fn extension_offsets_are_bounded_and_not_recursive() {
        let valid = [0, 1, 0, 2, 0, 0, 0, 8, 0, 1];
        let sub = RawSubtable {
            kind: 9,
            data: &valid,
        }
        .unwrap(9)
        .unwrap();
        assert_eq!(sub.kind, 2);
        assert_eq!(sub.data, &[0, 1]);
        let recursive = [0, 1, 0, 9, 0, 0, 0, 8];
        assert!(RawSubtable {
            kind: 9,
            data: &recursive
        }
        .unwrap(9)
        .is_none());
        let truncated = [0, 1, 0, 2, 0, 0, 0, 20];
        assert!(RawSubtable {
            kind: 9,
            data: &truncated
        }
        .unwrap(9)
        .is_none());
    }

    #[test]
    fn standard_unicode_ligatures_match_reference() {
        assert_eq!(standard_ligature(&[0x66, 0x66]), Some(0xfb00));
        assert_eq!(standard_ligature(&[0x66, 0x69]), Some(0xfb01));
        assert_eq!(standard_ligature(&[0x66, 0x6c]), Some(0xfb02));
        assert_eq!(standard_ligature(&[0x66, 0x66, 0x69]), Some(0xfb03));
        assert_eq!(standard_ligature(&[0x66, 0x66, 0x6c]), Some(0xfb04));
        assert_eq!(standard_ligature(&[0x17f, 0x74]), Some(0xfb05));
        assert_eq!(standard_ligature(&[0x73, 0x74]), Some(0xfb06));
        assert_eq!(standard_ligature(&[0x78, 0x79]), None);
    }
}
