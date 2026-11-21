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

#include "proofingmagnifiermodel.h"

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtPdf/QPdfDocument>
#include <QtPdf/QPdfDocumentRenderOptions>

#include "notation/inotation.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationselection.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"

#include "log.h"

using namespace mu::reprise;

namespace {
//! Longest-side pixel budget for a rendered magnifier page. The magnifier shows a zoomed CROP of a single spot, so it
//! renders higher than the A3 following band (BAND_PAGE_MAX_SIDE = 2600): big enough that a magnified note head stays
//! crisp (~345 dpi for US Letter), bounded so a whole book still renders quickly and one decoded page stays modest in
//! memory. The scan itself is the detail ceiling (a 300-450 dpi scan), so beyond this the magnifier simply upscales the
//! native scan pixels -- exactly as PhotoScore does; you cannot invent detail the scan never captured.
constexpr int32_t MAGNIFIER_PAGE_MAX_SIDE = 4000;
}

ProofingMagnifierModel::ProofingMagnifierModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: nothing that resolves the IoC context may run here. iocCtxForQmlObject(this) is lazy (it reads
    //! qmlContext(this), attached only AFTER this ctor returns); touching projectSession() / iocContext() now trips the
    //! "QQmlContext is not set" assert and crashes on first use. All context-dependent setup is deferred to load().
}

ProofingMagnifierModel::~ProofingMagnifierModel()
{
    clearRendered();
}

void ProofingMagnifierModel::load()
{
    if (!m_inited)
    {
        m_inited = true;

        //! Debounce hover-follow: apply only the latest hovered note/measure once the mouse settles (~100ms), so a fast
        //! sweep doesn't thrash the magnifier's page switch + crop. Single-shot; owned by this (QObject parent).
        m_hoverDebounce = new QTimer(this);
        m_hoverDebounce->setSingleShot(true);
        m_hoverDebounce->setInterval(100);
        connect(m_hoverDebounce, &QTimer::timeout, this, [this]()
        {
            applyHover();
        });

        //! Re-render whenever a fresh import replaces (or clears) the held map, independent of notation-change timing.
        proofingMapService()->mapChanged().onNotify(this, [this]()
        {
            rebuild();
        });

        //! Re-point the selection subscription whenever the user switches to a different score/part.
        projectSession()->currentNotationChanged().onNotify(this, [this]()
        {
            resubscribeSelection();
        });
    }

    rebuild();
    resubscribeSelection();

    return;
}

void ProofingMagnifierModel::rebuild()
{
    clearRendered();
    m_pages.clear();

    const auto svc = proofingMapService();
    if (svc && svc->hasMap())
    {
        const ProofingMap& map = svc->map();
        ++m_generation;

        const QString dir = renderDir();
        QDir().mkpath(dir);

        const QString source = QString::fromStdString(map.sourcePath);
        if (source.endsWith(".pdf", Qt::CaseInsensitive))
        {
            renderPdfPages(source, map, dir);
        }
        else
        {
            renderImagePages(source, map, dir);
        }
    }

    m_hasMap = !m_pages.isEmpty();

    m_currentPageIndex = m_pages.isEmpty() ? -1 : 0;
    m_hasCurrent = false;
    m_isFallback = false;
    m_currentMeasure = -1;
    m_currentNoteName.clear();

    emit changed();

    //! Recompute the magnified box for whatever is currently selected against the fresh map.
    updateFollow();

    return;
}

void ProofingMagnifierModel::renderPdfPages(const QString& sourcePath, const ProofingMap& map, const QString& dir)
{
    QPdfDocument doc;
    if (doc.load(sourcePath) != QPdfDocument::Error::None || doc.status() != QPdfDocument::Status::Ready)
    {
        LOGW() << "Reprise: proofing magnifier: cannot load source PDF " << sourcePath;
        return;
    }

    const int32_t docPages = doc.pageCount();
    if (docPages <= 0)
    {
        return;
    }

    //! Render exactly the recognised page count (map sheets), taken as the leading pages -- correct for an "all pages" /
    //! from-page-1 import; fall back to every page when the map has no page list (mirrors the following band).
    int32_t pageCount = map.pages.empty() ? docPages : static_cast<int32_t>(map.pages.size());
    pageCount = std::min(pageCount, docPages);

    QPdfDocumentRenderOptions options;

    for (int32_t i = 0; i < pageCount; ++i)
    {
        const QSizeF pts = doc.pagePointSize(i);
        if (pts.width() <= 0.0 || pts.height() <= 0.0)
        {
            continue;
        }

        //! Fit the page into a MAGNIFIER_PAGE_MAX_SIDE box, preserving aspect, and render straight to that pixel size.
        const double aspect = pts.width() / pts.height();
        int32_t h = MAGNIFIER_PAGE_MAX_SIDE;
        int32_t w = static_cast<int32_t>(std::lround(static_cast<double>(h) * aspect));
        if (w > MAGNIFIER_PAGE_MAX_SIDE)
        {
            w = MAGNIFIER_PAGE_MAX_SIDE;
            h = static_cast<int32_t>(std::lround(static_cast<double>(w) / aspect));
        }

        const QImage rendered = doc.render(i, QSize(std::max<int32_t>(1, w), std::max<int32_t>(1, h)), options);
        if (rendered.isNull())
        {
            LOGW() << "Reprise: proofing magnifier: PDF page " << i << " rendered empty";
            continue;
        }

        const int32_t sheetW = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageWidth : 0;
        const int32_t sheetH = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageHeight : 0;
        appendPage(rendered, dir, i, sheetW, sheetH);
    }

    return;
}

void ProofingMagnifierModel::renderImagePages(const QString& sourcePath, const ProofingMap& map, const QString& dir)
{
    QImageReader reader(sourcePath);

    //! imageCount() FIRST: it primes qtiff's directory count so jumpToNextImage() walks a multi-frame TIFF (see the
    //! multi-frame TIFF idiom in reference_musescore_build_gotchas). For a plain single-frame image it returns 1.
    int32_t frameCount = reader.imageCount();
    if (frameCount <= 0)
    {
        frameCount = 1;
    }

    int32_t pageCount = map.pages.empty() ? frameCount : static_cast<int32_t>(map.pages.size());
    pageCount = std::min(pageCount, frameCount);

    for (int32_t i = 0; i < pageCount; ++i)
    {
        if (i > 0 && !reader.jumpToNextImage())
        {
            break;
        }

        const QImage frame = reader.read();
        if (frame.isNull())
        {
            LOGW() << "Reprise: proofing magnifier: image frame " << i << " could not be read from " << sourcePath;
            break;
        }

        const int32_t sheetW = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageWidth : 0;
        const int32_t sheetH = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageHeight : 0;
        appendPage(frame, dir, i, sheetW, sheetH);
    }

    return;
}

void ProofingMagnifierModel::appendPage(const QImage& image, const QString& dir, int32_t index, int32_t sheetW, int32_t sheetH)
{
    QImage out = image;
    const int32_t longest = std::max(out.width(), out.height());
    if (longest > MAGNIFIER_PAGE_MAX_SIDE)
    {
        out = (out.width() >= out.height())
              ? out.scaledToWidth(MAGNIFIER_PAGE_MAX_SIDE, Qt::SmoothTransformation)
              : out.scaledToHeight(MAGNIFIER_PAGE_MAX_SIDE, Qt::SmoothTransformation);
    }

    const QString path = dir + QString("/sheet%1_%2.png").arg(index).arg(m_generation);
    if (!out.save(path, "PNG"))
    {
        LOGW() << "Reprise: proofing magnifier: failed to write page png " << path;
        return;
    }

    //! Fall back to the rendered dims when the map carries no sheet-image size for this page: the QML then scales the
    //! (absent) boxes against the render itself, which keeps the page displayable even if follow has nothing to line up.
    const int32_t sw = sheetW > 0 ? sheetW : out.width();
    const int32_t sh = sheetH > 0 ? sheetH : out.height();

    QVariantMap entry;
    entry["url"] = QUrl::fromLocalFile(path).toString();
    entry["width"] = out.width();
    entry["height"] = out.height();
    entry["sheetWidth"] = sw;
    entry["sheetHeight"] = sh;
    m_pages.append(entry);

    return;
}

void ProofingMagnifierModel::resubscribeSelection()
{
    //! Destroy + recreate the receiver so the previous notation's selectionChanged + hover subscriptions are dropped before
    //! we bind the new ones (prevents a stale score's selection/hover from driving this magnifier after a switch).
    m_selectionReceiver = std::make_unique<muse::async::Asyncable>();

    //! Any pending hover apply belonged to the previous notation -- drop it so a switch never follows a stale note/measure.
    m_pendingHoverNote = mu::notation::HoveredNoteInfo();
    m_pendingHoverMeasure = -1;
    if (m_hoverDebounce)
    {
        m_hoverDebounce->stop();
    }

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (notation && notation->interaction())
    {
        notation->interaction()->selectionChanged().onNotify(m_selectionReceiver.get(), [this]()
        {
            updateFollow();
        });

        //! Hover-follow ALONGSIDE selection: the ungated hover path on the input controller emits the note AND the measure
        //! under the mouse; we debounce + apply (note-level preferred, measure-level fallback). Click/selection unchanged.
        notation->interaction()->hoveredNoteChanged().onReceive(m_selectionReceiver.get(), [this](const mu::notation::HoveredNoteInfo& note)
        {
            scheduleHoverNote(note);
        });

        notation->interaction()->hoveredMeasureChanged().onReceive(m_selectionReceiver.get(), [this](int measure)
        {
            scheduleHoverMeasure(static_cast<int32_t>(measure));
        });
    }

    updateFollow();

    return;
}

void ProofingMagnifierModel::updateFollow()
{
    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;

    //! Resolve the selection to the note to magnify (preferred) and/or the enclosing measure (the fallback target).
    mu::engraving::Note* note = nullptr;
    mu::engraving::MeasureBase* measureBase = nullptr;

    if (notation && notation->interaction())
    {
        const mu::notation::INotationSelectionPtr sel = notation->interaction()->selection();
        if (sel && !sel->isNone())
        {
            mu::engraving::EngravingItem* el = sel->element();
            if (el && el->isNote())
            {
                note = mu::engraving::toNote(el);
            }
            else
            {
                //! A chord/stem/range hit: take the first selected note so the magnifier still follows a real head.
                const std::vector<mu::engraving::Note*> notes = sel->notes();
                if (!notes.empty())
                {
                    note = notes.front();
                }
            }

            //! The enclosing measure drives the fallback marker (grace/untrusted note, or a pure measure selection).
            measureBase = sel->startMeasureBase();
        }
    }

    const bool hasMap = proofingMapService() && proofingMapService()->hasMap();

    if (hasMap && note && note->chord() && note->chord()->measure())
    {
        //! Derive the Phase B1 runtime key straight from the live Note (NO translation). The service matches on
        //! (gm, staffIdx, tickInMeasure, step, octave); voice is advisory and excluded.
        const mu::engraving::Measure* measure = note->chord()->measure();
        const int32_t gm = measure->measureIndex();
        const int32_t staffIdx = static_cast<int32_t>(note->staffIdx());
        const int32_t tickInMeasure = (note->tick() - measure->tick()).ticks();
        const int32_t step = mu::engraving::tpc2step(note->tpc());
        const int32_t octave = note->octave();

        //! Header label shows the note's REAL pitch INCLUDING accidental (e.g. "E♭4"), taken straight from the live
        //! Note's tpc+pitch -- tpcUserName renders Unicode ♭/♯ with the sounding octave. The box MATCH stays diatonic
        //! (step+octave are what noteBox() keys on); only the displayed label carries the accidental, so a flat/sharp
        //! head reads correctly in the header instead of collapsing to its diatonic letter.
        const QString noteName = mu::engraving::tpcUserName(note->tpc(), note->pitch()).toQString();

        followNoteKey(gm, staffIdx, tickInMeasure, step, octave, noteName);

        return;
    }

    //! A measure/range selection with no single note (or a grace/untrusted note that has no chord-measure) -> dashed
    //! measure-level fallback. Nothing selected / no map -> clear hasCurrent (keeps the current page).
    if (hasMap && measureBase)
    {
        followMeasureOnly(measureBase->measureIndex());

        return;
    }

    applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

    return;
}

void ProofingMagnifierModel::applyFollow(bool found, bool fallback, int32_t page, int32_t gm,
                                         int32_t bx, int32_t by, int32_t bw, int32_t bh, const QString& noteName)
{
    const int32_t newPage = found ? page
                          : (m_currentPageIndex >= 0 ? m_currentPageIndex
                                                     : (m_pages.isEmpty() ? -1 : 0));

    const int32_t newMeasure = found ? gm : -1;
    const QString newName = fallback ? QString() : noteName;

    const bool unchanged = (found == m_hasCurrent) && (fallback == m_isFallback) && (newPage == m_currentPageIndex)
                           && (newMeasure == m_currentMeasure) && (newName == m_currentNoteName)
                           && (!found || (bx == m_boxX && by == m_boxY && bw == m_boxW && bh == m_boxH));
    if (unchanged)
    {
        return;
    }

    m_hasCurrent = found;
    m_isFallback = fallback;
    m_currentPageIndex = newPage;
    m_currentMeasure = newMeasure;
    m_currentNoteName = newName;
    if (found)
    {
        m_boxX = bx;
        m_boxY = by;
        m_boxW = bw;
        m_boxH = bh;
    }

    emit followChanged();

    return;
}

void ProofingMagnifierModel::followNoteKey(int32_t gm, int32_t staffIdx, int32_t tickInMeasure, int32_t step, int32_t octave,
                                           const QString& noteName)
{
    if (!proofingMapService() || !proofingMapService()->hasMap())
    {
        applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

        return;
    }

    const ProofingNoteBox* box = proofingMapService()->noteBox(gm, staffIdx, tickInMeasure, step, octave);
    if (box)
    {
        applyFollow(true, false, box->pageIndex, gm, box->x, box->y, box->w, box->h, noteName);

        return;
    }

    //! SELF-CHECK (Phase B1 worker flagged): a lookup miss where we expected a hit is the one thing to watch. Log the key +
    //! the engraving tick so a miss is diagnosable against the stored boxes (reprise.get-proofing-map), then fall back to
    //! the enclosing measure rect -- never a false note box.
    LOGD() << "Reprise: magnifier note box MISS -> measure fallback: gm=" << gm << " staff=" << staffIdx
           << " tickInMeasure=" << tickInMeasure << " step=" << step << " octave=" << octave;

    followMeasureOnly(gm);

    return;
}

void ProofingMagnifierModel::followMeasureOnly(int32_t gm)
{
    if (gm < 0 || !proofingMapService() || !proofingMapService()->hasMap())
    {
        applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

        return;
    }

    const ProofingMap& map = proofingMapService()->map();
    for (const ProofingMeasureRect& r : map.measures)
    {
        if (r.globalMeasureIndex == gm)
        {
            applyFollow(true, true, r.pageIndex, gm, r.x, r.y, r.w, r.h, QString());

            return;
        }
    }

    //! Map held but no rect for this measure index -> clear hasCurrent (keeps the current page).
    applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

    return;
}

void ProofingMagnifierModel::scheduleHoverNote(const mu::notation::HoveredNoteInfo& note)
{
    //! Ignore all hover while no map is held (nothing to magnify). Otherwise latch the latest hovered note and (re)arm the
    //! debounce; applyHover() decides note-vs-measure once the mouse settles.
    if (!m_hasMap)
    {
        return;
    }

    m_pendingHoverNote = note;
    if (m_hoverDebounce)
    {
        m_hoverDebounce->start();
    }

    return;
}

void ProofingMagnifierModel::scheduleHoverMeasure(int32_t measure)
{
    //! Latch the latest hovered measure (INCLUDING -1 on leave, so applyHover() can tell a leave from a rest-hover) and
    //! (re)arm the debounce. Ignored while no map is held.
    if (!m_hasMap)
    {
        return;
    }

    m_pendingHoverMeasure = measure;
    if (m_hoverDebounce)
    {
        m_hoverDebounce->start();
    }

    return;
}

void ProofingMagnifierModel::applyHover()
{
    //! Note-level wins when the cursor is over a head; else the hovered measure drives the dashed measure-level fallback
    //! (rest / between notes / empty). A leave (no note AND measure < 0) holds the last view rather than blanking it --
    //! matching the following band, so moving the mouse off the score to read the magnifier itself doesn't clear it.
    if (m_pendingHoverNote.isValid())
    {
        const QString noteName = mu::engraving::tpcUserName(m_pendingHoverNote.tpc, m_pendingHoverNote.pitch).toQString();
        followNoteKey(m_pendingHoverNote.globalMeasureIndex, m_pendingHoverNote.staffIdx, m_pendingHoverNote.tickInMeasure,
                      m_pendingHoverNote.step, m_pendingHoverNote.octave, noteName);

        return;
    }

    if (m_pendingHoverMeasure >= 0)
    {
        followMeasureOnly(m_pendingHoverMeasure);

        return;
    }

    return;
}

void ProofingMagnifierModel::clearRendered()
{
    QDir dir(renderDir());
    if (!dir.exists())
    {
        return;
    }

    const QStringList files = dir.entryList(QStringList() << "*.png", QDir::Files);
    for (const QString& file : files)
    {
        dir.remove(file);
    }

    return;
}

QString ProofingMagnifierModel::renderDir() const
{
    //! Per-instance subdir so the magnifier never tramples the following band's (or another view's) rendered pages, and
    //! the dtor's sweep only removes this magnifier's own files.
    return QDir::tempPath() + "/reprise-proofing-magnifier/" + QString::number(reinterpret_cast<quintptr>(this));
}
