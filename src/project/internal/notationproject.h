/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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

#include "../inotationproject.h"

#include "async/asyncable.h"

#include "modularity/ioc.h"
#include "io/ifilesystem.h"
#include "../iprojectconfiguration.h"
#include "inotationreadersregister.h"
#include "inotationwritersregister.h"

#include "engraving/engravingproject.h"
#include "engraving/rendering/iscorerenderer.h"

#include "notation/inotationconfiguration.h"
#include "projectaudiosettings.h"
#include "iprojectmigrator.h"

#include "global/iglobalconfiguration.h"
#include "project/iprojectsession.h"

#include "reprise/internal/iproofingmapservice.h"

namespace mu::engraving {
class MscReader;
class MscWriter;

namespace write {
class WriteContext;
}
}

namespace mu::project {
class NotationProject : public INotationProject, public muse::Contextable, public muse::async::Asyncable
{
    muse::GlobalInject<muse::io::IFileSystem> fileSystem;
    muse::GlobalInject<IProjectConfiguration> configuration;
    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;
    muse::GlobalInject<notation::INotationConfiguration> notationConfiguration;
    muse::GlobalInject<INotationReadersRegister> readers;
    muse::GlobalInject<INotationWritersRegister> writers;
    muse::GlobalInject<engraving::rendering::IScoreRenderer> renderer;
    muse::ContextInject<project::IProjectSession> projectSession = { this };
    muse::ContextInject<IProjectMigrator> migrator = { this };
    // Reprise: the scan<->score proofing coordinate map, resolved only when loading/saving a .repr bundle (null in
    // non-GUI/converter runs where the reprise module is not loaded -- guarded at every use).
    muse::GlobalInject<reprise::IProofingMapService> proofingMapService;

public:
    NotationProject(const muse::modularity::ContextPtr& iocCtx)
        : muse::Contextable(iocCtx) {}
    ~NotationProject() override;

    static QString scoreDefaultTitle();

    muse::Ret load(const muse::io::path_t& path, const OpenParams& params = {}, const std::string& format = "") override;
    muse::Ret createNew(const ProjectCreateOptions& projectInfo) override;

    muse::io::path_t path() const override;
    void setPath(const muse::io::path_t& path) override;
    muse::async::Notification pathChanged() const override;

    QString displayName() const override;
    muse::async::Notification displayNameChanged() const override;

    bool isCloudProject() const override;
    const CloudProjectInfo& cloudInfo() const override;
    void setCloudInfo(const CloudProjectInfo& info) override;

    const CloudAudioInfo& cloudAudioInfo() const override;
    void setCloudAudioInfo(const CloudAudioInfo& audioInfo) override;

    bool isNewlyCreated() const override;
    void markAsNewlyCreated() override;

    bool isImported() const override;

    void markAsUnsaved() override;

    bool isNeedSave() const override;
    muse::async::Notification needSaveChanged() const override;
    muse::Ret canSave() const override;

    bool needAutoSave() const override;
    void setNeedAutoSave(bool val) override;

    muse::Ret save(
        const muse::io::path_t& path = muse::io::path_t(), SaveMode saveMode = SaveMode::Save, bool createBackup = true) override;
    muse::Ret savePage(const muse::io::path_t& path, const size_t pageNum) override;
    muse::async::Channel<muse::io::path_t, SaveMode> saveComplited() const override;

    muse::Ret writeToDevice(QIODevice* device) override;

    ProjectMeta metaInfo() const override;
    void setMetaInfo(const ProjectMeta& meta, bool undoable = false) override;

    notation::IMasterNotationPtr masterNotation() const override;
    IProjectAudioSettingsPtr audioSettings() const override;

    // Reprise: record the source scan + manifest provenance so Save/Save-As can (re)assemble the .repr bundle. Set once
    // on import (the auto-save-as-.repr) and again when opening an existing .repr (originalScanPath = the extracted scan).
    void setReprBundleInfo(const muse::io::path_t& originalScanPath, const std::string& originalSuffix,
                           const std::string& importProvenanceJson,
                           const muse::io::path_t& proofingScanPath = {},
                           const std::string& proofingScanSuffix = {}) override;

private:
    void setupProject();

    muse::Ret loadTemplate(const ProjectCreateOptions& projectOptions);

    muse::Ret doLoad(const muse::io::path_t& path, const OpenParams& params, const std::string& format);
    muse::Ret doImport(const muse::io::path_t& path, const OpenParams& params);

    // Reprise .repr: shared post-read engraving/audio/view setup, factored out of doLoad so both the file reader (doLoad)
    // and the in-memory score.mscz reader (doLoadRepr) run the identical body.
    muse::Ret doLoadByReader(engraving::MscReader& reader, const muse::io::path_t& path, const OpenParams& params);
    // Reprise .repr: open a bundle -> score.mscz (in-memory MscReader) + extract original to a real temp path + restore map.
    muse::Ret doLoadRepr(const muse::io::path_t& path, const OpenParams& params);
    // Reprise .repr: assemble the bundle (score.mscz + original + proofingmap.json + manifest.json) and atomically write it.
    muse::Ret doSaveRepr(const muse::io::path_t& path, bool isAutosave);

    muse::Ret saveScore(const muse::io::path_t& path, const std::string& fileSuffix, bool generateBackup = true,
                        bool createThumbnail = true, bool isAutosave = false, const engraving::write::WriteContext* ctx = nullptr);
    muse::Ret saveSelectionOnScore(const muse::io::path_t& path = muse::io::path_t());
    muse::Ret exportProject(const muse::io::path_t& path, const std::string& suffix);
    muse::Ret doSave(const muse::io::path_t& path, engraving::MscIoMode ioMode, bool generateBackup = true, bool createThumbnail = true,
                     bool isAutosave = false, const engraving::write::WriteContext* ctx = nullptr);
    muse::Ret makeBackup(muse::io::path_t filePath);
    muse::Ret writeProject(const muse::io::path_t& path, const engraving::write::WriteContext* ctx = nullptr);
    muse::Ret writeProject(engraving::MscWriter& msczWriter, bool createThumbnail = true,
                           const engraving::write::WriteContext* ctx = nullptr);
    muse::Ret checkSavedFileForCorruption(engraving::MscIoMode ioMode, const muse::io::path_t& path, const muse::io::path_t& scoreFileName);

    void listenIfNeedSaveChanges();
    void markAsSaved(const muse::io::path_t& path);
    void setNeedSave(bool needSave);

    mu::engraving::EngravingProjectPtr m_engravingProject = nullptr;
    notation::IMasterNotationPtr m_masterNotation = nullptr;
    ProjectAudioSettingsPtr m_projectAudioSettings = nullptr;
    mutable CloudProjectInfo m_cloudInfo;
    mutable CloudAudioInfo m_cloudAudioInfo;

    muse::io::path_t m_path;
    muse::async::Notification m_pathChanged;
    muse::async::Notification m_displayNameChanged;

    muse::async::Notification m_needSaveChanged;

    muse::async::Channel<muse::io::path_t, SaveMode> m_saved;

    // Reprise .repr: source scan + manifest provenance retained for Save/Save-As re-assembly (see setReprBundleInfo).
    muse::io::path_t m_reprOriginalPath;   // on-disk path to read the original scan bytes from when saving the bundle
    std::string m_reprOriginalSuffix;      // e.g. "pdf"
    std::string m_reprOriginalFileName;    // e.g. "waltz.pdf" (recorded in the manifest)
    std::string m_reprManifestImport;      // manifest "import" provenance JSON object (compact), re-emitted on save
    // Reprise .repr (0.16.5+): the deskewed render Audiveris consumed, stored alongside the literal original. Empty when
    // no page was deskewed (bundle then stays byte-identical to a pre-0.16.5 save).
    muse::io::path_t m_reprProofingScanPath; // on-disk path to read the deskewed render bytes from when saving the bundle
    std::string m_reprProofingScanSuffix;    // e.g. "pdf"
    std::vector<double> m_reprSkewAnglesDeg; // optional per-page deskew angles (provenance only)
    // Reprise .repr (0.16.15+): when a deskewed .repr is opened, the verbatim original is NOT extracted to temp (the band +
    // magnifier render the deskewed proofing-scan, not the original). This holds the source bundle path so doSaveRepr can
    // extract-on-demand the original straight from it; empty unless the deferred-original path was taken.
    muse::io::path_t m_reprSourceBundlePath;

    bool m_isNewlyCreated = false; /// true if the file has never been saved yet
    bool m_isImported = false;
    bool m_isNeedSave = false;
    bool m_needAutoSave = false;
    bool m_hasNonUndoStackChanges = false;
};
}
