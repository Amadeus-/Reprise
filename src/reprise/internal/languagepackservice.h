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

#include <QString>

#include "ilanguagepackservice.h"
#include "irepriseconfiguration.h"

namespace mu::reprise {
//! settings-independent implementation of ILanguagePackService. Resolves the shipped tessdata folder at RUNTIME from
//! the running executable's own directory (<exeDir>/tessdata), so the deployed standalone build and the dev build each
//! manage the tessdata beside their own Reprise.exe. Holds the single curated code->friendly-name table that both the
//! enumeration and the display names derive from. The injected configuration is consulted only to refuse removing the
//! current default OCR language.
class LanguagePackService : public ILanguagePackService
{
public:
    explicit LanguagePackService(const std::shared_ptr<IRepriseConfiguration>& configuration)
        : m_configuration(configuration) {}

    std::vector<LanguagePackInfo> installedPacks() const override;
    std::vector<LanguagePackInfo> availablePacks() const override;
    std::string displayName(const std::string& code) const override;
    bool isInstalled(const std::string& code) const override;

    muse::Ret addOrUpdatePack(const std::string& code) override;
    muse::Ret removePack(const std::string& code) override;

private:
    //! Reprise's shipped tessdata folder beside the running executable (<exeDir>/tessdata), resolved at runtime. The
    //! install target for new packs and the folder the installed list is enumerated from. Returned whether or not it
    //! exists yet (so a first install can create it); empty only when the application dir itself is unknown.
    QString shippedTessdataDir() const;

    //! Whether a code is one of the curated known languages (guards against arbitrary codes / path injection on the
    //! download URL + install path). Lower-cased comparison; 'osd' is NOT a curated text language.
    bool isKnownCode(const QString& code) const;

    std::shared_ptr<IRepriseConfiguration> m_configuration;
};
}
