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
#include "reprisepreferencesmodel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "io/path.h"

using namespace mu::preferences;

//! Serialize a list of language packs to a JSON array of {code, name} the QML builds its models from.
static QString packsToJson(const std::vector<mu::reprise::LanguagePackInfo>& packs)
{
    QJsonArray arr;
    for (const mu::reprise::LanguagePackInfo& pack : packs)
    {
        QJsonObject obj;
        obj["code"] = QString::fromStdString(pack.code);
        obj["name"] = QString::fromStdString(pack.name);
        arr.append(obj);
    }

    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

ReprisePreferencesModel::ReprisePreferencesModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

void ReprisePreferencesModel::load()
{
}

QString ReprisePreferencesModel::detectAudiveris() const
{
    return repriseConfiguration()->detectAudiveris().toQString();
}

QString ReprisePreferencesModel::fileDirectory(const QString& filePath) const
{
    return muse::io::dirpath(filePath.toStdString()).toQString();
}

QString ReprisePreferencesModel::installedPacksJson() const
{
    return packsToJson(languagePackService()->installedPacks());
}

QString ReprisePreferencesModel::availablePacksJson() const
{
    return packsToJson(languagePackService()->availablePacks());
}

QString ReprisePreferencesModel::addOrUpdateLanguage(const QString& code)
{
    const muse::Ret ret = languagePackService()->addOrUpdatePack(code.toStdString());
    if (ret)
    {
        return QString();
    }

    return QString::fromStdString(ret.text());
}

QString ReprisePreferencesModel::removeLanguage(const QString& code)
{
    const muse::Ret ret = languagePackService()->removePack(code.toStdString());
    if (ret)
    {
        return QString();
    }

    return QString::fromStdString(ret.text());
}

QString ReprisePreferencesModel::audiverisPath() const
{
    return repriseConfiguration()->audiverisPath().toQString();
}

int ReprisePreferencesModel::scanResolution() const
{
    return repriseConfiguration()->scanResolution();
}

bool ReprisePreferencesModel::closePopupOnComplete() const
{
    return repriseConfiguration()->closePopupOnComplete();
}

QString ReprisePreferencesModel::ocrLanguage() const
{
    return QString::fromStdString(repriseConfiguration()->ocrLanguage());
}

void ReprisePreferencesModel::setAudiverisPath(QString path)
{
    if (path == audiverisPath())
    {
        return;
    }

    repriseConfiguration()->setAudiverisPath(path.toStdString());
    emit audiverisPathChanged(path);
}

void ReprisePreferencesModel::setScanResolution(int dpi)
{
    if (dpi == scanResolution())
    {
        return;
    }

    repriseConfiguration()->setScanResolution(dpi);
    emit scanResolutionChanged(dpi);
}

void ReprisePreferencesModel::setClosePopupOnComplete(bool close)
{
    if (close == closePopupOnComplete())
    {
        return;
    }

    repriseConfiguration()->setClosePopupOnComplete(close);
    emit closePopupOnCompleteChanged(close);
}

void ReprisePreferencesModel::setOcrLanguage(QString code)
{
    if (code == ocrLanguage())
    {
        return;
    }

    repriseConfiguration()->setOcrLanguage(code.toStdString());
    emit ocrLanguageChanged(code);
}
