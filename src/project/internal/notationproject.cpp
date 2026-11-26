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

#include "notationproject.h"

#include <memory>

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QCursor>

#include "global/concurrency/concurrent.h"
#include "global/io/buffer.h"
#include "global/io/dir.h"
#include "global/io/file.h"
#include "global/io/ioretcodes.h"
#include "global/io/devtools/allzerosbuffercorruptor.h"

#include "engraving/compat/engravingcompat.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/page.h"
#include "engraving/dom/repeatlist.h"
#include "engraving/dom/system.h"
#include "engraving/editing/editscoreproperties.h"
#include "engraving/editing/editstyle.h"
#include "engraving/editing/transaction/transaction.h"
#include "engraving/engravingerrors.h"
#include "engraving/engravingproject.h"
#include "engraving/infrastructure/mscio.h"
#include "engraving/rw/inoutdata.h"
#include "engraving/rw/write/writecontext.h"

#include "iprojectautosaver.h"
#include "reprfile.h"
#include "notation/iexcerptnotation.h" // IWYU pragma: keep
#include "notation/inotationundostack.h" // IWYU pragma: keep
#include "notation/inotationviewstate.h"
#include "notation/internal/masternotation.h"
#include "notation/notationerrors.h"
#include "projectaudiosettings.h"
#include "projectfileinfoprovider.h"
#include "projecterrors.h"
#include "types/projectcreateoptions.h"
#include "types/projectmeta.h"

#include "defer.h"
#include "log.h"

using namespace mu;
using namespace muse;
using namespace muse::io;
using namespace mu::engraving;
using namespace mu::notation;
using namespace mu::project;

static void setupScoreMetaTags(mu::engraving::MasterScore* masterScore, const ProjectCreateOptions& projectOptions)
{
    if (!projectOptions.title.isEmpty()) {
        masterScore->setMetaTag(WORK_TITLE_TAG, projectOptions.title);
    }
    if (!projectOptions.subtitle.isEmpty()) {
        masterScore->setMetaTag(SUBTITLE_TAG, projectOptions.subtitle);
    }
    if (!projectOptions.composer.isEmpty()) {
        masterScore->setMetaTag(COMPOSER_TAG, projectOptions.composer);
    }
    if (!projectOptions.lyricist.isEmpty()) {
        masterScore->setMetaTag(LYRICIST_TAG, projectOptions.lyricist);
    }
    if (!projectOptions.copyright.isEmpty()) {
        masterScore->setMetaTag(COPYRIGHT_TAG, projectOptions.copyright);
    }
    if (!projectOptions.templatePath.empty()) {
        masterScore->setMetaTag(CREATION_DATE_TAG, QDate::currentDate().toString(Qt::ISODate));
    }
}

QString NotationProject::scoreDefaultTitle()
{
    return muse::qtrc("project", "Untitled score");
}

NotationProject::~NotationProject()
{
    m_projectAudioSettings = nullptr;
    m_masterNotation = nullptr;
    m_engravingProject = nullptr;
}

void NotationProject::setupProject()
{
    TRACEFUNC;

    m_engravingProject = EngravingProject::create(iocContext());
    m_engravingProject->setFileInfoProvider(std::make_shared<ProjectFileInfoProvider>(this));
    m_masterNotation = std::shared_ptr<MasterNotation>(new MasterNotation(this, iocContext()));
    m_projectAudioSettings = std::shared_ptr<ProjectAudioSettings>(new ProjectAudioSettings(iocContext()));
}

Ret NotationProject::load(const muse::io::path_t& path, const OpenParams& openParams, const std::string& format_)
{
    TRACEFUNC;

    std::string format = format_.empty() ? io::suffix(path) : format_;

    LOGD() << "try load: " << path << ", format: " << format;

    setupProject();
    setPath(path);

    // Reprise .repr: a bundle restores the full project (score + original scan + proofing map), so it is neither a
    // plain MuseScore file nor an "import" -- it has its own load branch and opens as a saved, non-imported project.
    if (format == REPRISE_PROJECT_SUFFIX) {
        Ret ret = doLoadRepr(path, openParams);
        if (!ret) {
            LOGE() << "failed load .repr, err: " << ret.toString();
            return ret;
        }

        listenIfNeedSaveChanges();
        setNeedSave(false);

        m_isNewlyCreated = false;
        m_isImported = false;

        // Reprise: a .repr with a usable map set it above (doLoadRepr); a .repr with no map leaves the previous document's
        // map stale -> onDocumentLoaded drops it so the band/magnifier reset. See IProofingMapService::onDocumentLoaded.
        if (auto svc = proofingMapService()) {
            svc->onDocumentLoaded(path.toStdString());
        }

        // Reprise 0.18.0: doLoadRepr set the navigator state above (claim armed); this binds it to the opened document so
        // a later Save re-emits it and a subsequent different-document load drops it. See INavigatorStateService.
        if (auto nsvc = navigatorStateService()) {
            nsvc->onDocumentLoaded(path.toStdString());
        }

        // Reprise 0.18.2: doLoadRepr set the proofing view state above (claim armed); bind it to the opened document so a
        // later Save re-emits it and a subsequent different-document load reverts to defaults. See IProofingViewStateService.
        if (auto vsvc = proofingViewStateService()) {
            vsvc->onDocumentLoaded(path.toStdString());
        }

        return ret;
    }

    if (!isMuseScoreFile(format)) {
        Ret ret = doImport(path, openParams);
        if (ret) {
            listenIfNeedSaveChanges();

            // Reprise: an OMR import set its map just before opening this MusicXML (it claims ownership here); any other
            // import has no map, so the previous document's stale map is dropped. See IProofingMapService::onDocumentLoaded.
            if (auto svc = proofingMapService()) {
                svc->onDocumentLoaded(path.toStdString());
            }

            // Reprise 0.18.0: an import never carries navigator state, so this drops any state held from a previous .repr
            // (the new document becomes the owner with an empty state). See INavigatorStateService::onDocumentLoaded.
            if (auto nsvc = navigatorStateService()) {
                nsvc->onDocumentLoaded(path.toStdString());
            }

            // Reprise 0.18.2: an import carries no proofing view state, so this reverts to the defaults (first-note / band 0.5
            // / magnifier 1.0), dropping any view state held from a previous .repr. See IProofingViewStateService::onDocumentLoaded.
            if (auto vsvc = proofingViewStateService()) {
                vsvc->onDocumentLoaded(path.toStdString());
            }
        }

        return ret;
    }

    Ret ret = doLoad(path, openParams, format);
    if (!ret) {
        LOGE() << "failed load, err: " << ret.toString();
        return ret;
    }

    bool treatAsImported = (m_masterNotation->mscVersion() < 400 && !isCloudProject()) || format == MSCZ_BACKUP;

    listenIfNeedSaveChanges();
    setNeedSave(treatAsImported);

    m_isNewlyCreated = treatAsImported;
    m_isImported = treatAsImported;

    // Reprise: a plain score has no proofing map -> drop any stale map held from the previously-open document so the
    // band/magnifier do not show the wrong scan. See IProofingMapService::onDocumentLoaded.
    if (auto svc = proofingMapService()) {
        svc->onDocumentLoaded(path.toStdString());
    }

    // Reprise 0.18.0: a plain score has no navigator state -> drop any state held from a previously-open .repr.
    if (auto nsvc = navigatorStateService()) {
        nsvc->onDocumentLoaded(path.toStdString());
    }

    // Reprise 0.18.2: a plain score has no proofing view state -> revert to defaults, dropping any view state from a previous .repr.
    if (auto vsvc = proofingViewStateService()) {
        vsvc->onDocumentLoaded(path.toStdString());
    }

    return ret;
}

Ret NotationProject::doLoad(const muse::io::path_t& path, const OpenParams& openParams, const std::string& format)
{
    TRACEFUNC;

    MscReader::Params params;
    params.filePath = path.toQString();
    params.mode = mscIoModeBySuffix(format);
    IF_ASSERT_FAILED(params.mode != MscIoMode::Unknown) {
        return make_ret(Ret::Code::InternalError);
    }

    MscReader reader(params);
    Ret ret = reader.open();
    if (!ret) {
        return ret;
    }

    return doLoadByReader(reader, path, openParams);
}

Ret NotationProject::doLoadByReader(MscReader& reader, const muse::io::path_t& path, const OpenParams& openParams)
{
    TRACEFUNC;

    // Load engraving project
    m_engravingProject->setFileInfoProvider(std::make_shared<ProjectFileInfoProvider>(this));

    rw::ReadInOutData inOutData;
    inOutData.forcePageMode = openParams.forcePageMode;
    Ret ret = m_engravingProject->loadMscz(reader, &inOutData, openParams.forceMode);
    if (!ret) {
        return ret;
    }

    MasterScore* masterScore = m_engravingProject->masterScore();
    IF_ASSERT_FAILED(masterScore) {
        return engraving::make_ret(engraving::Err::UnknownError, reader.params().filePath);
    }

    masterScore->lockUpdates(true);
    DEFER {
        masterScore->lockUpdates(false);
    };

    // Setup master score
    ret = m_engravingProject->setupMasterScore(openParams.forceMode);
    if (!ret) {
        return ret;
    }

    // Migration
    if (migrator()) {
        masterScore->lockUpdates(false); // because migration needs a second layout
        ret = migrator()->migrateEngravingProjectIfNeed(m_engravingProject);
        if (!ret) {
            return ret;
        }
        masterScore->lockUpdates(true);
    }

    // Load style if present
    if (!openParams.stylePath.empty()) {
        muse::io::File styleFile(openParams.stylePath);
        mu::engraving::MasterScore* ms = m_engravingProject->masterScore();
        mu::engraving::EditStyle::loadStyle(ms->transactionManager()->currentOrDummyTransaction(), ms, styleFile);
    }

    mu::engraving::compat::EngravingCompat::doPreLayoutCompatIfNeeded(m_engravingProject->masterScore());

    masterScore->updateCapo(/* ignoreNotationUpdate */ true);

    if (openParams.unrollRepeats && masterScore->repeatList().size() > 1) {
        MasterScore* original = masterScore;
        masterScore = original->unrollRepeats();
        delete original;
        m_engravingProject->setMasterScore(masterScore);
    } else {
        masterScore->lockUpdates(false);
        masterScore->setLayoutAll();
        masterScore->update();
    }

    mu::engraving::compat::EngravingCompat::doPostLayoutCompatIfNeeded(m_engravingProject->masterScore());

    // Load audio settings
    bool tryCompatAudio = false;
    if (!openParams.disablePlayback) {
        ret = m_projectAudioSettings->read(reader);
        if (!ret) {
            m_projectAudioSettings->makeDefault();
            tryCompatAudio = true;
        }
    }

    // Load cloud info
    {
        m_cloudInfo.sourceUrl = masterScore->metaTags()[SOURCE_TAG].toQString();
        m_cloudInfo.revisionId = masterScore->metaTags()[SOURCE_REVISION_ID_TAG].toInt();

        m_cloudAudioInfo.url = masterScore->metaTags()[AUDIO_COM_URL_TAG].toQString();

        if (configuration()->isLegacyCloudProject(path)) {
            m_cloudInfo.name = io::filename(path, false).toQString();
        }
    }

    // Set current if all success
    m_masterNotation->setMasterScore(masterScore, openParams.disablePlayback);

    // Load view settings & solo-mute states (needs to be done after notations are created)
    m_masterNotation->notation()->viewState()->read(reader);
    m_masterNotation->notation()->soloMuteState()->read(reader);
    const int mscVersion = m_masterNotation->mscVersion();
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        const INotationPtr excerptNotation = excerpt->notation();
        if (!excerpt->hasFileName() || mscVersion < 440) {
            m_masterNotation->initNotationSoloMuteState(excerptNotation);
            continue;
        }
        muse::io::path_t ePath = u"Excerpts/" + excerpt->fileName() + u"/";
        excerptNotation->viewState()->read(reader, ePath);
        excerptNotation->soloMuteState()->read(reader, ePath);
    }

    // Apply compat audio settings (needs to be done after notations are created)
    if (!openParams.disablePlayback && tryCompatAudio && !inOutData.settingsCompat.audioSettings.empty()) {
        for (const auto& audioCompat : inOutData.settingsCompat.audioSettings) {
            notation::INotationSoloMuteState::SoloMuteState state = { audioCompat.second.mute, audioCompat.second.solo };
            INotationSoloMuteStatePtr soloMuteStatePtr = m_masterNotation->notation()->soloMuteState();
            soloMuteStatePtr->setTrackSoloMuteState(audioCompat.second.instrumentId, state);
        }
    }

    return make_ret(Ret::Code::Ok);
}

Ret NotationProject::doLoadRepr(const muse::io::path_t& path, const OpenParams& openParams)
{
    TRACEFUNC;

    // A .repr open runs the whole bundle read + embedded score load + proofing render synchronously on the UI thread, so
    // show the busy cursor for the duration. DEFER guarantees the restore on EVERY exit path (early returns + exceptions),
    // since it fires on scope exit. The magnifier render is now deferred off this scope (see ProofingMagnifierModel), so by
    // the time the cursor is restored the remaining synchronous work (score load + following-band render) is done.
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    DEFER {
        QGuiApplication::restoreOverrideCursor();
    };

    // Read the whole bundle (manifest + score.mscz + original + proofingmap.json) off disk.
    ReprBundle bundle;
    Ret ret = readRepr(path, bundle);
    if (!ret) {
        LOGE() << "failed read .repr bundle: " << ret.toString();
        return ret;
    }

    // Open the embedded score.mscz straight from memory -- no temp .mscz on disk is needed for the score itself. The
    // bytes must outlive the reader, so scoreBytes stays in scope through doLoadByReader.
    ByteArray scoreBytes = bundle.scoreMscz;
    Buffer scoreBuf = Buffer::opened(IODevice::OpenMode::ReadOnly, &scoreBytes);

    MscReader::Params rparams;
    rparams.device = &scoreBuf;
    rparams.mode = MscIoMode::Zip;

    MscReader reader(rparams);
    ret = reader.open();
    if (!ret) {
        LOGE() << "failed open embedded score.mscz from .repr";
        return ret;
    }

    ret = doLoadByReader(reader, path, openParams);
    reader.close();
    if (!ret) {
        return ret;
    }

    // Retain the manifest provenance + original scan info so a later Save/Save-As can re-assemble the bundle.
    m_reprOriginalFileName = bundle.originalFileName;
    m_reprOriginalSuffix = bundle.originalSuffix;
    m_reprManifestImport = bundle.importJson;
    // Reprise 0.16.5+: carry the deskew provenance forward so a resave re-emits it (alignment itself comes from the map).
    m_reprSkewAnglesDeg = bundle.skewAnglesDeg;

    const QString tempRoot = QDir::tempPath() + "/reprise-repr";

    // The original scan. When a deskewed proofing-scan is present (extracted below), the band + magnifier render THAT, not
    // the original -- the original is needed only to re-bundle on a future Save. So in that case DEFER its temp extraction:
    // record the source bundle path and let doSaveRepr extract the original on demand (readReprOriginal), keeping a
    // potentially-large temp write off the .repr open path. When there is NO proofing-scan, the original IS the proofing/
    // display source and must be on disk now, so it is still extracted to a REAL temp path (QPdfDocument::load / QImageReader
    // render by filesystem path -- a path inside the ZIP would never render). Best-effort: a missing/failed original just
    // opens the score with no proofing view.
    QString proofingSourceQ;
    if (bundle.hasProofingScan && !bundle.original.empty()) {
        // Deferred: the deskewed render (extracted below) is the display source; remember where to get the original at save.
        m_reprSourceBundlePath = path;
    } else if (!bundle.original.empty()) {
        const QString baseName = bundle.originalFileName.empty()
                                 ? (QStringLiteral("original.")
                                    + QString::fromStdString(bundle.originalSuffix.empty() ? std::string("dat") : bundle.originalSuffix))
                                 : QString::fromStdString(bundle.originalFileName);

        QDir().mkpath(tempRoot);
        const QString extractedQ = tempRoot + "/" + QString::number(QDateTime::currentMSecsSinceEpoch()) + "-" + baseName;
        const muse::io::path_t extracted(extractedQ);

        Ret wret = fileSystem()->writeFile(extracted, bundle.original);
        if (wret) {
            m_reprOriginalPath = extracted;
            proofingSourceQ = extractedQ;   // default proofing source: the literal original (overridden below if deskewed)
        } else {
            LOGW() << "Reprise .repr: failed to extract original scan to temp: " << extractedQ;
        }
    }

    // Reprise 0.16.5+: when the bundle also carries a deskewed render, extract it too and make IT the proofing source, so
    // the band/magnifier render the straightened page the serialized boxes live in (boxes are in deskewed-image space).
    // Retain its path + suffix so a resave re-bundles the render losslessly. The literal original above is untouched.
    if (bundle.hasProofingScan) {
        const QString psSuffix = QString::fromStdString(
            bundle.proofingScanSuffix.empty() ? std::string("pdf") : bundle.proofingScanSuffix);
        const QString psName = QStringLiteral("proofing-scan.") + psSuffix;

        QDir().mkpath(tempRoot);
        const QString psQ = tempRoot + "/" + QString::number(QDateTime::currentMSecsSinceEpoch()) + "-" + psName;
        const muse::io::path_t psPath(psQ);

        Ret wret = fileSystem()->writeFile(psPath, bundle.proofingScan);
        if (wret) {
            m_reprProofingScanPath = psPath;
            m_reprProofingScanSuffix = bundle.proofingScanSuffix;
            proofingSourceQ = psQ;   // deskewed render becomes the proofing source
        } else {
            LOGW() << "Reprise .repr: failed to extract deskewed render to temp: " << psQ;
        }
    }

    // Deferred-original safety net: if we deferred the original (deskewed bundle) but the deskewed render could NOT be
    // extracted, there is no display source. Extract the original now as the fallback source so proofing still works, and
    // retain its temp path so Save reads from it (no longer the deferred source). Rare -- only on a temp write failure above.
    if (proofingSourceQ.isEmpty() && m_reprOriginalPath.empty() && !bundle.original.empty()) {
        const QString baseName = bundle.originalFileName.empty()
                                 ? (QStringLiteral("original.")
                                    + QString::fromStdString(bundle.originalSuffix.empty() ? std::string("dat") : bundle.originalSuffix))
                                 : QString::fromStdString(bundle.originalFileName);

        QDir().mkpath(tempRoot);
        const QString extractedQ = tempRoot + "/" + QString::number(QDateTime::currentMSecsSinceEpoch()) + "-" + baseName;
        const muse::io::path_t extracted(extractedQ);

        Ret wret = fileSystem()->writeFile(extracted, bundle.original);
        if (wret) {
            m_reprOriginalPath = extracted;
            m_reprSourceBundlePath = muse::io::path_t();   // temp now present -> no longer the deferred extract-on-demand source
            proofingSourceQ = extractedQ;
        } else {
            LOGW() << "Reprise .repr: deferred-original fallback extraction also failed: " << extractedQ;
        }
    }

    // Point the proofing map at the chosen proofing source (deskewed render if present, else the literal original). The
    // serialized boxes align to that image's coordinate space, so the strip + magnifier render correctly on reopen.
    if (bundle.hasMap && !proofingSourceQ.isEmpty()) {
        bundle.map.sourcePath = proofingSourceQ.toStdString();
        if (auto svc = proofingMapService()) {
            // mapChanged() fires -> the strip + magnifier rehydrate exactly as they do for a fresh import's map.
            svc->setMap(bundle.map);
        }
    }

    // Reprise 0.18.0: inject the bad-timing navigator per-record collapse/ignore state (an empty map when the bundle
    // carried none) into the service. setStates fires statesChanged -> the live BadTimingNavigatorModel re-hydrates, and
    // arms the one-shot claim consumed by onDocumentLoaded (in load()) so this document owns the state for later saves.
    if (auto nsvc = navigatorStateService()) {
        nsvc->setStates(bundle.navigatorState);
    }

    // Reprise 0.18.2: inject the proofing view state (the bundle's values, or the defaults followedMeasure -1 / band 0.5 /
    // magnifier 1.0 when the entry was absent) into the service. setViewState fires viewStateChanged -> the live band +
    // magnifier models re-hydrate (silently), and arms the one-shot claim consumed by onDocumentLoaded (in load()) so this
    // document owns the view state for later saves.
    if (auto vsvc = proofingViewStateService()) {
        vsvc->setViewState(bundle.viewState);
    }

    return make_ret(Ret::Code::Ok);
}

Ret NotationProject::doSaveRepr(const muse::io::path_t& path, bool isAutosave)
{
    TRACEFUNC;

    // v1: .repr opts OUT of periodic autosave (a bundle with the full scan can be large). Crash-recovery autosave stays
    // MSCZ-shaped and is not wired for .repr; skip quietly so the autosaver never fails on a .repr project.
    if (isAutosave) {
        return make_ret(Ret::Code::Ok);
    }

    // Build score.mscz into a memory buffer (same idiom as writeToDevice): a complete standalone .mscz, no temp on disk.
    ByteArray scoreBytes;
    {
        auto buf = Buffer::opened(IODevice::WriteOnly);

        MscWriter::Params params;
        params.device = &buf;
        params.filePath = QString::fromStdString(REPR_ENTRY_SCORE);   // internal rootfile -> "score.mscx"
        params.mode = MscIoMode::Zip;

        MscWriter msczWriter(params);
        Ret ret = writeProject(msczWriter, true /*createThumbnail*/);
        msczWriter.close();
        if (!ret) {
            LOGE() << "failed to build score.mscz for .repr: " << ret.toString();
            return ret;
        }
        if (msczWriter.hasError()) {
            LOGE() << "MscWriter error building score.mscz for .repr";
            return make_ret(Ret::Code::UnknownError);
        }

        buf.open(IODevice::OpenMode::ReadOnly);
        scoreBytes = buf.readAll();
    }

    // The proofing coordinate map: the live service map (its sourcePath is machine-local and dropped on serialize).
    reprise::ProofingMap map;
    if (auto svc = proofingMapService()) {
        if (svc->hasMap()) {
            map = svc->map();
        }
    }

    // The original scan bytes, from the retained source path (the real scan at import time, or the extracted scan after
    // opening a non-deskewed .repr). A missing original is non-fatal -- the score still round-trips, only the proofing view
    // is lost. The suffix/name are tracked alongside so the deferred path can backfill them from the source bundle.
    ByteArray originalBytes;
    std::string savedOriginalName = m_reprOriginalFileName;
    std::string savedOriginalSuffix = m_reprOriginalSuffix;
    if (!m_reprOriginalPath.empty() && fileSystem()->exists(m_reprOriginalPath)) {
        RetVal<ByteArray> rv = fileSystem()->readFile(m_reprOriginalPath);
        if (rv.ret) {
            originalBytes = rv.val;
        } else {
            LOGW() << "Reprise .repr save: cannot read original scan: " << m_reprOriginalPath;
        }
    } else if (!m_reprSourceBundlePath.empty() && fileSystem()->exists(m_reprSourceBundlePath)) {
        // Reprise 0.16.15: deferred-original path. Opening a deskewed .repr did not extract the original to temp, so read
        // just the original entry straight from the source bundle, so the resaved .repr still carries the verbatim scan.
        ByteArray ob;
        std::string obSuffix;
        std::string obName;
        Ret rret = readReprOriginal(m_reprSourceBundlePath, ob, obSuffix, obName);
        if (rret && !ob.empty()) {
            originalBytes = ob;
            if (savedOriginalSuffix.empty()) {
                savedOriginalSuffix = obSuffix;
            }
            if (savedOriginalName.empty()) {
                savedOriginalName = obName;
            }
        } else {
            LOGW() << "Reprise .repr save: cannot extract original from source bundle: " << m_reprSourceBundlePath;
        }
    } else {
        LOGW() << "Reprise .repr save: no readable original scan path retained; bundle will omit the scan";
    }

    const std::string originalName = !savedOriginalName.empty()
                                     ? savedOriginalName
                                     : io::filename(m_reprOriginalPath).toStdString();

    // Reprise 0.16.5+: the deskewed render bytes, from the retained proofing-scan path (set only when a page was
    // straightened). Empty path / missing file -> no proofing-scan entry -> bundle is byte-identical to a straight import.
    ByteArray proofingScanBytes;
    if (!m_reprProofingScanPath.empty() && fileSystem()->exists(m_reprProofingScanPath)) {
        RetVal<ByteArray> rv = fileSystem()->readFile(m_reprProofingScanPath);
        if (rv.ret) {
            proofingScanBytes = rv.val;
        } else {
            LOGW() << "Reprise .repr save: cannot read deskewed render: " << m_reprProofingScanPath;
        }
    }

    // Reprise 0.18.0: the live bad-timing navigator per-record collapse/ignore state. The QML model pushes it into the
    // service on every toggle (syncFromModel); read it here so a Save captures the current collapse/ignore. Empty (no
    // non-default record) -> assembleRepr omits the navigatorstate.json entry, keeping the bundle byte-identical.
    reprise::NavigatorStateMap navState;
    if (auto nsvc = navigatorStateService()) {
        navState = nsvc->states();
    }

    // Reprise 0.18.2: the live proofing view state. The band + magnifier models push the followed measure + their wheel-zoom
    // into the service (syncFollowedMeasure/syncBandZoom/syncMagnifierZoom); read it here so a Save captures the latest. Null
    // service (converter run) or a document never navigated/zoomed -> the defaults, so a fresh import round-trips its default look.
    reprise::ProofingViewState viewState;
    if (auto vsvc = proofingViewStateService()) {
        viewState = vsvc->viewState();
    }

    const ByteArray bundle = assembleRepr(scoreBytes, originalBytes, originalName, savedOriginalSuffix,
                                          m_reprManifestImport, map,
                                          proofingScanBytes, m_reprProofingScanSuffix, m_reprSkewAnglesDeg,
                                          navState, viewState);

    // Atomic-ish write: write a "_saving" sibling, copy over the target, remove the temp (mirrors doSave's dance).
    const muse::io::path_t normalizedPath = muse::io::Dir::fromNativeSeparators(path);
    const muse::io::path_t savePath = normalizedPath.toQString() + "_saving";

    Ret ret = fileSystem()->writeFile(savePath, bundle);
    if (!ret) {
        LOGE() << "failed to write .repr _saving file: " << savePath;
        return ret;
    }

    ret = fileSystem()->copy(savePath, normalizedPath, true /*replace*/);
    if (!ret) {
        LOGE() << "failed to move .repr into place: " << normalizedPath;
        fileSystem()->remove(savePath);
        return ret;
    }

    fileSystem()->remove(savePath);

    QFile::setPermissions(normalizedPath.toQString(),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);

    // Reprise 0.16.15: on the deferred-original path (no temp was extracted), the bundle we just wrote is self-contained
    // (it carries the verbatim original). Point the extract-on-demand source at this saved file so a later Save still finds
    // the scan even if the originally-opened bundle has since moved or been removed.
    if (m_reprOriginalPath.empty() && !originalBytes.empty()) {
        m_reprSourceBundlePath = normalizedPath;
    }

    LOGI() << "Reprise: saved .repr bundle: " << normalizedPath;
    return make_ret(Ret::Code::Ok);
}

void NotationProject::flushReprViewStateOnClose()
{
    // Reprise 0.18.2 write-on-close: see the header. Only a .repr-backed project with a real file on disk -- a plain score or an
    // unsaved fresh project has no .repr to patch.
    const muse::io::path_t reprPath = path();
    if (io::suffix(reprPath) != REPRISE_PROJECT_SUFFIX) {
        return;
    }

    if (!fileSystem()->exists(reprPath)) {
        return;
    }

    // The CURRENT live view state + navigator state (from the app-lifetime services; defaults if a service is absent).
    reprise::ProofingViewState viewState;
    if (auto vsvc = proofingViewStateService()) {
        viewState = vsvc->viewState();
    }

    reprise::NavigatorStateMap navState;
    if (auto nsvc = navigatorStateService()) {
        navState = nsvc->states();
    }

    // Re-read the current ON-DISK bundle so we re-pack with ONLY the viewstate + navigatorstate entries replaced -- an unsaved
    // SCORE edit is deliberately NOT written here (that stays the normal save/prompt flow). A bundle that cannot be read
    // (moved/corrupt) -> skip silently.
    ReprBundle bundle;
    Ret rr = readRepr(reprPath, bundle);
    if (!rr) {
        LOGW() << "Reprise: write-on-close: cannot read .repr to patch view state: " << reprPath;
        return;
    }

    // UNCHANGED vs what is already stored -> skip the re-pack so an untouched file is never rewritten.
    if (bundle.viewState == viewState && bundle.navigatorState == navState) {
        return;
    }

    // Re-assemble with the two entries replaced; every other entry carried verbatim from the on-disk bundle.
    const ByteArray out = assembleRepr(bundle.scoreMscz, bundle.original, bundle.originalFileName, bundle.originalSuffix,
                                       bundle.importJson, bundle.map, bundle.proofingScan, bundle.proofingScanSuffix,
                                       bundle.skewAnglesDeg, navState, viewState);

    // Atomic write (same temp + replace dance as doSaveRepr). No markAsUnsaved / no prompt -- a silent view-state flush.
    const muse::io::path_t normalizedPath = muse::io::Dir::fromNativeSeparators(reprPath);
    const muse::io::path_t savePath = normalizedPath.toQString() + "_saving";

    Ret ret = fileSystem()->writeFile(savePath, out);
    if (!ret) {
        LOGE() << "Reprise: write-on-close: failed to write _saving file: " << savePath;
        return;
    }

    ret = fileSystem()->copy(savePath, normalizedPath, true /*replace*/);
    if (!ret) {
        LOGE() << "Reprise: write-on-close: failed to move .repr into place: " << normalizedPath;
        fileSystem()->remove(savePath);
        return;
    }

    fileSystem()->remove(savePath);

    LOGI() << "Reprise: write-on-close: flushed proofing view state into " << normalizedPath;

    return;
}

void NotationProject::setReprBundleInfo(const muse::io::path_t& originalScanPath, const std::string& originalSuffix,
                                        const std::string& importProvenanceJson,
                                        const muse::io::path_t& proofingScanPath, const std::string& proofingScanSuffix)
{
    m_reprOriginalPath = originalScanPath;
    m_reprOriginalSuffix = originalSuffix;
    // Normalize separators first: upstream io::filename splits on '/' only, so a Windows '\' scan path would otherwise
    // leave the directory portion in the recorded manifest file name.
    m_reprOriginalFileName = io::filename(muse::io::Dir::fromNativeSeparators(originalScanPath)).toStdString();
    m_reprManifestImport = importProvenanceJson;
    // Reprise 0.16.5+: the deskewed render consumed by Audiveris (empty when the source was not deskewed -> bundle stays
    // byte-identical to a pre-0.16.5 save, storing only the literal original).
    m_reprProofingScanPath = proofingScanPath;
    m_reprProofingScanSuffix = proofingScanSuffix;
}

Ret NotationProject::doImport(const muse::io::path_t& path, const OpenParams& openParams)
{
    TRACEFUNC;

    // Find import reader
    std::string suffix = io::suffix(path);
    INotationReaderPtr scoreReader = readers()->reader(suffix);
    if (!scoreReader) {
        return make_ret(engraving::Err::FileUnknownType, path);
    }

    // Setup import reader
    INotationReader::Options options;
    if (openParams.forceMode) {
        options[INotationReader::OptionKey::ForceMode] = Val(openParams.forceMode);
    }

    // Setup engraving project
    mu::engraving::ScoreLoad sl;
    m_engravingProject->setFileInfoProvider(std::make_shared<ProjectFileInfoProvider>(this));
    mu::engraving::MasterScore* score = m_engravingProject->masterScore();

    // The order of the steps matches the order in MS3
    // (see https://github.com/musescore/MuseScore/blob/2513676e512d29d554cb6c4d37d3efaf53ea2c5b/mscore/file.cpp#L2260)

    io::path_t stylePath = openParams.stylePath.empty() ? notationConfiguration()->styleFileImportPath() : openParams.stylePath;

    // Load style if present
    if (!stylePath.empty()) {
        muse::io::File styleFile(stylePath);
        mu::engraving::EditStyle::loadStyle(score->transactionManager()->currentOrDummyTransaction(), score, styleFile);
    }

    // Init ChordList
    score->checkChordList();

    // Read (import) master score
    Ret ret = scoreReader->read(score, path, options);
    if (!ret) {
        return ret;
    }

    // Setup master score post-reading
    ret = m_engravingProject->setupMasterScore(openParams.forceMode);
    if (!ret) {
        return ret;
    }

    if (openParams.unrollRepeats && score->repeatList().size() > 1) {
        MasterScore* original = score;
        score = original->unrollRepeats();
        delete original;
        m_engravingProject->setMasterScore(score);
    }

    // Setup audio settings
    m_projectAudioSettings->makeDefault();

    // Setup view state
    m_masterNotation->notation()->viewState()->makeDefault();
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        excerpt->notation()->viewState()->makeDefault();
    }

    // Set current if all success
    m_masterNotation->setMasterScore(score, openParams.disablePlayback);
    setPath(path);
    setNeedSave(false);
    score->setMetaTag(u"originalFormat", QString::fromStdString(suffix));

    m_isNewlyCreated = true;
    m_isImported = true;

    return make_ret(Ret::Code::Ok);
}

Ret NotationProject::createNew(const ProjectCreateOptions& projectOptions)
{
    TRACEFUNC;

    setupProject();

    // Load template if present
    if (!projectOptions.templatePath.empty()) {
        Ret ret = loadTemplate(projectOptions);
        return ret;
    }

    // Create new engraving project
    setPath(projectOptions.title.isEmpty() ? scoreDefaultTitle() : projectOptions.title);
    m_engravingProject->setFileInfoProvider(std::make_shared<ProjectFileInfoProvider>(this));

    mu::engraving::MasterScore* masterScore = m_engravingProject->masterScore();
    setupScoreMetaTags(masterScore, projectOptions);

    // Setup new master score
    Ret ret = m_masterNotation->setupNewScore(masterScore, projectOptions.scoreOptions);
    if (!ret) {
        return ret;
    }

    // Setup audio settings
    m_projectAudioSettings->makeDefault();

    // Setup view state
    m_masterNotation->notation()->viewState()->makeDefault();
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        excerpt->notation()->viewState()->makeDefault();
    }

    listenIfNeedSaveChanges();
    markAsUnsaved();

    m_isNewlyCreated = true;

    return make_ret(Ret::Code::Ok);
}

Ret NotationProject::loadTemplate(const ProjectCreateOptions& projectOptions)
{
    TRACEFUNC;

    Ret ret = load(projectOptions.templatePath);

    if (ret) {
        setPath(projectOptions.title.isEmpty() ? scoreDefaultTitle() : projectOptions.title);

        mu::engraving::MasterScore* masterScore = m_masterNotation->masterScore();
        setupScoreMetaTags(masterScore, projectOptions);

        m_masterNotation->notation()->undoStack()->lock();
        m_masterNotation->applyOptions(masterScore, projectOptions.scoreOptions, true /*createdFromTemplate*/);
        m_masterNotation->notation()->undoStack()->unlock();

        setNeedSave(false);
        m_isNewlyCreated = true;
    }

    return ret;
}

muse::io::path_t NotationProject::path() const
{
    return m_path;
}

void NotationProject::setPath(const muse::io::path_t& path)
{
    if (m_path == path) {
        return;
    }

    m_path = path;
    m_pathChanged.notify();
    m_displayNameChanged.notify();
}

muse::async::Notification NotationProject::pathChanged() const
{
    return m_pathChanged;
}

QString NotationProject::displayName() const
{
    if (isNewlyCreated()) {
        if (m_path.empty()) {
            QString workTitle = m_masterNotation->notation()->workTitle();
            if (workTitle.isEmpty()) {
                return scoreDefaultTitle();
            }
            return workTitle;
        }
        return io::filename(m_path).toQString();
    }

    if (isCloudProject()) {
        return m_cloudInfo.name;
    }

    bool isSuffixInteresting = io::suffix(m_path) != engraving::MSCZ;
    return io::filename(m_path, isSuffixInteresting).toQString();
}

muse::async::Notification NotationProject::displayNameChanged() const
{
    return m_displayNameChanged;
}

bool NotationProject::isCloudProject() const
{
    return configuration()->isCloudProject(m_path);
}

const CloudProjectInfo& NotationProject::cloudInfo() const
{
    return m_cloudInfo;
}

void NotationProject::setCloudInfo(const CloudProjectInfo& info)
{
    m_cloudInfo = info;
    m_masterNotation->masterScore()->setMetaTag(SOURCE_TAG, info.sourceUrl.toString());
    m_masterNotation->masterScore()->setMetaTag(SOURCE_REVISION_ID_TAG, String::number(info.revisionId));

    m_displayNameChanged.notify();
}

const CloudAudioInfo& NotationProject::cloudAudioInfo() const
{
    return m_cloudAudioInfo;
}

void NotationProject::setCloudAudioInfo(const CloudAudioInfo& audioInfo)
{
    m_cloudAudioInfo = audioInfo;
    m_masterNotation->masterScore()->setMetaTag(AUDIO_COM_URL_TAG, audioInfo.url.toString());
}

Ret NotationProject::save(const muse::io::path_t& path, SaveMode saveMode, bool createBackup)
{
    TRACEFUNC;

    Ret ret;
    muse::io::path_t savePath = path;

    switch (saveMode) {
    case SaveMode::SaveSelection: {
        ret = saveSelectionOnScore(savePath);
    } break;
    case SaveMode::Save:
    case SaveMode::SaveAs:
    case SaveMode::SaveCopy: {
        if (savePath.empty()) {
            IF_ASSERT_FAILED(!m_path.empty()) {
                return false;
            }

            savePath = m_path;
        }

        std::string suffix = io::suffix(savePath);

        // Whether a backup file will be created depends on both the caller's and user's will
        bool shouldCreateBackup = createBackup && configuration()->createBackupBeforeSaving();

        ret = saveScore(savePath, suffix, shouldCreateBackup);
        if (ret) {
            if (saveMode != SaveMode::SaveCopy) {
                markAsSaved(savePath);
            }

            // Re-compute headers and footers on save, to force timestamps update (if any)
            bool timestampsUpdated = false;
            for (Score* s : m_engravingProject->masterScore()->scoreList()) {
                if (renderer()->scoreHasTimestampHeadersFooters(s)) {
                    renderer()->layoutHeadersFooters(s);
                    timestampsUpdated = true;
                }
            }
            if (timestampsUpdated) {
                projectSession()->currentNotation()->notationChanged().send(muse::RectF());
            }
        }
    } break;
    case SaveMode::AutoSave: {
        std::string suffix = io::suffix(savePath);
        if (suffix == IProjectAutoSaver::AUTOSAVE_SUFFIX) {
            suffix = io::suffix(io::completeBasename(savePath));
        }

        if (suffix.empty()) {
            // Then it must be a MSCX folder
            suffix = engraving::MSCX;
        }

        ret = saveScore(savePath, suffix, false /*generateBackup*/, false /*createThumbnail*/, true /*isAutosave*/);
    } break;
    case SaveMode::SavePage:
        ASSERT_X("Use savePage(path, pageNum)");
        ret = muse::make_ret(Ret::Code::UnknownError);
        break;
    }

    if (ret) {
        m_saved.send(savePath, saveMode);
    }

    return ret;
}

muse::Ret NotationProject::savePage(const muse::io::path_t& path, const size_t pageNum)
{
    TRACEFUNC;

    const MasterScore* score = m_masterNotation->masterScore();
    IF_ASSERT_FAILED(pageNum < score->npages()) {
        return make_ret(notation::Err::UnknownError);
    }

    const Page* page = score->pages().at(pageNum);
    const std::vector<System*>& systems = page->systems();
    if (systems.empty()) {
        return make_ret(notation::Err::UnknownError);
    }

    write::WriteRange range;
    range.startStaffIdx = 0;
    range.endStaffIdx = score->nstaves();
    range.startMeasure = systems.front()->first();
    // StaffWrite::writeStaff stops before reaching endMeasure, so use the measure after
    // the last one we want, otherwise the page's last measure/frame is dropped
    MeasureBase* lastMeasure = systems.back()->last();
    range.endMeasure = lastMeasure ? lastMeasure->next() : nullptr;

    write::WriteContext ctx(score);
    ctx.setRange(range);

    Ret ret = writeProject(path, &ctx);
    if (ret) {
        m_saved.send(path, SaveMode::SavePage);
    }

    return ret;
}

muse::async::Channel<path_t, SaveMode> NotationProject::saveComplited() const
{
    return m_saved;
}

Ret NotationProject::writeToDevice(QIODevice* device)
{
    TRACEFUNC;

    IF_ASSERT_FAILED(!m_path.empty()) {
        return make_ret(notation::Err::UnknownError);
    }

    auto buf = Buffer::opened(IODevice::WriteOnly);

    MscWriter::Params params;
    params.device = &buf;
    params.filePath = m_path.toQString();
    params.mode = MscIoMode::Zip;

    MscWriter msczWriter(params);
    msczWriter.open();

    Ret ret = writeProject(msczWriter, false);
    msczWriter.close();

    if (ret) {
        if (msczWriter.hasError()) {
            LOGE() << "MSCZ writer has error";
            return make_ret(Ret::Code::UnknownError);
        }

        buf.open(IODevice::OpenMode::ReadOnly);
        ByteArray ba = buf.readAll();

        if ((size_t)device->write(ba.toQByteArrayNoCopy()) != ba.size()) {
            LOGE() << "Error writing to device";
            return make_ret(Ret::Code::UnknownError);
        }
    }

    return ret;
}

Ret NotationProject::saveScore(const muse::io::path_t& path, const std::string& fileSuffix,
                               bool generateBackup, bool createThumbnail, bool isAutosave,
                               const write::WriteContext* ctx)
{
    // Reprise .repr: our custom project container -- route to doSaveRepr BEFORE the writers-register export fallthrough
    // (which would otherwise treat an unknown suffix as an export format).
    if (fileSuffix == REPRISE_PROJECT_SUFFIX) {
        return doSaveRepr(path, isAutosave);
    }

    if (!isMuseScoreFile(fileSuffix) && !fileSuffix.empty()) {
        return exportProject(path, fileSuffix);
    }

    MscIoMode ioMode = mscIoModeBySuffix(fileSuffix);

    return doSave(path, ioMode, generateBackup, createThumbnail, isAutosave, ctx);
}

Ret NotationProject::doSave(const muse::io::path_t& path, engraving::MscIoMode ioMode,
                            bool generateBackup, bool createThumbnail, bool isAutosave,
                            const write::WriteContext* ctx)
{
    TRACEFUNC;

    // Reprise: normalize separators so a Windows backslash save target (e.g. an MCP save-at arg) yields a correct basename .mscx entry and '/'-rootfile. Upstream muse FileInfo::completeBaseName splits on '/' only, so a '\' path would make the whole drive-stripped path the ZIP entry name and break reopen. Covers every save caller (save-at / Save / Save-As / autosave).
    const muse::io::path_t normalizedPath = muse::io::Dir::fromNativeSeparators(path);

    QString targetContainerPath = engraving::containerPath(normalizedPath).toQString();
    muse::io::path_t targetMainFilePath = engraving::mainFilePath(normalizedPath);
    muse::io::path_t targetMainFileName = engraving::mainFileName(normalizedPath);
    QString savePath = targetContainerPath + "_saving";

    // Step 1: check writable
    {
        if ((fileSystem()->exists(savePath) && !fileSystem()->isWritable(savePath))
            || (fileSystem()->exists(targetContainerPath) && !fileSystem()->isWritable(targetContainerPath))) {
            LOGE() << "failed save, not writable path: " << targetContainerPath;
            return make_ret(io::Err::FSWriteError);
        }

        if (ioMode == engraving::MscIoMode::Dir) {
            // Dir needs to be created, otherwise we can't move to it
            if (!QDir(targetContainerPath).mkpath(".")) {
                LOGE() << "Couldn't create container directory: " << targetContainerPath;
                return make_ret(io::Err::FSMakingError);
            }
        }
    }

    // Step 2: write project
    {
        MscWriter::Params params;
        params.filePath = savePath;
        params.mainFileName = targetMainFileName.toQString();
        params.mode = ioMode;
        IF_ASSERT_FAILED(params.mode != MscIoMode::Unknown) {
            return make_ret(Ret::Code::InternalError);
        }

        const bool shouldCorrupt = ioMode == MscIoMode::Zip
                                   && !isAutosave
                                   && globalConfiguration()->devModeEnabled()
                                   && savePath.contains(" - ALL_ZEROS_CORRUPTED.mscz");

        std::unique_ptr<Buffer> maybeOutBuf;
        if (shouldCorrupt) {
            // Create corrupted data so devs/qa can simulate a saved corrupted file.
            maybeOutBuf = std::make_unique<AllZerosBufferCorruptor>();
        } else if (ioMode != engraving::MscIoMode::Dir) {
            maybeOutBuf = std::make_unique<Buffer>();
        }
        params.device = maybeOutBuf.get();

        MscWriter msczWriter(params);
        Ret ret = writeProject(msczWriter, createThumbnail, ctx);
        msczWriter.close();

        if (!ret) {
            LOGE() << "failed write project to buffer: " << ret.toString();
            return ret;
        }

        if (msczWriter.hasError()) {
            LOGE() << "MscWriter has error after writing project";
            return make_ret(Ret::Code::UnknownError);
        }

        if (maybeOutBuf) {
#ifdef MUSE_THREADS_SUPPORT
            if (isAutosave) {
                // For autosave, write to disk on a background thread to avoid stuttering.
                // The buffer is fully serialized at this point so it's safe to move off the main thread.
                muse::ByteArray data = maybeOutBuf->data();
                QString savePathCopy = savePath;
                QString targetContainerPathCopy = targetContainerPath;
                muse::io::path_t targetMainFilePathCopy = targetMainFilePath;
                auto fs = fileSystem();
                Concurrent::run([fs, savePathCopy, targetContainerPathCopy, targetMainFilePathCopy, data]() {
                    Ret writeRet = fs->writeFile(savePathCopy, data);
                    if (!writeRet) {
                        LOGE() << "Autosave: failed to write project file: " << writeRet.toString();
                        return;
                    }
                    Ret copyRet = fs->copy(savePathCopy, targetContainerPathCopy, true);
                    if (!copyRet) {
                        LOGE() << "Autosave: failed to copy to target: " << copyRet.toString();
                        return;
                    }
                    fs->remove(savePathCopy);
                    QFile::setPermissions(targetMainFilePathCopy.toQString(),
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);
                    LOGD() << "Autosave: background write complete: " << targetContainerPathCopy;
                });
                return make_ret(Ret::Code::Ok);
            }
#endif

            ret = fileSystem()->writeFile(savePath, maybeOutBuf->data());
            if (!ret) {
                LOGE() << "Failed to write project file";
                return ret;
            }
        }
    }

    // Step 3: create backup if need
    {
        if (generateBackup) {
            makeBackup(targetContainerPath);
        }
    }

    // Step 4: replace to saved file
    {
        if (ioMode == MscIoMode::Dir) {
            RetVal<io::paths_t> filesToBeMoved = fileSystem()->scanFiles(savePath, { "*" }, io::ScanMode::FilesAndFoldersInCurrentDir);
            if (!filesToBeMoved.ret) {
                return filesToBeMoved.ret;
            }

            Ret ret = muse::make_ok();

            for (const muse::io::path_t& fileToBeMoved : filesToBeMoved.val) {
                muse::io::path_t destinationFile
                    = muse::io::path_t(targetContainerPath).appendingComponent(io::filename(fileToBeMoved));
                LOGD() << fileToBeMoved << " to " << destinationFile;
                ret = fileSystem()->move(fileToBeMoved, destinationFile, true);
                if (!ret) {
                    return ret;
                }
            }

            // Try to remove the temp save folder (not problematic if fails)
            ret = fileSystem()->remove(savePath, true);
            if (!ret) {
                LOGW() << ret.toString();
            }
        } else {
            Ret ret = muse::make_ok();

            ret = fileSystem()->copy(savePath, targetContainerPath, true);
            if (!ret) {
                return ret;
            }

            if (!isAutosave) {
                ret = checkSavedFileForCorruption(ioMode, targetContainerPath, targetMainFileName.toQString());
                if (!ret) {
                    if (ret.code() == (int)Err::CorruptionUponSavingError) {
                        // Validate the temporary "saving" file too.
                        Ret ret2 = checkSavedFileForCorruption(ioMode, savePath, targetMainFileName.toQString());
                        if (!ret2) {
                            return ret2;
                        }
                    }
                    return ret;
                }
            }

            // Remove the temp save file (not problematic if fails)
            fileSystem()->remove(savePath);
        }
    }

    // make file readable by all
    {
        QFile::setPermissions(targetMainFilePath.toQString(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther);
    }

    LOGI() << "success save file: " << targetContainerPath;
    return make_ret(Ret::Code::Ok);
}

Ret NotationProject::makeBackup(muse::io::path_t filePath)
{
    TRACEFUNC;

    if (io::suffix(filePath) != engraving::MSCZ) {
        LOGW() << "backup allowed only for MSCZ, currently: " << filePath;
        return make_ret(Ret::Code::Ok);
    }

    Ret ret = fileSystem()->exists(filePath);
    if (!ret) {
        LOGI() << "Backup won't be created, file does not exist: " << filePath;
        return make_ret(Ret::Code::Ok);
    }

    muse::io::path_t backupPath = configuration()->projectBackupPath(filePath);
    muse::io::path_t backupDir = io::absoluteDirpath(backupPath);
    ret = fileSystem()->makePath(backupDir);
    if (!ret) {
        LOGE() << "failed to create backup directory: " << backupDir;
        return ret;
    }

    fileSystem()->setAttribute(backupDir, io::IFileSystem::Attribute::Hidden);

    ret = fileSystem()->copy(filePath, backupPath, true);
    if (!ret) {
        LOGE() << "failed to copy: " << filePath << " to: " << backupPath;
        return ret;
    }

    fileSystem()->setAttribute(backupPath, io::IFileSystem::Attribute::Hidden);

    return ret;
}

muse::Ret NotationProject::writeProject(const muse::io::path_t& path, const write::WriteContext* ctx)
{
    TRACEFUNC;

    IF_ASSERT_FAILED(path != m_path) {
        return make_ret(notation::Err::UnknownError);
    }

    std::string suffix = io::suffix(path);

    if (mscIoModeBySuffix(suffix) == MscIoMode::Unknown) {
        return make_ret(Ret::Code::InternalError);
    }

    return saveScore(path, suffix, false /*generateBackup*/, true /*createThumbnail*/, false /*isAutosave*/, ctx);
}

Ret NotationProject::writeProject(MscWriter& msczWriter, bool createThumbnail, const write::WriteContext* ctx)
{
    TRACEFUNC;

    // Create MsczWriter
    Ret ret = msczWriter.open();
    if (!ret) {
        LOGE() << "failed open writer: " << ret.toString();
        return ret;
    }

    // Write engraving project
    ret = m_engravingProject->writeMscz(msczWriter, createThumbnail, ctx);
    if (!ret) {
        LOGE() << "failed write engraving project to mscz: " << ret.toString();
        return make_ret(notation::Err::UnknownError);
    }

    // Write master audio settings
    ret = m_projectAudioSettings->write(msczWriter, m_masterNotation->notation()->soloMuteState());
    if (!ret) {
        LOGE() << "failed write project audio settings, err: " << ret.toString();
        return ret;
    }

    // Write master view settings
    m_masterNotation->notation()->viewState()->write(msczWriter);

    if (ctx && ctx->shouldWriteRange()) {
        return make_ret(Ret::Code::Ok);
    }

    // Write excerpt view settings and solo-mute state
    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        muse::io::path_t path = u"Excerpts/" + excerpt->fileName() + u"/";
        excerpt->notation()->viewState()->write(msczWriter, path);

        ByteArray soloMuteData;
        auto soloMuteBuf = Buffer::opened(IODevice::WriteOnly, &soloMuteData);
        excerpt->notation()->soloMuteState()->write(&soloMuteBuf /*out*/);
        msczWriter.writeAudioSettingsJsonFile(soloMuteData, path);
    }

    return make_ret(Ret::Code::Ok);
}

Ret NotationProject::saveSelectionOnScore(const muse::io::path_t& path)
{
    TRACEFUNC;

    const MasterScore* score = m_masterNotation->masterScore();
    if (score->selectionEmpty()) {
        LOGE() << "failed save, empty selection";
        return make_ret(notation::Err::EmptySelection);
    }

    const Selection& selection = score->selection();

    write::WriteRange range;
    range.startStaffIdx = selection.staffStart();
    range.endStaffIdx = selection.staffEnd();

    range.startMeasure = selection.startSegment()->measure();
    if (range.startMeasure && range.startMeasure->isMeasure() && toMeasure(range.startMeasure)->isMMRest()) {
        range.startMeasure = toMeasure(range.startMeasure)->mmRestFirst();
    }

    if (const Segment* endSeg = selection.endSegment()) {
        range.endMeasure = endSeg->measure()->next();
    } else {
        range.endMeasure = nullptr;
    }

    write::WriteContext ctx(score);
    ctx.setRange(range);
    ctx.setFilter(score->selectionFilter());

    return writeProject(path, &ctx);
}

Ret NotationProject::checkSavedFileForCorruption(MscIoMode ioMode, const muse::io::path_t& path,
                                                 const muse::io::path_t& scoreFileName)
{
    TRACEFUNC;

    if (ioMode != MscIoMode::Zip) {
        return muse::make_ok();
    }

    MscReader::Params params;
    params.filePath = path;
    params.mainFileName = scoreFileName.toString();
    params.mode = MscIoMode::Zip;

    MscReader msczReader(params);
    Ret ret = msczReader.open();
    if (!ret) {
        return Ret(static_cast<int>(Err::CorruptionUponSavingError),
                   muse::mtrc("project/save", "File “%1” could not be opened for validation. %2")
                   .arg(path.toString(), String(ret.toString().c_str()))
                   .toStdString());
    }

    // Try to extract the main score file.
    ByteArray scoreFile = msczReader.readScoreFile();
    msczReader.close();

    if (scoreFile.empty()) {
        return Ret(static_cast<int>(Err::CorruptionUponSavingError),
                   muse::mtrc("project/save", "“%1” is corrupted or damaged.")
                   .arg(path.toString())
                   .toStdString());
    }

    return muse::make_ok();
}

Ret NotationProject::exportProject(const muse::io::path_t& path, const std::string& suffix)
{
    TRACEFUNC;

    Buffer outBuf = Buffer::opened(IODevice::WriteOnly);

    auto writer = writers()->writer(suffix);
    if (!writer) {
        LOGE() << "Unknown export format: " << suffix;
        return false;
    }

    Ret ret = writer->writeSync(m_masterNotation->notation(), outBuf);
    if (!ret) {
        return ret;
    }
    outBuf.close();

    return File::writeFile(path, outBuf.data());
}

IMasterNotationPtr NotationProject::masterNotation() const
{
    return m_masterNotation;
}

bool NotationProject::isNewlyCreated() const
{
    return m_isNewlyCreated;
}

void NotationProject::markAsNewlyCreated()
{
    m_isNewlyCreated = true;

    mu::engraving::MasterScore* masterScore = m_masterNotation->masterScore();

    QString title = masterScore->metaTag(WORK_TITLE_TAG);
    setPath(!title.isEmpty() ? title : scoreDefaultTitle());

    markAsUnsaved();

    m_masterNotation->notation()->undoStack()->stackChanged().notify();
}

bool NotationProject::isImported() const
{
    return m_isImported;
}

void NotationProject::markAsUnsaved()
{
    setNeedSave(true);
}

void NotationProject::listenIfNeedSaveChanges()
{
    if (isCloudProject()) {
        //! NOTE: master is always open once loaded; set before listening so it doesn't count as a change
        m_masterNotation->notation()->setIsOpen(true);
    }

    m_masterNotation->notation()->undoStack()->changesChannel().onReceive(this, [this](const ScoreChanges&) {
        bool isStackClean = m_masterNotation && m_masterNotation->notation()->undoStack()->isStackClean();

        if (isStackClean && !m_hasNonUndoStackChanges) {
            setNeedSave(false);
        } else {
            setNeedSave(true);
        }
    });

    auto listenNonUndoStackChanges = [this](const INotationPtr& notation) {
        notation->openChanged().onNotify(this, [this]() {
            markAsUnsaved();
            m_hasNonUndoStackChanges = true;
        }, Mode::SetReplace);

        notation->viewState()->stateChanged().onNotify(this, [this]() {
            markAsUnsaved();
            m_hasNonUndoStackChanges = true;
        }, Mode::SetReplace);

        notation->soloMuteState()->trackSoloMuteStateChanged().onReceive(
            this, [this](const InstrumentTrackId&, const notation::INotationSoloMuteState::SoloMuteState&) {
            markAsUnsaved();
            m_hasNonUndoStackChanges = true;
        }, Mode::SetReplace);
    };

    listenNonUndoStackChanges(m_masterNotation->notation());

    for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
        listenNonUndoStackChanges(excerpt->notation());
    }

    m_masterNotation->excerptsChanged().onNotify(this, [this, listenNonUndoStackChanges]() {
        for (const IExcerptNotationPtr& excerpt : m_masterNotation->excerpts()) {
            listenNonUndoStackChanges(excerpt->notation());
        }
    });

    m_projectAudioSettings->settingsChanged().onNotify(this, [this]() {
        markAsUnsaved();
        m_hasNonUndoStackChanges = true;
    });
}

void NotationProject::markAsSaved(const muse::io::path_t& path)
{
    TRACEFUNC;

    //! NOTE: order is important
    m_isNewlyCreated = false;

    setNeedSave(false);

    setPath(path);

    m_masterNotation->notation()->undoStack()->stackChanged().notify();
}

void NotationProject::setNeedSave(bool needSave)
{
    mu::engraving::MasterScore* score = m_masterNotation->masterScore();
    if (!score) {
        return;
    }

    setNeedAutoSave(needSave);

    if (!needSave) {
        m_hasNonUndoStackChanges = false;
    }

    if (m_isNeedSave == needSave) {
        return;
    }

    m_isNeedSave = needSave;
    m_needSaveChanged.notify();
}

bool NotationProject::isNeedSave() const
{
    return m_isNeedSave;
}

muse::async::Notification NotationProject::needSaveChanged() const
{
    return m_needSaveChanged;
}

Ret NotationProject::canSave() const
{
    if (!m_masterNotation->hasParts()) {
        return make_ret(Err::NoPartsError);
    }

    Ret ret = m_engravingProject->checkCorrupted();
    if (!ret) {
        Err errorCode = m_engravingProject->isCorruptedUponLoading() ? Err::CorruptionUponOpenningError : Err::CorruptionError;
        ret.setCode(static_cast<int>(errorCode));
    }

    return ret;
}

bool NotationProject::needAutoSave() const
{
    return m_needAutoSave;
}

void NotationProject::setNeedAutoSave(bool val)
{
    m_needAutoSave = val;
}

ProjectMeta NotationProject::metaInfo() const
{
    TRACEFUNC;

    const engraving::MasterScore* score = m_masterNotation->masterScore();
    const std::map<String, String>& allTags = score->metaTags();

    const auto initProjectMetaString = [&allTags](QString& string, const String& tag) {
        const auto search = allTags.find(tag);
        if (search == allTags.end()) {
            LOGW() << "Project meta tag not found: " << tag;
            return;
        }
        string = search->second;
    };

    ProjectMeta meta;
    meta.filePath = m_path;

    initProjectMetaString(meta.title, WORK_TITLE_TAG);
    initProjectMetaString(meta.subtitle, SUBTITLE_TAG);
    initProjectMetaString(meta.composer, COMPOSER_TAG);
    initProjectMetaString(meta.arranger, ARRANGER_TAG);
    initProjectMetaString(meta.lyricist, LYRICIST_TAG);
    initProjectMetaString(meta.translator, TRANSLATOR_TAG);
    initProjectMetaString(meta.copyright, COPYRIGHT_TAG);

    if (allTags.find(CREATION_DATE_TAG) != allTags.end()) {
        meta.creationDate = QDate::fromString(allTags.at(CREATION_DATE_TAG), Qt::ISODate);
    } else {
        LOGW() << "Project meta tag not found: " << CREATION_DATE_TAG;
    }

    meta.partsCount = score->excerpts().size();

    initProjectMetaString(meta.source, SOURCE_TAG);
    initProjectMetaString(meta.audioComUrl, AUDIO_COM_URL_TAG);
    initProjectMetaString(meta.platform, PLATFORM_TAG);

    meta.musescoreVersion = score->mscoreVersion();
    meta.musescoreRevision = score->mscoreRevision();
    meta.mscVersion = score->mscVersion();

    for (const auto& [tag, value] : allTags) {
        if (isRepresentedInProjectMeta(tag)) {
            continue;
        }

        meta.additionalTags[tag] = value.toQString();
    }

    return meta;
}

void NotationProject::setMetaInfo(const ProjectMeta& meta, bool undoable)
{
    if (meta == metaInfo()) {
        return;
    }

    std::map<String, String> tags {
        { WORK_TITLE_TAG, meta.title },
        { SUBTITLE_TAG, meta.subtitle },
        { COMPOSER_TAG, meta.composer },
        { ARRANGER_TAG, meta.arranger },
        { LYRICIST_TAG, meta.lyricist },
        { TRANSLATOR_TAG, meta.translator },
        { COPYRIGHT_TAG, meta.copyright },
        { CREATION_DATE_TAG, meta.creationDate.toString(Qt::ISODate) },
        { SOURCE_TAG, meta.source },
        { AUDIO_COM_URL_TAG, meta.audioComUrl },
        { PLATFORM_TAG, meta.platform },
    };

    for (auto it = meta.additionalTags.cbegin(); it != meta.additionalTags.cend(); ++it) {
        tags[it.key()] = it.value().toString();
    }

    MasterScore* score = m_masterNotation->masterScore();

    if (undoable) {
        m_masterNotation->notation()->undoStack()->prepareChanges(TranslatableString("undoableAction", "Edit project properties"));
        score->undo(new mu::engraving::ChangeMetaTags(score, tags));
        m_masterNotation->notation()->undoStack()->commitChanges();
        m_masterNotation->notation()->notationChanged().send(muse::RectF());
    } else {
        score->setMetaTags(tags);
    }
}

IProjectAudioSettingsPtr NotationProject::audioSettings() const
{
    return m_projectAudioSettings;
}
