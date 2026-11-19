/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include "mainwindowtitleprovider.h"

#include "translation.h"

#include "notation/inotation.h"
#include "project/inotationproject.h"
#include "project/types/projecturis.h"
#include "project/types/reprisenameutils.h"

using namespace mu::appshell;

MainWindowTitleProvider::MainWindowTitleProvider(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

void MainWindowTitleProvider::load()
{
    update();

    context()->projectChanged().onNotify(this, [this]() {
        if (auto currentProject = context()->project()) {
            currentProject->displayNameChanged().onNotify(this, [this]() {
                update();
            });

            currentProject->needSaveChanged().onNotify(this, [this]() {
                update();
            });
        }
    });

    context()->currentNotationChanged().onNotify(this, [this]() {
        update();
    });

    //! Reprise: recompute the title whenever the current page changes (e.g. Edit <-> Home/Scans), so the
    //! title drops back to "Reprise" the moment the Home page is shown even if a score remains the current
    //! project. Dialogs/popups push on top of the primary page, so this fires for them too, but update()
    //! keys off the primary page (isEditingNotation) and is therefore stable under transient dialogs.
    interactive()->currentUri().ch.onReceive(this, [this](const muse::Uri&) {
        update();
    });

    commandsController()->isRequestedFullScreenChanged().onNotify(this, [this]() {
        emit requestedFullScreenChanged();
    });
}

QString MainWindowTitleProvider::title() const
{
    return m_title;
}

QString MainWindowTitleProvider::filePath() const
{
    return m_filePath;
}

bool MainWindowTitleProvider::fileModified() const
{
    return m_fileModified;
}

bool MainWindowTitleProvider::requestedFullScreen() const
{
    return commandsController()->isRequestedFullScreen();
}

void MainWindowTitleProvider::setTitle(const QString& title)
{
    if (title == m_title) {
        return;
    }

    m_title = title;
    emit titleChanged(title);
}

void MainWindowTitleProvider::setFilePath(const QString& filePath)
{
    if (filePath == m_filePath) {
        return;
    }

    m_filePath = filePath;
    emit filePathChanged(filePath);
}

void MainWindowTitleProvider::setFileModified(bool fileModified)
{
    if (fileModified == m_fileModified) {
        return;
    }

    m_fileModified = fileModified;
    emit fileModifiedChanged(fileModified);
}

bool MainWindowTitleProvider::isEditingNotation() const
{
    if (!interactive())
    {
        return false;
    }

    return interactive()->isOpened(project::NOTATION_PAGE_URI).val
           || interactive()->isOpened(project::NOTATION_REVIEW_PAGE_URI).val;
}

void MainWindowTitleProvider::update()
{
    project::INotationProjectPtr project = context()->project();

    //! Reprise: file path + modified state drive the OS document-proxy icon and the unsaved-changes
    //! indicator (via MainWindowBridge). Keep them in sync regardless of which page is shown.
    setFilePath((!project || project->isNewlyCreated() || project->isCloudProject())
                ? "" : project->path().toQString());
    setFileModified(project && project->isNeedSave());

    //! Reprise: the window title reads "Reprise" on Home / with no score, and "<clean name> - Reprise"
    //! (em dash) only while a score is actually being edited on the notation page. This keys off the
    //! current MAIN page, not merely whether a project exists, which stops the old MuseScore behavior of
    //! leaking the current score's filename into the title while the Home/Scans page is showing.
    static const QString appName = muse::qtrc("appshell", "Reprise");

    if (!project || !isEditingNotation())
    {
        setTitle(appName);

        return;
    }

    //! Reprise: reuse the fork's shared clean-name transform (same one the Scans list uses) on the project
    //! path (LAST-dot extension strip, keeps internal dots, then drops the ".reprise-stripped" marker).
    //! A newly created / cloud project has no on-disk path, so fall back to its display name.
    const QString path = project->path().toQString();
    QString cleanName = project::repriseCleanDisplayName(path.isEmpty() ? project->displayName() : path);
    if (cleanName.isEmpty())
    {
        cleanName = project->displayName();
    }

    setTitle(cleanName + QString(" ") + QChar(0x2014) + QString(" ") + appName);

    return;
}
