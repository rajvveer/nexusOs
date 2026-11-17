"""Generate arch/x86_64/keymaps_data.h: the keyboard layouts Aurora offers (aurora_keyboard.c CATALOG), as the kernel's
early-boot prompts (console_prompt.c, keymaps.c: the encrypted-root and crypttab passphrases, the installer) need them.
The tables come from the same xkeyboard-config symbol files Aurora's xkbcommon compiles (rules evdev, model pc105):
four shift levels (base, Shift, AltGr, Shift+AltGr) of the 48 main-block keys plus the two JIS keys, as Unicode code
points; dead keys become combining marks U+0300..U+036F, composed with the next character (NFC) at run time.
xkeyboard-config is MIT licensed; only generated data goes into the kernel.

    python -I arch/x86_64/gen_keymaps.py [XKB_DIR] [KEYSYMS_H]
defaults: arch/x86_64/out/chrome-root/usr/share/X11/xkb and arch/x86_64/out/desktop-sdk/usr/include/xkbcommon/xkbcommon-keysyms.h"""
from pathlib import Path
import re, sys, unicodedata

HERE = Path(__file__).resolve().parent
XKB = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / 'out/chrome-root/usr/share/X11/xkb'
KEYSYMS = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / 'out/desktop-sdk/usr/include/xkbcommon/xkbcommon-keysyms.h'
OUT = HERE / 'keymaps_data.h'

# aurora_keyboard.c CATALOG (code, variant)
LAYOUTS = [('us', '', 'English (US)'), ('gb', '', 'English (UK)'), ('us', 'intl', 'English (US, international)'), ('de', '', 'German'),
           ('fr', '', 'French'), ('es', '', 'Spanish'), ('it', '', 'Italian'), ('pt', '', 'Portuguese'), ('in', '', 'Hindi (InScript)'),
           ('ru', '', 'Russian'), ('gr', '', 'Greek'), ('il', '', 'Hebrew'), ('ara', '', 'Arabic'), ('th', '', 'Thai'),
           ('jp', '', 'Japanese (kana keys)'), ('kr', '', 'Korean'), ('tr', '', 'Turkish'), ('pl', '', 'Polish'), ('se', '', 'Swedish')]
# xkb key name -> Linux (evdev) key code; the order is the table column order
KEYS = ['TLDE'] + [f'AE{i:02d}' for i in range(1, 13)] + [f'AD{i:02d}' for i in range(1, 13)] + ['BKSL'] + \
       [f'AC{i:02d}' for i in range(1, 12)] + ['LSGT'] + [f'AB{i:02d}' for i in range(1, 11)] + ['AE13', 'AB11']
CODES = [41] + list(range(2, 14)) + list(range(16, 28)) + [43] + list(range(30, 41)) + [86] + list(range(44, 54)) + [124, 89]
DEAD = {'grave': 0x300, 'acute': 0x301, 'circumflex': 0x302, 'tilde': 0x303, 'macron': 0x304, 'breve': 0x306, 'abovedot': 0x307,
        'diaeresis': 0x308, 'abovering': 0x30A, 'doubleacute': 0x30B, 'caron': 0x30C, 'cedilla': 0x327, 'ogonek': 0x328,
        'belowdot': 0x323, 'hook': 0x309, 'horn': 0x31B, 'iota': 0x345, 'belowmacron': 0x331, 'semivoiced_sound': None, 'voiced_sound': None}


def keysym_table():
    names = {}
    for m in re.finditer(r'#define XKB_KEY_(\w+)\s+0x([0-9a-fA-F]+)(?:\s*/\*\s*(?:\(?)U\+([0-9A-Fa-f]{4,6}))?', KEYSYMS.read_text(errors='replace')):
        name, value, uni = m.group(1), int(m.group(2), 16), m.group(3)
        if 0x20 <= value <= 0x7E or 0xA0 <= value <= 0xFF: cp = value
        elif value & 0xFF000000 == 0x01000000: cp = value & 0xFFFFFF
        elif uni: cp = int(uni, 16)
        else: cp = None
        names[name] = cp
    return names


def strip(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


SECTIONS = {}
def sections(file):
    if file in SECTIONS: return SECTIONS[file]
    text = strip((XKB / 'symbols' / file).read_text(errors='replace'))
    found, default = {}, None
    for m in re.finditer(r'((?:\w+\s+)*)xkb_symbols\s+"([^"]+)"\s*\{', text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += text[i] == '{'; depth -= text[i] == '}'; i += 1
        found[m.group(2)] = text[m.end():i - 1]
        if 'default' in m.group(1).split() and default is None: default = m.group(2)
    SECTIONS[file] = (found, default or next(iter(found)))
    return SECTIONS[file]


def body_of(spec):
    m = re.fullmatch(r'([\w\-/]+)(?:\(([^)]+)\))?(?::\d+)?', spec.strip())
    file, name = m.group(1), m.group(2)
    found, default = sections(file)
    return found[name or default]


STATEMENT = re.compile(r'(?P<inc>(?:include|augment|override|replace)\s+"(?P<spec>[^"]+)")|'
                       r'(?P<key>(?:(?P<mode>replace|override|augment)\s+)?key\s+<(?P<name>\w+)>\s*\{(?P<inner>[^{}]*)\})')
def evaluate(body, keys, mode='override'):
    for m in STATEMENT.finditer(body):
        if m.group('inc'):
            how = m.group(0).split()[0]
            for part in re.split(r'[+|]', m.group('spec')):
                if part: evaluate(body_of(part), keys, 'augment' if how == 'augment' else 'override')
            continue
        key, inner = m.group('name'), m.group('inner')
        if key not in KEYS: continue
        group1 = re.search(r'symbols\s*\[\s*group1\s*\]\s*=\s*\[([^\]]*)\]', inner, flags=re.I)
        lists = re.findall(r'\[([^\]]*)\]', re.sub(r'\w+\s*\[\s*group\d\s*\]', ' ', inner, flags=re.I))
        chosen = group1.group(1) if group1 else (lists[0] if lists else None)
        if chosen is None: continue
        levels = [s.strip() for s in chosen.split(',')]
        old = keys.get(key, [])
        statement_mode = m.group('mode') or mode
        if statement_mode == 'replace': keys[key] = levels; continue
        merged = list(old) + [''] * (len(levels) - len(old))
        for i, s in enumerate(levels):
            if not s or s in ('NoSymbol', 'any'): continue
            if statement_mode == 'augment' and i < len(old) and old[i] and old[i] != 'NoSymbol': continue
            merged[i] = s
        keys[key] = merged


def codepoint(sym, table):
    if not sym or sym in ('NoSymbol', 'VoidSymbol', 'any'): return 0
    if sym.startswith('dead_'):
        mark = DEAD.get(sym[5:]); return mark or 0
    m = re.fullmatch(r'U([0-9A-Fa-f]{4,6})', sym)
    if m: return int(m.group(1), 16)
    m = re.fullmatch(r'0x([0-9A-Fa-f]+)', sym)
    if m:
        v = int(m.group(1), 16); return v & 0xFFFFFF if v & 0xFF000000 == 0x01000000 else (v if v < 0x100 else 0)
    if len(sym) == 1: return ord(sym)
    cp = table.get(sym)
    return cp if cp and cp >= 0x20 and not (0xD800 <= cp < 0xE000) else 0


def main():
    table = keysym_table()
    rows, marks = [], set()
    for code, variant, title in LAYOUTS:
        keys = {}
        evaluate(body_of(f'{code}({variant})' if variant else code), keys)
        levels = []
        for key in KEYS:
            syms = keys.get(key, [])
            cps = [codepoint(syms[i] if i < len(syms) else '', table) for i in range(4)]
            if not cps[1] and cps[0]: cps[1] = cps[0]
            for cp in cps:
                if 0x300 <= cp <= 0x36F: marks.add(cp)
            levels.append(cps)
        alpha = sum(1 << i for i, cps in enumerate(levels) if cps[0] and cps[1] != cps[0] and chr(cps[0]).upper() == chr(cps[1]))
        rows.append((code, variant, title, alpha, levels))
    us = rows[0][4]
    assert us[KEYS.index('AD01')][:2] == [ord('q'), ord('Q')] and us[KEYS.index('AE01')][:2] == [ord('1'), ord('!')], 'us table'
    de = rows[3][4]
    assert de[KEYS.index('AD06')][0] == ord('z') and de[KEYS.index('AB01')][0] == ord('y') and de[KEYS.index('AE11')][0] == 0xDF, 'de table'
    bases = [chr(c) for c in range(0x41, 0x5B)] + [chr(c) for c in range(0x61, 0x7B)] + [' ']
    compose = []
    spacing = {0x300: 0x60, 0x301: 0xB4, 0x302: 0x5E, 0x303: 0x7E, 0x304: 0xAF, 0x306: 0x2D8, 0x307: 0x2D9, 0x308: 0xA8, 0x30A: 0x2DA,
               0x30B: 0x2DD, 0x30C: 0x2C7, 0x327: 0xB8, 0x328: 0x2DB, 0x323: 0x2E, 0x309: 0x2C0, 0x31B: 0x31B, 0x345: 0x37A, 0x331: 0x2CD}
    for mark in sorted(marks):
        compose.append((mark, 0x20, spacing.get(mark, mark)))
        for b in bases[:-1]:
            c = unicodedata.normalize('NFC', b + chr(mark))
            if len(c) == 1: compose.append((mark, ord(b), ord(c)))
    out = ['/* Generated by arch/x86_64/gen_keymaps.py from xkeyboard-config (MIT license) -- do not edit.',
           ' * Columns: Linux key codes KEYMAP_CODES; levels base, Shift, AltGr, Shift+AltGr (Unicode, 0 = none;',
           ' * U+0300..U+036F = dead key); alphabetic: the keys Caps Lock shifts (bit = column). */',
           f'#define KEYMAP_KEYS {len(KEYS)}',
           'static const uint8_t KEYMAP_CODES[KEYMAP_KEYS] = {' + ', '.join(map(str, CODES)) + '};',
           f'#define KEYMAP_COUNT {len(rows)}',
           'static const struct keymap_table { const char* layout; const char* variant; const char* title; uint64_t alphabetic; uint32_t level[KEYMAP_KEYS][4]; } KEYMAP_TABLES[KEYMAP_COUNT] = {']
    for code, variant, title, alpha, levels in rows:
        cells = ', '.join('{' + ', '.join(f'0x{c:X}' for c in cps) + '}' for cps in levels)
        out.append(f'    {{"{code}", "{variant}", "{title}", 0x{alpha:X}ULL, {{{cells}}}}},')
    out.append('};')
    out.append(f'#define KEYMAP_COMPOSE {len(compose)}')
    out.append('static const struct keymap_compose { uint16_t mark; uint16_t base; uint16_t result; } KEYMAP_COMPOSITIONS[KEYMAP_COMPOSE] = {')
    for i in range(0, len(compose), 6):
        out.append('    ' + ' '.join(f'{{0x{m:X}, 0x{b:X}, 0x{r:X}}},' for m, b, r in compose[i:i + 6]))
    out.append('};')
    OUT.write_bytes(('\n'.join(out) + '\n').encode())
    print(f'[keymaps] {len(rows)} layouts, {len(compose)} compositions, dead keys {", ".join(f"U+{m:04X}" for m in sorted(marks))} -> {OUT}')


main()
