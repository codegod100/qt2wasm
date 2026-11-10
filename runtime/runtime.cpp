#include "qt2wasm/runtime.h"

#include "wasieventdispatcher.h"

#include <QCoreApplication>
#include <QEvent>

#ifdef QT2WASM_GUI
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#endif

#ifdef QT2WASM_GUI
// Defined by the generated fonts.cpp when the build bundles fonts.
extern "C" __attribute__((weak)) void qt2wasm_bundled_fonts(void (*add)(const unsigned char *, size_t));
#endif

namespace qt2wasm {

namespace {
int g_argc = 1;
char g_arg0[] = "qt2wasm";
char *g_argv[] = {g_arg0, nullptr};

#ifdef QT2WASM_GUI
// There is no filesystem or fontconfig inside a component. Fonts come from
// `qt2wasm build --font`, which compiles them into a byte table and defines
// qt2wasm_bundled_fonts(); fonts in a .qrc under :/qt2wasm/fonts also work.
void addFont(const unsigned char *data, size_t size) {
    QFontDatabase::addApplicationFontFromData(
        QByteArray::fromRawData(reinterpret_cast<const char *>(data), qsizetype(size)));
}

void registerBundledFonts() {
    if (qt2wasm_bundled_fonts)
        qt2wasm_bundled_fonts(&addFont);
    const QDir dir(QStringLiteral(":/qt2wasm/fonts"));
    for (const QString &f : dir.entryList(QDir::Files))
        QFontDatabase::addApplicationFont(dir.filePath(f));
}
#endif
}  // namespace

void ensureApplication() {
    if (QCoreApplication::instance())
        return;
    QCoreApplication::setEventDispatcher(new WasiEventDispatcher);
#ifdef QT2WASM_GUI
#ifdef QT2WASM_QPA_PLATFORM
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QT2WASM_QPA_PLATFORM);
#endif
    // No font directory exists in a component; point Qt at the resource root
    // so it does not warn, and register compiled-in fonts below.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", ":/");
    new QGuiApplication(g_argc, g_argv);
    registerBundledFonts();
#else
    new QCoreApplication(g_argc, g_argv);
#endif
}

void pump() {
    if (auto *d = QCoreApplication::eventDispatcher())
        d->processEvents(QEventLoop::AllEvents);
    // deleteLater() needs an explicit DeferredDelete pass outside exec().
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

}  // namespace qt2wasm
