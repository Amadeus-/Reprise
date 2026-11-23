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
#include <tuple>

#include "rcommand/commandtypes.h"
#include "rcommand/typedcommand.h"

namespace mu::reprise {
inline static const muse::rcommand::Command REPRISE_IMPORT_PDF_COMMAND("command://reprise/import-pdf");

//! The scan file types Reprise's interactive OMR import accepts (PDF + the raster image formats Audiveris can read). The
//! argument is a lower-cased suffix with no leading dot (io::suffix form). Kept here so every entry point tests the SAME
//! set: the File > Open dispatch (OpenProjectScenario), the app-shell drag-and-drop handler, and the New-Scan picker.
inline bool isRepriseScanSuffix(const std::string& suffix)
{
    return suffix == "pdf" || suffix == "png" || suffix == "jpg" || suffix == "jpeg"
           || suffix == "tif" || suffix == "tiff" || suffix == "bmp" || suffix == "gif";
}

//! NOTE Reprise read-only MCP/API query: progress of the current/last Import-PDF (Audiveris OMR) run. Lets a
//! headless agent poll a running import without any GUI interaction.
struct GetImportProgressCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/get-import-progress");
    static const inline auto title = muse::MnemonicString("reprise", "Get import progress");
    static const inline auto description = muse::TranslatableString("reprise", "Get progress of the current or last PDF import");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    struct Output {
        bool active = false;
        std::string status;      // "idle" | "running" | "done" | "error"
        std::string phase;
        int percent = -1;
        int page = 0;
        int totalPages = 0;
        std::string pdfPath;
    };

    using O = GetImportProgressCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "active", &O::active, "Whether an import is currently running" },
        muse::rcommand::Field{ "status", &O::status, "idle | running | done | error" },
        muse::rcommand::Field{ "phase", &O::phase, "Human-readable current recognition stage" },
        muse::rcommand::Field{ "percent", &O::percent, "Progress 0-100, or -1 while indeterminate" },
        muse::rcommand::Field{ "page", &O::page, "Current sheet being recognised (1-based, 0 until known)" },
        muse::rcommand::Field{ "totalPages", &O::totalPages, "Total sheets in the run (0 until known)" },
        muse::rcommand::Field{ "pdfPath", &O::pdfPath, "Source PDF of the current/last run" },
    };
};

//! NOTE Reprise read-only MCP/API query: error(s) from the current/last Import-PDF run.
struct GetImportErrorsCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/get-import-errors");
    static const inline auto title = muse::MnemonicString("reprise", "Get import errors");
    static const inline auto description = muse::TranslatableString("reprise", "Get error details from the current or last PDF import");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    struct Output {
        bool hasError = false;
        std::string error;
        std::string details;
    };

    using O = GetImportErrorsCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "hasError", &O::hasError, "Whether the last finished import ended in error" },
        muse::rcommand::Field{ "error", &O::error, "Short error message (empty if none)" },
        muse::rcommand::Field{ "details", &O::details, "A tail of Audiveris output for the error (empty if none)" },
    };
};

//! NOTE Reprise read-only MCP/API query: the Two-Pane Proofing coordinate map captured at the last OMR import. Returns
//! the source scan path, the measure count, the per-page sheet-image dimensions + DPI, and one rect per measure (in
//! global measure order) in that page's pixel space. `pages` and `measures` are JSON arrays encoded as strings (the
//! list-languages idiom), so a headless agent can verify the whole map. hasMap=false when no import has been captured.
struct GetProofingMapCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/get-proofing-map");
    static const inline auto title = muse::MnemonicString("reprise", "Get proofing map");
    static const inline auto description
        = muse::TranslatableString("reprise", "Get the scan-to-score coordinate map captured at the last OMR import");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    struct Output {
        bool hasMap = false;
        std::string sourcePath;
        int measureCount = 0;
        int pageCount = 0;
        int noteCount = 0;
        std::string pages;       // JSON array: [{index,w,h,dpi}]
        std::string measures;    // JSON array: [{gm,page,x,y,w,h}]
        std::string notes;       // JSON array: [{gm,staff,tick,voice,step,octave,page,x,y,w,h}]
    };

    using O = GetProofingMapCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "hasMap", &O::hasMap, "Whether a proofing map has been captured" },
        muse::rcommand::Field{ "sourcePath", &O::sourcePath, "The original scanned PDF/image of the captured map" },
        muse::rcommand::Field{ "measureCount", &O::measureCount, "Number of measures in the map" },
        muse::rcommand::Field{ "pageCount", &O::pageCount, "Number of pages (sheets) in the map" },
        muse::rcommand::Field{ "noteCount", &O::noteCount, "Number of per-note head boxes in the map (Phase B1)" },
        muse::rcommand::Field{ "pages", &O::pages, "JSON array of pages: [{index,w,h,dpi}] (sheet-image pixels)" },
        muse::rcommand::Field{ "measures", &O::measures,
                               "JSON array of measure rects in global order: [{gm,page,x,y,w,h}] (page pixel space)" },
        muse::rcommand::Field{ "notes", &O::notes,
                               "JSON array of per-note head boxes: [{gm,staff,tick,voice,step,octave,page,x,y,w,h}] "
                               "(step 0..6=C..B, diatonic; page pixel space; empty for an unmatched/grace note)" },
    };
};

//! NOTE Reprise dev MCP/API tool: capture the running GUI to a PNG and return its absolute path. target="window"
//! (the default) grabs the whole main application window; target="score" renders the current score's page (0-based,
//! default page 0) through the registered "png" notation writer; target="dialog" grabs the topmost open dialog/popup
//! QQuickWindow (owned popups/dialogs/menus are NOT part of the main-window grab). The handler runs synchronously on the Qt GUI thread
//! (the same dispatch path score.get-counts uses), so QQuickWindow::grabWindow() + the PNG writer are called directly
//! -- no cross-thread marshalling. An optional "path" overrides the per-target fixed overwrite filename; "page"
//! applies only to the score target. Failures carry ok=false + an error string (the call itself still succeeds) so a
//! headless agent always receives structured output with ok/path/error.
struct CaptureScreenshotCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/capture-screenshot");
    static const inline auto title = muse::MnemonicString("reprise", "Capture screenshot");
    static const inline auto description
        = muse::TranslatableString("reprise", "Capture the GUI window or current score to a PNG and return its path");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    std::string target;    // "window" (default when empty) | "score" | "dialog"
    std::string path;      // optional absolute output path override
    int page = 0;          // score target only: 0-based page to render

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "target", &CaptureScreenshotCommand::target,
                               "What to capture: 'window' (default), 'score' or 'dialog' (the topmost open dialog/popup)",
                               muse::rcommand::Presence::Optional },
        muse::rcommand::Field{ "path", &CaptureScreenshotCommand::path,
                               "Absolute output PNG path (optional; default is a per-target overwrite file)",
                               muse::rcommand::Presence::Optional },
        muse::rcommand::Field{ "page", &CaptureScreenshotCommand::page,
                               "Score page to render, 0-based (score target only; default 0)",
                               muse::rcommand::Presence::Optional },
    };

    struct Output {
        bool ok = false;
        std::string path;
        std::string error;
    };

    using O = CaptureScreenshotCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether the capture succeeded" },
        muse::rcommand::Field{ "path", &O::path, "Absolute path of the written PNG (empty on failure)" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
    };
};

//! NOTE Reprise dev MCP/API tool: open the Import-Scan intake dialog (ImportPdfOptionsDialog) for a given file and
//! capture it to a PNG, so the modal intake dialog can be verified headlessly (it is never reachable via the normal
//! import flow headlessly, since that path skips the picker + the dialog). "path" is the scan (PDF or image) whose
//! intake dialog to render; the handler runs the SAME dialog-prep the interactive flow uses (resolution auto-detect,
//! page-count/frame inspection, image pre-flight) to pass identical params, then opens the dialog ASYNC (NOT openSync,
//! so there is no blocking nested modal loop), spins the GUI event loop until the dialog's QQuickWindow is exposed,
//! grabWindow()s it to a PNG, and closes the dialog again -- all on the Qt GUI thread. "outPath" optionally overrides
//! the default <userAppDataPath>/screenshots/mcp-import-dialog.png. Failures carry ok=false + an error string (the
//! call itself still succeeds) so a headless agent always receives structured ok/path/error output.
struct CaptureImportDialogCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/capture-import-dialog");
    static const inline auto title = muse::MnemonicString("reprise", "Capture import dialog");
    static const inline auto description
        = muse::TranslatableString("reprise", "Open the Import-Scan intake dialog for a file and capture it to a PNG");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    std::string path;      // required: absolute path of the scan (PDF or image) whose intake dialog to render
    std::string outPath;   // optional absolute output PNG path override
    bool expandOptions = false;   // optional: open with the "Options" disclosure expanded (for a capture of that state)

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "path", &CaptureImportDialogCommand::path,
                               "Absolute path of the scan (PDF or image) whose intake dialog to render",
                               muse::rcommand::Presence::Required },
        muse::rcommand::Field{ "outPath", &CaptureImportDialogCommand::outPath,
                               "Absolute output PNG path (optional; default is a fixed overwrite file)",
                               muse::rcommand::Presence::Optional },
        muse::rcommand::Field{ "expandOptions", &CaptureImportDialogCommand::expandOptions,
                               "Open the dialog with the Options panel expanded, to capture that state (optional; default false)",
                               muse::rcommand::Presence::Optional },
    };

    struct Output {
        bool ok = false;
        std::string path;
        std::string error;
    };

    using O = CaptureImportDialogCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether the dialog capture succeeded" },
        muse::rcommand::Field{ "path", &O::path, "Absolute path of the written PNG (empty on failure)" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
    };
};

//! NOTE Reprise dev MCP/API helper: list the OCR language packs Reprise knows about -- the installed ones (in the
//! shipped tessdata folder) and the curated packs available to add -- plus the current default language. Feeds the
//! SAME ILanguagePackService the Preferences pack manager and the intake dialog use, so a headless agent can verify
//! add / remove / list without the GUI. Read-only.
struct ListLanguagesCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/list-languages");
    static const inline auto title = muse::MnemonicString("reprise", "List languages");
    static const inline auto description = muse::TranslatableString("reprise", "List installed and available OCR language packs");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    struct Output {
        std::string installedJson;
        std::string availableJson;
        std::string defaultLanguage;
    };

    using O = ListLanguagesCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "installedJson", &O::installedJson, "JSON array of installed packs: [{code,name}]" },
        muse::rcommand::Field{ "availableJson", &O::availableJson, "JSON array of not-yet-installed packs: [{code,name}]" },
        muse::rcommand::Field{ "defaultLanguage", &O::defaultLanguage, "The current default OCR language code" },
    };
};

//! NOTE Reprise dev MCP/API helper: download + install (or update) an OCR language pack by code, via the SAME
//! ILanguagePackService code path the Preferences "Add language..." button uses. Blocking (the download runs to
//! completion before returning). ok=false + an error on an unknown code / no internet / a failed install.
struct AddLanguageCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/add-language");
    static const inline auto title = muse::MnemonicString("reprise", "Add language");
    static const inline auto description = muse::TranslatableString("reprise", "Download and install an OCR language pack");
    static const inline auto availabilities = muse::rcommand::Availability::API;

    std::string code;      // required: the Tesseract language code to install, e.g. "lat"

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "code", &AddLanguageCommand::code,
                               "Tesseract language code to download and install, e.g. 'lat' or 'fra'",
                               muse::rcommand::Presence::Required },
    };

    struct Output {
        bool ok = false;
        std::string error;
        std::string installedJson;
    };

    using O = AddLanguageCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether the pack was installed" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
        muse::rcommand::Field{ "installedJson", &O::installedJson, "JSON array of installed packs after the call" },
    };
};

//! NOTE Reprise dev MCP/API helper: remove an installed OCR language pack by code, via the SAME ILanguagePackService
//! code path the Preferences "Remove" button uses. Refuses to remove English or the current default language.
struct RemoveLanguageCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/remove-language");
    static const inline auto title = muse::MnemonicString("reprise", "Remove language");
    static const inline auto description = muse::TranslatableString("reprise", "Remove an installed OCR language pack");
    static const inline auto availabilities = muse::rcommand::Availability::API;

    std::string code;      // required: the Tesseract language code to remove

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "code", &RemoveLanguageCommand::code,
                               "Tesseract language code of the installed pack to remove, e.g. 'lat'",
                               muse::rcommand::Presence::Required },
    };

    struct Output {
        bool ok = false;
        std::string error;
        std::string installedJson;
    };

    using O = RemoveLanguageCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether the pack was removed" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
        muse::rcommand::Field{ "installedJson", &O::installedJson, "JSON array of installed packs after the call" },
    };
};

//! NOTE Reprise dev MCP/API tool: open the Preferences dialog at the Reprise page and capture it to a PNG, so the
//! Preferences language-pack UI -- otherwise only reachable through live GUI clicks -- can be verified headlessly.
//! Opens "muse://preferences?currentPageId=reprise" ASYNC, spins the GUI event loop until the dialog window is
//! exposed, grabWindow()s it, then closes + fully settles the dialog (same pattern as capture-import-dialog).
//! "outPath" optionally overrides the default <userAppDataPath>/screenshots/mcp-preferences.png.
struct CapturePreferencesCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/capture-preferences");
    static const inline auto title = muse::MnemonicString("reprise", "Capture preferences");
    static const inline auto description
        = muse::TranslatableString("reprise", "Open the Preferences dialog at the Reprise page and capture it to a PNG");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    std::string outPath;   // optional absolute output PNG path override

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "outPath", &CapturePreferencesCommand::outPath,
                               "Absolute output PNG path (optional; default is a fixed overwrite file)",
                               muse::rcommand::Presence::Optional },
    };

    struct Output {
        bool ok = false;
        std::string path;
        std::string error;
    };

    using O = CapturePreferencesCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether the capture succeeded" },
        muse::rcommand::Field{ "path", &O::path, "Absolute path of the written PNG (empty on failure)" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
    };
};

//! NOTE Reprise dev MCP/API "drive" tool for the Two-Pane Proofing follow band (Phase A3): select a measure by its
//! GLOBAL measure index (0-based, pickup = 0) in the current score and report the proofing-map rect that index resolves
//! to. Selecting the measure drives the live selectionChanged the follow band observes, so a headless agent can verify
//! the band scrolls to + highlights the CORRECT original measure at a KNOWN position, and the returned {gm,page,x,y,w,h}
//! proves the notation current-measure index == the map's globalMeasureIndex (both positional). Runs synchronously on the
//! Qt GUI thread (the same dispatch path the other reprise queries use). found=false when the index has no map rect;
//! ok=false + error when there is no current score / the index is out of range. Marked ReadOnly so it is allowlisted with
//! the other dev tools -- it only changes the selection, not the document.
struct ProofingDebugGotoCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/proofing-debug-goto");
    static const inline auto title = muse::MnemonicString("reprise", "Proofing debug goto");
    static const inline auto description
        = muse::TranslatableString("reprise", "Select a measure by global index and report its proofing-map rect (drives the follow band)");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    int measure = 0;      // required: global measure index to select (0-based, pickup = 0)

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "measure", &ProofingDebugGotoCommand::measure,
                               "Global measure index to select, 0-based (pickup/anacrusis = 0)",
                               muse::rcommand::Presence::Required },
    };

    struct Output {
        bool ok = false;
        bool found = false;
        int measure = 0;
        int gm = -1;
        int page = -1;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int measureCount = 0;
        std::string error;
    };

    using O = ProofingDebugGotoCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether the measure was selected (a current score exists and the index is in range)" },
        muse::rcommand::Field{ "found", &O::found, "Whether that measure index has a rect in the proofing map" },
        muse::rcommand::Field{ "measure", &O::measure, "The requested global measure index (echoed back)" },
        muse::rcommand::Field{ "gm", &O::gm, "The matched map globalMeasureIndex (-1 if not found)" },
        muse::rcommand::Field{ "page", &O::page, "0-based page the measure sits on (-1 if not found)" },
        muse::rcommand::Field{ "x", &O::x, "Measure rect x in sheet-image pixels" },
        muse::rcommand::Field{ "y", &O::y, "Measure rect y in sheet-image pixels" },
        muse::rcommand::Field{ "w", &O::w, "Measure rect width in sheet-image pixels" },
        muse::rcommand::Field{ "h", &O::h, "Measure rect height in sheet-image pixels" },
        muse::rcommand::Field{ "measureCount", &O::measureCount, "Number of measures in the held map" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
    };
};

//! NOTE Reprise dev MCP/API "drive" tool for the Two-Pane Proofing FULL DETAIL VIEW magnifier (Phase B2): select ONE
//! NOTE in the current score by its measure + staff + in-measure index, which drives the live selectionChanged the
//! floating magnifier observes, so a headless agent can verify the magnifier zooms + highlights the CORRECT recognised
//! note. The handler walks to the Nth positional measure (pickup = 0), scans its ChordRest segments in tick order on the
//! requested staff, picks the `index`-th note, and selects it SINGLE. It ALSO computes the Phase B1 runtime key straight
//! from that Note (gm/staffIdx/tickInMeasure/step/octave/voice) and reports whether the proofing map holds a head box
//! for it plus that box's geometry -- so the note<->box match can be checked directly against reprise.get-proofing-map
//! (self-check: the reported staff == the note's staffIdx by construction; a box MISS flags the grace/untrusted fallback
//! the magnifier then shows as a dashed measure marker). Runs synchronously on the Qt GUI thread. ok=false + error on no
//! current score / out-of-range measure or index. Marked ReadOnly so it is allowlisted with the other dev tools -- it
//! only changes the selection, not the document.
struct ProofingDebugSelectNoteCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/proofing-debug-select-note");
    static const inline auto title = muse::MnemonicString("reprise", "Proofing debug select note");
    static const inline auto description
        = muse::TranslatableString("reprise", "Select a note by measure/staff/index and report its proofing-map head box (drives the magnifier)");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    int measure = 0;      // required: global measure index (0-based, pickup = 0)
    int staff = 0;        // optional: global 0-based staff index (default 0)
    int index = 0;        // optional: 0-based note index within that measure+staff, tick then chord order (default 0)

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "measure", &ProofingDebugSelectNoteCommand::measure,
                               "Global measure index to select within, 0-based (pickup/anacrusis = 0)",
                               muse::rcommand::Presence::Required },
        muse::rcommand::Field{ "staff", &ProofingDebugSelectNoteCommand::staff,
                               "Global 0-based staff index to pick the note from (optional; default 0)",
                               muse::rcommand::Presence::Optional },
        muse::rcommand::Field{ "index", &ProofingDebugSelectNoteCommand::index,
                               "0-based note index within the measure+staff, in tick then chord order (optional; default 0)",
                               muse::rcommand::Presence::Optional },
    };

    struct Output {
        bool ok = false;
        bool found = false;
        int measure = 0;
        int staff = 0;
        int index = 0;
        int noteCount = 0;
        int gm = -1;
        int tick = 0;
        int step = -1;
        int octave = 0;
        int voice = 0;
        std::string noteName;
        bool hasBox = false;
        bool isFallback = false;
        int page = -1;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        std::string error;
    };

    using O = ProofingDebugSelectNoteCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether a note was selected (a current score exists and the indices are in range)" },
        muse::rcommand::Field{ "found", &O::found, "Whether a note was found at measure+staff+index" },
        muse::rcommand::Field{ "measure", &O::measure, "The requested global measure index (echoed back)" },
        muse::rcommand::Field{ "staff", &O::staff, "The requested staff index (echoed back; == the note's staffIdx)" },
        muse::rcommand::Field{ "index", &O::index, "The requested in-measure note index (echoed back)" },
        muse::rcommand::Field{ "noteCount", &O::noteCount, "Number of notes found in that measure+staff" },
        muse::rcommand::Field{ "gm", &O::gm, "The selected note's global measure index (B1 key; -1 if none)" },
        muse::rcommand::Field{ "tick", &O::tick, "The selected note's tickInMeasure (MuseScore 480/quarter; B1 key)" },
        muse::rcommand::Field{ "step", &O::step, "The selected note's diatonic step 0..6=C..B (B1 key; -1 if none)" },
        muse::rcommand::Field{ "octave", &O::octave, "The selected note's scientific octave (B1 key)" },
        muse::rcommand::Field{ "voice", &O::voice, "The selected note's voice (advisory; not part of the box match)" },
        muse::rcommand::Field{ "noteName", &O::noteName, "Short pitch label of the selected note, e.g. 'C4'" },
        muse::rcommand::Field{ "hasBox", &O::hasBox, "Whether the proofing map holds a head box for the note's B1 key" },
        muse::rcommand::Field{ "isFallback", &O::isFallback,
                               "True when no head box was found (grace/untrusted) -> the magnifier shows a measure fallback" },
        muse::rcommand::Field{ "page", &O::page, "0-based page the head box (or fallback measure rect) sits on (-1 if none)" },
        muse::rcommand::Field{ "x", &O::x, "Head box x in sheet-image pixels (or measure-rect x on a fallback)" },
        muse::rcommand::Field{ "y", &O::y, "Head box y in sheet-image pixels (or measure-rect y on a fallback)" },
        muse::rcommand::Field{ "w", &O::w, "Head box width in sheet-image pixels (or measure-rect width on a fallback)" },
        muse::rcommand::Field{ "h", &O::h, "Head box height in sheet-image pixels (or measure-rect height on a fallback)" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
    };
};

//! NOTE Reprise dev MCP/API "drive" tool for the Two-Pane Proofing panels: set the LIVE follow band's or floating
//! magnifier's user zoom factor, so a headless agent can drive + screenshot the zoom states and regression-check them.
//! `panel` picks which live view-model to drive ("band" = the A3 follow band, "magnifier" = the B2 full-detail
//! magnifier); `factor` is the user zoom (1.0 = fit-to-width / the auto-chosen note zoom). The handler reaches the
//! transient QML-instantiated view-model through IProofingViewControl (each model self-registers on load) and drives its
//! `userZoom` Q_PROPERTY via the Qt property system -- the model's setUserZoom clamps to [0.5, 4.0], and the CLAMPED
//! applied value is read back into `zoom`. Runs synchronously on the Qt GUI thread (the same dispatch path the other
//! proofing-debug tools use). ok=false + error ("no <panel> proofing panel is open") when that panel is not currently
//! loaded, or when `panel` is neither "band" nor "magnifier". Marked ReadOnly so it is allowlisted with the other dev
//! tools -- it only changes a view-model's zoom, not the document.
struct ProofingDebugSetZoomCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/proofing-debug-set-zoom");
    static const inline auto title = muse::MnemonicString("reprise", "Proofing debug set zoom");
    static const inline auto description
        = muse::TranslatableString("reprise", "Set the live proofing follow band's or magnifier's user zoom factor");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    std::string panel;    // required: "band" or "magnifier"
    double factor = 1.0;  // required: user zoom factor (1.0 = fit-to-width; clamped to [0.5, 4.0] by the model)

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "panel", &ProofingDebugSetZoomCommand::panel,
                               "Which live proofing panel to zoom: 'band' (A3 follow band) or 'magnifier' (B2 full-detail view)",
                               muse::rcommand::Presence::Required },
        muse::rcommand::Field{ "factor", &ProofingDebugSetZoomCommand::factor,
                               "User zoom factor (1.0 = fit-to-width / auto note zoom; clamped to [0.5, 4.0])",
                               muse::rcommand::Presence::Required },
    };

    struct Output {
        bool ok = false;
        double zoom = 0.0;
        std::string error;
    };

    using O = ProofingDebugSetZoomCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether the requested panel was live and its zoom was set" },
        muse::rcommand::Field{ "zoom", &O::zoom, "The clamped user zoom actually applied (0 on failure)" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
    };
};

//! NOTE Reprise dev MCP/API "drive" tool: navigate the app shell to a named screen IN-PROCESS, with no foreground
//! activation and no synthetic mouse input (the Firefox-bridge model). The handler runs synchronously on the Qt GUI
//! thread (the same dispatch path the capture tools use), calling interactive()->open(UriQuery(...)) for the resolved
//! target: "scans"/"plugins"/"learn" open the Home page at that section, "home" opens the Home page (default section
//! scores), "notation" opens the notation/score page. An unknown target carries ok=false + an error naming the valid
//! targets, so a headless agent always receives structured ok/page/error output.
struct NavigateCommand {
    static const inline auto id = muse::rcommand::Command("command://reprise/navigate");
    static const inline auto title = muse::MnemonicString("reprise", "Navigate");
    static const inline auto description = muse::TranslatableString("reprise", "Navigate the app shell to a named screen in-process");
    static const inline auto availabilities = muse::rcommand::Availability::API;
    static const inline auto annotations = muse::rcommand::Annotation::ReadOnly;

    std::string target;    // required: "scans" | "plugins" | "learn" | "home" | "notation"

    static const inline auto input = std::tuple {
        muse::rcommand::Field{ "target", &NavigateCommand::target,
                               "Screen to open: 'scans', 'plugins', 'learn', 'home' or 'notation'",
                               muse::rcommand::Presence::Required },
    };

    struct Output {
        bool ok = false;
        std::string page;
        std::string error;
    };

    using O = NavigateCommand::Output;
    static const inline auto output = std::tuple {
        muse::rcommand::Field{ "ok", &O::ok, "Whether navigation succeeded" },
        muse::rcommand::Field{ "page", &O::page, "The resolved target/URI that was opened (empty on failure)" },
        muse::rcommand::Field{ "error", &O::error, "Error message (empty on success)" },
    };
};
}
