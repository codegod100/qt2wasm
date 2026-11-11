// qt2wasm/gui.h: a QtGui program as a GUI component (`qt2wasm build --gui`).
//
// The component exports latha:gui/app (qt2wasm gui/wit/gui.wit): a host such
// as perry-gui opens a native window, sends it input and shows the frames the
// app paints. Inside, the `qt2wasmgui` platform plugin turns that into
// ordinary Qt: QWindow / QRasterWindow / QPainter, key, mouse and wheel
// events, the clipboard, cursors, the dark / light color scheme, HiDPI.
//
// Write the start-up as a function instead of main():
//
//     #include <qt2wasm/gui.h>
//     int qt2wasm_gui_main(int argc, char **argv) {
//         auto *w = new ClockWindow;   // the QGuiApplication already exists
//         w->resize(400, 400);         // the first top-level fills the host window
//         w->show();
//         return 0;                    // never call exec(): the host runs the loop
//     }
//
// The clipboard: qt6-wasi is built without Qt's clipboard feature (no
// QClipboard), so use the functions below; they talk to the host directly.
// When a Qt build has it, QGuiApplication::clipboard() works as well.
//
// A non-zero return makes the host exit with an error. QGuiApplication::quit()
// (or exit(code)) closes the host window. Timers, queued connections and
// QWindow::requestUpdate() work: the host calls back when they are due.
#pragma once

#include <QString>

extern "C" int qt2wasm_gui_main(int argc, char **argv);

namespace qt2wasm::gui {
QString clipboardText();
void setClipboardText(const QString &text);
/// The X11 primary selection (middle-click paste); "" where the host has none.
QString primaryText();
void setPrimaryText(const QString &text);
}  // namespace qt2wasm::gui
