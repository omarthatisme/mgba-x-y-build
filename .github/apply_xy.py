from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def replace(rel, old, new, count=1):
    p = ROOT / rel
    s = p.read_text()
    if s.count(old) != count:
        raise SystemExit(f'{rel}: expected {count} matches, found {s.count(old)}')
    p.write_text(s.replace(old, new, count))

replace('include/mgba/internal/gba/input.h',
'''\tGBA_KEY_R = 8,
\tGBA_KEY_L = 9,
\tGBA_KEY_MAX,''',
'''\tGBA_KEY_R = 8,
\tGBA_KEY_L = 9,
\tGBA_KEY_X = 10,
\tGBA_KEY_Y = 11,
\tGBA_KEY_MAX,''')

replace('include/mgba/internal/gba/serialize.h',
'''DECL_BITS(GBASerializedMiscFlags, KeyIRQKeys, 4, 11);''',
'''DECL_BITS(GBASerializedMiscFlags, KeyIRQKeys, 4, 13);''')

replace('src/gba/input.c',
'''\t\t"R",
\t\t"L"
''',
'''\t\t"R",
\t\t"L",
\t\t"X",
\t\t"Y"
''')

replace('src/gba/gba.c', '\tgba->keysLast = 0x400;', '\tgba->keysLast = 0x1000;', 2)
replace('src/gba/gba.c', '\tkeycnt &= 0x3FF;', '\tkeycnt &= 0x0FFF;')
replace('src/gba/gba.c', '\t\tgba->keysLast = 0x400;', '\t\tgba->keysLast = 0x1000;')

replace('src/gba/io.c', '\tgba->memory.io[REG_KEYINPUT >> 1] = 0x3FF;', '\tgba->memory.io[REG_KEYINPUT >> 1] = 0x0FFF;')
replace('src/gba/io.c', '\tcase REG_KEYCNT:\n\t\tvalue &= 0xC3FF;\n\t\tif (gba->keysLast < 0x400) {', '\tcase REG_KEYCNT:\n\t\tvalue &= 0xCFFF;\n\t\tif (gba->keysLast < 0x1000) {')
replace('src/gba/io.c', '\t\t\t\tinput &= 0x30F;', '\t\t\t\tinput &= 0xF30F;')
replace('src/gba/io.c', '\t\t\tgba->memory.io[address >> 1] = 0x3FF ^ input;', '\t\t\tgba->memory.io[address >> 1] = 0x0FFF ^ input;')

replace('src/platform/qt/GBAKeyEditor.h',
'''\tKeyEditor* m_keyL;
\tKeyEditor* m_keyR;''',
'''\tKeyEditor* m_keyL;
\tKeyEditor* m_keyR;
\tKeyEditor* m_keyX;
\tKeyEditor* m_keyY;''')
replace('src/platform/qt/GBAKeyEditor.cpp',
'''\tm_keyL = new KeyEditor(this);
\tm_keyR = new KeyEditor(this);''',
'''\tm_keyL = new KeyEditor(this);
\tm_keyR = new KeyEditor(this);
\tm_keyX = new KeyEditor(this);
\tm_keyY = new KeyEditor(this);''')
replace('src/platform/qt/GBAKeyEditor.cpp',
'''\t\tm_keyL,
\t\tm_keyR
''',
'''\t\tm_keyL,
\t\tm_keyR,
\t\tm_keyX,
\t\tm_keyY
''')
replace('src/platform/qt/GBAKeyEditor.cpp',
'''\tsetLocation(m_keyL, 0.1, 0.1);
\tsetLocation(m_keyR, 0.9, 0.1);''',
'''\tsetLocation(m_keyL, 0.1, 0.1);
\tsetLocation(m_keyR, 0.9, 0.1);
\tsetLocation(m_keyX, 0.1, 0.18);
\tsetLocation(m_keyY, 0.9, 0.18);''')
replace('src/platform/qt/GBAKeyEditor.cpp',
'''\tbindKey(m_keyL, GBA_KEY_L);
\tbindKey(m_keyR, GBA_KEY_R);''',
'''\tbindKey(m_keyL, GBA_KEY_L);
\tbindKey(m_keyR, GBA_KEY_R);
\tbindKey(m_keyX, GBA_KEY_X);
\tbindKey(m_keyY, GBA_KEY_Y);''')
replace('src/platform/qt/GBAKeyEditor.cpp',
'''\tlookupBinding(map, m_keyL, GBA_KEY_L);
\tlookupBinding(map, m_keyR, GBA_KEY_R);''',
'''\tlookupBinding(map, m_keyL, GBA_KEY_L);
\tlookupBinding(map, m_keyR, GBA_KEY_R);
\tlookupBinding(map, m_keyX, GBA_KEY_X);
\tlookupBinding(map, m_keyY, GBA_KEY_Y);''')
replace('src/platform/qt/GBAKeyEditor.cpp',
'''\tcase GBA_KEY_L:
\t\treturn m_keyL;
\tcase GBA_KEY_R:
\t\treturn m_keyR;''',
'''\tcase GBA_KEY_L:
\t\treturn m_keyL;
\tcase GBA_KEY_R:
\t\treturn m_keyR;
\tcase GBA_KEY_X:
\t\treturn m_keyX;
\tcase GBA_KEY_Y:
\t\treturn m_keyY;''')

replace('src/platform/qt/InputProfile.cpp',
'''\t\tkeys.keyR,
\t\tkeys.keyL,
\t}''',
'''\t\tkeys.keyR,
\t\tkeys.keyL,
\t\t-1,
\t\t-1,
\t}''')
replace('src/platform/qt/InputProfile.cpp',
'''\t\taxes.keyR,
\t\taxes.keyL,
\t}''',
'''\t\taxes.keyR,
\t\taxes.keyL,
\t\t{ GamepadAxisEvent::Direction::NEUTRAL, -1 },
\t\t{ GamepadAxisEvent::Direction::NEUTRAL, -1 },
\t}''')

print('X/Y patch applied successfully')
