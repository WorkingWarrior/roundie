#!/usr/bin/env python3
"""Regenerate components/roundie_fonts: Google Sans Flex Medium 14/20/28/48 with Polish letters.

The source is the variable font released by Google Fonts (SIL OFL 1.1, see
components/roundie_fonts/OFL.txt), pinned by SHA-256 and not stored in Git:
  https://github.com/google/fonts/blob/bd8f81ddb5c74d5c8897b36ad88b440266245103/
  ofl/googlesansflex/GoogleSansFlex[ROND,opsz,slnt,wdth,wght].ttf
For each size, fontTools makes a static instance (weight 500, optical size equal
to the pixel size, normal width, no rounding or slant). lv_font_conv renders it
with the options and FontAwesome symbols of LVGL's built-in Montserrat fonts
(pinned managed_components/lvgl__lvgl/scripts/built_in_font), so LV_SYMBOL_*
keep working. The output is post-processed for LVGL 9 as LVGL's own fonts are,
and its digits are made tabular (equal advance, centred, no kerning), so
changing numbers such as a running stopwatch do not shift sideways.

Usage: tools/generate_fonts.py --font GoogleSansFlex.ttf --converter PATH/TO/lv_font_conv
Requires lv_font_conv 1.5.2 and fontTools.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = Path(__file__).resolve().parents[1]
SYMBOL_SOURCE = ROOT / "managed_components/lvgl__lvgl/scripts/built_in_font"
OUTPUT = ROOT / "components/roundie_fonts"
FONT_SHA256 = "a9ac770798857d5e8ac593ced42b22e40a7c08cec98319f92b0eccc2783a77ba"
SIZES = (14, 20, 28, 48)
WEIGHT = 500
# ASCII, degree and bullet as in LVGL; then ÓóĄąĆćĘęŁłŃńŚśŹźŻż.
RANGES = ("0x20-0x7F,0xB0,0x2022,0xD3,0xF3,0x104-0x107,0x118-0x119,0x141-0x144,"
          "0x15A-0x15B,0x179-0x17C")
SYMBOLS = ("61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,"
           "61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,"
           "61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,"
           "61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,"
           "62212,62189,62810,63426,63650")


def lvgl9(text):
    """LVGL 9 has no glyph cache field; fonts are const; include lvgl.h directly."""
    text = re.sub(r"#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n.*?#endif\n", '#include "lvgl.h"\n', text,
                  count=1, flags=re.S)
    text = text.replace("LV_VERSION_CHECK(8, 0, 0)", "LVGL_VERSION_MAJOR >= 8")
    text = text.replace("LV_VERSION_CHECK(7, 4, 0) || ", "")
    text = re.sub(r"#if LVGL_VERSION_MAJOR >= 8\n/\*Store all the custom data of the font\*/\n"
                  r"static  lv_font_fmt_txt_glyph_cache_t cache;\n", "#if LVGL_VERSION_MAJOR >= 8\n",
                  text, count=1)
    text = re.sub(r"#if LVGL_VERSION_MAJOR >= 8\n    \.cache = &cache\n#endif\n", "", text, count=1)
    if "cache" in text or "LV_VERSION_CHECK" in text:
        raise SystemExit("unexpected lv_font_conv output; update the LVGL 9 post-processing")
    return text


DIGIT_IDS = range(0x30 - 0x20 + 1, 0x39 - 0x20 + 2)  # glyph id 1 is U+0020


def tabular_digits(text):
    """Equal advance for 0-9 (the widest), each glyph centred in it, and no
    kerning classes for digits."""
    table = re.search(r"glyph_dsc\[\] = \{\n(.*?)\n\};", text, flags=re.S)
    lines = table.group(1).split("\n")
    fields = re.compile(r"\.adv_w = (\d+),(.*?)\.ofs_x = (-?\d+)")
    widest = max(int(fields.search(lines[i]).group(1)) for i in DIGIT_IDS)
    for i in DIGIT_IDS:
        match = fields.search(lines[i])
        advance = int(match.group(1))
        shift = round((widest - advance) / 32)  # half the difference; advance is in 1/16 px
        lines[i] = (lines[i][:match.start()] + f".adv_w = {widest},{match.group(2)}.ofs_x = "
                    f"{int(match.group(3)) + shift}" + lines[i][match.end():])
    text = text[:table.start(1)] + "\n".join(lines) + text[table.end(1):]
    for name in ("kern_left_class_mapping", "kern_right_class_mapping"):
        mapping = re.search(name + r"\[\] =\n\{\n(.*?)\n\};", text, flags=re.S)
        values = [v.strip() for v in mapping.group(1).replace("\n", "").split(",") if v.strip()]
        for i in DIGIT_IDS:
            values[i] = "0"
        rows = ["    " + ", ".join(values[j:j + 8]) for j in range(0, len(values), 8)]
        text = text[:mapping.start(1)] + ",\n".join(rows) + text[mapping.end(1):]
    return text


def static_instance(font_path, size, target):
    font = TTFont(font_path)
    axes = {"wght": WEIGHT, "opsz": size, "wdth": 100, "ROND": 0, "slnt": 0}
    instancer.instantiateVariableFont(font, axes, inplace=True)
    font.save(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--converter", required=True, type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256(args.font.read_bytes()).hexdigest()
    if digest != FONT_SHA256:
        raise SystemExit(f"{args.font}: SHA-256 {digest}, expected {FONT_SHA256}")
    version = subprocess.run([str(args.converter), "--version"], check=True, capture_output=True,
                             text=True).stdout.strip()
    if version != "1.5.2":
        raise SystemExit(f"lv_font_conv 1.5.2 required, found {version}")
    symbols = SYMBOL_SOURCE / "FontAwesome5-Solid+Brands+Regular.woff"
    with tempfile.TemporaryDirectory() as work:
        for size in SIZES:
            name = f"roundie_font_{size}"
            instance = Path(work) / f"{name}.ttf"
            static_instance(args.font, size, instance)
            target = OUTPUT / f"{name}.c"
            subprocess.run([str(args.converter), "--no-compress", "--no-prefilter", "--bpp", "4",
                            "--size", str(size), "--font", str(instance), "-r", RANGES,
                            "--font", str(symbols), "-r", SYMBOLS,
                            "--format", "lvgl", "-o", str(target), "--force-fast-kern-format"],
                           check=True)
            text = target.read_text().replace(str(instance), f"Google Sans Flex wght {WEIGHT} opsz {size}")
            text = text.replace(str(symbols), symbols.name)
            text = text.replace(str(target), target.name)
            target.write_text(tabular_digits(lvgl9(text)))
            print(f"{target.relative_to(ROOT)}: {target.stat().st_size} bytes")


if __name__ == "__main__":
    main()
