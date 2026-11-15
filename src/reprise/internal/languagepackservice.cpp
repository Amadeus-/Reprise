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

#include "languagepackservice.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

#include "global/translation.h"
#include "global/log.h"

using namespace muse;
using namespace mu::reprise;

//! The curated common set of OCR language packs Reprise offers: Tesseract code -> friendly English display name. This
//! is the SINGLE source both the installed-list naming and the "Add language..." picker derive from. Latin is kept for
//! sacred / choral scores. 'eng' is the bundled baseline (always shipped). More can be added here freely.
namespace {
static const struct { const char* code; const char* name; } kKnownLanguages[] = {
    { "eng", "English" },
    { "deu", "German" },
    { "fra", "French" },
    { "ita", "Italian" },
    { "spa", "Spanish" },
    { "lat", "Latin" },
    { "por", "Portuguese" },
    { "nld", "Dutch" },
    { "rus", "Russian" },
    { "pol", "Polish" },
};

//! The bundled baseline pack that ships with Reprise and must never be removed.
static const char* const BASELINE_CODE = "eng";

//! A downloaded payload smaller than this is treated as a failure (a GitHub 404 page, a truncated transfer, an error
//! body) -- real .traineddata files are several megabytes, so this floor can never reject a genuine pack.
static constexpr qint64 MIN_PACK_BYTES = 50 * 1024;

//! Abort a stalled download after this long so the user-initiated call can never hang the UI indefinitely.
static constexpr int DOWNLOAD_TIMEOUT_MS = 60000;
}

QString LanguagePackService::shippedTessdataDir() const
{
    //! Resolved at RUNTIME from the running executable's directory, never a build-time path, so the deployed
    //! standalone build and the dev build each manage the tessdata folder shipped beside their own Reprise.exe.
    const QString appDir = QCoreApplication::applicationDirPath();
    if (appDir.isEmpty())
    {
        return QString();
    }

    return QDir(appDir).absoluteFilePath("tessdata");
}

bool LanguagePackService::isKnownCode(const QString& code) const
{
    for (const auto& entry : kKnownLanguages)
    {
        if (code.compare(QString::fromLatin1(entry.code), Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }

    return false;
}

std::string LanguagePackService::displayName(const std::string& code) const
{
    const QString qcode = QString::fromStdString(code);
    for (const auto& entry : kKnownLanguages)
    {
        if (qcode.compare(QString::fromLatin1(entry.code), Qt::CaseInsensitive) == 0)
        {
            return std::string(entry.name);
        }
    }

    return code;
}

bool LanguagePackService::isInstalled(const std::string& code) const
{
    const QString dir = shippedTessdataDir();
    if (dir.isEmpty())
    {
        return false;
    }

    const QString file = QDir(dir).absoluteFilePath(QString::fromStdString(code) + ".traineddata");

    return QFileInfo::exists(file);
}

std::vector<LanguagePackInfo> LanguagePackService::installedPacks() const
{
    std::vector<LanguagePackInfo> result;

    //! Enumerate the *.traineddata packs in Reprise's shipped tessdata folder (the same folder TESSDATA_PREFIX points
    //! at). Each file's base name is the language code; 'osd' is orientation/script detection, NOT a text language, so
    //! it is excluded from the picker. Sorted by code so the list is stable across runs.
    const QString dirPath = shippedTessdataDir();
    if (dirPath.isEmpty())
    {
        return result;
    }

    const QDir dir(dirPath);
    const QStringList files = dir.entryList(QStringList() << "*.traineddata", QDir::Files, QDir::Name);
    for (const QString& file : files)
    {
        const QString code = QFileInfo(file).completeBaseName();
        if (code.isEmpty() || code.compare("osd", Qt::CaseInsensitive) == 0)
        {
            continue;
        }

        LanguagePackInfo info;
        info.code = code.toStdString();
        info.name = displayName(info.code);
        info.installed = true;
        result.push_back(info);
    }

    return result;
}

std::vector<LanguagePackInfo> LanguagePackService::availablePacks() const
{
    std::vector<LanguagePackInfo> result;

    //! Every curated known language that is not already installed. 'eng' is the bundled baseline (always installed), so
    //! in practice it never appears here; the isInstalled() check covers it regardless.
    for (const auto& entry : kKnownLanguages)
    {
        const std::string code(entry.code);
        if (isInstalled(code))
        {
            continue;
        }

        LanguagePackInfo info;
        info.code = code;
        info.name = std::string(entry.name);
        info.installed = false;
        result.push_back(info);
    }

    return result;
}

muse::Ret LanguagePackService::addOrUpdatePack(const std::string& code)
{
    const QString qcode = QString::fromStdString(code).trimmed().toLower();
    if (qcode.isEmpty() || !isKnownCode(qcode))
    {
        return make_ret(Ret::Code::UnknownError, muse::trc("reprise", "Unknown language pack."));
    }

    const QString dirPath = shippedTessdataDir();
    if (dirPath.isEmpty())
    {
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "Could not locate Reprise's language folder."));
    }

    QDir dir(dirPath);
    if (!dir.exists() && !QDir().mkpath(dirPath))
    {
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "Could not create Reprise's language folder."));
    }

    const QString finalPath = dir.absoluteFilePath(qcode + ".traineddata");
    const QString tmpPath = finalPath + ".part";

    //! Download from the MAIN tessdata repo -- it ships the legacy+LSTM COMBINED model Audiveris's legacy-mode OCR init
    //! requires (the tessdata_fast / tessdata_best repos are LSTM-only and would silently fail), the same repo + branch
    //! the bundled eng/osd packs came from.
    const QString url = "https://raw.githubusercontent.com/tesseract-ocr/tessdata/main/" + qcode + ".traineddata";

    QNetworkAccessManager manager;
    QNetworkRequest request((QUrl(url)));
    request.setTransferTimeout(DOWNLOAD_TIMEOUT_MS);
    //! Explicit, user-initiated request only: no custom tracking headers, no telemetry -- a plain GET of a public file.

    LOGI() << "Reprise: downloading language pack '" << qcode << "' from " << url;

    QNetworkReply* reply = manager.get(request);

    //! Block (pumping the event loop so the UI stays painted / a busy indicator animates) until the download finishes
    //! or the transfer timeout aborts it. This is a user-initiated action, and the single code path the MCP helper also
    //! calls -- both need the synchronous result.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorString = reply->errorString();
    const QByteArray data = reply->readAll();
    reply->deleteLater();

    if (netError != QNetworkReply::NoError)
    {
        LOGE() << "Reprise: language pack download failed for '" << qcode << "': " << netErrorString;
        return make_ret(Ret::Code::UnknownError,
                        muse::qtrc("reprise", "Could not download the %1 language pack. Check your internet connection "
                                              "and try again.").arg(QString::fromStdString(displayName(code))).toStdString());
    }

    if (data.size() < MIN_PACK_BYTES)
    {
        LOGE() << "Reprise: language pack download for '" << qcode << "' returned only " << data.size()
               << " bytes (expected a multi-megabyte pack); treating as a failed download";
        return make_ret(Ret::Code::UnknownError,
                        muse::qtrc("reprise", "The downloaded %1 language pack looks incomplete. Please try again.")
                        .arg(QString::fromStdString(displayName(code))).toStdString());
    }

    //! Write to a temp name first, then rename into place, so a truncated / failed download never replaces a good pack
    //! and Audiveris never sees a half-written .traineddata.
    QFile::remove(tmpPath);
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly))
    {
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "Could not write the language pack to disk."));
    }

    const qint64 written = tmp.write(data);
    tmp.close();
    if (written != data.size())
    {
        QFile::remove(tmpPath);
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "Could not write the language pack to disk."));
    }

    //! QFile::rename will not overwrite an existing file on Windows, so remove the previous pack first (this is also the
    //! update path: re-download the latest and replace).
    QFile::remove(finalPath);
    if (!QFile::rename(tmpPath, finalPath))
    {
        QFile::remove(tmpPath);
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "Could not install the downloaded language pack."));
    }

    LOGI() << "Reprise: installed language pack '" << qcode << "' (" << data.size() << " bytes) at " << finalPath;

    return make_ok();
}

muse::Ret LanguagePackService::removePack(const std::string& code)
{
    const QString qcode = QString::fromStdString(code).trimmed().toLower();
    if (qcode.isEmpty())
    {
        return make_ret(Ret::Code::UnknownError, muse::trc("reprise", "Unknown language pack."));
    }

    //! The bundled English baseline is never removable.
    if (qcode.compare(QString::fromLatin1(BASELINE_CODE), Qt::CaseInsensitive) == 0)
    {
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "English is the built-in language and cannot be removed."));
    }

    //! Never remove the pack that is the current default OCR language (it would leave imports pointing at a missing
    //! language). The user must switch the default first.
    if (m_configuration)
    {
        const QString defaultLang = QString::fromStdString(m_configuration->ocrLanguage());
        if (qcode.compare(defaultLang, Qt::CaseInsensitive) == 0)
        {
            return make_ret(Ret::Code::UnknownError,
                            muse::trc("reprise", "This is the default recognition language. Choose a different default "
                                                 "before removing it."));
        }
    }

    const QString dirPath = shippedTessdataDir();
    if (dirPath.isEmpty())
    {
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "Could not locate Reprise's language folder."));
    }

    const QString filePath = QDir(dirPath).absoluteFilePath(qcode + ".traineddata");
    if (!QFileInfo::exists(filePath))
    {
        //! Already gone -- treat as success (idempotent).
        return make_ok();
    }

    if (!QFile::remove(filePath))
    {
        return make_ret(Ret::Code::UnknownError,
                        muse::trc("reprise", "Could not remove the language pack."));
    }

    LOGI() << "Reprise: removed language pack '" << qcode << "' at " << filePath;

    return make_ok();
}
