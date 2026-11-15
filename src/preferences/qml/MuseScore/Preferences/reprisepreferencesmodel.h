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

#include <qqmlintegration.h>

#include <QObject>

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "reprise/internal/irepriseconfiguration.h"
#include "reprise/internal/ilanguagepackservice.h"

namespace mu::preferences {
//! View-model behind the Reprise Preferences page. Reads/writes the single Reprise configuration (the Audiveris
//! executable path, the scan resolution, the "close popup after import" toggle, and the default OCR language) and
//! drives the OCR language-pack manager through the shared ILanguagePackService (install / update / remove packs,
//! list installed + available). The Audiveris Detect button calls the ported detection and returns the found path.
class ReprisePreferencesModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT
    QML_ELEMENT;

    Q_PROPERTY(QString audiverisPath READ audiverisPath WRITE setAudiverisPath NOTIFY audiverisPathChanged)
    Q_PROPERTY(int scanResolution READ scanResolution WRITE setScanResolution NOTIFY scanResolutionChanged)
    Q_PROPERTY(bool closePopupOnComplete READ closePopupOnComplete WRITE setClosePopupOnComplete NOTIFY closePopupOnCompleteChanged)
    Q_PROPERTY(QString ocrLanguage READ ocrLanguage WRITE setOcrLanguage NOTIFY ocrLanguageChanged)

    muse::GlobalInject<mu::reprise::IRepriseConfiguration> repriseConfiguration;
    muse::GlobalInject<mu::reprise::ILanguagePackService> languagePackService;

public:
    explicit ReprisePreferencesModel(QObject* parent = nullptr);

    Q_INVOKABLE void load();

    //! Run the ported detection and return the found Audiveris path (empty if nothing was found), for the Detect
    //! button to prefill the field + flash a status.
    Q_INVOKABLE QString detectAudiveris() const;

    //! Directory of a file path, used as the FilePicker start folder (mirrors ImportPreferencesModel::fileDirectory).
    Q_INVOKABLE QString fileDirectory(const QString& filePath) const;

    //! Language-pack manager, all delegating to the shared ILanguagePackService (the same code the MCP helpers call).
    //! installedPacksJson / availablePacksJson return JSON arrays of {code, name} the QML builds its lists from.
    Q_INVOKABLE QString installedPacksJson() const;
    Q_INVOKABLE QString availablePacksJson() const;

    //! Download + install (or update) a pack by code. Returns "" on success, else a user-facing error message. The
    //! QML shows a busy state around the (blocking) call and refreshes the lists afterward.
    Q_INVOKABLE QString addOrUpdateLanguage(const QString& code);

    //! Remove an installed pack by code. Returns "" on success, else a user-facing error message (e.g. the baseline
    //! English pack or the current default language cannot be removed).
    Q_INVOKABLE QString removeLanguage(const QString& code);

    QString audiverisPath() const;
    int scanResolution() const;
    bool closePopupOnComplete() const;
    QString ocrLanguage() const;

public slots:
    void setAudiverisPath(QString path);
    void setScanResolution(int dpi);
    void setClosePopupOnComplete(bool close);
    void setOcrLanguage(QString code);

signals:
    void audiverisPathChanged(QString audiverisPath);
    void scanResolutionChanged(int scanResolution);
    void closePopupOnCompleteChanged(bool closePopupOnComplete);
    void ocrLanguageChanged(QString ocrLanguage);
};
}
