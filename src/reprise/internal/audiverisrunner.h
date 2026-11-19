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
#include <string>
#include <utility>
#include <vector>

#include <QObject>
#include <QStringList>
#include <QString>

#include "modularity/ioc.h"
#include "global/io/path.h"

#include "irepriseconfiguration.h"
#include "ilanguagepackservice.h"

class QProcess;

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

    //! Kill the running Audiveris process and abandon the run. finished() is NOT emitted after a cancel().
    void cancel();

    //! Number of pages recognised in this run: the count of explicitly requested sheets, or the whole-book total
    //! (parsed from Audiveris' output) when every page was processed. Valid once the run has reported a total.
    int pageCount() const { return m_requestedSheetCount > 0 ? m_requestedSheetCount : m_totalSheets; }

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

    //! Guards the PDF pad retry so it happens at most ONCE per import: a padded re-run that still fails is reported, not
    //! re-padded. Deliberately NOT reset in start() (the retry re-enters start()), only default-initialised per runner.
    bool m_retriedWithPad = false;

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

    QStringList m_tail;
    bool m_cancelled = false;
};
}
