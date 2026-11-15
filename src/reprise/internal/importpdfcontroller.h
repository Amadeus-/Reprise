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

#include "modularity/ioc.h"
#include "async/asyncable.h"
#include "interactive/iinteractive.h"
#include "rcommand/icommanddispatcher.h"
#include "rcommand/commandable.h"
#include "global/types/ret.h"
#include "global/iglobalconfiguration.h"
#include "global/io/ifilesystem.h"
#include "global/io/path.h"

#include "appshell/internal/isessionsmanager.h"

#include "irepriseconfiguration.h"
#include "ilanguagepackservice.h"
#include "iimportprogresstracker.h"
#include "audiverisrunner.h"

#include "reprise/reprisecommands.h"

class QQuickWindow;

namespace mu::reprise {
//! Orchestrates Reprise's "Import PDF" flow: pick a PDF -> intake dialog (pages + resolution) -> show the async
//! recognition progress popup. The popup's view-model (ImportProgressModel) owns the Audiveris run, the import,
//! and the Grand-Piano override; this controller only drives the picker + intake dialog and launches the popup.
class ImportPdfController : public muse::Contextable, public muse::async::Asyncable, public muse::rcommand::Commandable
{
    muse::ContextInject<muse::IInteractive> interactive = { this };
    muse::ContextInject<muse::rcommand::ICommandDispatcher> commandDispatcher = { this };
    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;
    muse::GlobalInject<muse::io::IFileSystem> fileSystem;
    muse::GlobalInject<mu::reprise::IRepriseConfiguration> repriseConfiguration;
    muse::GlobalInject<mu::reprise::ILanguagePackService> languagePackService;
    muse::GlobalInject<mu::reprise::IImportProgressTracker> importProgressTracker;
    muse::ContextInject<mu::appshell::ISessionsManager> sessionsManager = { this };

public:
    explicit ImportPdfController(const muse::modularity::ContextPtr& iocCtx)
        : muse::Contextable(iocCtx) {}

    void init();

private:
    //! Classic interactive flow (no "path" arg) OR headless flow (a "path" arg skips the picker + options dialog).
    muse::Ret importPdf(const muse::rcommand::Params& params);

    //! Launch the recognition progress popup for pdfPath with the given page range + resolution + recognition options
    //! (the popup's view-model owns the Audiveris run + import). Also seeds the progress tracker so a poll sees it
    //! immediately. The options ride the progress UriQuery into the popup, which hands them to AudiverisRunner::start().
    void launchImport(const muse::io::path_t& pdfPath, const std::string& pages, int dpi, const RecognitionOptions& options);

    //! The OCR languages installed in Reprise's shipped tessdata, serialized as a JSON array of {code, name} objects
    //! for the intake dialog's language dropdown (QML cannot enumerate files; the enumeration is done in C++ via
    //! AudiverisRunner::installedOcrLanguages(), which excludes 'osd'). Always includes at least English so the
    //! dropdown is never empty even if the tessdata folder cannot be read.
    std::string buildLanguagesJson() const;

    //! Pre-flight the pixel size of an IMAGE input (PDFs are skipped -- Audiveris rasterises them at a chosen DPI, so
    //! there is no fixed pixel count to check). Mirrors Audiveris' SheetStub maxPixelCount cap (default 20,000,000):
    //! a multi-frame TIFF is multiple sheets, so every frame is checked and the largest decides. Returns an error Ret
    //! (with a clear message) when a sheet exceeds the cap, so BOTH the interactive dialog and the headless/MCP caller
    //! can block the run up front instead of letting Audiveris abort mid-run with a raw non-zero exit code.
    muse::Ret preflightImageSize(const muse::io::path_t& path) const;

    //! Everything the intake dialog needs to decide what to show, computed ONCE in C++ so the decision is a single
    //! source of truth that is also logged (see inspectImage). pageCount is the reliable multi-frame count for an image
    //! (QImageReader::imageCount() under-reports some TIFFs) or the PDF page count (0 == unknown). width/height are the
    //! image's native pixel dimensions (0 for a PDF). showPageRange is the final decision: show the page-range field.
    struct ImageInfo
    {
        bool isImage = false;
        int pageCount = 1;
        bool pdfConfident = false;
        int width = 0;
        int height = 0;
        int embeddedDpi = 0;
        bool showPageRange = true;
    };

    //! Inspect the picked input and LOG the intake-dialog decision values, so a HEADLESS/MCP import makes the dialog's
    //! show/hide logic observable in the log without a GUI. For an image: reliable frame count + native resolution ->
    //! showPageRange = (frames > 1). For a PDF: in-house page count -> showPageRange = NOT (confidently a single page),
    //! so an uncertain count still shows the range and never silently drops pages. Called by BOTH the interactive and
    //! headless paths.
    ImageInfo inspectImage(const muse::io::path_t& path, bool isImage) const;

    //! Force-close the dialog opened for `query` and wait (bounded, pumping the GUI event loop) until it is FULLY settled:
    //! off the interactive stack AND -- when `known` is supplied -- its window object actually destroyed. This is the core
    //! of the second-call-crash fix. interactive()->close(query) does NOT hide a dialog opened with the fire-and-forget
    //! async interactive()->open() (the close request never reaches the window), so the dialog would linger on the stack;
    //! the NEXT open then races its teardown and trips Interactive::onClose's objectId.isValid() assert -> SIGSEGV.
    //! Closing the QQuickWindow directly fires Hide -> WindowView::onHidden -> closed -> Interactive::onClose
    //! synchronously, popping it off the stack. We then drain the deferred QML destroy() + queued onClosed callback so the
    //! next open starts from a clean state. Pass the grabbed window as `known` (precise), or nullptr to act on whatever is
    //! the top dialog for `query`. No-op (no delay) when nothing matching is open; bounded, so it can never hang.
    void forceCloseAndSettle(const muse::UriQuery& query, QQuickWindow* known) const;

    //! Read-only MCP/API queries backed by the shared progress tracker.
    muse::RetVal<GetImportProgressCommand::Output> getImportProgress() const;
    muse::RetVal<GetImportErrorsCommand::Output> getImportErrors() const;

    //! OCR language-pack MCP/API helpers (list / add / remove), all delegating to the shared ILanguagePackService so
    //! they drive the EXACT install/remove/enumerate code path the Preferences pack-manager buttons use (DRY).
    muse::RetVal<ListLanguagesCommand::Output> listLanguages() const;
    muse::RetVal<AddLanguageCommand::Output> addLanguage(const AddLanguageCommand& cmd);
    muse::RetVal<RemoveLanguageCommand::Output> removeLanguage(const RemoveLanguageCommand& cmd);

    //! JSON array ([{code,name}]) of the currently-installed OCR packs, shared by the list/add/remove helpers.
    std::string installedPacksJson() const;

    //! Dev MCP/API tool: open the Import-Scan intake dialog (ImportPdfOptionsDialog) for a given file and grab it to a
    //! PNG, so the modal intake dialog -- unreachable headlessly via the normal import flow -- can be verified. Runs the
    //! SAME dialog-prep the interactive path uses (detectPdfResolution / inspectImage / preflightImageSize) to pass
    //! identical params, opens the dialog ASYNC (never openSync, so there is no blocking nested modal loop), spins the
    //! GUI event loop until the dialog window is exposed, QQuickWindow::grabWindow()s it, then closes the dialog. All on
    //! the Qt GUI thread (the same synchronous dispatch path as the other reprise queries). ok=false + error on failure.
    muse::RetVal<CaptureImportDialogCommand::Output> captureImportDialog(const CaptureImportDialogCommand& cmd);

    //! Dev MCP/API tool: open the Preferences dialog at the Reprise page and grab it to a PNG, so the Preferences
    //! language-pack UI can be verified headlessly. Opens "muse://preferences?currentPageId=reprise" ASYNC, spins the
    //! GUI event loop until the dialog is exposed, grabWindow()s it, then force-closes + settles it (same pattern as
    //! captureImportDialog). ok=false + error on failure.
    muse::RetVal<CapturePreferencesCommand::Output> capturePreferences(const CapturePreferencesCommand& cmd);

    //! Start folder for the PDF picker: the stored Reprise folder if it still exists, else the Documents folder.
    muse::io::path_t defaultImportPdfDir() const;
    //! Persist the folder of the picked PDF (Reprise-private, independent of File > Open). Non-empty picks only.
    void rememberImportPdfDir(const muse::io::path_t& pdfPath);

    //! Last-used scan resolution, used as the dialog default (defaults to 300 dpi).
    int scanResolution() const;
    void setScanResolution(int dpi);

    //! Kept only for the pre-flight findAudiveris() check; the actual run lives in ImportProgressModel.
    std::shared_ptr<AudiverisRunner> m_audiverisRunner;
};
}
