/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2023 MuseScore Limited and others
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
#include <QTimer>
#include <qqmlintegration.h>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "iprojectconfiguration.h"
#include "iprojectsession.h"
#include "rcommand/icommanddispatcher.h"
#include "rcommand/icommandsstate.h"
#include "interactive/iplatforminteractive.h"
#include "interactive/iinteractive.h"
#include "cloud/musescorecom/imusescorecomservice.h"

class QString;

namespace mu::project {
class ScoresPageModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    Q_PROPERTY(ViewType viewType READ viewType WRITE setViewType NOTIFY viewTypeChanged)
    //! FIX 1: false while a scan recognition is already active, so the "New Scan" entries grey out and a second import
    //! cannot be launched. Bound to the shared reprise command state (REPRISE_IMPORT_PDF_COMMAND), which RepriseCommandsState
    //! flips off while a run is active -- the SAME state that greys the File->New menu item.
    Q_PROPERTY(bool importEnabled READ importEnabled NOTIFY importEnabledChanged)

    QML_ELEMENT

    muse::GlobalInject<IProjectConfiguration> configuration;
    muse::GlobalInject<muse::cloud::IMuseScoreComService> museScoreComService;
    muse::GlobalInject<muse::IPlatformInteractive> platformInteractive;
    muse::ContextInject<muse::IInteractive> interactive = { this };
    muse::ContextInject<muse::rcommand::ICommandDispatcher> commandDispatcher = { this };
    muse::ContextInject<muse::rcommand::ICommandsState> commandsState = { this };
    //! Reprise: observe open COMPLETION (the opened project becoming the current notation) so the busy cursor shown while a
    //! recent scan loads is restored at the right moment -- the open is async, so a plain scope guard would restore too early.
    muse::ContextInject<IProjectSession> projectSession = { this };

public:
    explicit ScoresPageModel(QObject* parent = nullptr);
    ~ScoresPageModel() override;

    enum ViewType {
        Grid = int(IProjectConfiguration::HomeScoresPageViewType::Grid),
        List = int(IProjectConfiguration::HomeScoresPageViewType::List),
    };
    Q_ENUM(ViewType);

    ViewType viewType() const;
    void setViewType(ViewType type);

    bool importEnabled() const;

    //! Deferred subscribe to the reprise import command's state changes (NOT the ctor -- qmlContext is null there for a
    //! QML-instantiated model). Driven from ScoresPage.qml's Component.onCompleted. Idempotent.
    Q_INVOKABLE void init();

    Q_INVOKABLE void createNewScore();
    Q_INVOKABLE void openScore(const QString& scorePath, const QString& displayNameOverride);
    Q_INVOKABLE void revealInFileBrowser(const QString& scorePath);
    Q_INVOKABLE void viewOnline(int scoreId);
    Q_INVOKABLE void clearRecentScores();

signals:
    void viewTypeChanged();
    void importEnabledChanged();

private:
    //! Reprise: show a busy (wait) cursor while a recent scan (.repr) opens from the home. The open is ASYNC (~3-5s), so we
    //! set the override cursor when openScore dispatches the open and restore it on the REAL completion -- the next
    //! IProjectSession::currentNotationChanged with a current notation (the opened score) -- or on a timeout backstop so the
    //! cursor can never get stuck busy if an open never completes. The destructor also restores (safety) if this model is torn
    //! down mid-open. Reprise-only; a tracked §8 divergence in this upstream file (match its K&R style).
    void beginScanOpenBusyCursor();
    void endScanOpenBusyCursor();

    bool m_subscribed = false;
    bool m_scanOpenBusy = false;
    QTimer m_scanOpenBusyTimeout;
};
}
