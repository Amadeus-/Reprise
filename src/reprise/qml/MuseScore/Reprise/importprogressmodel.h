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

#include <memory>

#include <QObject>
#include <QString>
#include <qqmlintegration.h>

#include "modularity/ioc.h"
#include "async/asyncable.h"
#include "rcommand/commandtypes.h"
#include "project/iprojectsession.h"
#include "project/iopenprojectscenario.h"
#include "notation/iinstrumentsrepository.h"

#include "internal/irepriseconfiguration.h"
#include "internal/iimportprogresstracker.h"

namespace mu::reprise {
class AudiverisRunner;

//! View-model behind the Reprise recognition progress popup. Owns the async Audiveris run: it starts the runner,
//! streams its parsed progress into bindable properties, and on success performs the import (opens the recognised
//! MusicXML) + the temporary Grand-Piano override, then flips to the Done state (and auto-closes if the user asked).
//! On failure it exposes the error state (message + a tail of Audiveris output). Cancel kills Audiveris and aborts.
class ImportProgressModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    //! Gray sub-line parts (filename is `title`): "pages 1-3" style label and the scan dpi.
    Q_PROPERTY(QString pagesLabel READ pagesLabel NOTIFY titleChanged)
    Q_PROPERTY(int dpi READ dpi NOTIFY titleChanged)
    Q_PROPERTY(QString stageText READ stageText NOTIFY progressChanged)
    Q_PROPERTY(int percent READ percent NOTIFY progressChanged)
    Q_PROPERTY(bool indeterminate READ indeterminate NOTIFY progressChanged)
    //! "Page X of N" caption shown beneath the progress bar (empty until the page count is known).
    Q_PROPERTY(QString progressCaption READ progressCaption NOTIFY progressChanged)
    //! One of "running" | "done" | "error".
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString errorDetails READ errorDetails NOTIFY statusChanged)
    //! The streamed Audiveris stdout/stderr, surfaced in the popup's "Details" recognition-log expander.
    Q_PROPERTY(QString logText READ logText NOTIFY logTextChanged)
    Q_PROPERTY(bool closePopupOnComplete READ closePopupOnComplete WRITE setClosePopupOnComplete NOTIFY closePopupOnCompleteChanged)

    QML_ELEMENT

    muse::ContextInject<mu::project::IProjectSession> projectSession = { this };
    muse::ContextInject<mu::project::IOpenProjectScenario> openProjectScenario = { this };
    muse::GlobalInject<mu::notation::IInstrumentsRepository> instrumentsRepository;
    muse::GlobalInject<mu::reprise::IRepriseConfiguration> repriseConfiguration;
    muse::GlobalInject<mu::reprise::IImportProgressTracker> importProgressTracker;

public:
    explicit ImportProgressModel(QObject* parent = nullptr);
    ~ImportProgressModel() override;

    QString title() const { return m_title; }
    QString pagesLabel() const { return m_pagesLabel; }
    int dpi() const { return m_dpi; }
    QString stageText() const { return m_stageText; }
    int percent() const { return m_percent; }
    bool indeterminate() const { return m_indeterminate; }
    QString progressCaption() const;
    QString status() const { return m_status; }
    QString errorDetails() const { return m_errorDetails; }
    QString logText() const { return m_logText; }

    bool closePopupOnComplete() const;
    void setClosePopupOnComplete(bool close);

    //! Called from the popup's Component.onCompleted with the params handed in by the controller. The recognition
    //! options (text/lyric OCR master, language code, chord symbols, lyrics-above-staff) are forwarded verbatim to
    //! AudiverisRunner::start(), which turns them into the per-run '-constant' switches.
    Q_INVOKABLE void load(const QString& pdfPath, const QString& pages, int dpi,
                          bool recognizeText, const QString& language, bool chordSymbols, bool lyricsAboveStaff);

    //! Cancel button / the header X during a run: kill Audiveris, abort the import, request the popup to close.
    Q_INVOKABLE void cancel();

    //! Error state's "Copy" button: copy the whole error (the red headline message + the recognition log) to the system
    //! clipboard, so the user can paste it when asking for help. Mirrors the About dialog's copyRevisionToClipboard idiom.
    Q_INVOKABLE void copyErrorToClipboard() const;

    //! Register this popup's own QQuickWindow (passed from QML as root.window) with the app-lifetime progress tracker,
    //! so ImportPdfController can close it DIRECTLY and settled on the success auto-close path -- see onRunnerFinished /
    //! the masterNotationChanged handler. Closing the fire-and-forget popup via the controller's forceCloseAndSettle
    //! (which only closes while the dialog is still on the interactive stack, then pumps until the window is destroyed)
    //! avoids the bare root.hide() auto-close racing the project-open's stack churn and double-delivering onClose.
    Q_INVOKABLE void registerPopupWindow(QObject* window);

signals:
    void titleChanged();
    void progressChanged();
    void statusChanged();
    void logTextChanged();
    void closePopupOnCompleteChanged();

    //! Ask the popup QML to hide() itself (auto-close on success when the checkbox is set).
    void closeRequested();

private:
    //! Deferred IoC-context setup (runner + signal wiring + master-notation subscription + settings default),
    //! run from load() -- NOT the ctor, where qmlContext(this) is still null. Idempotent (first call only).
    void initContext();

    void onRunnerProgress(int percent, int sheet, int total, const QString& stageHuman);
    void onRunnerFinished(bool ok, const QString& outputPath, const QString& error, const QString& tail);

    //! Append one streamed Audiveris output line to the recognition log (capped).
    void appendLog(const QString& line);

    //! Build the "pages 1-3"/"all pages" sub-line label from the free-form page range.
    QString formatPagesLabel(const QString& pages) const;

    //! Once the imported score is open, fill the Done line with the real measure + page counts.
    void updateDoneLine();

    void setStatus(const QString& status);

    //! Open the recognised MusicXML through MuseScore's native import, armed to default every part to Grand Piano.
    void openImported(const QString& outputPath);

    //! Declare the import a graceful failure because the recognised score never opened -- the open-scenario promise
    //! resolved with an error Ret, or the generous backstop elapsed with no master ever becoming current. Guarded by
    //! m_awaitingOpen (fires at most once, never after a successful open) and re-checks masterNotation() so a
    //! slow-but-valid open that landed first is never mis-flagged. Surfaces the error in the popup AND via
    //! reprise.get-import-errors, consistent with the zero-part guard -- never a silent "Done".
    void reportOpenFailed();

    //! Finalize "Done" for a NEW-WINDOW open (a second import while a score is already open). MuseScore routes such an
    //! open to a separate instance (OpenProjectScenario::openProject step 4 -> openNewWindow -> QProcess::startDetached),
    //! so masterNotationChanged never fires in THIS instance and the imported score never becomes the current master
    //! here. Called off the resolved open-scenario promise (success) when a project was already open at import time, so
    //! the popup still reaches Done + auto-closes. Guarded by m_awaitingOpen so it fires at most once per import. Because
    //! the imported score lives in another instance, the Grand-Piano override and the real measure count are NOT applied
    //! here (the current master is the previously-open score and must never be touched) -- the Done line is plain "Done".
    void finalizeOpenedInNewWindow();

    //! Count the <score-part> declarations in the produced MusicXML (.mxl ZIP or plain .xml). Used to detect a
    //! degenerate scan (zero recognised parts) so it can be refused GRACEFULLY before opening, instead of handing
    //! the engraving layout a zero-part / zero-visible-part score. Returns the part count, or -1 when the file
    //! cannot be read/parsed -- in which case the caller must NOT refuse (fail open: proceed with the normal open).
    int recognizedPartCount(const QString& outputPath) const;

    //! TEMPORARY Reprise default: force every imported part to Grand Piano (applied on the next master-notation change).
    void applyGrandPianoDefault();

    std::shared_ptr<AudiverisRunner> m_runner;

    QString m_title;
    QString m_pages;
    QString m_pagesLabel;
    int m_dpi = 300;
    QString m_stageText;
    int m_percent = -1;
    bool m_indeterminate = true;
    int m_currentPage = -1;
    int m_totalPages = -1;
    QString m_status = "running";
    QString m_errorDetails;
    QString m_logText;

    //! Set just before the imported score is opened; consumed on the next master-notation change.
    bool m_pendingGrandPianoReset = false;

    //! Set just before the imported score is opened; consumed to fill the Done line's measure + page counts.
    bool m_pendingDoneCounts = false;

    //! Set when the imported score's open is dispatched; cleared when the open resolves either way. On a successful
    //! open masterNotationChanged fires and finalizes the Done state; a declined open resolves the open-scenario
    //! promise with an error Ret and reportOpenFailed() surfaces a graceful error. Guards both the success
    //! finalization and the open-failure path so each fires exactly once per import.
    bool m_awaitingOpen = false;
};
}
