// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
class QAction;
class QWidget;
QWidget* CreateCitrosisNativeToolbar(QWidget* parent, const std::array<QAction*, 6>& actions);
