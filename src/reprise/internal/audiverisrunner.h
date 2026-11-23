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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QImage>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QStringList>
#include <QString>

#include "modularity/ioc.h"
#include "global/io/path.h"

#include "irepriseconfiguration.h"
#include "ilanguagepackservice.h"

class QProcess;
class QPdfDocument;

namespace mu::reprise {
//! Per-import recognition options collected from the intake dialog's "Options" panel (or the headless/MCP args),
//! turned into Audiveris '-constant FQN.field=value' switches by buildTextConstants(). recognizeText is the master:
//! OFF skips the whole TEXTS/OCR step (notes-only). The defaults mirror Audiveris's own defaults (lyrics on, chords
//! off, lyrics-below-staff, eng), so the default path passes NO text constants at all and the proven OCR behavior
//! is preserved untouched.
struct RecognitionOptions
{
    bool recognizeText = true;       //! master: recognize text & lyrics (OCR). OFF -> useOCR=false, notes only.
    QString language = "eng";        //! Tesseract language code (eng default); '+'-joined accepted for multi-language.
    bool chordSymbols = false;       //! detect chord symbols -> ProcessingSwitches.chordNames=true
    bool lyricsAboveStaff = false;   //! lyrics sit above the staff (uncommon) -> ProcessingSwitches.lyricsAboveStaff=true
};

//! NOTE: Reprise's single, local OMR path. Drives the Audiveris CLI on a whole PDF ASYNCHRONOUSLY via a raw
//! Qt QProcess in signal mode (start() + readyReadStandardOutput + finished), so a live progress popup and a
//! working Cancel are possible without blocking the UI thread. Progress is parsed out of Audiveris' own stdout
//! (StepMonitoring stages + the "N sheets" total + the "#<sheet>" index). On success it reports the produced
//! MusicXML (.mxl, else .xml) to hand to MuseScore's native import.
class AudiverisRunner : public QObject, public muse::Contextable
{
    Q_OBJECT

public:
    explicit AudiverisRunner(const muse::modularity::ContextPtr& iocCtx, QObject* parent = nullptr)
        : QObject(parent), muse::Contextable(iocCtx) {}

    //! Out-of-line so m_deskewPdf (unique_ptr<QPdfDocument>, forward-declared in this header) is destroyed in the .cpp
    //! where QPdfDocument is a complete type -- importprogressmodel.cpp holds a shared_ptr<AudiverisRunner> but never
    //! sees QtPdf, so the implicit destructor cannot be emitted there.
    ~AudiverisRunner() override;

    //! Locate the Audiveris executable: the configured override (if set and still present) wins, else the detection
    //! search order ported from the C# Reprise Recognizer. Empty if not found.
    muse::io::path_t findAudiveris() const;

    //! True once start() has launched a process that has not yet finished/been cancelled.
    bool isRunning() const;

    //! Begin an async Audiveris run. pages: free-form range ("all"/"" = every page, "3-7", "1,4,6-9");
    //! dpi: scan resolution passed to Audiveris; options: the per-import recognition switches (text/lyric OCR,
    //! language, chord symbols, lyrics-above-staff) turned into '-constant' args by buildTextConstants(). Progress
    //! arrives via progressChanged(); the outcome via finished(). Returns false (and emits no signals) only if
    //! Audiveris itself could not be found.
    bool start(const muse::io::path_t& pdfPath, const QString& pages, int dpi, const RecognitionOptions& options);

    //! The OCR text languages installed in Reprise's shipped tessdata folder (the same path findRepriseTessdata()
    //! resolves), as (code, friendly-display-name) pairs for the intake dialog's language dropdown. 'osd'
    //! (orientation/script detection, not a text language) is EXCLUDED. A code with no known friendly name falls
    //! back to the raw code. Empty when the shipped tessdata folder is missing or holds no language packs.
    std::vector<std::pair<std::string, std::string> > installedOcrLanguages() const;

    //! PART 2 smart-deskew ladder: tell the runner the user's RAW original scan + whether THIS run's input is the
    //! pre-deskewed render, so onFinished()'s retry-raw ladder (LADDER A) can re-run on the raw original (which Audiveris
    //! self-deskews) after a deskewed-input failure. Called by the model before each recognition run; the internal ladder
    //! retries manage the deskewed flag themselves. A no-op for the #711 pad re-entry (which preserves this context).
    void setRetryContext(const muse::io::path_t& rawOriginal, bool inputIsDeskewed);

    //! True when the import succeeded only after the ladder (LADDER B) dropped OCR and re-ran notes-only -- so the model
    //! surfaces the "couldn't read the text/lyrics -- imported the notes only" notice. Valid after a successful finished(ok=true).
    bool droppedTextOnRetry() const { return m_droppedTextOnRetry; }

    //! Kill the running Audiveris process and abandon the run. finished() is NOT emitted after a cancel().
    void cancel();

    //! === PART 1: per-DOCUMENT DESKEW (pre-Audiveris) ========================================================
    //! A projection-profile skew ENGINE plus a CHUNKED pipeline the progress model drives one page per event-loop turn,
    //! so the popup stays responsive and Cancel works mid-straighten. The gate is per-DOCUMENT all-or-nothing: the chunked
    //! detect pass (deskewStep) rasterizes each SELECTED page at the run DPI and measures its OWN raw skew; finishDeskew then
    //! looks at the document's MAX page angle and either straightens EVERY page (each by its own angle, with a small apply
    //! floor so near-straight pages are harmless no-ops) or feeds EVERY page RAW. A multi-page scan is therefore never left
    //! half-straightened. When the document is not tilted enough finishDeskew() returns empty so the ORIGINAL is fed byte-
    //! identical; the straightened pages are otherwise re-emitted as ONE multi-page PDF fed to start().

    //! Estimate a single page's skew (degrees) by projection-profile variance: downsample to <=~1000 px, binarize to an
    //! ink mask, and for theta in [-6,+6] deg (0.2 deg steps) project the ink onto the rotated vertical axis and take the
    //! row-sum VARIANCE -- staff lines peak (max variance) when level. A pure MEASUREMENT primitive: returns the RAW measured
    //! angle (the per-document trigger + per-page apply floor are decided by finishDeskew, NOT here), except it returns 0.0 for
    //! an out-of-range |best| beyond kSkewMaxDeg (never apply a wild angle). Sign matches deskewImage()/QTransform.
    static double detectSkewAngle(const QImage& gray);

    //! Rotate a page by angleDeg (QTransform().rotate, smooth) composited onto a WHITE canvas (no black corners) plus the
    //! standard #711 white margin, so a straightened page keeps the same edge-crash protection as the pad paths.
    static QImage deskewImage(const QImage& src, double angleDeg);

    //! Open `input` (PDF via QPdfDocument, else a raster image via QImageReader), resolve the SELECTED page indices
    //! (expandSheets semantics; all pages when none specified), and arm the chunked state. Returns true when there is at
    //! least one page to process (the model then drives deskewStep()/finishDeskew()); false -> the caller skips straight to
    //! start() on the original. Does NOT launch Audiveris.
    bool beginDeskew(const muse::io::path_t& input, const QString& pages, int dpi);

    //! Number of SELECTED pages beginDeskew() armed (what the chunk loop iterates).
    int deskewPageCount() const { return static_cast<int>(m_deskewPages.size()); }

    //! DETECT PASS for the i-th SELECTED page: render it at the run DPI, measure its OWN raw skew (detectSkewAngle), store the
    //! raw render + its raw angle, and emit preprocessing(...). Does NOT rotate -- the apply is deferred to finishDeskew so the
    //! per-document max-angle decision can be made once every page has been measured.
    void deskewStep(int i);

    //! Make the per-DOCUMENT decision, then assemble. If the document's MAX measured page angle is below
    //! kSkewPreDeskewThresholdDeg, return EMPTY so the caller feeds the ORIGINAL untouched (regression no-op). Otherwise
    //! straighten EVERY page -- each by its own measured angle, a page below kSkewApplyFloorDeg carried as a padded no-op --
    //! assemble them into ONE multi-page PDF (page sized px*72/dpi, order preserved), and return its path. m_deskewAngles is
    //! rewritten to the angle actually APPLIED per page (0.0 where none) for PART 2's .repr provenance.
    muse::io::path_t finishDeskew();

    //! Per-SELECTED-page skew angles (degrees) ACTUALLY APPLIED, in page order: 0.0 for a page left untouched and 0.0 for every
    //! page when the document was fed raw. Valid only after finishDeskew(); used for PART 2's .repr provenance.
    std::vector<double> deskewAnglesDeg() const { return m_deskewAngles; }

    //! The input Audiveris actually recognized after any preprocessing (deskew + #711 pad) -- i.e. m_pdfPath. The
    //! Two-Pane Proofing coordinate map is built against THIS, so the band/magnifier render the page the boxes are in.
    muse::io::path_t consumedInputPath() const { return m_pdfPath; }

    //! Number of pages recognised in this run: the count of explicitly requested sheets, or the whole-book total
    //! (parsed from Audiveris' output) when every page was processed. Valid once the run has reported a total.
    int pageCount() const { return m_requestedSheetCount > 0 ? m_requestedSheetCount : m_totalSheets; }

    //! The Audiveris .omr book produced by the last run (under the run's output dir), or empty if none is found. Audiveris
    //! writes it via '-save'; it holds the per-sheet pixel geometry the Two-Pane Proofing coordinate map is built from.
    //! Valid after a successful finished(ok=true); the temp output dir persists (autoRemove off) so it is readable then.
    muse::io::path_t omrPath() const;

    //! The complete, un-truncated Audiveris output (stdout + stderr, in arrival order) for this run, joined with '\n'. Unlike
    //! the short error tail, nothing is dropped -- this is what the error window shows in full and what Copy copies.
    QString fullLog() const { return m_fullLog.join('\n'); }

    //! The pages Audiveris could not recognise on this run, as (1-based sheet number -> reason code) pairs in ascending
    //! sheet order. Reason codes are "no-music" (cover/title page or staves not found), "low-res" (interline too small for
    //! the staff size), "too-large" (image over Audiveris's per-sheet pixel cap). Populated from the per-sheet failure
    //! signatures scanned in pushTail(); empty when every requested page recognised. Read after a successful finished(ok=true)
    //! by the "Import finished" notice (the valid pages still imported; this lists which pages were skipped).
    QList<QPair<int, QString> > skippedPages() const;

    //! True when the (re-export) pass produced more than one MusicXML movement -- i.e. survivor pages were non-contiguous
    //! and Audiveris split them into separate movements, so only part of the music could be combined into one score. Valid
    //! after a successful finished(ok=true) that went through the re-export path.
    bool producedMultipleMovements() const { return m_multiMovement; }

    //! The on-disk copy of the full log (reprise-audiveris.log under the run's output dir), written on finish/error. Empty
    //! if it could not be written; the error window's "Open log file" button is enabled only when this is non-empty.
    QString logFilePath() const { return m_logFilePath; }

signals:
    //! percent: 0-100 determinate, or -1 while the sheet total is still unknown (indeterminate/animated bar).
    //! sheet/total: 1-based current sheet and the total (both -1 until known). stageHuman: a friendly, already
    //! localised-at-source step label (e.g. "detecting staves...").
    void progressChanged(int percent, int sheet, int total, const QString& stageHuman);

    //! ok=true -> outputPath is the produced .mxl/.xml. ok=false -> error is a short message and tail holds the
    //! last lines of Audiveris output (stdout+stderr) for surfacing in the popup's error state.
    void finished(bool ok, const QString& outputPath, const QString& error, const QString& tail);

    //! One line of Audiveris output (stdout or stderr) as it streams, for the popup's recognition-log expander.
    void outputLine(const QString& line);

    //! FIX 1: emitted when the #711 edge-probe crash was hit and a SECOND recognition pass is about to begin on a
    //! white-margin-padded copy of the PDF. Lets the progress UI surface WHY recognition restarts, so the full double
    //! pass is explained rather than looking like a silent duplicate run. Fires at most once per import (guarded by the
    //! same m_retriedWithPad latch that gates the pad retry).
    void retryingAfterError();

    //! PART 1 deskew progress: the current page (1-based) of total being straightened, plus an already-localised stage
    //! label ("Straightening the scan..." when total==1, else "Straightening page N of M..."). Emitted once per
    //! deskewStep(); the model mirrors it into the stage text.
    void preprocessing(int page, int total, const QString& stageHuman);

private:
    muse::GlobalInject<mu::reprise::IRepriseConfiguration> repriseConfiguration;
    muse::GlobalInject<mu::reprise::ILanguagePackService> languagePackService;

    //! Expand a free-form page range into explicit 1-based sheet numbers for Audiveris '-sheets'. Empty = all pages.
    std::vector<int> expandSheets(const QString& pages) const;

    //! Turn the recognition options into the Audiveris '-constant FQN.field=value' switch tokens (a flat list ready to
    //! append to the command: "-constant", "org....=value", ...). The SINGLE place both the GUI and the headless/MCP
    //! paths converge on, since both reach start() with a RecognitionOptions. Master ON at defaults -> empty (no text
    //! constants, preserving Audiveris's own defaults); master OFF -> only useOCR=false (and nothing else).
    std::vector<std::string> buildTextConstants(const RecognitionOptions& options) const;

    //! Reprise's OWN shipped tessdata folder beside the running executable (<exeDir>/tessdata), resolved at runtime.
    //! It holds the legacy+LSTM combined traineddata Audiveris needs for its LEGACY-mode OCR init, and is the SOLE
    //! authoritative tessdata source (Reprise ships + manages the data). Empty if the shipped folder is missing or
    //! holds no *.traineddata -- which is logged as an error (never silently falls back to a wrong system model).
    muse::io::path_t findRepriseTessdata() const;

    //! Search a directory (recursively) for the first file named <base>.<ext>, else any *.<ext>. Empty if none.
    muse::io::path_t locateOutput(const muse::io::path_t& dir, const std::string& base, const std::string& ext) const;

    //! Launch a minimal SECOND Audiveris pass that re-exports the already-recognised .omr book, SKIPPING the pages that
    //! failed the first run, so the valid pages still import. Reuses the first run's output dir (keeps m_fullLog +
    //! m_invalidSheetReasons), wires the SAME QProcess signals + TESSDATA_PREFIX as start(), and sets m_reExportedOmr so
    //! onFinished() handles the re-export outcome. Returns false (launching nothing) only if Audiveris or omrPath is empty.
    bool startOmrReExport(const muse::io::path_t& omrPath);

    //! All files matching *.<ext> anywhere under dir (recursive), sorted ascending by path. Used to collect the possibly
    //! multiple MusicXML movements the re-export pass emits when the survivor pages were non-contiguous; mirrors locateOutput().
    std::vector<muse::io::path_t> locateAllOutputs(const muse::io::path_t& dir, const std::string& ext) const;

    //! True if `suffix` (io::suffix: lower-case, no dot) is a SINGLE-frame raster image this Qt kit can decode, so it is
    //! eligible for the always-on single-image white-margin pad (padImageToTemp). TIFF is handled separately by
    //! padTiffToTemp (a TIFF may hold many frames, which the single-frame pad would destroy -> 0.12.11).
    static bool isQtDecodableImage(const std::string& suffix);

    //! Composite a decodable raster image centered onto a larger white canvas (a fixed 100 px border) and write it to a
    //! PNG in the run's output dir; returns that padded path, or empty to feed the ORIGINAL unchanged (decode failure,
    //! or the border would push the frame past Audiveris's per-sheet pixel cap). Fixes the Audiveris #711 edge-probe
    //! crash (AreaUtil.verticalCore ArrayIndexOutOfBoundsException when a staff sits at the image edge) for image intake.
    muse::io::path_t padImageToTemp(const muse::io::path_t& imagePath) const;

    //! Pad a TIFF (now that the kit ships qtiff) with the same white margin as other images, preserving ALL frames. A
    //! single-frame TIFF takes the ordinary single-image pad (padImageToTemp, a padded PNG). A MULTI-frame TIFF is a
    //! page container like a PDF, so every frame is decoded via QImageReader, padded, and re-emitted as ONE multi-page
    //! padded PDF (Qt 6's qtiff handler cannot WRITE a multi-page TIFF -- its write() has no TIFFWriteDirectory/append --
    //! so a PDF is used, which Audiveris reads identically; the page round-trip at the run DPI preserves native pixels).
    //! Returns empty on ANY decode/write failure or an unreadable frame count, so the caller feeds the ORIGINAL unpadded
    //! TIFF (which Audiveris reads natively) -- the worst case equals today's behavior, never a dropped/reordered page.
    muse::io::path_t padTiffToTemp(const muse::io::path_t& imagePath) const;

    //! Re-rasterize a PDF to grayscale, anti-aliasing-OFF page images (matching PDFBox's ImageType.GRAY at the run DPI),
    //! pad each with a DPI-scaled white margin, and re-emit a single multi-page PDF; returns that path, or empty on any
    //! failure. The PDF retry-on-crash counterpart to the always-on image pad (same #711 fix), run at full DPI only.
    muse::io::path_t buildPaddedPdf(const muse::io::path_t& pdfPath, int32_t dpi) const;

    //! Drain + parse new stdout; drives progressChanged().
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();
    void onFinished(int exitCode);
    void onErrorOccurred();

    //! Parse a single Audiveris output line for the sheet total, the current sheet, and the current step.
    void parseLine(const QString& line);

    //! Map an Audiveris step name (LOAD/BINARY/GRID/HEADS/...) to a friendly label; raw (lowercased) as fallback.
    QString humanStage(const QString& rawStep) const;

    //! Compute 0-100 from the given (clamped) sheet + step position within the known step order; -1 if total unknown.
    int computePercent(int sheet) const;

    //! Keep only the last N lines (a small ring) for the error tail.
    void pushTail(const QString& line);

    //! Write the full accumulated log (m_fullLog) to reprise-audiveris.log under the run's output dir; records the path in
    //! m_logFilePath and returns it. Empty (and m_logFilePath left unset) if there is nothing to write or the write fails.
    QString writeFullLogToFile();

    QProcess* m_process = nullptr;
    muse::io::path_t m_pdfPath;
    QString m_outDir;
    QString m_stdoutBuffer;

    //! The exact run parameters (page range, resolution, recognition options), kept so the PDF retry-on-crash path in
    //! onFinished() can re-launch an identical run on the padded copy. m_pdfPath holds the actual input fed to Audiveris
    //! (the padded PNG for an image intake), so the retry's PDF check reads it directly.
    QString m_pages;
    int32_t m_dpi = 0;
    RecognitionOptions m_options;

    //! Set once the #711 signature (ArrayIndexOutOfBoundsException) is seen anywhere in this run's output. Scanned over
    //! EVERY line (not just the short error tail, whose ring can scroll the signature out), so the PDF retry trigger is
    //! reliable. Reset per fresh run in start(); drives the one-shot pad retry together with m_retriedWithPad.
    bool m_sawArrayIndexCrash = false;

    //! Set once Audiveris's own failure signatures are seen anywhere in this run's output, so onFinished() can turn a
    //! non-zero exit into a specific, friendly message instead of the generic "exit code N". Reset per fresh run in start().
    bool m_sawNoStaves = false;
    bool m_sawLowInterline = false;
    bool m_sawTooLarge = false;

    //! Guards the PDF pad retry so it happens at most ONCE per import: a padded re-run that still fails is reported, not
    //! re-padded. Deliberately NOT reset in start() (the retry re-enters start()), only default-initialised per runner.
    bool m_retriedWithPad = false;

    //! Guards the skip-invalid re-export so it happens at most ONCE per import: a re-export that still fails is reported, not
    //! re-exported. Deliberately NOT reset in start() (the re-export re-enters the QProcess machinery), only default-initialised
    //! per runner -- same discipline as m_retriedWithPad.
    bool m_reExportedOmr = false;

    //! === PART 2 smart-deskew ladder state ===================================================================
    //! The user's ORIGINAL scan (never a deskewed or padded copy), set by the model via setRetryContext() before the first
    //! recognition run. LADDER A retries on this so Audiveris self-deskews the raw raster when a pre-deskewed-input run failed.
    muse::io::path_t m_rawOriginalPath;

    //! Whether the CURRENT run's input is the pre-deskewed render (set by the model via setRetryContext; cleared by LADDER A
    //! before the raw retry so a further failure routes to LADDER B). Deliberately NOT reset in start() -- the ladder retries
    //! re-enter start() and must preserve this (same discipline as the one-shot guards).
    bool m_inputWasDeskewed = false;

    //! Set once the fatal Audiveris text-role NPE signature (a null TextRole on a lyric/sentence inter -> createSystemInters /
    //! TextRole.ordinal() / because "role" is null) is seen anywhere in this run's output. The rotated+padded deskew render can
    //! trigger it, crashing the whole import. Reset per fresh run in start() (a per-run signature, like m_sawArrayIndexCrash);
    //! drives LADDER B's notes-only retry.
    bool m_sawTextRoleCrash = false;

    //! One-shot guards for the smart-deskew ladder, each firing at most once per import. Deliberately NOT reset in start() (the
    //! ladder retries re-enter start()), only default-initialised per runner -- same discipline as m_retriedWithPad. LADDER A's
    //! retry on the raw original, and LADDER B's notes-only (OCR off) retry.
    bool m_retriedRaw = false;
    bool m_retriedNotesOnly = false;

    //! Set when LADDER B launched a notes-only retry (OCR dropped to clear the text-role NPE). Read by the model after a
    //! successful finish (droppedTextOnRetry()) to surface the "imported the notes only" notice. NOT reset in start().
    bool m_droppedTextOnRetry = false;

    //! Per-sheet failure reasons recorded during the FIRST pass (1-based sheet number -> "no-music" / "low-res" / "too-large"),
    //! in ascending key order (QMap). Drives both the skip-invalid re-export (which pages to drop) and the "Import finished"
    //! notice's skipped-page list. Reset per fresh run in start(); PRESERVED across the re-export re-launch.
    QMap<int, QString> m_invalidSheetReasons;

    //! Set by the re-export finish when it produced more than one MusicXML movement (non-contiguous survivors). Surfaced via
    //! producedMultipleMovements() so the notice can add the interior "some pages in the middle..." explanation.
    bool m_multiMovement = false;

    int m_totalSheets = -1;
    int m_currentSheet = -1;
    int m_requestedSheetCount = 0;
    QString m_currentStep;

    //! Once the authoritative sheet total is known (the requested page count, else the book's one-time "N sheets"
    //! line), lock it so a later stray "<n> sheet(s)" status line can never clobber it back down (e.g. to 1).
    bool m_totalLocked = false;

    //! Recognition only moves forward: the highest sheet / percent reported so far, so the popup bar and the
    //! "page X" caption are monotonic and never regress on a transient stdout re-order or a late total correction.
    int m_maxSheet = 0;
    int m_maxPercent = -1;

    //! === PART 1 deskew state ================================================================================
    //! Armed by beginDeskew(), consumed by deskewStep()/finishDeskew(). The source is held open across the chunked steps
    //! (the PDF document, or the pre-decoded image frames) so each step renders without reparsing. During the DETECT pass
    //! m_deskewImages holds the RAW per-page renders and m_deskewAngles their RAW measured angles; finishDeskew() then makes
    //! the per-document decision, rewrites m_deskewImages in place to the straightened (or padded no-op) pages and m_deskewAngles
    //! to the angle actually applied, and sets m_deskewRotatedAny true only when the document triggered (a page was straightened).
    std::unique_ptr<QPdfDocument> m_deskewPdf;
    std::vector<QImage> m_deskewSourceFrames;
    bool m_deskewIsPdf = false;
    std::vector<int> m_deskewPages;
    int32_t m_deskewDpi = 0;
    QString m_deskewInputPath;
    QString m_deskewOutDir;
    std::vector<QImage> m_deskewImages;
    std::vector<double> m_deskewAngles;
    bool m_deskewRotatedAny = false;

    QStringList m_tail;

    //! The complete un-truncated output (every line routed through pushTail) and the path of its on-disk copy. The error
    //! window shows m_fullLog in full (the tail is only a last-resort fallback) and offers m_logFilePath via "Open log file".
    QStringList m_fullLog;
    QString m_logFilePath;

    bool m_cancelled = false;
};
}
