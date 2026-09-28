from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "mgba"

def replace(rel, old, new, count=1):
    p = ROOT / rel
    with p.open("r", encoding="utf-8", newline="") as f:
        s = f.read()
    if s.count(old) != count:
        raise SystemExit(f'{rel}: expected {count} matches, found {s.count(old)}')
    with p.open("w", encoding="utf-8", newline="") as f:
        f.write(s.replace(old, new, count))

replace('include/mgba/internal/gba/io.h',
'''\tREG_JOYSTAT = 0x158,\n''',
'''\tREG_JOYSTAT = 0x158,\n\tREG_MGBA_KEYINPUT_XY = 0x15A,\n''')

replace('include/mgba/internal/gba/input.h',
'''\tGBA_KEY_R = 8,
\tGBA_KEY_L = 9,
\tGBA_KEY_MAX,''',
'''\tGBA_KEY_R = 8,
\tGBA_KEY_L = 9,
\tGBA_KEY_X = 10,
\tGBA_KEY_Y = 11,
\tGBA_KEY_MAX,''')

replace('src/gba/input.c',
'''\t\t"R",
\t\t"L"
''',
'''\t\t"R",
\t\t"L",
\t\t"X",
\t\t"Y"
''')

replace('include/mgba/internal/gba/serialize.h',
'''DECL_BITS(GBASerializedMiscFlags, KeyIRQKeys, 4, 11);''',
'''\tDECL_BITS(GBASerializedMiscFlags, KeyIRQKeys, 4, 13);''')

replace('src/gba/gba.c', '\tgba->keysLast = 0x400;', '\tgba->keysLast = 0x1000;', 3)
replace('src/gba/gba.c', '\tkeycnt &= 0x3FF;', '\tkeycnt &= 0x0FFF;')

replace('src/gba/io.c', '\tgba->memory.io[REG_KEYINPUT >> 1] = 0x3FF;', '\tgba->memory.io[REG_KEYINPUT >> 1] = 0x0FFF;')
replace('src/gba/io.c', '\tcase REG_KEYINPUT:\n\tcase REG_KEYCNT:', '\tcase REG_KEYINPUT:\n\tcase REG_MGBA_KEYINPUT_XY:\n\tcase REG_KEYCNT:')
replace('src/gba/io.c', '\tcase REG_KEYCNT:\n\t\tvalue &= 0xC3FF;\n\t\tif (gba->keysLast < 0x400) {', '\tcase REG_KEYCNT:\n\t\tvalue &= 0xCFFF;\n\t\tif (gba->keysLast < 0x1000) {')
replace('src/gba/io.c', '\tcase REG_KEYINPUT: {', '''\tcase REG_MGBA_KEYINPUT_XY:\n\t\treturn GBAIORead(gba, REG_KEYINPUT);\n\tcase REG_KEYINPUT: {''')

replace('src/gba/io.c',
'''\tcase 0x142:
\tcase 0x15A:
\tcase 0x206:''',
'''\tcase 0x142:
\tcase 0x206:''')
replace('src/gba/io.c', '\t\t\t\tinput &= 0x30F;', '\t\t\t\tinput &= 0x0F0F;')
replace('src/gba/io.c', '\t\t\tgba->memory.io[address >> 1] = 0x3FF ^ input;', '\t\t\tgba->memory.io[address >> 1] = 0x0FFF ^ input;')

replace('src/platform/qt/InputController.cpp',
'''void InputController::bindKey(uint32_t type, int key, GBAKey gbaKey) {
	return mInputBindKey(&m_inputMap, type, key, gbaKey);
}''',
'''void InputController::bindKey(uint32_t type, int key, GBAKey gbaKey) {
	// Keep one physical key/button mapped to at most one GBA input.
	for (int i = 0; i < GBA_KEY_MAX; ++i) {
		if (i != gbaKey && mInputQueryBinding(&m_inputMap, type, i) == key) {
			mInputUnbindKey(&m_inputMap, type, i);
		}
	}
	return mInputBindKey(&m_inputMap, type, key, gbaKey);
}''')

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

# X/Y are added directly to the fixed-size arrays, leaving legacy default
# KeyList aggregate initializers untouched and therefore preserving defaults.
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

replace('src/platform/qt/IOViewer.cpp',
'''\t\t{ tr("R"), 8 },
\t\t{ tr("L"), 9 },''',
'''\t\t{ tr("R"), 8 },
\t\t{ tr("L"), 9 },
\t\t{ tr("X"), 10 },
\t\t{ tr("Y"), 11 },''', 2)

# Upstream mGBA's Windows deploy script filters ntldd/gdb output for
# "mingw", which misses the /ucrt64 paths used by this MSYS2 build.
replace('tools/deploy-win.sh',
'''grep -i mingw''',
'''grep -Ei "mingw|ucrt64"''', 2)

replace('src/platform/qt/Window.cpp',
'''\tm_actions.addHeldAction(tr("Autofire B"), "autofireB", [this](bool held) {
\t\tif (m_controller) {
\t\t\tm_controller->setAutofire(GBA_KEY_B, held);
\t\t}
\t}, "autofire");''',
'''\tm_actions.addHeldAction(tr("Autofire B"), "autofireB", [this](bool held) {
\t\tif (m_controller) {
\t\t\tm_controller->setAutofire(GBA_KEY_B, held);
\t\t}
\t}, "autofire");
\tm_actions.addHeldAction(tr("Autofire X"), "autofireX", [this](bool held) {
\t\tif (m_controller) {
\t\t\tm_controller->setAutofire(GBA_KEY_X, held);
\t\t}
\t}, "autofire");
\tm_actions.addHeldAction(tr("Autofire Y"), "autofireY", [this](bool held) {
\t\tif (m_controller) {
\t\t\tm_controller->setAutofire(GBA_KEY_Y, held);
\t\t}
\t}, "autofire");''')

# Non-Latin keyboard layouts (Greek, Russian, Arabic, Hebrew...): Qt's
# QKeyEvent::key() returns the typed character (e.g. Greek chi instead of X),
# so the default Latin keyboard bindings never match. On Windows the virtual-key
# code for letter/digit keys stays Latin regardless of layout, so use it when
# key() is a character beyond Latin-1. Latin layouts (incl. AZERTY/QWERTZ) keep
# their existing behavior.
(ROOT / 'src/platform/qt/LayoutKey.h').write_text(
'''#pragma once

#include <QKeyEvent>

namespace QGBA {

// Returns the Qt key for an event, mapped back to the Latin key on the same
// physical key when the active layout produces a non-Latin character.
inline int layoutIndependentKey(const QKeyEvent* event) {
	int key = event->key();
#ifdef Q_OS_WIN
	if (key >= 0x100 && key < 0x01000000) {
		quint32 vk = event->nativeVirtualKey();
		if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
			return static_cast<int>(vk); // Qt::Key_A..Key_Z / Key_0..Key_9 equal their ASCII values
		}
	}
#endif
	return key;
}

}
''', encoding='utf-8', newline='\n')

replace('src/platform/qt/Window.cpp',
'''\tGBAKey key = m_inputController.mapKeyboard(event->key());''',
'''\tGBAKey key = m_inputController.mapKeyboard(layoutIndependentKey(event));''', 2)
replace('src/platform/qt/Window.cpp',
'''#include "Window.h"\n''',
'''#include "Window.h"\n#include "LayoutKey.h"\n''')

replace('src/platform/qt/KeyEditor.cpp',
'''setValue(event->key() | (m_key &''',
'''setValue(layoutIndependentKey(event) | (m_key &''')
replace('src/platform/qt/KeyEditor.cpp',
'''\t\t\tsetValue(event->key());\n\t\t}\n\t}\n\tevent->accept();''',
'''\t\t\tsetValue(layoutIndependentKey(event));\n\t\t}\n\t}\n\tevent->accept();''')
replace('src/platform/qt/KeyEditor.cpp',
'''#include "KeyEditor.h"\n''',
'''#include "KeyEditor.h"\n#include "LayoutKey.h"\n''')

replace('src/platform/qt/ShortcutController.cpp',
'''\t\tint key = keyEvent->key();\n\t\tif (!isModifierKey(key)) {''',
'''\t\tint key = layoutIndependentKey(keyEvent);\n\t\tif (!isModifierKey(key)) {''')
replace('src/platform/qt/ShortcutController.cpp',
'''#include "ShortcutController.h"\n''',
'''#include "ShortcutController.h"\n#include "LayoutKey.h"\n''')

print('X/Y patch applied successfully')
