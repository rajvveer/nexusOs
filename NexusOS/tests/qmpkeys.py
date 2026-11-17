"""QMP and keyboard typing for the boot-prompt tests (test_x64_cryptroot.py, test_x64_crypttab.py).

    q = Qmp(port); type_text(q, 'text\\n')                       # US key positions (QEMU qcodes name US keys)
    type_layout(q, 'Grüße é\\n', 'de')                           # the physical keys a German keyboard needs
type_layout reads the kernel's own tables (arch/x86_64/keymaps_data.h, generated from xkeyboard-config), so a
character needing Shift, AltGr (right Alt) or a dead key is pressed the way that layout produces it."""
from pathlib import Path
import json, re, socket, time, unicodedata

ROOT = Path(__file__).resolve().parents[1]


class Qmp:
    def __init__(self, port, timeout=60):
        deadline = time.time() + timeout
        while True:
            try: self.s = socket.create_connection(('127.0.0.1', port), timeout=60); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(.3)
        self.f = self.s.makefile('rwb'); self.read(); self.cmd('qmp_capabilities')
    def read(self):
        while True:
            message = json.loads(self.f.readline() or b'{"error": "closed"}')
            if 'event' not in message: return message
    def cmd(self, name, arguments=None):
        self.f.write(json.dumps({'execute': name, **({'arguments': arguments} if arguments else {})}).encode() + b'\n'); self.f.flush()
        reply = self.read()
        if 'error' in reply: raise RuntimeError(reply['error'])
        return reply.get('return')


def press(q, keys, device=None):
    """keys: qcodes held together (modifiers first), e.g. ['shift', 'a']"""
    down = [{'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': k}}} for k in keys]
    up = [{'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': k}}} for k in reversed(keys)]
    for batch in (down, up):
        q.cmd('input-send-event', {**({'device': device} if device else {}), 'events': batch}); time.sleep(.04)


US = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash', ';': 'semicolon',
      "'": 'apostrophe', '`': 'grave_accent', ',': 'comma', '.': 'dot', '/': 'slash'}
US_SHIFT = dict(zip('!@#$%^&*()', '1234567890')) | {'_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', '|': 'backslash',
                                                    ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent', '<': 'comma', '>': 'dot', '?': 'slash'}
def type_text(q, text, device=None):
    for ch in text:
        if ch in US_SHIFT: press(q, ['shift', US_SHIFT[ch]], device)
        elif ch.isupper(): press(q, ['shift', ch.lower()], device)
        else: press(q, [US.get(ch, ch)], device)


# keymaps_data.h columns (gen_keymaps.py KEYS) as QEMU qcodes (US key names of the same physical keys)
COLUMNS = (['grave_accent', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'minus', 'equal'] + list('qwertyuiop') + ['bracket_left', 'bracket_right', 'backslash'] +
           list('asdfghjkl') + ['semicolon', 'apostrophe', 'less'] + list('zxcvbnm') + ['comma', 'dot', 'slash', 'yen', 'ro'])
_TABLES = {}
def layout_table(name):
    if not _TABLES:
        text = (ROOT / 'arch/x86_64/keymaps_data.h').read_text(encoding='utf-8')
        for m in re.finditer(r'\{"(\w+)", "(\w*)", "[^"]*", 0x[0-9A-F]+ULL, \{(.*?)\}\},\n', text):
            cells = [[int(v, 16) for v in c] for c in re.findall(r'\{(0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+)\}', m.group(3))]
            _TABLES[m.group(1) + (f'({m.group(2)})' if m.group(2) else '')] = cells
    return _TABLES[name]


def layout_keys(text, layout):
    """[[qcode, ...], ...]: the key presses that type `text` on `layout` (dead key + base for composed letters)"""
    table = layout_table(layout); out = []
    def find(cp):
        for column, levels in enumerate(table):
            for level, value in enumerate(levels):
                if value == cp: return [m for m, on in (('shift', level & 1), ('alt_r', level & 2)) if on] + [COLUMNS[column]]
        return None
    for ch in text:
        if ch == ' ': out.append(['spc']); continue
        if ch == '\n': out.append(['ret']); continue
        keys = find(ord(ch))
        if keys: out.append(keys); continue
        decomposed = unicodedata.normalize('NFD', ch)
        if len(decomposed) == 2 and (dead := find(ord(decomposed[1]))) and (base := find(ord(decomposed[0]))): out += [dead, base]; continue
        raise ValueError(f'{layout} cannot type {ch!r}')
    return out


def type_layout(q, text, layout, device=None):
    for keys in layout_keys(text, layout): press(q, keys, device)
