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
#include <vector>

#include "modularity/imoduleinterface.h"
#include "global/types/ret.h"

namespace mu::reprise {
//! A single OCR language pack (one Tesseract <code>.traineddata file): its code ("eng"), a friendly display name
//! ("English"), and whether it is currently installed in Reprise's shipped tessdata folder.
struct LanguagePackInfo
{
    std::string code;
    std::string name;
    bool installed = false;
};

//! NOTE: Single owner of OCR language-pack management for Reprise, exported globally so the Preferences pack manager,
//! the Import-Scan intake dialog (via AudiverisRunner), and the agent-facing MCP helper commands all share ONE code
//! path. Packs live as <code>.traineddata files in Reprise's own shipped tessdata folder beside the executable (the
//! same folder Audiveris reads via TESSDATA_PREFIX). Downloads are EXPLICIT, on-demand, user-initiated only (a button
//! click or an MCP call) -- no background fetch, telemetry, or phone-home: Reprise stays offline-only, and the bundled
//! English pack works fully offline out of the box. Packs are pulled from the MAIN tesseract-ocr/tessdata repo, which
//! ships the legacy+LSTM COMBINED model Audiveris's legacy-mode OCR init requires (the LSTM-only repos silently fail).
class ILanguagePackService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(ILanguagePackService)

public:
    virtual ~ILanguagePackService() = default;

    //! The packs installed in the shipped tessdata folder (osd excluded -- it is orientation/script detection, not a
    //! text language), each {code, friendly-name, installed=true}, sorted by code. Empty if the folder is unreadable.
    virtual std::vector<LanguagePackInfo> installedPacks() const = 0;

    //! The curated common packs that are NOT yet installed, each {code, friendly-name, installed=false}, so the
    //! "Add language..." picker offers only what can actually be added.
    virtual std::vector<LanguagePackInfo> availablePacks() const = 0;

    //! Friendly display name for a language code (falls back to the raw code when unmapped; never blank).
    virtual std::string displayName(const std::string& code) const = 0;

    //! Whether <code>.traineddata is present in the shipped tessdata folder.
    virtual bool isInstalled(const std::string& code) const = 0;

    //! Download (or re-download, to update) <code>.traineddata from the main tessdata repo and install it into the
    //! shipped tessdata folder where Audiveris reads it. BLOCKING (user-initiated): downloads to a temp name, verifies
    //! the payload is a plausible pack (not an error page / truncated download), then renames it into place -- a
    //! truncated file is never left behind. A clear error Ret on an unknown code, no internet, an HTTP error, or a
    //! failed write (the caller surfaces the message; nothing crashes).
    virtual muse::Ret addOrUpdatePack(const std::string& code) = 0;

    //! Delete <code>.traineddata from the shipped tessdata folder. Refuses to remove the bundled English baseline and
    //! the pack that is the current default OCR language (a clear error Ret in either case).
    virtual muse::Ret removePack(const std::string& code) = 0;
};
}
