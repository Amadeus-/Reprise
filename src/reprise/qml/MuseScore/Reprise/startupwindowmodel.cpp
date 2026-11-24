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

#include "startupwindowmodel.h"

#include <QWindow>

#include "log.h"

using namespace mu::reprise;

StartupWindowModel::StartupWindowModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: nothing that resolves the IoC context may run here -- iocCtxForQmlObject(this) is lazy (it reads qmlContext(this),
    //! attached only AFTER this ctor returns). All context access is deferred to maximizeOnFirstLaunchIfNeeded().
}

StartupWindowModel::~StartupWindowModel() = default;

void StartupWindowModel::maximizeOnFirstLaunchIfNeeded()
{
    if (m_handled)
    {
        return;
    }
    m_handled = true;

    //! Ground truth for "no geometry was restored": the persisted "window" state (read by DockWindow::restoreGeometry) is
    //! empty on a fresh profile. We deliberately maximize ONLY in that case, so a launch that DID restore saved geometry is
    //! never force-maximized over the user's chosen size/position.
    if (!uiState()->windowGeometry().isEmpty())
    {
        return;
    }

    QWindow* window = mainWindow()->qWindow();
    if (!window)
    {
        LOGW() << "no main window to maximize on first launch";
        return;
    }

    window->showMaximized();

    return;
}
