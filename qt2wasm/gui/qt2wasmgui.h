// The qt2wasmgui QPA platform plugin: a QtGui program as a latha:gui
// component (wit/gui.wit). Linked statically into `qt2wasm build --gui`
// components; see qt2wasmgui.cpp.
#pragma once

#include <qpa/qplatformintegrationplugin.h>

class Qt2WasmGuiIntegrationPlugin : public QPlatformIntegrationPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QPA.QPlatformIntegrationFactoryInterface.5.3" FILE "qt2wasmgui.json")
public:
    QPlatformIntegration *create(const QString &key, const QStringList &params) override;
};
