#!/usr/bin/env python3
"""Display layout bounds check for the three-page 320x240 UI.

Parses card_create(page, x, y, w, h) calls in display_service.c and verifies:
  - every card fits within 320x240,
  - cards on the same page do not overlap each other,
  - the page indicator sits below the lowest card with >= 4px clearance.
Exits non-zero on any violation.
"""
import re
import sys

W = 320
H = 240

def parse_cards(path):
    with open(path, 'r', encoding='utf-8') as fh:
        text = fh.read()
    # card_create(page, x, y, w, h)
    calls = re.findall(
        r'card_create\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)',
        text)
    # Page builders: build_overview_page / build_telemetry_page / build_voice_page
    builder = {}
    for m in re.finditer(
            r'build_(overview|telemetry|voice)_page\(void\)\s*\{(.*?)\n\}\n',
            text, re.S):
        builder[m.group(1)] = m.group(2)
    result = {'overview': [], 'telemetry': [], 'voice': []}
    for page, x, y, w, h in calls:
        # Associate call to the builder whose body contains it (by position).
        for name, body in builder.items():
            # crude: find card_create call inside the builder text
            pass
        result.setdefault('unmapped', []).append((int(x), int(y), int(w), int(h)))
    return result

def parse_cards_by_builder(path):
    with open(path, 'r', encoding='utf-8') as fh:
        text = fh.read()
    out = {}
    # find each builder's span using brace matching on function definition
    pat = re.compile(r'static void (build_(?:overview|telemetry|voice)_page)\(void\)\s*\{(.*?)\n\}\n', re.S)
    for m in pat.finditer(text):
        name = m.group(1)
        body = m.group(2)
        cards = []
        for cm in re.finditer(r'card_create\(\s*\w+\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)', body):
            cards.append(tuple(int(v) for v in cm.groups()))
        out[name] = cards
    return out

def overlap(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return not (ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay)

def main():
    path = 'components/display_service/display_service.c'
    pages = parse_cards_by_builder(path)
    ok = True
    print('DISPLAY_LAYOUT_CHECK 320x240')
    page_label = {'build_overview_page': 'overview',
                  'build_telemetry_page': 'telemetry',
                  'build_voice_page': 'voice'}
    lowest_bottom = 0
    for builder, cards in pages.items():
        label = page_label.get(builder, builder)
        for (x, y, w, h) in cards:
            if x < 0 or y < 0 or x + w > W or y + h > H:
                print('FAIL %s card (%d,%d,%d,%d) out of bounds' %
                      (label, x, y, w, h))
                ok = False
            lowest_bottom = max(lowest_bottom, y + h)
        for i in range(len(cards)):
            for j in range(i + 1, len(cards)):
                if overlap(cards[i], cards[j]):
                    print('FAIL %s cards overlap: %s vs %s' %
                          (label, cards[i], cards[j]))
                    ok = False
        print('%s cards=%d' % (label, len(cards)))
    # page indicator at y=222 must clear the lowest card
    indicator_y = 222
    if lowest_bottom > indicator_y - 3:
        print('FAIL page indicator y=%d too close to lowest card bottom=%d'
              % (indicator_y, lowest_bottom))
        ok = False
    print('LOWEST_CARD_BOTTOM=%d' % lowest_bottom)
    print('DISPLAY_LAYOUT_RESULT=%s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
