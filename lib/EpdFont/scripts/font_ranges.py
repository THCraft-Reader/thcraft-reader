"""Unicode coverage shared by legacy bitmap and native outline generation.

These are inclusive intervals. The converter merges their overlaps and orders
before intersecting with the source cmap; changing this list changes both paths.
"""

BUILTIN_INTERVALS = [
    (0x0000, 0x007F),  # Basic Latin
    (0x0080, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A
    (0x01A0, 0x01A1),  # Vietnamese O with horn
    (0x01AF, 0x01B0),  # Vietnamese U with horn
    (0x01C4, 0x021F),  # European Latin Extended-B
    (0x1EA0, 0x1EF9),  # Vietnamese precomposed tone marks
    (0x2000, 0x206F),  # General punctuation and joiners
    (0x2010, 0x203A),  # Dashes, quotes, prime marks
    (0x2040, 0x205F),  # Miscellaneous punctuation
    (0x20A0, 0x20CF),  # Currency symbols
    (0x0300, 0x036F),  # Combining diacritical marks
    (0x0400, 0x04FF),  # Cyrillic
    (0x2070, 0x209F),  # Superscripts and subscripts
    (0x2200, 0x22FF),  # Mathematical operators
    (0x2190, 0x21FF),  # Arrows
    (0xFB00, 0xFB06),  # Latin ligature presentation forms
    (0xFFFD, 0xFFFD),  # Replacement character
]

# Additional legacy UI coverage in convert-builtin-fonts.sh. Native generation
# also keeps the decomposed bases and script marks: HarfBuzz consumes logical
# Unicode, not MiniBidi's already-shaped Arabic presentation forms.
UI_INTERVALS = [
    (0x05D0, 0x05EA),
    (0x060C, 0x060C),
    (0x061B, 0x061B),
    (0x061F, 0x061F),
    (0x0621, 0x0621),
    (0x0640, 0x0640),
    (0x0654, 0x0654),
    (0x0660, 0x0669),
    (0x06BA, 0x06BA),
    (0x06D4, 0x06D4),
    (0x06D5, 0x06D5),
    (0x06F0, 0x06F9),
    (0xFB56, 0xFB59),
    (0xFB66, 0xFB69),
    (0xFB7A, 0xFB7D),
    (0xFB88, 0xFB95),
    (0xFB9E, 0xFB9F),
    (0xFBA6, 0xFBB1),
    (0xFBFC, 0xFBFF),
    (0xFE80, 0xFEFC),
]

NATIVE_SCRIPT_INTERVALS = [
    (0x0590, 0x05FF),  # Hebrew bases, points and punctuation
    (0x0600, 0x06FF),  # Arabic bases, harakat and digits
    (0x0E00, 0x0E7F),  # Complete Thai Unicode block
    (0x25CC, 0x25CC),  # Dotted circle for isolated marks, where present
]
