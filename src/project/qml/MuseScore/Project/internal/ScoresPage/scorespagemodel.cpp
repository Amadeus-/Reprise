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

#include "scorespagemodel.h"

#include <QCursor>
#include <QGuiApplication>
#include <QString>

#include "project/projectcommands.h"
#include "reprise/reprisecommands.h"
#include "translation.h"
#include "log.h"

using namespace mu::project;

//! Reprise: hard cap on how long the recent-scan busy cursor may stay up if the open-completion signal never arrives (a failed
//! or cancelled open), so the cursor can never get stuck busy. Comfortably longer than a normal multi-page .repr open (~3-5s).
static constexpr int SCAN_OPEN_BUSY_TIMEOUT_MS = 15000;

ScoresPageModel::ScoresPageModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

ScoresPageModel::~ScoresPageModel()
{
    //! Safety: if this model is destroyed while a scan open is still in flight (home page torn down mid-open), restore the
    //! override cursor here so it is never left stuck busy. restoreOverrideCursor is a no-op mismatch-safe only if we pushed
    //! one, so guard on our own flag.
    if (m_scanOpenBusy) {
        QGuiApplication::restoreOverrideCursor();
        m_scanOpenBusy = false;
    }
}

void ScoresPageModel::init()
{
    if (m_subscribed) {
        return;
    }
    m_subscribed = true;

    //! Re-notify importEnabled whenever the reprise import command's state flips (RepriseCommandsState disables it while a
    //! recognition is active), so the "New Scan" entries grey out / re-enable live.
    commandsState()->commandStateChanged().onReceive(this,
                                                      [this](const muse::rcommand::Command& command, const muse::rcommand::CommandState&) {
        if (command == reprise::REPRISE_IMPORT_PDF_COMMAND) {
            emit importEnabledChanged();
        }
    });

    //! Reprise: busy-cursor restore on open COMPLETION. A recent scan opened from here loads asynchronously; the opened score
    //! becoming the current notation is the true "done" signal (the same one the proofing view-models key off). Restore only
    //! once a notation is actually current (a null transition at open start must not clear the cursor early); the timeout below
    //! backstops a failed/cancelled open that never yields a notation.
    m_scanOpenBusyTimeout.setSingleShot(true);
    m_scanOpenBusyTimeout.setInterval(SCAN_OPEN_BUSY_TIMEOUT_MS);
    QObject::connect(&m_scanOpenBusyTimeout, &QTimer::timeout, this, [this]() {
        endScanOpenBusyCursor();
    });

    projectSession()->currentNotationChanged().onNotify(this, [this]() {
        //! Restore the busy cursor at the TRUE end of the open. currentNotationChanged fires as the opened score becomes
        //! current, but a SYNCHRONOUS score layout/render then blocks the UI thread (TRACED at ~2.9s on a multi-page .repr,
        //! measured as the gap between currentNotationChanged and the next event-loop turn) BEFORE control returns to the
        //! event loop -- that block IS the perceived freeze. Restoring directly here would clear the cursor at the START of
        //! the freeze (cursor spins a split second, then a normal cursor during the 1-3s freeze -- the reported bug). So we
        //! QUEUE the restore (singleShot 0): it runs on the first event-loop turn AFTER the synchronous layout completes, so
        //! the busy cursor spans the WHOLE perceived open. Gated on a non-null current notation (a null transition at open
        //! start must not trigger it). endScanOpenBusyCursor is idempotent, so repeat currentNotationChanged fires are safe.
        //! The 15s timeout + the destructor remain the backstops if this model is torn down before the queued restore runs.
        if (m_scanOpenBusy && projectSession()->currentNotation()) {
            QTimer::singleShot(0, this, [this]() {
                endScanOpenBusyCursor();
            });
        }
    });
}

bool ScoresPageModel::importEnabled() const
{
    return commandsState()->commandState(reprise::REPRISE_IMPORT_PDF_COMMAND).enabled;
}

void ScoresPageModel::createNewScore()
{
    commandDispatcher()->dispatch(reprise::REPRISE_IMPORT_PDF_COMMAND);
}

void ScoresPageModel::openScore(const QString& scorePath, const QString& displayNameOverride)
{
    //! Reprise: show the busy cursor for the (async) load of a recent scan. Set BEFORE the dispatch so it is up the instant the
    //! user commits; restored on the open's completion (currentNotationChanged) or the timeout backstop -- see init().
    beginScanOpenBusyCursor();

    commandDispatcher()->dispatch(project::PROJECT_OPEN_COMMAND, { { "url", muse::Val(QUrl::fromLocalFile(
                                                                                          scorePath).toString()) }, { "display_name", muse::Val(
                                                                                                                          displayNameOverride) } });
}

void ScoresPageModel::beginScanOpenBusyCursor()
{
    //! Idempotent: never stack more than one override cursor (a second rapid open would otherwise need a second restore).
    if (m_scanOpenBusy) {
        return;
    }

    m_scanOpenBusy = true;
    QGuiApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    m_scanOpenBusyTimeout.start();
}

void ScoresPageModel::endScanOpenBusyCursor()
{
    if (!m_scanOpenBusy) {
        return;
    }

    m_scanOpenBusy = false;
    m_scanOpenBusyTimeout.stop();
    QGuiApplication::restoreOverrideCursor();
}

void ScoresPageModel::revealInFileBrowser(const QString& scorePath)
{
    muse::Ret ret = platformInteractive()->revealInFileBrowser(scorePath);
    if (!ret) {
        LOGE() << ret.toString();
    }
}

void ScoresPageModel::viewOnline(int scoreId)
{
    if (scoreId <= 0) {
        return;
    }

    museScoreComService()->downloadScoreInfo(scoreId)
    .onResolve(this, [this, scoreId](const muse::RetVal<muse::cloud::ScoreInfo>& scoreInfo) {
        if (!scoreInfo.ret) {
            LOGE() << scoreInfo.ret.toString();
            return;
        }

        QUrl scoreUrl = QUrl::fromUserInput(scoreInfo.val.url);
        if (!scoreUrl.isValid() || scoreUrl.isEmpty()) {
            LOGE() << "Invalid score URL for cloud score" << scoreId << ":" << scoreInfo.val.url;
            return;
        }

        platformInteractive()->openUrl(scoreUrl);
    });
}

void ScoresPageModel::clearRecentScores()
{
    std::string title = muse::trc("project", "Clear recent scans history?");
    std::string body = muse::trc("project", "This won't delete your files.");

    // Build explicit ButtonData so the affirmative button carries a label: the bare standard
    // Button::Clear enum has no text in muse's Interactive::buttonData(), so it rendered blank.
    muse::IInteractive::ButtonData cancelBtn = interactive()->buttonData(muse::IInteractive::Button::Cancel);
    muse::IInteractive::ButtonData clearBtn(int(muse::IInteractive::Button::Clear),
                                            muse::trc("project", "Clear recent scans"), true, false,
                                            muse::IInteractive::AcceptRole);

    interactive()->question(title, muse::IInteractive::Text(body), { cancelBtn, clearBtn },
                            int(muse::IInteractive::Button::Cancel))
    .onResolve(this, [this](const muse::IInteractive::Result& res) {
        if (res.isButton(muse::IInteractive::Button::Clear)) {
            commandDispatcher()->dispatch(project::PROJECT_CLEAR_RECENT_COMMAND);
        }
    });

    return;
}

ScoresPageModel::ViewType ScoresPageModel::viewType() const
{
    return static_cast<ViewType>(configuration()->homeScoresPageViewType());
}

void ScoresPageModel::setViewType(ViewType type)
{
    if (viewType() == type) {
        return;
    }

    configuration()->setHomeScoresPageViewType(IProjectConfiguration::HomeScoresPageViewType(type));
    emit viewTypeChanged();
}
