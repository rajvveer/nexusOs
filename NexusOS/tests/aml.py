"""A small AML (ACPI Machine Language) assembler for test tables (ACPI 6.5 chapter 20 encodings).

Builds a DefinitionBlock from nested Python calls instead of ASL, so the tests need no iASL. Covers the
subset the power tests use: Scope, Device, ThermalZone, PowerResource, Name, Method, OperationRegion, Field,
If/Else/While, Return, Store, Notify, arithmetic/logic operators, Package, Buffer, EisaId, Local/Arg.

    from aml import *
    table = definition_block('SSDT', 'NEXUS', 'NXPOWER', [scope('\\_SB', [device('NXAC', [name('_HID', 'ACPI0003')])])])
"""
import struct

# ---- encodings ----
def pkg_length(body_length):
    """PkgLength for a body of `body_length` bytes (the encoding counts its own bytes)."""
    for extra in range(4):
        total = body_length + 1 + extra
        if extra == 0 and total < 0x40:
            return bytes([total])
        if extra and total < (1 << (4 + 8 * extra)):
            out = [(extra << 6) | (total & 0x0F)]
            value = total >> 4
            for _ in range(extra):
                out.append(value & 0xFF); value >>= 8
            return bytes(out)
    raise ValueError('package too long')


def field_length(bits):
    """FieldElement length (bits): PkgLength-style, not counting itself."""
    if bits < 0x40:
        return bytes([bits])
    for extra in range(1, 4):
        if bits < (1 << (4 + 8 * extra)):
            out = [(extra << 6) | (bits & 0x0F)]
            value = bits >> 4
            for _ in range(extra):
                out.append(value & 0xFF); value >>= 8
            return bytes(out)
    raise ValueError('field too long')


def nameseg(seg):
    seg = seg.upper()
    if not 1 <= len(seg) <= 4 or not (seg[0].isalpha() or seg[0] == '_'):
        raise ValueError('bad name segment ' + seg)
    return seg.ljust(4, '_').encode()


def namestring(path):
    prefix = b''
    while path.startswith('\\') or path.startswith('^'):
        prefix += path[0].encode(); path = path[1:]
    segs = [s for s in path.split('.') if s]
    if not segs:
        return prefix + b'\x00'
    if len(segs) == 1:
        return prefix + nameseg(segs[0])
    if len(segs) == 2:
        return prefix + b'\x2E' + nameseg(segs[0]) + nameseg(segs[1])
    return prefix + b'\x2F' + bytes([len(segs)]) + b''.join(nameseg(s) for s in segs)


class Raw(bytes):
    """Already-encoded AML (a term or an object)."""


def term(value):
    """Encode a Python value as an AML data object / term argument."""
    if isinstance(value, Raw):
        return bytes(value)
    if isinstance(value, bool):
        value = int(value)
    if isinstance(value, int):
        if value == 0: return b'\x00'
        if value == 1: return b'\x01'
        if value == 0xFFFFFFFFFFFFFFFF: return b'\xFF'
        if value < 0x100: return b'\x0A' + bytes([value])
        if value < 0x10000: return b'\x0B' + struct.pack('<H', value)
        if value < 0x100000000: return b'\x0C' + struct.pack('<I', value)
        return b'\x0E' + struct.pack('<Q', value)
    if isinstance(value, str):
        return b'\x0D' + value.encode('ascii') + b'\x00'
    if isinstance(value, (list, tuple)):
        return package(list(value))
    raise TypeError(value)


def ref(path):
    """A name reference used as a term (e.g. \\VGIA, LCD0, \\_SB.NXBT)."""
    return Raw(namestring(path))


def eisaid(text):
    """EisaId("PNP0C0A") -> 32-bit compressed EISA id."""
    c = [ord(ch) - 0x40 for ch in text[:3]]
    value = (c[0] << 26) | (c[1] << 21) | (c[2] << 16) | int(text[3:], 16)
    return struct.unpack('<I', struct.pack('>I', value))[0]


Local = [Raw(bytes([0x60 + i])) for i in range(8)]
Arg = [Raw(bytes([0x68 + i])) for i in range(7)]
NULL_TARGET = b'\x00'


def target(t):
    return NULL_TARGET if t is None else term(t)


# ---- named objects and blocks ----
def name(path, value):
    return Raw(b'\x08' + namestring(path) + term(value))


def scope(path, body):
    content = namestring(path) + b''.join(body)
    return Raw(b'\x10' + pkg_length(len(content)) + content)


def device(path, body):
    content = namestring(path) + b''.join(body)
    return Raw(b'\x5B\x82' + pkg_length(len(content)) + content)


def thermal_zone(path, body):
    content = namestring(path) + b''.join(body)
    return Raw(b'\x5B\x85' + pkg_length(len(content)) + content)


def power_resource(path, system_level, order, body):
    content = namestring(path) + bytes([system_level]) + struct.pack('<H', order) + b''.join(body)
    return Raw(b'\x5B\x84' + pkg_length(len(content)) + content)


def method(path, args, body, serialized=False):
    content = namestring(path) + bytes([args | (8 if serialized else 0)]) + b''.join(body)
    return Raw(b'\x14' + pkg_length(len(content)) + content)


def package(elements):
    content = bytes([len(elements)]) + b''.join(term(e) for e in elements)
    return Raw(b'\x12' + pkg_length(len(content)) + content)


def buffer(data):
    content = term(len(data)) + bytes(data)
    return Raw(b'\x11' + pkg_length(len(content)) + content)


SPACES = {'SystemMemory': 0, 'SystemIO': 1, 'PCI_Config': 2, 'EmbeddedControl': 3, 'SystemCMOS': 5}
ACCESS = {'AnyAcc': 0, 'ByteAcc': 1, 'WordAcc': 2, 'DWordAcc': 3, 'QWordAcc': 4, 'BufferAcc': 5}


def operation_region(path, space, offset, length):
    return Raw(b'\x5B\x80' + namestring(path) + bytes([SPACES[space]]) + term(offset) + term(length))


def field(region, access, units, lock=False, update='Preserve'):
    """Field(region, access, NoLock|Lock, Preserve) {units}: units = [(name or None for reserved, bits), ...]"""
    flags = ACCESS[access] | (0x10 if lock else 0) | {'Preserve': 0, 'WriteAsOnes': 0x20, 'WriteAsZeros': 0x40}[update]
    content = namestring(region) + bytes([flags])
    for unit_name, bits in units:
        content += (nameseg(unit_name) if unit_name else b'\x00') + field_length(bits)
    return Raw(b'\x5B\x81' + pkg_length(len(content)) + content)


def index_field(index_name, data_name, access, units, lock=False):
    """IndexField(index, data, access, NoLock, Preserve) {units}; units as for field(), (None, bits) = Offset gap."""
    flags = ACCESS[access] | (0x10 if lock else 0)
    content = namestring(index_name) + namestring(data_name) + bytes([flags])
    for unit_name, bits in units:
        content += (nameseg(unit_name) if unit_name else b'\x00') + field_length(bits)
    return Raw(b'\x5B\x86' + pkg_length(len(content)) + content)


# ---- resource templates (ACPI 6.4.2 small resource data types) ----
def io_resource(minimum, maximum, alignment, length, decode16=True):
    return bytes([0x47, 1 if decode16 else 0]) + struct.pack('<HHBB', minimum, maximum, alignment, length)


def irq_resource(irqs, edge=True, active_low=False, shared=False):
    mask = 0
    for irq in irqs: mask |= 1 << irq
    return bytes([0x23]) + struct.pack('<H', mask) + bytes([(1 if edge else 0) | (8 if active_low else 0) | (0x10 if shared else 0)])


def register_resource(space, bit_width, bit_offset, address, access_size=0):
    """Generic Register Descriptor (ACPI 6.4.3.7): space 0 memory, 1 SystemIO, 0x7F FFixedHW."""
    return bytes([0x82, 0x0C, 0x00, space, bit_width, bit_offset, access_size]) + struct.pack('<Q', address)


def resource_template(*descriptors):
    data = b''.join(descriptors) + b'\x79\x00'   # end tag (checksum 0 = ignored)
    return buffer(data)


# ---- statements ----
def if_(predicate, body, else_body=None):
    content = term(predicate) + b''.join(body)
    out = b'\xA0' + pkg_length(len(content)) + content
    if else_body is not None:
        e = b''.join(else_body)
        out += b'\xA1' + pkg_length(len(e)) + e
    return Raw(out)


def while_(predicate, body):
    content = term(predicate) + b''.join(body)
    return Raw(b'\xA2' + pkg_length(len(content)) + content)


def return_(value=0):
    return Raw(b'\xA4' + term(value))


def store(value, destination):
    return Raw(b'\x70' + term(value) + term(destination))


def notify(path, value):
    return Raw(b'\x86' + namestring(path) + term(value))


def call(path, *args):
    return Raw(namestring(path) + b''.join(term(a) for a in args))


def sleep(milliseconds):
    return Raw(b'\x5B\x22' + term(milliseconds))


def _binary(opcode):
    def op(a, b, destination=None):
        return Raw(bytes([opcode]) + term(a) + term(b) + target(destination))
    return op


add = _binary(0x72); subtract = _binary(0x74); multiply = _binary(0x77)
shift_left = _binary(0x79); shift_right = _binary(0x7A)
and_ = _binary(0x7B); or_ = _binary(0x7D); xor = _binary(0x7F); mod = _binary(0x85)


def increment(destination):
    return Raw(b'\x75' + term(destination))


def l_and(a, b): return Raw(b'\x90' + term(a) + term(b))
def l_or(a, b): return Raw(b'\x91' + term(a) + term(b))
def l_not(a): return Raw(b'\x92' + term(a))
def l_equal(a, b): return Raw(b'\x93' + term(a) + term(b))
def l_greater(a, b): return Raw(b'\x94' + term(a) + term(b))
def l_less(a, b): return Raw(b'\x95' + term(a) + term(b))
def l_not_equal(a, b): return l_not(l_equal(a, b))
def deref_of(a): return Raw(b'\x83' + term(a))
def index(source, i, destination=None): return Raw(b'\x88' + term(source) + term(i) + target(destination))


# ---- the table ----
def definition_block(signature, oem_id, oem_table_id, body, revision=2, oem_revision=1):
    aml = b''.join(body)
    header = struct.pack('<4sIBB6s8sI4sI', signature.encode(), 36 + len(aml), revision, 0,
                         oem_id.encode().ljust(6, b' '), oem_table_id.encode().ljust(8, b' '), oem_revision, b'NXAS', 1)
    table = bytearray(header + aml)
    table[9] = (-sum(table)) & 0xFF
    return bytes(table)
