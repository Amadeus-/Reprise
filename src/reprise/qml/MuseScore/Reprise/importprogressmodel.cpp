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

#include "importprogressmodel.h"

#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include "global/translation.h"
#include "global/log.h"
#include "global/types/val.h"
#include "global/types/bytearray.h"
#include "global/io/path.h"
#include "global/serialization/zipreader.h"
#include "global/serialization/xmlstreamreader.h"
#include "global/serialization/json.h"

#include "async/notifylist.h"

#include "notation/imasternotation.h"
#include "notation/inotationparts.h"
#include "notation/types/instrumentstypes.h"

#include "engraving/dom/instrument.h"
#include "engraving/dom/part.h"
#include "engraving/dom/masterscore.h"

#include "reprise/reprisecommands.h"
#include "project/iopenprojectscenario.h"
#include "project/inotationproject.h"
#include "project/types/projecttypes.h"
#include "project/types/recentfile.h"

#include "internal/audiverisrunner.h"
#include "internal/scangeometrystripper.h"
#include "internal/proofingmapbuilder.h"

using namespace mu;
using namespace mu::reprise;
using namespace muse;

//! How long the Done state lingers before an auto-close (ms) -- enough to show the gold check, and to let the
//! just-opened score's master-notation change fire so the Grand-Piano override lands before the model is gone.
static constexpr int AUTO_CLOSE_DELAY_MS = 1400;

//! Minimum time (ms) the deskew "Straightening..." banner must stay up so it is perceptible. On a single-page scan the
//! straighten is a few ms, so without this floor the banner flashes sub-perceptibly and the user never sees it. When the
//! chunked deskew loop finishes faster than this, startRecognition is deferred by the remainder via a timer (NOT a sleep,
//! so the UI thread stays live and Cancel keeps working) -- see stepDeskew().
static constexpr int DESKEW_MIN_VISIBLE_MS = 700;

//! Backstop deadline (ms) for the import open -- purely a safety net, NOT how failure is normally detected. The open
//! outcome is taken from the open-scenario promise owned in openImported(): a declined open reports its error Ret the
//! instant it resolves, and a successful open is finalized by masterNotationChanged -- so a slow-but-valid open is never
//! a race, it is just a promise that has not resolved yet (the popup stays on "Opening score..." for exactly as long as
//! the open genuinely takes). This deadline exists only for the pathological case where the promise resolves ok yet no
//! master ever becomes current, so the popup can never hang forever. It is success-aware (reportOpenFailed() re-checks
//! masterNotation()) and deliberately far longer than any real open, so it can never mis-flag a legitimately slow import.
static constexpr int32_t OPEN_BACKSTOP_TIMEOUT_MS = 120000;

ImportProgressModel::ImportProgressModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: Nothing that resolves the IoC context may run here. iocCtxForQmlObject(this) is lazy: it reads
    //! qmlContext(this), which QML only attaches AFTER this C++ ctor returns. Touching a ContextInject (e.g.
    //! projectSession()) or iocContext() now trips the "QQmlContext is not set" assert, resolves to a null
    //! service, and crashes on first use. All context-dependent setup is deferred to initContext(), run from
    //! load() (driven by the popup's Component.onCompleted, by which point the context is live).
}

ImportProgressModel::~ImportProgressModel() = default;

void ImportProgressModel::initContext()
{
    if (m_runner)
    {
        return;
    }

    m_runner = std::make_shared<AudiverisRunner>(iocContext());

    QObject::connect(m_runner.get(), &AudiverisRunner::progressChanged, this, &ImportProgressModel::onRunnerProgress);
    QObject::connect(m_runner.get(), &AudiverisRunner::finished, this, &ImportProgressModel::onRunnerFinished);
    QObject::connect(m_runner.get(), &AudiverisRunner::outputLine, this, &ImportProgressModel::appendLog);
    QObject::connect(m_runner.get(), &AudiverisRunner::retryingAfterError, this, &ImportProgressModel::onRunnerRetrying);
    QObject::connect(m_runner.get(), &AudiverisRunner::preprocessing, this, &ImportProgressModel::onRunnerPreprocessing);

    //! Once the imported score becomes the current master (the open is async): apply the Grand-Piano override and
    //! fill the Done line with the real measure count.
    projectSession()->masterNotationChanged().onNotify(this, [this]()
    {
        if (!projectSession()->masterNotation())
        {
            return;
        }

        if (m_pendingGrandPianoReset)
        {
            m_pendingGrandPianoReset = false;
            applyGrandPianoDefault();
        }

        if (m_pendingDoneCounts)
        {
            m_pendingDoneCounts = false;
            updateDoneLine();
        }

        //! The imported score definitively opened -- this is the only reliable success signal (see OPEN_RESOLVE_TIMEOUT_MS).
        //! Finalize the Done state here, which also cancels the pending open-decline fallback (it checks m_awaitingOpen).
        if (m_awaitingOpen)
        {
            m_awaitingOpen = false;

            //! Reprise Phase C: the imported score is now open + current in THIS window (Grand-Piano override already
            //! applied above) -- persist the whole session as a .repr bundle before flipping to Done.
            autoSaveImportedAsRepr();

            setStatus("done");
            importProgressTracker()->reportFinished(true, std::string(), std::string());

            //! "Import finished" notice (set AFTER the success reportFinished(true,...)): if any pages were skipped or the
            //! survivors were split, open the separate gold notice window now that the score is confirmed open. No-op otherwise.
            surfacePartialImportNoticeIfAny();

            //! Only a CLEAN import (nothing skipped / not split) uses the checkbox-governed auto-close. When the import is
            //! notice-worthy, the controller's showImportNotice() DISMISSES this popup (settled) as part of showing the
            //! notice; a second auto-close scheduled here would be a RACING teardown of this same popup -- that race is
            //! exactly the ticked-checkbox bug where the notice and the auto-close tore each other down and the notice
            //! vanished. So skip the auto-close whenever the notice will take over closing this popup.
            const bool noticeWorthy = !m_skippedPages.isEmpty() || m_multiMovement || m_textDropped;
            if (!noticeWorthy && closePopupOnComplete())
            {
                //! Auto-close the popup after the Done dwell. CRITICAL: do NOT emit closeRequested() -> root.hide() here.
                //! root.hide() closes this fire-and-forget async-opened dialog from INSIDE itself, which races the
                //! project-open's interactive-stack / dock-rebuild churn still in flight at this moment -- two close
                //! deliveries then reach muse's Interactive::onClose for one objectId and the second trips its
                //! obj.objectId.isValid() assert (interactive.cpp:1326) -> SIGSEGV (reproduced on 0.12.6). Instead ask
                //! the app-lifetime ImportPdfController (which OUTLIVES this popup) to run forceCloseAndSettle on this
                //! window: it closes the window only while the dialog is still on the stack (so onClose always finds a
                //! valid objectId -- never a double-delivery), then pumps until the window is destroyed, draining any
                //! racing teardown. Same close-settle idiom as the 0.9.9 capture-import-dialog fix.
                QTimer::singleShot(AUTO_CLOSE_DELAY_MS, this, [this]() { importProgressTracker()->requestPopupClose(); });
            }
        }
    });
}

bool ImportProgressModel::closePopupOnComplete() const
{
    return repriseConfiguration()->closePopupOnComplete();
}

void ImportProgressModel::setClosePopupOnComplete(bool close)
{
    if (close == closePopupOnComplete())
    {
        return;
    }

    repriseConfiguration()->setClosePopupOnComplete(close);
    emit closePopupOnCompleteChanged();
}

void ImportProgressModel::load(const QString& pdfPath, const QString& pages, int dpi,
                               bool recognizeText, const QString& language, bool chordSymbols, bool lyricsAboveStaff)
{
    //! Resolve the IoC context + build the runner now (first call only) -- the QQmlContext is attached by the
    //! time Component.onCompleted calls load(), so this is safe here where the ctor was not.
    initContext();

    m_title = QFileInfo(pdfPath).fileName();
    m_sourcePath = pdfPath;
    m_pages = pages;
    m_dpi = dpi;
    m_pagesLabel = formatPagesLabel(pages);
    //! Retain the recognition options for the .repr manifest's import-provenance block (auto-save-on-import).
    m_recognizeText = recognizeText;
    m_language = language.trimmed().isEmpty() ? QString("eng") : language.trimmed();
    m_chordSymbols = chordSymbols;
    m_lyricsAboveStaff = lyricsAboveStaff;
    emit titleChanged();

    //! Reset ALL per-import state so every run starts independent of any earlier one (idempotent).
    m_currentPage = -1;
    m_totalPages = -1;
    m_logText.clear();
    emit logTextChanged();
    m_pendingGrandPianoReset = false;
    m_pendingDoneCounts = false;
    m_awaitingOpen = false;
    m_skippedPages.clear();
    m_multiMovement = false;
    m_textDropped = false;

    //! PART 1 deskew: reset the chunk-loop state so each fresh import starts independent of any earlier one.
    m_deskewTotal = 0;
    m_deskewNext = 0;
    m_deskewAborted = false;

    //! Min-visible-duration: invalidate the banner timer so this run measures the "Straightening..." dwell from its own
    //! first preprocessing stage, not a stale earlier run's.
    m_deskewBannerTimer.invalidate();

    //! FIX 1: clear any recovery banner left from a prior run so each fresh import starts without it.
    m_retryNotice.clear();
    emit retryNoticeChanged();

    setStatus("running");
    m_indeterminate = true;
    m_percent = -1;
    m_stageText = muse::qtrc("reprise", "Starting Audiveris...");
    emit progressChanged();

    //! PART 1 deskew: before recognition, straighten each selected page. beginDeskew() opens the input and arms the
    //! per-page state; when it has at least one page to process, drive it CHUNKED (one page per event-loop turn via a
    //! single-shot timer) so the popup stays responsive and Cancel works between pages. The loop (stepDeskew) finishes by
    //! starting recognition on the straightened PDF, or on the original when the regression gate left it a no-op. When
    //! beginDeskew() returns false (nothing to process), start recognition directly on the original, exactly as before.
    if (m_runner->beginDeskew(io::path_t(pdfPath), pages, dpi))
    {
        m_deskewTotal = m_runner->deskewPageCount();
        m_deskewNext = 0;
        //! Kick the first step on the next event-loop turn so the popup paints before any page is straightened.
        QTimer::singleShot(0, this, [this]() { stepDeskew(); });
        return;
    }

    startRecognition(pdfPath);
}

void ImportProgressModel::startRecognition(const QString& inputPath)
{
    RecognitionOptions options;
    options.recognizeText = m_recognizeText;
    options.language = m_language.trimmed().isEmpty() ? QString("eng") : m_language.trimmed();
    options.chordSymbols = m_chordSymbols;
    options.lyricsAboveStaff = m_lyricsAboveStaff;

    //! PART 2 smart-deskew ladder: tell the runner the RAW original scan + whether THIS input is the pre-deskewed render, so a
    //! deskewed-input failure can retry on the raw original (which Audiveris self-deskews). inputPath differs from m_sourcePath
    //! exactly when the deskew loop produced a straightened PDF (finishDeskew returned non-empty); for a no-op deskew or an
    //! image import inputPath == m_sourcePath, so inputWasDeskewed is correctly false.
    m_runner->setRetryContext(io::path_t(m_sourcePath), inputPath != m_sourcePath);

    const bool started = m_runner->start(io::path_t(inputPath), m_pages, m_dpi, options);
    if (!started)
    {
        //! FIX 2: route to the dedicated error window (deferred, so the popup finishes constructing first).
        const QString message = muse::qtrc("reprise", "Audiveris was not found.");
        importProgressTracker()->reportFinished(false, message.toStdString(), std::string());
        importProgressTracker()->requestErrorWindow();
    }
}

void ImportProgressModel::stepDeskew()
{
    //! Cancelled mid-deskew -> abort before touching the runner again. cancel() set the flag and already reported aborted,
    //! so there is nothing more to do here (no recognition is started).
    if (m_deskewAborted || !m_runner)
    {
        return;
    }

    if (m_deskewNext >= m_deskewTotal)
    {
        //! Every selected page processed. finishDeskew() returns the straightened multi-page PDF, or EMPTY when no page
        //! actually needed correction (the regression gate) -> feed Audiveris the ORIGINAL untouched (byte-identical).
        const muse::io::path_t deskewed = m_runner->finishDeskew();
        const QString nextInput = deskewed.empty() ? m_sourcePath : deskewed.toQString();

        //! Min-visible-duration: a single-page deskew can finish in a few ms, so the "Straightening..." banner would flash
        //! sub-perceptibly. Hold it up for at least DESKEW_MIN_VISIBLE_MS measured from when it first appeared; if less has
        //! elapsed, defer startRecognition by the remainder via a single-shot timer (NOT a UI-thread sleep -- the popup
        //! stays live and Cancel still works). The deferred turn re-checks m_deskewAborted so a Cancel during the hold
        //! aborts cleanly instead of starting recognition. Applies only to a deskew run: this branch is reached solely via
        //! the chunk loop (load() enters it only when beginDeskew armed pages), and the timer is started only by the deskew
        //! preprocessing signal -- a no-deskew import never gets here.
        const qint64 elapsed = m_deskewBannerTimer.isValid() ? m_deskewBannerTimer.elapsed() : DESKEW_MIN_VISIBLE_MS;
        const int remaining = static_cast<int>(DESKEW_MIN_VISIBLE_MS - elapsed);
        if (remaining > 0)
        {
            QTimer::singleShot(remaining, this, [this, nextInput]()
            {
                if (m_deskewAborted || !m_runner)
                {
                    return;
                }

                startRecognition(nextInput);
            });
            return;
        }

        startRecognition(nextInput);
        return;
    }

    //! Process one page, then yield to the event loop so the bar + stage text repaint and Cancel stays responsive.
    m_runner->deskewStep(m_deskewNext);
    ++m_deskewNext;
    QTimer::singleShot(0, this, [this]() { stepDeskew(); });
}

void ImportProgressModel::onRunnerPreprocessing(int page, int total, const QString& stageHuman)
{
    //! PART 1 deskew progress: a determinate per-page bar with the already-localised straighten stage (mirrors
    //! onRunnerRetrying's banner idiom). Shown before the recognition stage; recognition's own progressChanged replaces it
    //! once Audiveris starts.
    //! Min-visible-duration: start the dwell clock the FIRST time the banner is shown, so stepDeskew() can hold it up for
    //! at least DESKEW_MIN_VISIBLE_MS even when a single-page deskew finishes in a few ms.
    if (!m_deskewBannerTimer.isValid())
    {
        m_deskewBannerTimer.start();
    }

    m_stageText = stageHuman;
    if (total > 0)
    {
        m_percent = (page * 100) / total;
        m_indeterminate = false;
    }
    else
    {
        m_percent = -1;
        m_indeterminate = true;
    }

    emit progressChanged();
}

void ImportProgressModel::cancel()
{
    if (m_runner)
    {
        m_runner->cancel();
    }

    //! PART 1 deskew: abort a chunked straighten in flight -- the next scheduled stepDeskew() turn returns immediately
    //! instead of processing another page or starting recognition.
    m_deskewAborted = true;

    m_pendingGrandPianoReset = false;
    m_pendingDoneCounts = false;
    m_awaitingOpen = false;

    //! FIX 1: clear the single-import guard. Cancel does NOT go through reportFinished (it is not an error) nor the
    //! success popup-close path, so without this the tracker's active flag -- and thus the greyed New-Scan command --
    //! would stick set after a cancel. reportAborted clears active without recording an error.
    importProgressTracker()->reportAborted();

    emit closeRequested();
}

void ImportProgressModel::registerPopupWindow(QObject* window)
{
    //! Called from the popup QML's Component.onCompleted with root.window (the dialog's QQuickWindow). Hand it to the
    //! app-lifetime tracker so the controller can close + settle this window on the success auto-close (see the
    //! masterNotationChanged handler). Held as a QPointer in the tracker, so it safely auto-nulls if closed elsewhere.
    importProgressTracker()->setProgressWindow(window);
}

void ImportProgressModel::onRunnerProgress(int percent, int sheet, int total, const QString& stageHuman)
{
    m_percent = percent;
    m_indeterminate = (percent < 0);

    if (total > 0)
    {
        m_totalPages = total;
    }

    if (sheet > 0)
    {
        m_currentPage = sheet;
    }

    if (total > 0 && sheet > 0)
    {
        //: %1 = current page number, %2 = total pages; an em-dash + a friendly step label (e.g. "reading notes...") follow.
        m_stageText = muse::qtrc("reprise", "Analyzing page %1 of %2").arg(sheet).arg(total)
                      + " " + QChar(0x2014) + " " + stageHuman;
    }
    else
    {
        m_stageText = stageHuman;
    }

    //! Mirror into the app-lifetime tracker so reprise.get-import-progress can report it after this popup is gone.
    importProgressTracker()->reportProgress(percent, m_currentPage > 0 ? m_currentPage : 0,
                                            m_totalPages > 0 ? m_totalPages : 0, stageHuman.toStdString());

    emit progressChanged();
}

QString ImportProgressModel::progressCaption() const
{
    if (m_status == "error")
    {
        return QString();
    }

    const int total = m_totalPages;
    int current = m_currentPage > 0 ? m_currentPage : (total > 0 ? 1 : -1);

    //! Defensive: never display current > total (the runner already clamps, but guard the caption too).
    if (total > 0 && current > total)
    {
        current = total;
    }

    if (total > 0 && current > 0)
    {
        return muse::qtrc("reprise", "Page %1 of %2").arg(current).arg(total);
    }

    return QString();
}

void ImportProgressModel::appendLog(const QString& line)
{
    if (!m_logText.isEmpty())
    {
        m_logText += '\n';
    }
    m_logText += line;

    //! Cap the retained log so a very long run cannot grow it without bound (keep the most recent output).
    static const int MAX_LOG_CHARS = 40000;
    if (m_logText.size() > MAX_LOG_CHARS)
    {
        m_logText = m_logText.right(MAX_LOG_CHARS);
    }

    emit logTextChanged();
}

void ImportProgressModel::onRunnerRetrying()
{
    //! FIX 1: the #711 edge-probe crash was hit and the runner is re-running on a white-margin-padded copy -- a full
    //! SECOND recognition pass. Surface a persistent banner so the restart reads as a recovery step, not a silent
    //! duplicate run. The em-dash is injected via QChar(0x2014) to keep this source ASCII (matching this file's idiom).
    const QString dash = QString(QChar(0x2014));
    m_retryNotice = muse::qtrc("reprise", "A page had trouble %1 retrying to recover your music...").arg(dash);
    emit retryNoticeChanged();
}

QString ImportProgressModel::formatPagesLabel(const QString& pages) const
{
    const QString trimmed = pages.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("all", Qt::CaseInsensitive) == 0)
    {
        return muse::qtrc("reprise", "all pages");
    }

    //! Display hyphenated ranges with an en-dash ("1-3" -> "1<U+2013>3"); comma lists are left as-is.
    QString display = trimmed;
    display.replace('-', QChar(0x2013));

    return muse::qtrc("reprise", "pages %1").arg(display);
}

void ImportProgressModel::updateDoneLine()
{
    const int pages = m_runner ? m_runner->pageCount() : m_totalPages;

    int measures = 0;
    const notation::IMasterNotationPtr master = projectSession()->masterNotation();
    if (master && master->masterScore())
    {
        measures = static_cast<int>(master->masterScore()->nmeasures());
    }

    if (measures > 0 && pages > 0)
    {
        const QString measuresText = (measures == 1)
                                     ? muse::qtrc("reprise", "%1 measure").arg(measures)
                                     : muse::qtrc("reprise", "%1 measures").arg(measures);
        const QString pagesText = (pages == 1)
                                  ? muse::qtrc("reprise", "%1 page").arg(pages)
                                  : muse::qtrc("reprise", "%1 pages").arg(pages);

        //: Done line, e.g. "Done - recognized 21 measures across 3 pages." (%1 = measures, %2 = pages); the dash is an em-dash.
        m_stageText = muse::qtrc("reprise", "Done") + " " + QChar(0x2014) + " "
                      + muse::qtrc("reprise", "recognized %1 across %2").arg(measuresText).arg(pagesText);
    }
    else
    {
        m_stageText = muse::qtrc("reprise", "Done");
    }

    emit progressChanged();
}

void ImportProgressModel::onRunnerFinished(bool ok, const QString& outputPath, const QString& error, const QString& tail)
{
    if (!ok)
    {
        //! FIX 2: route failure to the dedicated "Recognition failed" window instead of showing it in-place. Record the
        //! message + a tail of Audiveris output in the snapshot, then ask the app-lifetime controller to close this popup
        //! and open the error window (it reads the snapshot back). The popup never doubles as the error view.
        const QString message = error.isEmpty() ? muse::qtrc("reprise", "Audiveris could not recognise this scan.") : error;
        const QString fullLog = m_runner ? m_runner->fullLog() : tail;
        const QString logPath = m_runner ? m_runner->logFilePath() : QString();
        importProgressTracker()->reportFinished(false, message.toStdString(), fullLog.toStdString(), logPath.toStdString());
        importProgressTracker()->requestErrorWindow();
        return;
    }

    //! Reprise degenerate-scan guard: a scan that recognised no usable parts yields MusicXML with zero <score-part>
    //! entries. Opening it lays out a score with zero visible parts, which the engraving instrument-name layout has
    //! no valid position to compute. Refuse GRACEFULLY here with a clear message (surfaced in the popup AND via
    //! reprise.get-import-errors) instead. A count of -1 means "could not inspect the file" -> fail open (proceed to
    //! open; the engraving layout guard still prevents a hard crash if such a score ever reaches it).
    const int partCount = recognizedPartCount(outputPath);
    if (partCount == 0)
    {
        //! FIX 2: degenerate scan (no recognised parts) -> the dedicated error window, not an in-place error state.
        const QString message = muse::qtrc("reprise", "No music could be recognised in this scan. Try a clearer or higher-resolution scan.");
        const QString fullLog = m_runner ? m_runner->fullLog() : QString();
        const QString logPath = m_runner ? m_runner->logFilePath() : QString();
        importProgressTracker()->reportFinished(false, message.toStdString(), fullLog.toStdString(), logPath.toStdString());
        importProgressTracker()->requestErrorWindow();
        return;
    }

    //! Capture the skip-invalid result from the runner NOW (before the score opens): the pages Audiveris could not read (and
    //! why), and whether the survivors were split into movements. Surfaced later by surfacePartialImportNoticeIfAny() at the
    //! Done site, so the "Import finished" notice opens only after the score is confirmed open.
    if (m_runner)
    {
        m_skippedPages = m_runner->skippedPages();
        m_multiMovement = m_runner->producedMultipleMovements();
        //! PART 2 smart-deskew ladder: capture whether OCR was dropped by the notes-only retry, so the Done site surfaces the
        //! "imported the notes only" notice.
        m_textDropped = m_runner->droppedTextOnRetry();
    }

    //! Recognition succeeded: show a full determinate bar and open the score. The score is NOT "Done" until it actually
    //! opens, so keep the status "running" with an interim "Opening score..." line -- the popup then neither flashes the
    //! gold check nor auto-closes before the open resolves. The Done state (with the real measure + page counts filled
    //! by updateDoneLine) and the auto-close are finalized on the next master-notation change once the imported score
    //! becomes the current master; a declined open (score too corrupt/incomplete to open) is caught by the bounded
    //! fallback armed in openImported() and surfaced as a graceful error instead of a silent "Done".
    m_percent = 100;
    m_indeterminate = false;
    const int pages = m_runner ? m_runner->pageCount() : m_totalPages;
    if (pages > 0)
    {
        m_totalPages = pages;
        m_currentPage = pages;
    }
    m_stageText = muse::qtrc("reprise", "Opening score...");
    emit progressChanged();

    openImported(outputPath);
}

void ImportProgressModel::openImported(const QString& outputPath)
{
    //! Reprise Two-Pane Proofing (Phase A1): capture the scan<->score coordinate map from Audiveris's .omr book NOW,
    //! while the recognition temp dir still exists and before the score opens. Keyed by global measure index (positional,
    //! 0-based, pickup included) -> its pixel rect on the scan page; the read-only reprise.get-proofing-map MCP query and
    //! (later, A2/A3) the top strip + follow loop consume it. BEST-EFFORT: any failure leaves no map (the proofing view
    //! simply stays unavailable) and never blocks the import. It uses the Audiveris .omr geometry, independent of the
    //! scan-geometry strip below (which only affects the MuseScore score's own re-engraved layout).
    if (m_runner)
    {
        const QString omrPath = m_runner->omrPath().toQString();
        if (!omrPath.isEmpty())
        {
            //! Pass the pre-strip Audiveris MusicXML (outputPath) too, so the per-NOTE head boxes are captured
            //! alongside the measure rects (Phase B1): its note order == the score about to open (the strip below
            //! preserves every musical value), giving each note box a runtime-computable key for B2's magnifier.
            //! PART 1 LIVE PROOFING: the band/magnifier must render the page the boxes are actually in -- that is the
            //! input Audiveris consumed (the deskewed PDF when a page was straightened, else the original), NOT the
            //! literal m_sourcePath. consumedInputPath() is exactly that post-preprocessing input; fall back to
            //! m_sourcePath only if it is somehow empty. (The .repr save path below is PART 2 and still uses m_sourcePath.)
            const QString scanPath = m_runner->consumedInputPath().toQString();
            const QString proofingSource = scanPath.isEmpty() ? m_sourcePath : scanPath;
            const ProofingMap pmap = ProofingMapBuilder::buildFromOmr(omrPath, proofingSource, m_dpi, outputPath);
            if (pmap.valid)
            {
                proofingMapService()->setMap(pmap);
                ProofingMapBuilder::logMap(pmap);
            }
            else
            {
                LOGW() << "Reprise: proofing-map capture produced no measures from " << omrPath;
            }
        }
        else
        {
            LOGW() << "Reprise: proofing-map capture skipped; no .omr book found for this import";
        }
    }

    //! Reprise scan-geometry strip: Audiveris bakes the SCAN's layout into its MusicXML (per-measure widths,
    //! system/page breaks, absolute positions, page/system/staff layout, slur beziers) and emits the title as
    //! scan-positioned <credit>s that overprint the metadata-built title frame -- so MuseScore mirrors the scan
    //! (stretched sparse bars, a doubled title) instead of engraving its own clean layout. Post-process the produced
    //! MusicXML to strip that geometry + de-duplicate the title credits, then open the cleaned SIBLING file instead.
    //! Applied UNCONDITIONALLY to every import. SAFE: on ANY failure stripToSibling returns empty and we fall back to
    //! opening the ORIGINAL unmodified file (a layout nicety must never block an import).
    QString pathToOpen = outputPath;
    const QString strippedPath = ScanGeometryStripper::stripToSibling(outputPath);
    if (!strippedPath.isEmpty())
    {
        pathToOpen = strippedPath;
        LOGI() << "Reprise: opening scan-geometry-stripped MusicXML: " << strippedPath;
    }

    else
    {
        LOGW() << "Reprise: scan-geometry strip failed; opening original unmodified file: " << outputPath;
    }

    //! Remember the temp file actually opened: the open adds it to the recent list, and the .repr auto-save removes it
    //! again in favour of the saved .repr.
    m_strippedOpenPath = pathToOpen;

    //! Hand the produced MusicXML to MuseScore's native open path; a .mxl/.xml routes through MusicXmlReader.
    //! Arm the Grand-Piano override and the Done-line measure count for the score about to open, mark the open as
    //! awaited, then open it.
    m_pendingGrandPianoReset = true;
    m_pendingDoneCounts = true;
    m_awaitingOpen = true;

    //! Was a score already open in THIS instance before this import opened? If so, MuseScore routes the open to a SEPARATE
    //! instance (OpenProjectScenario::openProject step 4 -> openNewWindow -> QProcess::startDetached), so setProject() is
    //! never called here and masterNotationChanged never fires in this instance -- the only signal the in-window success
    //! path relies on. That case is finalized off the resolved promise below (finalizeOpenedInNewWindow). When no score was
    //! open, the import opens in-window, setProject() fires masterNotationChanged synchronously, and that handler finalizes
    //! Done (Grand-Piano override + real counts) exactly as before -- so the promise path leaves it alone.
    const bool hadProjectBefore = (projectSession()->project() != nullptr);

    //! Open through MuseScore's own open scenario and OWN the resulting promise, so the REAL open outcome is visible
    //! here. The equivalent command://project/open dispatch swallows that outcome (its dispatch Ret is always ok --
    //! ProjectActionsController::runAsync discards the async result), which is why 0.12.3 had to INFER failure from a
    //! fixed wall-clock timer. A large multi-sheet score can take far longer to open than any fixed window (the #711
    //! pad+retry alone is two Audiveris passes), so that 8s timer false-flagged slow-but-valid opens as failures. With
    //! the promise owned, a declined open (MusicXML too corrupt/incomplete for the reader to accept -> an error Ret, or
    //! the user declining the force-load prompt) reports its error the INSTANT it resolves -- no race -- while a
    //! successful open is still finalized by masterNotationChanged (Grand-Piano override + Done counts). A slow open is
    //! simply a promise that has not resolved yet, so it can never be mistaken for a failure however long it takes.
    const QUrl url = QUrl::fromLocalFile(pathToOpen);
    openProjectScenario()->openProject(muse::rcommand::Params{ { "url", muse::Val(url.toString()) } })
    .onResolve(this, [this, hadProjectBefore](const muse::Ret& ret)
    {
        //! A genuine open failure: reportOpenFailed() (it re-checks master-presence, so an ok-looking resolve can never
        //! mis-flag a score that is in fact present).
        if (!ret)
        {
            reportOpenFailed();
            return;
        }

        //! Open succeeded. In-window (no score was open): setProject() already fired masterNotationChanged synchronously,
        //! which finalized Done and cleared m_awaitingOpen -- nothing to do here. New-window (a score was already open):
        //! the import opened in a SEPARATE instance, masterNotationChanged never fires here, so finalize Done off this
        //! resolved promise. m_awaitingOpen keeps it exactly-once (and no-ops if the in-window handler already finalized).
        if (hadProjectBefore && m_awaitingOpen)
        {
            finalizeOpenedInNewWindow();
        }
    });

    //! Backstop ONLY (see OPEN_BACKSTOP_TIMEOUT_MS): the owned promise above is the normal success/failure signal. This
    //! generous, success-aware deadline exists solely for the pathological case where the promise resolves ok yet no
    //! master ever becomes current, so the popup can never hang on the interim "Opening score..." forever. Deliberately
    //! far longer than any real open, so it can never race a legitimately slow import.
    QTimer::singleShot(OPEN_BACKSTOP_TIMEOUT_MS, this, [this]() { reportOpenFailed(); });

    return;
}

void ImportProgressModel::autoSaveImportedAsRepr()
{
    const project::INotationProjectPtr project = projectSession()->project();
    if (!project || m_sourcePath.isEmpty())
    {
        return;
    }

    //! Target folder = the user's Scores folder; fall back to the scan's own folder if it is not configured.
    QString dir = projectConfiguration() ? projectConfiguration()->userProjectsPath().toQString() : QString();
    if (dir.isEmpty())
    {
        dir = QFileInfo(m_sourcePath).absolutePath();
    }
    QDir().mkpath(dir);

    //! De-dupe the name so repeated imports of the SAME scan never clobber an existing bundle: "<scan>.repr", then
    //! "<scan> (2).repr", "<scan> (3).repr", ... . The .repr suffix is defined canonically in project/internal/reprfile.h.
    const QString base = QFileInfo(m_sourcePath).completeBaseName();
    QString candidate = dir + "/" + base + ".repr";
    for (int n = 2; QFileInfo::exists(candidate); ++n)
    {
        candidate = dir + "/" + base + " (" + QString::number(n) + ").repr";
    }

    //! Build the manifest "import" provenance object (feeds the backlogged recent-list page-range display).
    muse::JsonObject import;
    import.set("pages", (m_pages.trimmed().isEmpty() ? QString("all") : m_pages).toStdString());
    import.set("dpi", m_dpi);
    import.set("recognizeText", m_recognizeText);
    import.set("language", m_language.toStdString());
    import.set("chordSymbols", m_chordSymbols);
    import.set("lyricsAboveStaff", m_lyricsAboveStaff);
    const muse::ByteArray importBytes = muse::JsonDocument(import).toJson(muse::JsonDocument::Format::Compact);
    const std::string importJson(importBytes.constChar(), importBytes.size());

    const QString suffix = QFileInfo(m_sourcePath).suffix().toLower();

    //! PART 2 dual-storage: also hand the deskewed render Audiveris actually consumed, so the .repr stores BOTH the
    //! literal original (m_sourcePath) AND the straightened render -- a reopen then reproduces the aligned band. Only when
    //! a page was really deskewed (consumedInputPath != m_sourcePath); otherwise the proofing-scan args stay empty and the
    //! bundle is byte-identical to today (original only).
    const QString consumed = m_runner ? m_runner->consumedInputPath().toQString() : m_sourcePath;
    const bool deskewed = (!consumed.isEmpty() && consumed != m_sourcePath);
    const io::path_t proofingScanPath = deskewed ? io::path_t(consumed) : io::path_t();
    const std::string proofingScanSuffix = deskewed ? QFileInfo(consumed).suffix().toLower().toStdString() : std::string();

    project->setReprBundleInfo(io::path_t(m_sourcePath), suffix.toStdString(), importJson,
                               proofingScanPath, proofingScanSuffix);

    const io::path_t reprPath(candidate);
    const muse::Ret ret = project->save(reprPath, project::SaveMode::SaveAs);
    if (!ret)
    {
        LOGW() << "Reprise: auto-save as .repr failed: " << ret.toString();
        return;
    }

    //! Point the recent list at the saved .repr (replace the throwaway stripped-MusicXML temp the open added).
    if (recentFilesController())
    {
        if (!m_strippedOpenPath.isEmpty())
        {
            recentFilesController()->removeRecentFile(io::path_t(m_strippedOpenPath));
        }
        recentFilesController()->prependRecentFile(project::RecentFile(reprPath));
    }

    LOGI() << "Reprise: imported scan auto-saved as .repr: " << candidate;

    return;
}

void ImportProgressModel::reportOpenFailed()
{
    if (!m_awaitingOpen)
    {
        return;   //! the open already resolved to success (masterNotationChanged finalized it) or failure fired once
    }

    if (projectSession()->masterNotation())
    {
        return;   //! a score is present -> the open succeeded; let the success path finalize it
    }

    //! Definitively no score opened -- refuse GRACEFULLY, consistent with the zero-part guard (status = error +
    //! reprise.get-import-errors), never a silent "Done".
    m_awaitingOpen = false;
    m_pendingGrandPianoReset = false;
    m_pendingDoneCounts = false;

    //! FIX 2: the produced score never opened -> the dedicated error window, not a silent "Done" and not an in-place error.
    const QString message = muse::qtrc("reprise", "Reprise could not produce a usable score from this scan - it may be too "
                                                  "incomplete or damaged to open.");
    importProgressTracker()->reportFinished(false, message.toStdString(), std::string());
    importProgressTracker()->requestErrorWindow();

    return;
}

void ImportProgressModel::finalizeOpenedInNewWindow()
{
    if (!m_awaitingOpen)
    {
        return;   //! already finalized (in-window success path) or failed once
    }

    //! The imported score opened in a SEPARATE instance (see finalizeOpenedInNewWindow's declaration). The current master
    //! in THIS instance is still the previously-open score, so the armed Grand-Piano override and the measure-count read
    //! must NOT run here -- they would mutate / report the wrong score. Drop both pending overrides and show a plain "Done".
    m_awaitingOpen = false;
    m_pendingGrandPianoReset = false;
    m_pendingDoneCounts = false;
    m_stageText = muse::qtrc("reprise", "Done");
    emit progressChanged();
    setStatus("done");
    importProgressTracker()->reportFinished(true, std::string(), std::string());

    //! "Import finished" notice (new-window open path): same as the in-window Done site -- surface skipped pages now that the
    //! import's open has resolved successfully. No-op when nothing was skipped.
    surfacePartialImportNoticeIfAny();

    //! Only a CLEAN import uses the checkbox-governed auto-close; a notice-worthy import is closed by the controller as
    //! part of showing the notice, so a second auto-close here would race that teardown (the ticked-path suppression bug).
    const bool noticeWorthy = !m_skippedPages.isEmpty() || m_multiMovement || m_textDropped;
    if (!noticeWorthy && closePopupOnComplete())
    {
        //! Same settled auto-close as the in-window path: ask the app-lifetime controller to close + settle this popup's
        //! own window after the Done dwell (never a self-close, which races teardown and double-delivers onClose -> SIGSEGV).
        QTimer::singleShot(AUTO_CLOSE_DELAY_MS, this, [this]() { importProgressTracker()->requestPopupClose(); });
    }

    return;
}

int ImportProgressModel::recognizedPartCount(const QString& outputPath) const
{
    QByteArray xml;

    const muse::io::path_t path(outputPath);
    const std::string suffix = muse::io::suffix(path);
    if (suffix == "mxl")
    {
        //! A .mxl is a ZIP: the real MusicXML is the rootfile named in META-INF/container.xml -- the same extraction
        //! MuseScore's own MusicXML importer uses (importmusicxml.cpp::extractRootfile).
        muse::ZipReader zip(path);
        if (!zip.exists() || zip.hasError())
        {
            return -1;
        }

        const muse::ByteArray container = zip.fileData("META-INF/container.xml");
        std::string rootfile;
        muse::XmlStreamReader e(container);
        while (e.readNextStartElement())
        {
            if (e.name() != "container")
            {
                break;
            }

            while (e.readNextStartElement())
            {
                if (e.name() != "rootfiles")
                {
                    break;
                }

                while (e.readNextStartElement())
                {
                    if (e.name() == "rootfile")
                    {
                        rootfile = e.attribute("full-path").toStdString();
                        break;
                    }
                }
            }
        }

        if (rootfile.empty())
        {
            return -1;
        }

        xml = zip.fileData(rootfile).toQByteArray();
    }
    else
    {
        QFile f(outputPath);
        if (!f.open(QIODevice::ReadOnly))
        {
            return -1;
        }

        xml = f.readAll();
        f.close();
    }

    if (xml.isEmpty())
    {
        return -1;
    }

    //! Count <score-part id=...> declarations (the <part-list> entries). "<score-partwise" has no space after
    //! "score-part", so the root element is never miscounted; every real <score-part> carries its id attribute first.
    return static_cast<int>(xml.count("<score-part id"));
}

void ImportProgressModel::setStatus(const QString& status)
{
    if (status == m_status)
    {
        return;
    }

    m_status = status;
    emit statusChanged();
}

void ImportProgressModel::applyGrandPianoDefault()
{
    //! TEMPORARY Reprise default. Audiveris assigns semi-random instruments to recognised parts; until an intake
    //! step adds real instrument selection, force every imported part to Grand Piano. Mirrors the original
    //! ImportPdfController implementation. Remove when real instrument choice lands.
    static const muse::String GRAND_PIANO_ID(u"grand-piano");

    const notation::InstrumentTemplate& templ = instrumentsRepository()->instrumentTemplate(GRAND_PIANO_ID);
    if (templ.id.isEmpty())
    {
        LOGW() << "Reprise: 'grand-piano' instrument template not found; leaving imported instruments as-is";
        return;
    }

    const notation::IMasterNotationPtr master = projectSession()->masterNotation();
    if (!master)
    {
        return;
    }

    const notation::INotationPartsPtr parts = master->parts();
    if (!parts)
    {
        return;
    }

    //! Snapshot the keys first: each replaceInstrument() mutates the score and notifies part changes.
    std::vector<notation::InstrumentKey> keys;
    for (const engraving::Part* part : parts->partList())
    {
        if (!part)
        {
            continue;
        }

        notation::InstrumentKey key;
        key.partId = part->id();
        key.instrumentId = part->instrumentId();
        key.tick = engraving::Part::MAIN_INSTRUMENT_TICK;
        keys.push_back(key);
    }

    const engraving::Instrument grandPiano = engraving::Instrument::fromTemplate(&templ);
    for (const notation::InstrumentKey& key : keys)
    {
        parts->replaceInstrument(key, grandPiano);
    }

    LOGI() << "Reprise: defaulted " << keys.size() << " imported part(s) to Grand Piano";
}

void ImportProgressModel::surfacePartialImportNoticeIfAny()
{
    //! Nothing skipped, survivors not split, and OCR was not dropped -> no notice (the ordinary silent success).
    if (m_skippedPages.isEmpty() && !m_multiMovement && !m_textDropped)
    {
        return;
    }

    //! Build the blessed per-page report, one "Page N: <reason> -- skipped" line per skipped page. The em-dash is injected as
    //! %2 (QChar(0x2014)) to keep this source ASCII, matching the existing QChar(0x2014) idiom in this file. The interior
    //! "some pages in the middle..." paragraph is NOT prepended here: it is rendered as its own paragraph in the notice QML,
    //! gated on the multiMovement flag passed below, so it never double-renders alongside this per-page list.
    const QString dash = QString(QChar(0x2014));
    QStringList lines;
    for (const QPair<int, QString>& p : m_skippedPages)
    {
        const int page = p.first;
        const QString& reason = p.second;
        if (reason == "low-res")
        {
            lines << muse::qtrc("reprise", "Page %1: resolution too low %2 skipped").arg(page).arg(dash);
        }
        else if (reason == "too-large")
        {
            lines << muse::qtrc("reprise", "Page %1: too large %2 skipped").arg(page).arg(dash);
        }
        else
        {
            //! "no-music" (and any unrecognised code) -> cover/title page or staves not found.
            lines << muse::qtrc("reprise", "Page %1: no music / cover %2 skipped").arg(page).arg(dash);
        }
    }

    const QString report = lines.join('\n');
    //! PART 2 smart-deskew ladder: m_textDropped carries the "OCR was dropped by the notes-only retry" state into the notice,
    //! so it can show the "couldn't read the text/lyrics -- imported the notes only" line (with or without any skipped pages).
    importProgressTracker()->reportPartialImport(report.toStdString(), m_multiMovement, m_textDropped);
    importProgressTracker()->requestNoticeWindow();

    return;
}
