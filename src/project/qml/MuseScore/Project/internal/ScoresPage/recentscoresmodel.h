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

#pragma once

#include <cstdint>
#include <map>

#include <QString>
#include <qqmlintegration.h>

#include "abstractscoresmodel.h"

#include "async/asyncable.h"

#include "project/iprojectconfiguration.h"
#include "project/irecentfilescontroller.h"
#include "project/imscmetareader.h"
#include "io/ifilesystem.h"
#include "modularity/ioc.h"

namespace mu::project {
class RecentScoresModel : public AbstractScoresModel, public muse::async::Asyncable, public muse::Contextable
{
    Q_OBJECT
    QML_ELEMENT

public:
    muse::GlobalInject<IProjectConfiguration> configuration;
    muse::ContextInject<IRecentFilesController> recentFilesController = { this };
    muse::GlobalInject<muse::io::IFileSystem> fileSystem;
    muse::GlobalInject<IMscMetaReader> mscMetaReader;

public:
    RecentScoresModel(QObject* parent = nullptr);

    void load() override;
    Q_INVOKABLE void removeRecentScore(const QString& scorePath);

    QList<int> nonScoreItemIndices() const override;

private:
    void reloadRecentScores();
    void setRecentScores(const std::vector<QVariantMap>& items);

    void prependItem(const RecentFile& file);
    void updateItem(const muse::io::path_t& oldPath, const RecentFile& file);
    void removeItem(const muse::io::path_t& path);

    QVariantMap makeItem(const RecentFile& file) const;
    int findRow(const muse::io::path_t& path) const;

    // Reprise: per-card embedded-meta derived from the score file. Cached by path + mtime so each recent score's
    // title/composer (and a .repr's page-range label) is read once per change, not on every list rebuild.
    struct CardMeta {
        QString title;       //! embedded score title (empty when none / unreadable -> card falls back to the clean file name)
        QString composer;    //! embedded composer (empty when none -> the "(Composer)" part is omitted)
        QString pages;       //! decorated ".repr" page-range label e.g. "[all]"/"[page 5]"/"[pages 1-3]" (empty for non-.repr)
        int64_t mtimeMs = 0; //! last-modified epoch ms the cached values were read at (cache-invalidation key)
    };

    CardMeta cardMetaFor(const RecentFile& file) const;

    mutable std::map<QString, CardMeta> m_cardMetaCache;
};
}
