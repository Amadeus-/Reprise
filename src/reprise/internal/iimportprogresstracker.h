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

#include <string>

#include "modularity/imoduleinterface.h"
#include "async/notification.h"

class QObject;

namespace mu::reprise {
//! NOTE: A persistent, app-lifetime snapshot of the LATEST Import-PDF / OMR run, written by the transient progress
//! popup view-model (ImportProgressModel) as Audiveris runs, and read back by the reprise.get-import-progress /
//! reprise.get-import-errors MCP/API queries. The popup is destroyed when it closes, so this global tracker is the
//! only place a headless agent can observe import progress + errors.
struct ImportProgressSnapshot {
    bool active = false;             //! an import is currently running
    std::string status = "idle";     //! "idle" | "running" | "done" | "error"
    std::string phase;               //! human-readable Audiveris stage (e.g. "detecting staves")
    int percent = -1;                //! 0-100, or -1 while the sheet total is still unknown
    int page = 0;                    //! 1-based current sheet (0 until known)
    int totalPages = 0;              //! total sheets in the run (0 until known)
    std::string pdfPath;             //! source PDF of the current/last run
    bool hasError = false;           //! the last finished run ended in error
    std::string error;               //! short error message
    std::string details;             //! the full Audiveris output for the error (shown in the error window)
    std::string logFilePath;         //! on-disk copy of the full Audiveris log, for the error window's "Open log file"

    //! "Import finished" partial-import notice (set AFTER a successful reportFinished(true,...) when some pages were skipped):
    //! the pre-built, newline-joined per-page report ("Page N: ... -- skipped" lines, interior line prepended when split), and
    //! whether survivors were non-contiguous (multi-movement). Read back by ImportPdfController::showImportNotice to populate
    //! the notice window. Auto-cleared by reportStarted()'s fresh snapshot; NOT cleared by reportFinished (set after it).
    std::string partialSkippedReport;
    bool partialMultiMovement = false;

    //! PART 2 smart-deskew ladder: true when the import succeeded only after the notes-only retry dropped OCR (the text-role
    //! NPE recovery). Read back by ImportPdfController::showImportNotice so the "Import finished" notice shows the "couldn't
    //! read the text/lyrics -- imported the notes only" line. Set alongside the partial fields; auto-cleared by reportStarted().
    bool partialTextDropped = false;

    //! 0.17.2 generic-parts nudge: true when the imported score's instruments look like Audiveris placeholders rather than
    //! recognised instruments (a voice.oohs/voice.aahs placeholder id, or 2+ parts all sharing one identical musicXmlId).
    //! Read back by ImportPdfController::showImportNotice so the "Import finished" notice shows the "instruments are a best
    //! guess -- open the Layout panel" nudge. Set alongside the partial fields; auto-cleared by reportStarted().
    bool partialGenericParts = false;
};

class IImportProgressTracker : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IImportProgressTracker)

public:
    virtual ~IImportProgressTracker() = default;

    //! A new run has begun for pdfPath: resets the snapshot to a fresh "running" state.
    virtual void reportStarted(const std::string& pdfPath) = 0;

    //! Streamed progress update from the running OMR.
    virtual void reportProgress(int percent, int page, int totalPages, const std::string& phase) = 0;

    //! The run finished: ok -> status "done"; else status "error" with the message + details tail.
    virtual void reportFinished(bool ok, const std::string& error, const std::string& details,
                                const std::string& logFilePath = std::string()) = 0;

    //! The run was aborted (user Cancel / header-X while running): clears the active flag WITHOUT recording an error, so
    //! the single-import guard (bound to snapshot().active via activeChanged) can never stick set after a cancel. Distinct
    //! from reportFinished(false, ...) which records an error for reprise.get-import-errors; a cancel is not an error.
    virtual void reportAborted() = 0;

    //! The current snapshot (thread-confined to the UI thread; reads happen on the same thread as writes).
    virtual ImportProgressSnapshot snapshot() const = 0;

    //! Fired on every transition of the active flag (reportStarted -> true, reportFinished / reportAborted -> false).
    //! Observed by RepriseCommandsState to grey out / re-enable the New-Scan command (REPRISE_IMPORT_PDF_COMMAND) so a
    //! second import cannot be launched while one is active. UI-thread only (all tracker access is on the UI thread).
    virtual muse::async::Notification activeChanged() const = 0;

    //! The progress popup registers its own QQuickWindow here when it opens (passed as a QObject* from QML). This lets
    //! the app-lifetime ImportPdfController -- which outlives the transient popup -- close the popup's window DIRECTLY
    //! and settled (forceCloseAndSettle) on the success auto-close path, instead of the popup closing itself via a bare
    //! root.hide() that races the project-open's interactive-stack churn and double-delivers Interactive::onClose.
    virtual void setProgressWindow(QObject* window) = 0;
    virtual QObject* progressWindow() const = 0;

    //! The popup asks (after the Done dwell) for its window to be closed safely. Fired by ImportProgressModel's
    //! auto-close timer; observed by ImportPdfController, which runs the settled close on the next event-loop turn
    //! (so the popup's own call stack has fully unwound before its window + view-model are destroyed).
    virtual void requestPopupClose() = 0;
    virtual muse::async::Notification popupCloseRequested() const = 0;

    //! A recognition FAILED (any of the ImportProgressModel failure paths): ask the app-lifetime ImportPdfController to
    //! close the progress popup and open the dedicated "Recognition failed" window. Fired by ImportProgressModel right
    //! after reportFinished(false, ...) records the message + details in the snapshot; the controller reads them back to
    //! populate the error dialog. Separate from popupCloseRequested (the success auto-close), so a run resolves to exactly
    //! one of: Done auto-close, or a failure handed to the dedicated error window.
    virtual void requestErrorWindow() = 0;
    virtual muse::async::Notification errorWindowRequested() const = 0;

    //! Record the partial-import result into the snapshot (skippedReport = the newline-joined per-page list already built by
    //! ImportProgressModel; multiMovement = survivors were non-contiguous; genericParts = the instruments look like Audiveris
    //! placeholders). Called AFTER the success reportFinished(true,...), so it does NOT touch active/status -- it only fills the
    //! partial fields the notice window reads back.
    virtual void reportPartialImport(const std::string& skippedReport, bool multiMovement, bool textDropped = false,
                                     bool genericParts = false) = 0;

    //! Some pages were skipped on an otherwise-successful import: ask the app-lifetime ImportPdfController to open the
    //! dedicated "Import finished" notice window. Fired by ImportProgressModel once the imported score is confirmed open,
    //! after reportPartialImport recorded the report; the controller reads the snapshot back to populate the notice.
    virtual void requestNoticeWindow() = 0;
    virtual muse::async::Notification noticeWindowRequested() const = 0;
};
}
