// SPDX-License-Identifier: GPL-2.0-or-later
#include "citron/macos/native_toolbar.h"
#include <cstdint>
#include <QAction>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#import <AppKit/AppKit.h>

extern "C" void* citrosis_toolbar_create(void*, void (*)(void*, int32_t));
extern "C" void citrosis_toolbar_update(void*, int32_t, bool, bool, const char*);
extern "C" void citrosis_toolbar_release(void*);
namespace {
class NativeToolbar final : public QWidget {
public:
    NativeToolbar(QWidget* parent, const std::array<QAction*, 6>& actions_)
        : QWidget{parent}, actions{actions_} {
        setFixedHeight(64);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        host = citrosis_toolbar_create(this, [](void* context, int32_t id) {
            auto* toolbar = static_cast<NativeToolbar*>(context);
            if (id >= 0 && id < 6 && toolbar->actions[id]->isEnabled()) {
                toolbar->actions[id]->trigger();
            }
        });
        // A foreign-window container keeps Cocoa hit testing on the SwiftUI
        // view, rather than routing it through a Qt NSView.
        auto* window = QWindow::fromWinId(reinterpret_cast<WId>(host));
        container = QWidget::createWindowContainer(window, this);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(container);
        for (int32_t id = 0; id < 6; ++id) {
            connect(actions[id], &QAction::changed, this, [this, id] { Update(id); });
            Update(id);
        }
    }
    ~NativeToolbar() override {
        delete container;
        citrosis_toolbar_release(host);
    }
private:
    void Update(int32_t id) {
        const auto title = actions[id]->text().toUtf8();
        citrosis_toolbar_update(host, id, actions[id]->isEnabled(),
                                actions[id]->property("citrosisPaused").toBool(), title.constData());
    }
    void* host{};
    QWidget* container{};
    std::array<QAction*, 6> actions;
};
}
QWidget* CreateCitrosisNativeToolbar(QWidget* parent, const std::array<QAction*, 6>& actions) {
    return new NativeToolbar(parent, actions);
}
