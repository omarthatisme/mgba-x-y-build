"""Adds Direct3D 11 and Direct3D 9 display drivers to the mGBA Qt frontend.

Runs after apply_xyz.py. The new sources live in the repository's d3d/
directory and are copied into the upstream tree; everything else is a small,
exact-match text edit (the build fails loudly if upstream ever changes).
"""
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO / "mgba"
QT = ROOT / "src" / "platform" / "qt"


def replace(rel, old, new, count=1):
    p = ROOT / rel
    with p.open("r", encoding="utf-8", newline="") as f:
        s = f.read()
    if s.count(old) != count:
        raise SystemExit(f'{rel}: expected {count} matches, found {s.count(old)}')
    with p.open("w", encoding="utf-8", newline="") as f:
        f.write(s.replace(old, new, count))


# New sources (Qt-free renderer + Qt display)
for name in ("D3DRenderer.h", "D3DRenderer.cpp", "DisplayD3D.h", "DisplayD3D.cpp"):
    shutil.copyfile(REPO / "d3d" / name, QT / name)

# Driver ids. 0-2 are upstream's and must keep their values: they are stored
# in users' qt.ini.
replace('src/platform/qt/Display.h',
'''\t\tOPENGL1 = 2,
''',
'''\t\tOPENGL1 = 2,
\t\tDIRECT3D11 = 3,
\t\tDIRECT3D9 = 4,
''')

# Display factory
replace('src/platform/qt/Display.cpp',
'''#include "DisplayGL.h"
''',
'''#include "DisplayD3D.h"
#include "DisplayGL.h"
''')
replace('src/platform/qt/Display.cpp',
'''\tcase Driver::QT:
\t\treturn new DisplayQt(parent);
''',
'''#ifdef BUILD_D3D
\tcase Driver::DIRECT3D11:
\t\tif (!DisplayD3D::supports(D3DRenderer::Backend::D3D11)) {
\t\t\treturn nullptr;
\t\t}
\t\treturn new DisplayD3D(D3DRenderer::Backend::D3D11, parent);

\tcase Driver::DIRECT3D9:
\t\tif (!DisplayD3D::supports(D3DRenderer::Backend::D3D9)) {
\t\t\treturn nullptr;
\t\t}
\t\treturn new DisplayD3D(D3DRenderer::Backend::D3D9, parent);
#endif

\tcase Driver::QT:
\t\treturn new DisplayQt(parent);
''')

# Settings > display driver list
replace('src/platform/qt/SettingsView.cpp',
'''\tApplicationUpdater* updater = GBAApp::app()->updater();
\tm_ui.currentChannel->setText(''',
'''#ifdef BUILD_D3D
\tm_ui.displayDriver->addItem(tr("Direct3D 11 (low latency)"), static_cast<int>(Display::Driver::DIRECT3D11));
\tif (!displayDriver.isNull() && displayDriver.toInt() == static_cast<int>(Display::Driver::DIRECT3D11)) {
\t\tm_ui.displayDriver->setCurrentIndex(m_ui.displayDriver->count() - 1);
\t}

\tm_ui.displayDriver->addItem(tr("Direct3D 9"), static_cast<int>(Display::Driver::DIRECT3D9));
\tif (!displayDriver.isNull() && displayDriver.toInt() == static_cast<int>(Display::Driver::DIRECT3D9)) {
\t\tm_ui.displayDriver->setCurrentIndex(m_ui.displayDriver->count() - 1);
\t}
#endif

\tApplicationUpdater* updater = GBAApp::app()->updater();
\tm_ui.currentChannel->setText(''')

# OSD: lets displays that can't paint straight onto their surface skip the
# overlay when there is nothing to show.
replace('src/platform/qt/MessagePainter.h',
'''\tvoid paint(QPainter* painter);
''',
'''\tvoid paint(QPainter* painter);
\tbool hasContent() const { return m_drawFrameCounter || !m_message.text().isEmpty(); }
''')

# Build system
replace('src/platform/qt/CMakeLists.txt',
'''\tDisplay.cpp
\tDisplayGL.cpp
\tDisplayQt.cpp
''',
'''\tD3DRenderer.cpp
\tDisplay.cpp
\tDisplayD3D.cpp
\tDisplayGL.cpp
\tDisplayQt.cpp
''')

# BUILD_D3D is defined (as a real -D, so moc sees it too) for Windows builds
# only; on other platforms the new files compile to nothing.
replace('src/platform/qt/CMakeLists.txt',
'''\ttarget_link_libraries(${BINARY_NAME}-qt dwmapi)
''',
'''\ttarget_link_libraries(${BINARY_NAME}-qt dwmapi)
\ttarget_compile_definitions(${BINARY_NAME}-qt PRIVATE BUILD_D3D)
''')

print('Direct3D display patch applied successfully')
