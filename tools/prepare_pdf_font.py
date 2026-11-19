"""Build the renamed OFL font used for searchable PDF text (build time only)."""
import argparse
import copy
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--fonttools-wheel", required=True, type=Path)
parser.add_argument("--family", choices=["Sans", "Serif"], default="Sans")
args = parser.parse_args()
sys.path.insert(0, str(args.fonttools_wheel.resolve()))
from fontTools.ttLib import TTFont
from fontTools import subset

font = TTFont(args.source, recalcTimestamp=False)
# Noto shares outlines between ordinary ideographs and Kangxi radicals. Qt's
# PDF reverse cmap otherwise copies e.g. 示 as ⽰. Remove only radical aliases;
# the ordinary CJK codepoints and outlines remain intact.
for table in font["cmap"].tables:
    if table.isUnicode():
        for codepoint in list(table.cmap):
            if 0x2E80 <= codepoint <= 0x2FDF:
                del table.cmap[codepoint]

# The PDF writer derives Unicode from glyph IDs. Shared punctuation glyphs
# otherwise copy an en dash as a figure dash, or a full-width slash as a
# division slash. Give each remaining Unicode alias a distinct glyph ID.
# Noto CJK starts at the 65,535-glyph limit, so first remove unreachable glyphs
# while retaining every Unicode character and all layout-feature closures.
original_order = font.getGlyphOrder()[:]
options = subset.Options()
options.layout_features = ["*"]
subsetter = subset.Subsetter(options=options)
subsetter.populate(unicodes=font.getBestCmap())
subsetter.subset(font)
order = font.getGlyphOrder()[:]
retained = set(order)
spare_names = iter(name for name in original_order if name not in retained)
seen = set()
top = font["CFF "].cff.topDictIndex[0]
chars = top.CharStrings
for codepoint, glyph in sorted(font.getBestCmap().items()):
    if glyph not in seen:
        seen.add(glyph)
        continue
    if len(order) >= 65535:
        raise ValueError("Too many glyphs to give Unicode aliases unique IDs")
    name = next(spare_names)
    original_index = order.index(glyph)
    chars.charStrings[name] = len(chars.charStringsIndex)
    chars.charStringsIndex.append(copy.copy(chars[glyph]))
    top.charset.append(name)
    if hasattr(top, "FDSelect"):
        top.FDSelect.gidArray.append(top.FDSelect.gidArray[original_index])
    order.append(name)
    for metric in ("hmtx", "vmtx"):
        if metric in font:
            font[metric].metrics[name] = font[metric].metrics[glyph]
    for table in font["cmap"].tables:
        if table.isUnicode() and codepoint in table.cmap:
            table.cmap[codepoint] = name
font.setGlyphOrder(order)
font["maxp"].numGlyphs = len(order)

names = {
    1: "Yiyi PDF Sans", 2: "Regular", 3: "Yiyi PDF Sans Regular 1.0",
    4: "Yiyi PDF Sans Regular", 6: "YiyiPdfSans-Regular",
    16: "Yiyi PDF Sans", 17: "Regular",
}
if args.family == "Serif":
    names = {key: value.replace("Sans", "Serif") for key, value in names.items()}
for record in font["name"].names:
    if record.nameID in names:
        record.string = names[record.nameID].encode(record.getEncoding())
if "CFF " in font:
    cff = font["CFF "].cff
    cff.fontNames = [names[6]]
    for top in cff.topDictIndex:
        top.FullName = names[4]
        top.FamilyName = names[1]
font.save(args.output)
font.close()
