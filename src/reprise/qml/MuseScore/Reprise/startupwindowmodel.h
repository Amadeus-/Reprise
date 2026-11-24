/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QObject>
#include <qqmlintegration.h>

#include "modularity/ioc.h"

#include "ui/iuistate.h"
#include "ui/imainwindow.h"

namespace mu::reprise {
//! Reprise FIRST-LAUNCH WINDOW helper. On a fresh profile (no persisted window geometry -- nothing yet written into the
//! "window" key of the workspace state, which is only serialised on a graceful shutdown) Reprise's main ApplicationWindow
//! would otherwise open at its hardcoded QML default (1150x800, see AppWindow.qml) instead of maximized. muse's own
//! "show maximized when no geometry was restored" fallback is disabled upstream (dockwindow.cpp loadPage, the commented-out
//! showMaximized with a "->->" typo), and muse\ is kept pristine -- so this non-muse helper reinstates that intent from the
//! appshell side: WindowContent.qml calls maximizeOnFirstLaunchIfNeeded() on the first DockWindow::pageLoaded, exactly where
//! muse intended to maximize. It maximizes ONLY when no geometry was persisted; a normal subsequent launch (geometry
//! present and restored by KDDock) is left untouched, so the user's saved size/position is always honoured.
class StartupWindowModel : public QObject, public muse::Contextable
{
    Q_OBJECT

    QML_ELEMENT

    muse::ContextInject<muse::ui::IUiState> uiState = { this };
    muse::ContextInject<muse::ui::IMainWindow> mainWindow = { this };

public:
    explicit StartupWindowModel(QObject* parent = nullptr);
    ~StartupWindowModel() override;

    //! Called from WindowContent.qml on the first DockWindow::pageLoaded. Idempotent (runs its decision at most once): if no
    //! window geometry was persisted (first launch), show the main window maximized; otherwise leave the restored geometry
    //! alone. Safe to call on every pageLoaded -- it no-ops after the first decision.
    Q_INVOKABLE void maximizeOnFirstLaunchIfNeeded();

private:
    bool m_handled = false;
};
}
