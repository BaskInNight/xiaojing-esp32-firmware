#!/usr/bin/env python3
"""UTF-8 glyph coverage scan for the telemetry font.

Parses the LVGL cmap tables in components/display_service/telemetry_font.c,
builds the set of covered Unicode codepoints, then scans the display sources
for every non-ASCII codepoint used in string literals and reports any that the
font cannot render (would become a tofu box).  Exits non-zero when missing.
"""
import re
import sys

FONT = 'components/display_service/telemetry_font.c'
SOURCES = [
    'components/display_service/display_service.c',
    'components/display_service/display_model.c',
    'components/display_service/include/display_service.h',
]

def parse_u16_array(text, name):
    m = re.search(r'static const uint16_t ' + name + r'\[\]\s*=\s*\{(.*?)\};',
                  text, re.S)
    if not m:
        return []
    return [int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', m.group(1))]

def build_covered(font_text):
    covered = set()
    # cmaps block
    block = re.search(r'cmaps\[\]\s*=\s*\{(.*?)\};', font_text, re.S)
    cmap_entries = re.findall(
        r'\.range_start\s*=\s*(\d+)\s*,\s*\.range_length\s*=\s*(\d+)[^}]*?'
        r'\.unicode_list\s*=\s*(\w+)\s*,[^}]*?\.list_length\s*=\s*(\d+)',
        block.group(1), re.S)
    lists = {}
    for name in ('unicode_list_1', 'unicode_list_3'):
        lists[name] = parse_u16_array(font_text, name)
    for start_s, length_s, list_name, count_s in cmap_entries:
        start = int(start_s)
        length = int(length_s)
        count = int(count_s)
        if list_name == 'NULL':
            covered.update(range(start, start + length))
        else:
            arr = lists.get(list_name, [])
            for v in arr[:count]:
                covered.add(start + v)
    return covered

def iter_codepoints(path):
    with open(path, 'r', encoding='utf-8') as fh:
        text = fh.read()
    # strip comments so only real literals are checked
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    i = 0
    while i < len(text):
        ch = text[i]
        o = ord(ch)
        if o < 0x80:
            i += 1
            continue
        if o >= 0xD800 and o <= 0xDFFF:
            continue  # surrogate (shouldn't appear in UTF-8 file)
        yield ch, o, i
        i += 1

def main():
    with open(FONT, 'r', encoding='utf-8') as fh:
        font_text = fh.read()
    covered = build_covered(font_text)
    missing = {}
    total = 0
    for path in SOURCES:
        for ch, o, idx in iter_codepoints(path):
            total += 1
            if o not in covered:
                missing.setdefault(path, set()).add((ch, o))
    print('FONT_CMAP_COVERED_CODEPOINTS=%d' % len(covered))
    print('NON_ASCII_CHARS_SCANNED=%d' % total)
    if not missing:
        print('UTF8_GLYPH_SCAN=PASS (all non-ASCII chars covered)')
        return 0
    for path, chars in missing.items():
        for ch, o in sorted(chars):
            print('MISSING_U+%04X %s in %s' % (o, ch, path))
    print('UTF8_GLYPH_SCAN=FAIL')
    return 1

if __name__ == '__main__':
    sys.exit(main())
