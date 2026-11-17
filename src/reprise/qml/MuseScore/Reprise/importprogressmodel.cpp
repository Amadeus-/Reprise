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

#include "importprogressmodel.h"

#include <vector>

#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include "global/translation.h"
#include "global/log.h"
#include "global/types/val.h"
#include "global/types/bytearray.h"
#include "global/io/path.h"
#include "global/serialization/zipreader.h"
#include "global/serialization/xmlstreamreader.h"

#include "async/notifylist.h"

#include "notation/imasternotation.h"
#include "notation/inotationparts.h"
#include "notation/types/instrumentstypes.h"

#include "engraving/dom/instrument.h"
#include "engraving/dom/part.h"
#include "engraving/dom/masterscore.h"

#include "reprise/reprisecommands.h"
#include "project/projectcommands.h"

#include "internal/audiverisrunner.h"

using namespace mu;
using namespace mu::reprise;
using namespace muse;

//! How long the Done state lingers before an auto-close (ms) -- enough to show the gold check, and to let the
//! just-opened score's master-notation change fire so the Grand-Piano override lands before the model is gone.
static constexpr int AUTO_CLOSE_DELAY_MS = 1400;

//! How long to wait for command://project/open to resolve before concluding the recognised score never opened. The
//! dispatch returns before the async open completes and its Ret is always ok (ProjectActionsController::runAsync
//! swallows the real outcome), so the only reliable success signal is masterNotationChanged firing; this bounded
//! window -- re-checked against masterNotation() so a slow-but-valid open can never be mis-flagged -- is how a silent
//! open-decline becomes a clear error. Generous on purpose: a local MusicXML open is sub-second, so it never trips a
//! legitimately slow open.
static constexpr int OPEN_RESOLVE_TIMEOUT_MS = 8000;

ImportProgressModel::ImportProgressModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: Nothing that resolves the IoC context may run here. iocCtxForQmlObject(this) is lazy: it reads
    //! qmlContext(this), which QML only attaches AFTER this C++ ctor returns. Touching a ContextInject (e.g.
    //! projectSession()) or iocContext() now trips the "QQmlContext is not set" assert, resolves to a null
    //! service, and crashes on first use. All context-dependent setup is deferred to initContext(), run from
    //! load() (driven by the popup's Component.onCompleted, by which point the context is live).
}

ImportProgressModel::~ImportProgressModel() = default;

void ImportProgressModel::initContext()
{
    if (m_runner)
    {
        return;
    }

    m_runner = std::make_shared<AudiverisRunner>(iocContext());

    QObject::connect(m_runner.get(), &AudiverisRunner::progressChanged, this, &ImportProgressModel::onRunnerProgress);
    QObject::connect(m_runner.get(), &AudiverisRunner::finished, this, &ImportProgressModel::onRunnerFinished);
    QObject::connect(m_runner.get(), &AudiverisRunner::outputLine, this, &ImportProgressModel::appendLog);

    //! Once the imported score becomes the current master (the open is async): apply the Grand-Piano override and
    //! fill the Done line with the real measure count.
    projectSession()->masterNotationChanged().onNotify(this, [this]()
    {
        if (!projectSession()->masterNotation())
        {
            return;
        }

        if (m_pendingGrandPianoReset)
        {
            m_pendingGrandPianoReset = false;
            applyGrandPianoDefault();
        }

        if (m_pendingDoneCounts)
        {
            m_pendingDoneCounts = false;
            updateDoneLine();
        }

        //! The imported score definitively opened -- this is the only reliable success signal (see OPEN_RESOLVE_TIMEOUT_MS).
        //! Finalize the Done state here, which also cancels the pending open-decline fallback (it checks m_awaitingOpen).
        if (m_awaitingOpen)
        {
            m_awaitingOpen = false;
            setStatus("done");
            importProgressTracker()->reportFinished(true, std::string(), std::string());

            if (closePopupOnComplete())
            {
                QTimer::singleShot(AUTO_CLOSE_DELAY_MS, this, [this]() { emit closeRequested(); });
            }
        }
    });
}

bool ImportProgressModel::closePopupOnComplete() const
{
    return repriseConfiguration()->closePopupOnComplete();
}

void ImportProgressModel::setClosePopupOnComplete(bool close)
{
    if (close == closePopupOnComplete())
    {
        return;
    }

    repriseConfiguration()->setClosePopupOnComplete(close);
    emit closePopupOnCompleteChanged();
}

void ImportProgressModel::load(const QString& pdfPath, const QString& pages, int dpi,
                               bool recognizeText, const QString& language, bool chordSymbols, bool lyricsAboveStaff)
{
    //! Resolve the IoC context + build the runner now (first call only) -- the QQmlContext is attached by the
    //! time Component.onCompleted calls load(), so this is safe here where the ctor was not.
    initContext();

    m_title = QFileInfo(pdfPath).fileName();
    m_pages = pages;
    m_dpi = dpi;
    m_pagesLabel = formatPagesLabel(pages);
    emit titleChanged();

    //! Reset ALL per-import state so every run starts independent of any earlier one (idempotent).
    m_currentPage = -1;
    m_totalPages = -1;
    m_errorDetails.clear();
    m_logText.clear();
    emit logTextChanged();
    m_pendingGrandPianoReset = false;
    m_pendingDoneCounts = false;
    m_awaitingOpen = false;

    setStatus("running");
    m_indeterminate = true;
    m_percent = -1;
    m_stageText = muse::qtrc("reprise", "Starting Audiveris...");
    emit progressChanged();

    RecognitionOptions options;
    options.recognizeText = recognizeText;
    options.language = language.trimmed().isEmpty() ? QString("eng") : language.trimmed();
    options.chordSymbols = chordSymbols;
    options.lyricsAboveStaff = lyricsAboveStaff;

    const bool started = m_runner->start(io::path_t(pdfPath), pages, dpi, options);
    if (!started)
    {
        m_errorDetails.clear();
        setStatus("error");
        m_stageText = muse::qtrc("reprise", "Audiveris was not found.");
        emit progressChanged();
        importProgressTracker()->reportFinished(false, m_stageText.toStdString(), std::string());
    }
}

void ImportProgressModel::cancel()
{
    if (m_runner)
    {
        m_runner->cancel();
    }

    m_pendingGrandPianoReset = false;
    m_pendingDoneCounts = false;
    m_awaitingOpen = false;
    emit closeRequested();
}

void ImportProgressModel::onRunnerProgress(int percent, int sheet, int total, const QString& stageHuman)
{
    m_percent = percent;
    m_indeterminate = (percent < 0);

    if (total > 0)
    {
        m_totalPages = total;
    }

    if (sheet > 0)
    {
        m_currentPage = sheet;
    }

    if (total > 0 && sheet > 0)
    {
        //: %1 = current page number, %2 = total pages; an em-dash + a friendly step label (e.g. "reading notes...") follow.
        m_stageText = muse::qtrc("reprise", "Analyzing page %1 of %2").arg(sheet).arg(total)
                      + " " + QChar(0x2014) + " " + stageHuman;
    }
    else
    {
        m_stageText = stageHuman;
    }

    //! Mirror into the app-lifetime tracker so reprise.get-import-progress can report it after this popup is gone.
    importProgressTracker()->reportProgress(percent, m_currentPage > 0 ? m_currentPage : 0,
                                            m_totalPages > 0 ? m_totalPages : 0, stageHuman.toStdString());

    emit progressChanged();
}

QString ImportProgressModel::progressCaption() const
{
    if (m_status == "error")
    {
        return QString();
    }

    const int total = m_totalPages;
    int current = m_currentPage > 0 ? m_currentPage : (total > 0 ? 1 : -1);

    //! Defensive: never display current > total (the runner already clamps, but guard the caption too).
    if (total > 0 && current > total)
    {
        current = total;
    }

    if (total > 0 && current > 0)
    {
        return muse::qtrc("reprise", "Page %1 of %2").arg(current).arg(total);
    }

    return QString();
}

void ImportProgressModel::appendLog(const QString& line)
{
    if (!m_logText.isEmpty())
    {
        m_logText += '\n';
    }
    m_logText += line;

    //! Cap the retained log so a very long run cannot grow it without bound (keep the most recent output).
    static const int MAX_LOG_CHARS = 40000;
    if (m_logText.size() > MAX_LOG_CHARS)
    {
        m_logText = m_logText.right(MAX_LOG_CHARS);
    }

    emit logTextChanged();
}

QString ImportProgressModel::formatPagesLabel(const QString& pages) const
{
    const QString trimmed = pages.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("all", Qt::CaseInsensitive) == 0)
    {
        return muse::qtrc("reprise", "all pages");
    }

    //! Display hyphenated ranges with an en-dash ("1-3" -> "1<U+2013>3"); comma lists are left as-is.
    QString display = trimmed;
    display.replace('-', QChar(0x2013));

    return muse::qtrc("reprise", "pages %1").arg(display);
}

void ImportProgressModel::updateDoneLine()
{
    const int pages = m_runner ? m_runner->pageCount() : m_totalPages;

    int measures = 0;
    const notation::IMasterNotationPtr master = projectSession()->masterNotation();
    if (master && master->masterScore())
    {
        measures = static_cast<int>(master->masterScore()->nmeasures());
    }

    if (measures > 0 && pages > 0)
    {
        const QString measuresText = (measures == 1)
                                     ? muse::qtrc("reprise", "%1 measure").arg(measures)
                                     : muse::qtrc("reprise", "%1 measures").arg(measures);
        const QString pagesText = (pages == 1)
                                  ? muse::qtrc("reprise", "%1 page").arg(pages)
                                  : muse::qtrc("reprise", "%1 pages").arg(pages);

        //: Done line, e.g. "Done - recognized 21 measures across 3 pages." (%1 = measures, %2 = pages); the dash is an em-dash.
        m_stageText = muse::qtrc("reprise", "Done") + " " + QChar(0x2014) + " "
                      + muse::qtrc("reprise", "recognized %1 across %2").arg(measuresText).arg(pagesText);
    }
    else
    {
        m_stageText = muse::qtrc("reprise", "Done");
    }

    emit progressChanged();
}

void ImportProgressModel::onRunnerFinished(bool ok, const QString& outputPath, const QString& error, const QString& tail)
{
    if (!ok)
    {
        //! Surface the message plus a tail of Audiveris output so the user has something actionable.
        m_errorDetails = tail;
        m_stageText = error.isEmpty() ? muse::qtrc("reprise", "Audiveris could not recognise this scan.") : error;
        emit progressChanged();
        setStatus("error");
        importProgressTracker()->reportFinished(false, m_stageText.toStdString(), tail.toStdString());
        return;
    }

    //! Reprise degenerate-scan guard: a scan that recognised no usable parts yields MusicXML with zero <score-part>
    //! entries. Opening it lays out a score with zero visible parts, which the engraving instrument-name layout has
    //! no valid position to compute. Refuse GRACEFULLY here with a clear message (surfaced in the popup AND via
    //! reprise.get-import-errors) instead. A count of -1 means "could not inspect the file" -> fail open (proceed to
    //! open; the engraving layout guard still prevents a hard crash if such a score ever reaches it).
    const int partCount = recognizedPartCount(outputPath);
    if (partCount == 0)
    {
        m_errorDetails.clear();
        m_stageText = muse::qtrc("reprise", "No music could be recognised in this scan. Try a clearer or higher-resolution scan.");
        emit progressChanged();
        setStatus("error");
        importProgressTracker()->reportFinished(false, m_stageText.toStdString(), std::string());
        return;
    }

    //! Recognition succeeded: show a full determinate bar and open the score. The score is NOT "Done" until it actually
    //! opens, so keep the status "running" with an interim "Opening score..." line -- the popup then neither flashes the
    //! gold check nor auto-closes before the open resolves. The Done state (with the real measure + page counts filled
    //! by updateDoneLine) and the auto-close are finalized on the next master-notation change once the imported score
    //! becomes the current master; a declined open (score too corrupt/incomplete to open) is caught by the bounded
    //! fallback armed in openImported() and surfaced as a graceful error instead of a silent "Done".
    m_percent = 100;
    m_indeterminate = false;
    const int pages = m_runner ? m_runner->pageCount() : m_totalPages;
    if (pages > 0)
    {
        m_totalPages = pages;
        m_currentPage = pages;
    }
    m_stageText = muse::qtrc("reprise", "Opening score...");
    emit progressChanged();

    openImported(outputPath);
}

void ImportProgressModel::openImported(const QString& outputPath)
{
    //! Hand the produced MusicXML to MuseScore's native open path; a .mxl/.xml routes through MusicXmlReader.
    //! Arm the Grand-Piano override and the Done-line measure count for the score about to open, mark the open as
    //! awaited, then dispatch.
    m_pendingGrandPianoReset = true;
    m_pendingDoneCounts = true;
    m_awaitingOpen = true;

    const QUrl url = QUrl::fromLocalFile(outputPath);
    commandDispatcher()->dispatch(project::PROJECT_OPEN_COMMAND, { { "url", muse::Val(url.toString()) } });

    //! Open-decline fallback. command://project/open returns before the async open resolves, and its dispatch Ret is
    //! always ok, so a successful open is signalled ONLY by masterNotationChanged firing (which finalizes the Done
    //! state above) -- it never fires on a declined open (e.g. MusicXML too corrupt/incomplete for the reader to
    //! accept). If, after a generous window, that signal has not arrived AND no master notation is present, the score
    //! never opened: surface a graceful error instead of leaving the popup on the interim "Opening score..." forever.
    //! The master-notation re-check means a slow-but-valid open that lands first can never be mistaken for a failure.
    QTimer::singleShot(OPEN_RESOLVE_TIMEOUT_MS, this, [this]()
    {
        if (!m_awaitingOpen)
        {
            return;   //! the open already resolved to success (masterNotationChanged finalized it)
        }

        if (projectSession()->masterNotation())
        {
            return;   //! a score is present -> the open succeeded; let the success path finalize it
        }

        //! Definitively no score opened -- refuse GRACEFULLY, consistent with the zero-part guard (status = error +
        //! reprise.get-import-errors), never a silent "Done".
        m_awaitingOpen = false;
        m_pendingGrandPianoReset = false;
        m_pendingDoneCounts = false;
        m_errorDetails.clear();
        m_stageText = muse::qtrc("reprise", "Reprise could not produce a usable score from this scan - it may be too "
                                            "incomplete or damaged to open.");
        emit progressChanged();
        setStatus("error");
        importProgressTracker()->reportFinished(false, m_stageText.toStdString(), std::string());
    });
}

int ImportProgressModel::recognizedPartCount(const QString& outputPath) const
{
    QByteArray xml;

    const muse::io::path_t path(outputPath);
    const std::string suffix = muse::io::suffix(path);
    if (suffix == "mxl")
    {
        //! A .mxl is a ZIP: the real MusicXML is the rootfile named in META-INF/container.xml -- the same extraction
        //! MuseScore's own MusicXML importer uses (importmusicxml.cpp::extractRootfile).
        muse::ZipReader zip(path);
        if (!zip.exists() || zip.hasError())
        {
            return -1;
        }

        const muse::ByteArray container = zip.fileData("META-INF/container.xml");
        std::string rootfile;
        muse::XmlStreamReader e(container);
        while (e.readNextStartElement())
        {
            if (e.name() != "container")
            {
                break;
            }

            while (e.readNextStartElement())
            {
                if (e.name() != "rootfiles")
                {
                    break;
                }

                while (e.readNextStartElement())
                {
                    if (e.name() == "rootfile")
                    {
                        rootfile = e.attribute("full-path").toStdString();
                        break;
                    }
                }
            }
        }

        if (rootfile.empty())
        {
            return -1;
        }

        xml = zip.fileData(rootfile).toQByteArray();
    }
    else
    {
        QFile f(outputPath);
        if (!f.open(QIODevice::ReadOnly))
        {
            return -1;
        }

        xml = f.readAll();
        f.close();
    }

    if (xml.isEmpty())
    {
        return -1;
    }

    //! Count <score-part id=...> declarations (the <part-list> entries). "<score-partwise" has no space after
    //! "score-part", so the root element is never miscounted; every real <score-part> carries its id attribute first.
    return static_cast<int>(xml.count("<score-part id"));
}

void ImportProgressModel::setStatus(const QString& status)
{
    if (status == m_status)
    {
        return;
    }

    m_status = status;
    emit statusChanged();
}

void ImportProgressModel::applyGrandPianoDefault()
{
    //! TEMPORARY Reprise default. Audiveris assigns semi-random instruments to recognised parts; until an intake
    //! step adds real instrument selection, force every imported part to Grand Piano. Mirrors the original
    //! ImportPdfController implementation. Remove when real instrument choice lands.
    static const muse::String GRAND_PIANO_ID(u"grand-piano");

    const notation::InstrumentTemplate& templ = instrumentsRepository()->instrumentTemplate(GRAND_PIANO_ID);
    if (templ.id.isEmpty())
    {
        LOGW() << "Reprise: 'grand-piano' instrument template not found; leaving imported instruments as-is";
        return;
    }

    const notation::IMasterNotationPtr master = projectSession()->masterNotation();
    if (!master)
    {
        return;
    }

    const notation::INotationPartsPtr parts = master->parts();
    if (!parts)
    {
        return;
    }

    //! Snapshot the keys first: each replaceInstrument() mutates the score and notifies part changes.
    std::vector<notation::InstrumentKey> keys;
    for (const engraving::Part* part : parts->partList())
    {
        if (!part)
        {
            continue;
        }

        notation::InstrumentKey key;
        key.partId = part->id();
        key.instrumentId = part->instrumentId();
        key.tick = engraving::Part::MAIN_INSTRUMENT_TICK;
        keys.push_back(key);
    }

    const engraving::Instrument grandPiano = engraving::Instrument::fromTemplate(&templ);
    for (const notation::InstrumentKey& key : keys)
    {
        parts->replaceInstrument(key, grandPiano);
    }

    LOGI() << "Reprise: defaulted " << keys.size() << " imported part(s) to Grand Piano";
}
