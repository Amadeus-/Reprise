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
#include <QPointer>

#include "iimportprogresstracker.h"

namespace mu::reprise {
//! NOTE: Trivial in-memory holder for the latest Import-PDF run snapshot. All access is on the Qt UI thread
//! (both the popup view-model writes and the command-handler reads run there), so no locking is needed.
class ImportProgressTracker : public IImportProgressTracker
{
public:
    ImportProgressTracker() = default;

    void reportStarted(const std::string& pdfPath) override
    {
        m_snapshot = ImportProgressSnapshot();
        m_snapshot.active = true;
        m_snapshot.status = "running";
        m_snapshot.pdfPath = pdfPath;
        m_activeChanged.notify();
    }

    void reportProgress(int percent, int page, int totalPages, const std::string& phase) override
    {
        m_snapshot.active = true;
        m_snapshot.status = "running";
        m_snapshot.percent = percent;
        m_snapshot.page = page;
        m_snapshot.totalPages = totalPages;
        m_snapshot.phase = phase;
    }

    void reportFinished(bool ok, const std::string& error, const std::string& details,
                        const std::string& logFilePath = std::string()) override
    {
        m_snapshot.active = false;
        m_snapshot.status = ok ? "done" : "error";
        m_snapshot.hasError = !ok;
        m_snapshot.error = ok ? std::string() : error;
        m_snapshot.details = ok ? std::string() : details;
        m_snapshot.logFilePath = ok ? std::string() : logFilePath;
        if (ok) {
            m_snapshot.percent = 100;
        }
        m_activeChanged.notify();
    }

    void reportAborted() override
    {
        //! User cancelled: the run is over but it is NOT an error. Clear the active flag (so the single-import guard can
        //! never stick) and leave status idle without recording an error for reprise.get-import-errors.
        m_snapshot.active = false;
        m_snapshot.status = "idle";
        m_snapshot.hasError = false;
        m_snapshot.error.clear();
        m_snapshot.details.clear();
        m_snapshot.logFilePath.clear();
        m_activeChanged.notify();
    }

    ImportProgressSnapshot snapshot() const override
    {
        return m_snapshot;
    }

    muse::async::Notification activeChanged() const override
    {
        return m_activeChanged;
    }

    void setProgressWindow(QObject* window) override
    {
        m_progressWindow = window;
    }

    QObject* progressWindow() const override
    {
        return m_progressWindow.data();
    }

    void requestPopupClose() override
    {
        m_popupCloseRequested.notify();
    }

    muse::async::Notification popupCloseRequested() const override
    {
        return m_popupCloseRequested;
    }

    void requestErrorWindow() override
    {
        m_errorWindowRequested.notify();
    }

    muse::async::Notification errorWindowRequested() const override
    {
        return m_errorWindowRequested;
    }

private:
    ImportProgressSnapshot m_snapshot;

    //! The transient popup's own QQuickWindow (held as a QObject*/QPointer so it auto-nulls if the popup is destroyed
    //! by some other path first). Used by ImportPdfController to close + settle the popup on the success auto-close.
    QPointer<QObject> m_progressWindow;

    muse::async::Notification m_popupCloseRequested;
    muse::async::Notification m_activeChanged;
    muse::async::Notification m_errorWindowRequested;
};
}
