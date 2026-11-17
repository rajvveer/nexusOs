"""International text in Aurora: Notes shows a file in 19 scripts; aurora_text.c finds each script's Noto font through
fontconfig (desk_install.py fonts + the prebuilt cache) and HarfBuzz shapes it.

python -I tests/test_aurora_fonts.py [WORK] [--disk DESKTOP64.img] [--tcg]

One boot of the desktop disk (fonts installed) through a qcow2 overlay; /home/nexus/notes.txt is staged with one line
per script. Checked: the AURORA TEXT FONT lines (which font file each fallback came from) and a screendump.
"""
from pathlib import Path
import json
import re
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import desk_vm  # noqa: E402
import aurora_vm  # noqa: E402

OUT = ROOT / 'arch/x86_64/out'
args = [a for a in sys.argv[1:] if not a.startswith('--')]
WORK = Path(args[0]) if args else nexus_paths.runs('fonts')
DISK = Path(sys.argv[sys.argv.index('--disk') + 1]) if '--disk' in sys.argv else nexus_paths.test_disk()
ACCEL = 'tcg' if '--tcg' in sys.argv else 'whpx'
WORK.mkdir(parents=True, exist_ok=True)
LINES = ['English: Hello, world', '中文: 你好，世界', '日本語: こんにちは世界', '한국어: 안녕하세요', 'हिन्दी: नमस्ते दुनिया', 'বাংলা: হ্যালো বিশ্ব',
         'தமிழ்: வணக்கம்', 'తెలుగు: హలో ప్రపంచం', 'ગુજરાતી: નમસ્તે', 'ਪੰਜਾਬੀ: ਸਤ ਸ੍ਰੀ ਅਕਾਲ', 'ไทย: สวัสดีชาวโลก', 'Ελληνικά: Γειά σου κόσμε',
         'Русский: Привет, мир', 'Հայերեն: Բարեւ աշխարհ', 'ქართული: გამარჯობა', 'አማርኛ: ሰላም ልዑል', 'עברית: שלום עולם', 'العربية: مرحبا بالعالم',
         'Emoji: 😀 🎉 👍 🌍']
EXPECTED = {'CJK': 'NotoSansCJK', 'Devanagari': 'NotoSansDevanagari', 'Bengali': 'NotoSansBengali', 'Tamil': 'NotoSansTamil',
            'Telugu': 'NotoSansTelugu', 'Gujarati': 'NotoSansGujarati', 'Gurmukhi': 'NotoSansGurmukhi', 'Thai': 'NotoSansThai',
            'Armenian': 'NotoSansArmenian', 'Georgian': 'NotoSansGeorgian', 'Ethiopic': 'NotoSansEthiopic', 'Hebrew': 'NotoSansHebrew',
            'Arabic': 'NotoSansArabic', 'Emoji': 'NotoColorEmoji'}
aurora_vm.stage(DISK, {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'nexus/desktop.args': '--aurora\n',
                       'home/nexus/notes.txt': '\n'.join(LINES) + '\n'})
results = {}
vm = desk_vm.DeskVm(WORK, DISK, heads=1, accel=ACCEL, smp=2, usb=True, vnc_heads=[])
try:
    vm.wait(r'AURORA READY', seconds=300)
    mark = len(vm.text()); vm.tap('ctrl', 'spc'); time.sleep(1); vm.type('notes'); time.sleep(.5); vm.tap('ret')
    vm.wait(r'AURORA WINDOW app=nexus.notes', mark, seconds=120); time.sleep(6)
    fonts = re.findall(r'AURORA TEXT FONT (\S+) index=(\d+) for U\+([0-9A-F]+)', vm.text()[mark:])
    loaded = ' '.join(path for path, _, _ in fonts)
    missing = [script for script, name in EXPECTED.items() if name not in loaded]
    vm.shot('notes-scripts')
    assert not missing, ('no fallback font for', missing, fonts)
    results['fallback_fonts'] = '%d Noto fonts loaded through fontconfig for %s' % (len(fonts), ', '.join(EXPECTED))
    assert not any(bad in vm.text() for bad in ('KERNEL_FAULT', 'USER_FAULT'))
finally:
    vm.close()
    aurora_vm.module('fonts_fatput', ROOT / 'userland/fatput.py').edit(['fatput', str(DISK), 'rm', 'home/nexus/notes.txt'])
(WORK / 'results.json').write_text(json.dumps(results, indent=1))
print('PASS Aurora fonts:', '; '.join(f'{k}: {v}' for k, v in results.items()), flush=True)
