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

//! Map a diatonic step 0..6 (C..B) to its letter, for the header pitch label (e.g. "C4").
const char* stepLetter(int32_t step)
{
    static const char* k[7] = { "C", "D", "E", "F", "G", "A", "B" };
    return (step >= 0 && step < 7) ? k[step] : "?";
}
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
    //! Destroy + recreate the receiver so the previous notation's selectionChanged subscription is dropped before we bind
    //! the new one (prevents a stale score's selection from driving this magnifier after a switch).
    m_selectionReceiver = std::make_unique<muse::async::Asyncable>();

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (notation && notation->interaction())
    {
        notation->interaction()->selectionChanged().onNotify(m_selectionReceiver.get(), [this]()
        {
            updateFollow();
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

    bool found = false;
    bool fallback = false;
    int32_t page = m_currentPageIndex;
    int32_t gm = -1;
    int32_t bx = 0;
    int32_t by = 0;
    int32_t bw = 0;
    int32_t bh = 0;
    QString noteName;

    const bool hasMap = proofingMapService() && proofingMapService()->hasMap();

    if (hasMap && note && note->chord() && note->chord()->measure())
    {
        //! Derive the Phase B1 runtime key straight from the live Note (NO translation). The service matches on
        //! (gm, staffIdx, tickInMeasure, step, octave); voice is advisory and excluded.
        const mu::engraving::Measure* measure = note->chord()->measure();
        gm = measure->measureIndex();
        const int32_t staffIdx = static_cast<int32_t>(note->staffIdx());
        const int32_t tickInMeasure = (note->tick() - measure->tick()).ticks();
        const int32_t step = mu::engraving::tpc2step(note->tpc());
        const int32_t octave = note->octave();

        noteName = QString("%1%2").arg(stepLetter(step)).arg(octave);

        const ProofingNoteBox* box = proofingMapService()->noteBox(gm, staffIdx, tickInMeasure, step, octave);
        if (box)
        {
            found = true;
            page = box->pageIndex;
            bx = box->x;
            by = box->y;
            bw = box->w;
            bh = box->h;
        }
        else
        {
            //! SELF-CHECK (Phase B1 worker flagged): a lookup miss where we expected a hit is the one thing to watch.
            //! key.staffIdx == note->staffIdx() by construction here, so log the key + the engraving tick so a miss is
            //! diagnosable against the stored boxes (reprise.get-proofing-map). Then fall back to the measure rect.
            LOGD() << "Reprise: magnifier note box MISS -> measure fallback: gm=" << gm << " staff=" << staffIdx
                   << " tickInMeasure=" << tickInMeasure << " step=" << step << " octave=" << octave;
        }
    }

    //! Fallback marker: the selected note had no head box (grace/untrusted), OR the selection is a plain measure/range
    //! with no single note. Show the enclosing MEASURE rect as a dashed, measure-level marker -- never a false note box.
    if (!found && hasMap)
    {
        int32_t fgm = -1;
        if (note && note->chord() && note->chord()->measure())
        {
            fgm = note->chord()->measure()->measureIndex();
        }
        else if (measureBase)
        {
            fgm = measureBase->measureIndex();
        }

        if (fgm >= 0)
        {
            const ProofingMap& map = proofingMapService()->map();
            for (const ProofingMeasureRect& r : map.measures)
            {
                if (r.globalMeasureIndex == fgm)
                {
                    found = true;
                    fallback = true;
                    gm = fgm;
                    page = r.pageIndex;
                    bx = r.x;
                    by = r.y;
                    bw = r.w;
                    bh = r.h;
                    break;
                }
            }
        }
    }

    const int32_t newPage = found ? page
                          : (m_currentPageIndex >= 0 ? m_currentPageIndex
                                                     : (m_pages.isEmpty() ? -1 : 0));

    const int32_t newMeasure = found ? gm : -1;

    const bool unchanged = (found == m_hasCurrent) && (fallback == m_isFallback) && (newPage == m_currentPageIndex)
                           && (newMeasure == m_currentMeasure) && (noteName == m_currentNoteName)
                           && (!found || (bx == m_boxX && by == m_boxY && bw == m_boxW && bh == m_boxH));
    if (unchanged)
    {
        return;
    }

    m_hasCurrent = found;
    m_isFallback = fallback;
    m_currentPageIndex = newPage;
    m_currentMeasure = newMeasure;
    m_currentNoteName = fallback ? QString() : noteName;
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
