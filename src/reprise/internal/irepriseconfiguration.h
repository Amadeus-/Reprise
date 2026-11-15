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
#include "global/io/path.h"

namespace mu::reprise {
//! NOTE: Single Reprise-private configuration surface, exported globally so the Preferences page view-model, the
//! Import-PDF controller, the async runner, and the progress popup all read/write the SAME settings. Consolidates
//! the scattered reprise/* keys (lastImportPdfDir, scanResolution, closePopupOnComplete) plus the Audiveris program
//! path and the default OCR language. Also owns the Audiveris detection search-order (ported faithfully from the C#
//! Reprise Recognizer). The Tesseract OCR engine ships bundled inside Audiveris and Reprise ships + manages its
//! training data (see ILanguagePackService), so there is no standalone Tesseract path to configure any more.
class IRepriseConfiguration : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IRepriseConfiguration)

public:
    virtual ~IRepriseConfiguration() = default;

    //! Configured Audiveris executable (empty = auto-detect). A non-empty, existing value wins over detection.
    virtual muse::io::path_t audiverisPath() const = 0;
    virtual void setAudiverisPath(const muse::io::path_t& path) = 0;

    //! Scan resolution (dpi) handed to Audiveris for PDF rasterisation. Default 300.
    virtual int scanResolution() const = 0;
    virtual void setScanResolution(int dpi) = 0;

    //! Global default OCR text language (Tesseract code, e.g. "eng") the intake dialog's language dropdown opens on.
    //! Only a currently-installed language pack should be stored here. Default "eng" (the bundled baseline pack).
    virtual std::string ocrLanguage() const = 0;
    virtual void setOcrLanguage(const std::string& code) = 0;

    //! When set, the recognition progress popup auto-closes once the import succeeds. Default false.
    virtual bool closePopupOnComplete() const = 0;
    virtual void setClosePopupOnComplete(bool close) = 0;

    //! Last folder a PDF was imported from (Reprise-private, independent of File > Open). Default empty.
    virtual muse::io::path_t lastImportPdfDir() const = 0;
    virtual void setLastImportPdfDir(const muse::io::path_t& dir) = 0;

    //! Locate Audiveris by the C# Recognizer search order (PATH names audiveris/Audiveris/oemer, then the standard
    //! install folders under the common program-file bases). Empty if nothing is found.
    virtual muse::io::path_t detectAudiveris() const = 0;
};
}
