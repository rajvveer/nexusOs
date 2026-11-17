r"""Test SSDT for NexusOS's ACPI device drivers on QEMU, which has no battery, lid or thermal hardware.

The table describes an AC adapter (ACPI0003), a battery (PNP0C0A, _BIX/_BIF/_BST), a lid (PNP0C0D), control-method
power and sleep buttons (PNP0C0C/PNP0C0E), a thermal zone (_TMP/_CRT/_HOT/_PSV/_AC0 with a fan power resource,
_TC1/_TC2/_TSP) and an ACPI video output with a backlight (_BCL/_BCM/_BQC). Their methods read a 16-byte state block
in CMOS RAM (bytes 0x60-0x6F, an IndexField on ports 0x70/0x71) that the test rewrites at run time through the QEMU
monitor (`o` port writes). Events come through a GPE block device (ACPI0006, ACPI 6.5 section 9.10) whose status and
enable registers are QEMU pc-testdev's scratch register (ports 0xE0/0xE1) and whose interrupt is ISA IRQ 3, which the
test pulses through pc-testdev's IRQ-line ports (0x2003): uACPI dispatches the GPE to the block's _E00 method, which
clears the status byte (a plain register here, not write-1-to-clear) and Notify()s each device whose state changed
since the last snapshot - so everything below the interrupt is the kernel's real ACPI event path.

State block (CMOS 0x60.., little-endian):
  0 FLAG bit0 AC online, bit1 lid open, bit2 battery present     1 SLPC sleep-button presses
  2 BRUP brightness-up hotkey presses                           3 BSTA _BST state (1 discharging, 2 charging, 4 critical)
  4 RCAP remaining capacity, mWh (32 bit)                        8 RATE present rate, mW (16 bit)
 10 TEMP temperature, tenths of K (16 bit)                      12 BRDN brightness-down hotkey presses
 13 PWRC control-method power-button presses                    14 INFO battery information generation
 15 SEQN free (forces a change)                               16 PPCV processors' _PPC (first usable P-state)
 17 DSWV the lid's last _DSW: Arg0 | Arg1 << 4 (0x31 = armed for S3), written by the kernel's wake arming
Processors (\_SB.CPUS.C000.., one per CPU: build(cpus)): _PSS 2400/1800/1200 MHz whose _PCT control/status register is
pc-testdev's scratch byte 0xE2 (the test reads the state the kernel chose with `i /b 0xe2`), _PPC = PPCV (a change is
Notify(0x80)'d by the GPE method), _CST: C1 (FFixedHW class 1, i.e. HLT) and C2 through a SystemIO read of 0xE3.
"""
import struct
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from aml import *  # noqa: E402,F401,F403

CMOS_BASE = 0x60
DESIGN_CAPACITY, FULL_CAPACITY, DESIGN_VOLTAGE = 50000, 48000, 11100
TRIPS = {'critical': 3732, 'hot': 3632, 'passive': 3532, 'active0': 3432}   # 100, 90, 80, 70 C
GPE_PORT, GPE_IRQ = 0xE0, 3


def state_bytes(ac=True, lid=True, battery=True, sleep_presses=0, brightness_up=0, battery_state=2, remaining=40000,
                rate=5000, temperature=3082, brightness_down=0, power_presses=0, info=0, sequence=0, ppc=0):
    flags = (1 if ac else 0) | (2 if lid else 0) | (4 if battery else 0)
    return struct.pack('<BBBBIHHBBBBB', flags, sleep_presses & 255, brightness_up & 255, battery_state, remaining, rate, temperature,
                       brightness_down & 255, power_presses & 255, info & 255, sequence & 255, ppc & 255)


def state_commands(data):
    """QEMU monitor (HMP) commands that store the state block in CMOS."""
    out = []
    for i, byte in enumerate(data):
        out += [f'o /b 0x70 {CMOS_BASE + i:#x}', f'o /b 0x71 {byte:#x}']
    return out


def event_commands():
    """Set the GPE block's status bit 0 and pulse ISA IRQ 3 (edge)."""
    return [f'o /b {GPE_PORT:#x} 1', f'o /b {0x2000 + GPE_IRQ:#x} 1', f'o /b {0x2000 + GPE_IRQ:#x} 0']


PSTATES = [(2400, 15000, 0), (1800, 9000, 1), (1200, 5000, 2)]   # MHz, mW, control/status value
PSTATE_PORT, C2_PORT = 0xE2, 0xE3


def build(cpus=1):
    cmos = operation_region('\\NXCM', 'SystemIO', 0x70, 2)
    cmos_index = field('\\NXCM', 'ByteAcc', [('NXCI', 8), ('NXCD', 8)])
    state = index_field('\\NXCI', '\\NXCD', 'ByteAcc', [(None, CMOS_BASE * 8), ('FLAG', 8), ('SLPC', 8), ('BRUP', 8), ('BSTA', 8),
                                                      ('RCAP', 32), ('RATE', 16), ('TEMP', 16), ('BRDN', 8), ('PWRC', 8), ('INFO', 8), ('SEQN', 8), ('PPCV', 8), ('DSWV', 8)])
    ac = device('NXAC', [name('_HID', 'ACPI0003'), name('_PCL', package([ref('\\_SB')])),
                         method('_PSR', 0, [return_(and_(ref('\\FLAG'), 1))]),
                         method('_STA', 0, [return_(0x0F)])])
    strings = ['NX-BAT-01', 'SN0001', 'LION', 'NexusOS Test Cells']
    bix = [1, 0, DESIGN_CAPACITY, FULL_CAPACITY, 1, DESIGN_VOLTAGE, 4800, 960, 42, 98000, 0xFFFFFFFF, 0xFFFFFFFF,
           0xFFFFFFFF, 0xFFFFFFFF, 100, 100] + strings + [0]
    bif = [0, DESIGN_CAPACITY, FULL_CAPACITY, 1, DESIGN_VOLTAGE, 4800, 960, 100, 100] + strings
    battery = device('NXBT', [name('_HID', eisaid('PNP0C0A')), name('_UID', 1), name('_PCL', package([ref('\\_SB')])),
                              method('_STA', 0, [if_(and_(ref('\\FLAG'), 4), [return_(0x1F)]), return_(0x0F)]),
                              method('_BIX', 0, [return_(package(bix))]),
                              method('_BIF', 0, [return_(package(bif))]),
                              method('_BST', 0, [store(package([0, 0, 0, 0]), Local[0]),
                                                 store(ref('\\BSTA'), index(Local[0], 0)), store(ref('\\RATE'), index(Local[0], 1)),
                                                 store(ref('\\RCAP'), index(Local[0], 2)), store(11800, index(Local[0], 3)),
                                                 return_(Local[0])])])
    lid = device('NXLD', [name('_HID', eisaid('PNP0C0D')), method('_LID', 0, [return_(shift_right(and_(ref('\\FLAG'), 2), 1))]),
                          name('_PRW', package([package([ref('\\_SB.NXGB'), 0]), 3])),   # wake through the GPE block, down to S3
                          method('_DSW', 3, [store(or_(Arg[0], shift_left(Arg[1], 4)), ref('\\DSWV'))])])
    sleep_button = device('NXSB', [name('_HID', eisaid('PNP0C0E'))])
    power_button = device('NXPB', [name('_HID', eisaid('PNP0C0C'))])
    levels = [100, 40, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
    video = device('NXGF', [name('_ADR', 0x00020000), method('_DOS', 1, []),
                            device('LCD0', [name('_ADR', 0x400), name('BRTL', 70),
                                            method('_BCL', 0, [return_(package(levels))]),
                                            method('_BCM', 1, [store(Arg[0], ref('BRTL'))]),
                                            method('_BQC', 0, [return_(ref('BRTL'))])])])
    fan_power = power_resource('NXFP', 0, 0, [name('ONST', 0), method('_STA', 0, [return_(ref('ONST'))]),
                                              method('_ON', 0, [store(1, ref('ONST'))]), method('_OFF', 0, [store(0, ref('ONST'))])])
    fan = device('NXFN', [name('_HID', eisaid('PNP0C0B')), name('_PR0', package([ref('\\_SB.NXFP')]))])
    snapshots = [('PFLG', 'FLAG'), ('PSLP', 'SLPC'), ('PBUP', 'BRUP'), ('PBST', 'BSTA'), ('PCAP', 'RCAP'), ('PTMP', 'TEMP'),
                 ('PBDN', 'BRDN'), ('PPWR', 'PWRC'), ('PINF', 'INFO'), ('PPPC', 'PPCV')]
    snapshot = [store(ref('\\' + live), ref(saved)) for saved, live in snapshots]
    gpe_block = device('NXGB', [
        name('_HID', 'ACPI0006'), name('_UID', 1),
        name('_CRS', resource_template(io_resource(GPE_PORT, GPE_PORT, 1, 2), irq_resource([GPE_IRQ], edge=True))),
        operation_region('GBRG', 'SystemIO', GPE_PORT, 2), field('GBRG', 'ByteAcc', [('GSTS', 8), ('GENB', 8)]),
        *[name(saved, 0) for saved, _ in snapshots],
        method('_INI', 0, snapshot),
        method('_E00', 0, [
            store(0, ref('GSTS')),   # pc-testdev's register is not write-1-to-clear: the method acknowledges the event
            store(xor(ref('\\FLAG'), ref('PFLG')), Local[0]),
            if_(and_(Local[0], 1), [notify('\\_SB.NXAC', 0x80), notify('\\_SB.NXBT', 0x80)]),
            if_(and_(Local[0], 2), [notify('\\_SB.NXLD', 0x80)]),
            if_(and_(Local[0], 4), [notify('\\_SB.NXBT', 0x81)]),
            if_(l_not_equal(ref('\\SLPC'), ref('PSLP')), [notify('\\_SB.NXSB', 0x80)]),
            if_(l_not_equal(ref('\\PWRC'), ref('PPWR')), [notify('\\_SB.NXPB', 0x80)]),
            if_(l_not_equal(ref('\\BRUP'), ref('PBUP')), [notify('\\_SB.NXGF.LCD0', 0x86)]),
            if_(l_not_equal(ref('\\BRDN'), ref('PBDN')), [notify('\\_SB.NXGF.LCD0', 0x87)]),
            if_(l_not_equal(ref('\\INFO'), ref('PINF')), [notify('\\_SB.NXBT', 0x81)]),
            if_(l_or(l_not_equal(ref('\\BSTA'), ref('PBST')), l_not_equal(ref('\\RCAP'), ref('PCAP'))), [notify('\\_SB.NXBT', 0x80)]),
            if_(l_not_equal(ref('\\TEMP'), ref('PTMP')), [notify('\\_TZ.NXTZ', 0x80)]),
            if_(l_not_equal(ref('\\PPCV'), ref('PPPC')), [notify(f'\\_SB.CPUS.C{cpu:03X}', 0x80) for cpu in range(cpus)]),
            *snapshot], serialized=True)])
    zone = thermal_zone('NXTZ', [method('_TMP', 0, [return_(ref('\\TEMP'))]),
                                 method('_CRT', 0, [return_(TRIPS['critical'])]), method('_HOT', 0, [return_(TRIPS['hot'])]),
                                 method('_PSV', 0, [return_(TRIPS['passive'])]), method('_AC0', 0, [return_(TRIPS['active0'])]),
                                 name('_AL0', package([ref('\\_SB.NXFN')])),
                                 name('_TC1', 2), name('_TC2', 3), name('_TSP', 20), name('_TZP', 0)])
    pct = package([resource_template(register_resource(1, 8, 0, PSTATE_PORT)), resource_template(register_resource(1, 8, 0, PSTATE_PORT))])
    pss = package([package([mhz, mw, 10, 10, value, value]) for mhz, mw, value in PSTATES])
    cst = package([2, package([resource_template(register_resource(0x7F, 1, 1, 0)), 1, 1, 1000]),
                   package([resource_template(register_resource(1, 8, 0, C2_PORT)), 2, 50, 300])])
    processors = [scope(f'\\_SB.CPUS.C{cpu:03X}', [name('_PCT', pct), name('_PSS', pss), method('_PPC', 0, [return_(ref('\\PPCV'))]),
                                                  name('_CST', cst)]) for cpu in range(cpus)]
    return definition_block('SSDT', 'NEXUS', 'NXPOWER', [
        cmos, cmos_index, state,
        scope('\\_SB', [ac, battery, lid, sleep_button, power_button, video, fan_power, fan, gpe_block]),
        scope('\\_TZ', [zone]), *processors])


if __name__ == '__main__':
    out = Path(sys.argv[1] if len(sys.argv) > 1 else 'nexus-test-ssdt.aml')
    out.write_bytes(build())
    print(out, len(out.read_bytes()), 'bytes')
