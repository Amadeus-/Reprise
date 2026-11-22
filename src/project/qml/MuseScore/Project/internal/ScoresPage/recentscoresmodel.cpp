/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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
#include "recentscoresmodel.h"

#include <QDateTime>

#include "translation.h"
#include "dataformatter.h"
#include "io/fileinfo.h"
#include "types/reprisenameutils.h"
#include "types/projectmeta.h"
#include "internal/reprfile.h"

#include "global/types/bytearray.h"
#include "global/serialization/zipreader.h"
#include "global/serialization/json.h"

#include "log.h"

using namespace muse;
using namespace mu::project;

static constexpr int NO_RESULTS_FOUND_ROW = 1;
static constexpr int FIRST_SCORE_ROW = 2; // after "New score" and "No results found"

RecentScoresModel::RecentScoresModel(QObject* parent)
    : AbstractScoresModel(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

void RecentScoresModel::load()
{
    reloadRecentScores();

    recentFilesController()->recentFilesListChanged().onReceive(this, [this](const RecentFilesChange& change) {
        if (const RecentFilePrepended* prepended = std::get_if<RecentFilePrepended>(&change)) {
            prependItem(prepended->file);
        } else if (const RecentFilePathChanged* pathChanged = std::get_if<RecentFilePathChanged>(&change)) {
            updateItem(pathChanged->oldPath, pathChanged->file);
        } else if (const RecentFileRemoved* removed = std::get_if<RecentFileRemoved>(&change)) {
            removeItem(removed->path);
        } else {
            reloadRecentScores();
        }
    });
}

void RecentScoresModel::removeRecentScore(const QString& scorePath)
{
    recentFilesController()->removeRecentFile(scorePath);
}

void RecentScoresModel::reloadRecentScores()
{
    const RecentFilesList& recentScores = recentFilesController()->recentFilesList();

    std::vector<QVariantMap> items;
    items.reserve(recentScores.size() + 2);

    QVariantMap addItem;
    addItem[NAME_KEY] = muse::qtrc("project", "New Scan");
    addItem[IS_CREATE_NEW_KEY] = true;
    addItem[IS_NO_RESULTS_FOUND_KEY] = false;
    addItem[IS_CLOUD_KEY] = false;
    items.push_back(addItem);

    QVariantMap noResultsFoundItem;
    noResultsFoundItem[NAME_KEY] = "";
    noResultsFoundItem[IS_CREATE_NEW_KEY] = false;
    noResultsFoundItem[IS_NO_RESULTS_FOUND_KEY] = true;
    noResultsFoundItem[IS_CLOUD_KEY] = false;
    items.push_back(noResultsFoundItem);

    for (const RecentFile& file : recentScores) {
        items.push_back(makeItem(file));
    }

    setRecentScores(items);
}

void RecentScoresModel::setRecentScores(const std::vector<QVariantMap>& items)
{
    if (m_items == items) {
        return;
    }

    beginResetModel();
    m_items = items;
    endResetModel();
}

void RecentScoresModel::prependItem(const RecentFile& file)
{
    const int row = findRow(file.path);
    if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_items.erase(m_items.begin() + row);
        endRemoveRows();
    }

    beginInsertRows(QModelIndex(), FIRST_SCORE_ROW, FIRST_SCORE_ROW);
    m_items.insert(m_items.begin() + FIRST_SCORE_ROW, makeItem(file));
    endInsertRows();
}

void RecentScoresModel::updateItem(const muse::io::path_t& oldPath, const RecentFile& file)
{
    const int row = findRow(oldPath);
    if (row < 0) {
        reloadRecentScores();
        return;
    }

    m_items[row] = makeItem(file);

    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx);
}

void RecentScoresModel::removeItem(const muse::io::path_t& path)
{
    const int row = findRow(path);
    if (row < 0) {
        reloadRecentScores();
        return;
    }

    beginRemoveRows(QModelIndex(), row, row);
    m_items.erase(m_items.begin() + row);
    endRemoveRows();
}

// Reprise: DISPLAY-ONLY clean name for the Scans list NAME column. Delegates to the shared fork helper
// repriseCleanDisplayName() (LAST-dot extension strip + trailing ".reprise-stripped" removal) so the Scans
// list and the main-window title stay in lock-step. The underlying path (PATH_KEY) is untouched, so
// open/sort/identity are unaffected.
static QString cleanDisplayName(const RecentFile& file)
{
    if (!file.displayNameOverride.isEmpty())
    {
        return file.displayNameOverride;
    }

    return repriseCleanDisplayName(file.path.toQString());
}

// Reprise: format a .repr manifest "import.pages" value into the SQUARE-BRACKET card suffix. "all"/empty -> "[all]";
// a single page "5" -> "[page 5]"; a range "1-3" -> "[pages 1<U+2013>3]" (hyphen rendered as an en-dash); a comma list
// "1,3,5" -> "[pages 1,3,5]". ASCII source: the en-dash is injected via QChar(0x2013).
static QString formatReprPagesLabel(const QString& pagesRaw)
{
    const QString trimmed = pagesRaw.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("all", Qt::CaseInsensitive) == 0) {
        return QStringLiteral("[all]");
    }

    bool isSingleInt = false;
    trimmed.toInt(&isSingleInt);
    if (isSingleInt) {
        return "[page " + trimmed + "]";
    }

    QString display = trimmed;
    display.replace('-', QChar(0x2013));
    return "[pages " + display + "]";
}

// Reprise: LIGHT crack of a .repr outer zip -- pull ONLY the embedded score.mscz bytes (for the title/composer read) and
// the manifest's raw "import.pages" string (for the page-range label). Avoids readRepr's full extraction of the original
// scan + proofing render. Returns false (card falls back to the clean file name, no page part) on any failure.
static bool readReprCardSource(const muse::io::path_t& path, muse::ByteArray& scoreMsczOut, QString& pagesRawOut)
{
    muse::ZipReader zip(path);
    if (!zip.exists() || zip.hasError()) {
        return false;
    }

    scoreMsczOut = zip.fileData(REPR_ENTRY_SCORE);
    if (scoreMsczOut.empty()) {
        return false;
    }

    const muse::ByteArray manifestBytes = zip.fileData(REPR_ENTRY_MANIFEST);
    if (!manifestBytes.empty()) {
        std::string err;
        const muse::JsonDocument mdoc = muse::JsonDocument::fromJson(manifestBytes, &err);
        const muse::JsonObject manifest = mdoc.rootObject();
        if (manifest.contains("import")) {
            const muse::JsonObject import = manifest.value("import").toObject();
            pagesRawOut = QString::fromStdString(import.value("pages").toStdString());
        }
    }

    return true;
}

RecentScoresModel::CardMeta RecentScoresModel::cardMetaFor(const RecentFile& file) const
{
    const QString pathStr = file.path.toQString();

    const QDateTime lastModified = io::FileInfo(file.path).lastModified().toQDateTime();
    const int64_t mtimeMs = lastModified.isValid() ? lastModified.toMSecsSinceEpoch() : 0;

    const auto cached = m_cardMetaCache.find(pathStr);
    if (cached != m_cardMetaCache.end() && cached->second.mtimeMs == mtimeMs) {
        return cached->second;
    }

    CardMeta cm;
    cm.mtimeMs = mtimeMs;

    const std::string suffix = io::suffix(file.path);

    if (suffix == REPRISE_PROJECT_SUFFIX) {
        // .repr: read title/composer from the embedded score.mscz bytes, and the page label from the manifest -- one zip open.
        muse::ByteArray scoreMscz;
        QString pagesRaw;
        if (readReprCardSource(file.path, scoreMscz, pagesRaw)) {
            const muse::RetVal<ProjectMeta> meta = mscMetaReader()->readMeta(scoreMscz);
            if (meta.ret) {
                cm.title = meta.val.title;
                cm.composer = meta.val.composer;
            }
            cm.pages = formatReprPagesLabel(pagesRaw);
        }
    } else {
        // Plain recents (.mscz etc.): title/composer from the file directly when trivially available; readMeta refuses an
        // unsupported suffix (e.g. .musicxml) with a non-ok Ret, which leaves title/composer empty -> clean-file-name fallback.
        const muse::RetVal<ProjectMeta> meta = mscMetaReader()->readMeta(file.path);
        if (meta.ret) {
            cm.title = meta.val.title;
            cm.composer = meta.val.composer;
        }
    }

    m_cardMetaCache[pathStr] = cm;
    return cm;
}

QVariantMap RecentScoresModel::makeItem(const RecentFile& file) const
{
    QVariantMap obj;

    std::string suffix = io::suffix(file.path);

    RetVal<uint64_t> fileSize = fileSystem()->fileSize(file.path);
    QString fileSizeString = (fileSize.ret && fileSize.val > 0) ? DataFormatter::formatFileSize(fileSize.val).toQString() : QString();

    obj[NAME_KEY] = cleanDisplayName(file);

    // Reprise: decorated-card roles (NAME_KEY stays the plain clean name used for open/delete). The card QML composes
    // "{Title} ({Composer}) [pages]" from these: TITLE empty -> the card uses NAME_KEY; COMPOSER empty -> no "(...)";
    // PAGES empty (every non-.repr recent) -> no "[...]".
    const CardMeta cardMeta = cardMetaFor(file);
    obj[TITLE_KEY] = cardMeta.title;
    obj[COMPOSER_KEY] = cardMeta.composer;
    obj[PAGES_KEY] = cardMeta.pages;

    obj[PATH_KEY] = file.path.toQString();
    obj[SUFFIX_KEY] = QString::fromStdString(suffix);
    obj[FILE_SIZE_KEY] = fileSizeString;
    obj[IS_CLOUD_KEY] = configuration()->isCloudProject(file.path);
    obj[CLOUD_SCORE_ID_KEY] = configuration()->cloudScoreIdFromPath(file.path);
    obj[TIME_SINCE_MODIFIED_KEY] = DataFormatter::formatTimeSince(io::FileInfo(file.path).lastModified().date()).toQString();
    obj[IS_CREATE_NEW_KEY] = false;
    obj[IS_NO_RESULTS_FOUND_KEY] = false;

    return obj;
}

int RecentScoresModel::findRow(const muse::io::path_t& path) const
{
    const QString pathStr = path.toQString();

    for (size_t i = FIRST_SCORE_ROW; i < m_items.size(); ++i) {
        if (m_items[i].value(PATH_KEY).toString() == pathStr) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

QList<int> RecentScoresModel::nonScoreItemIndices() const
{
    return { 0, NO_RESULTS_FOUND_ROW };
}
