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

#include "proofingstripmodel.h"

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

#include "engraving/dom/measurebase.h"

#include "log.h"

using namespace mu::reprise;

namespace {
//! Longest-side pixel budget for a rendered band page. Phase A3 shows the page at a READABLE size (a whole system
//! legible), so the render is higher-resolution than the A2 thumbnail filmstrip was: big enough to stay crisp when the
//! page is displayed near full band width on a wide monitor (~236 dpi for US Letter), small enough that a whole book
//! renders quickly and the temp PNGs stay modest. The magnifier (Phase B) will render at full resolution on demand.
constexpr int32_t BAND_PAGE_MAX_SIDE = 2600;
}

ProofingStripModel::ProofingStripModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: nothing that resolves the IoC context may run here. iocCtxForQmlObject(this) is lazy (it reads
    //! qmlContext(this), attached only AFTER this ctor returns); touching projectSession() / iocContext() now trips the
    //! "QQmlContext is not set" assert and crashes on first use. All context-dependent setup is deferred to load().
}

ProofingStripModel::~ProofingStripModel()
{
    clearRendered();
}

void ProofingStripModel::load()
{
    if (!m_inited)
    {
        m_inited = true;

        //! Debounce hover-follow: apply only the latest hovered measure once the mouse settles (~100ms), so a fast sweep
        //! across measures doesn't thrash the band's page switch + scroll. Single-shot; owned by this (QObject parent).
        m_hoverDebounce = new QTimer(this);
        m_hoverDebounce->setSingleShot(true);
        m_hoverDebounce->setInterval(100);
        connect(m_hoverDebounce, &QTimer::timeout, this, [this]()
        {
            if (m_pendingHoverMeasure >= 0)
            {
                followMeasure(m_pendingHoverMeasure);
            }
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

void ProofingStripModel::rebuild()
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

    //! Only advertise the band once at least one page actually rendered -- an unreadable source should not leave an empty
    //! band hanging above the score.
    m_hasMap = !m_pages.isEmpty();

    //! Default the band to the first page until a selection drives the follow; -1 when there is nothing to show.
    m_currentPageIndex = m_pages.isEmpty() ? -1 : 0;
    m_hasCurrent = false;
    m_currentMeasure = -1;

    emit changed();

    //! Recompute the highlight for whatever is currently selected against the fresh map.
    updateFollow();

    return;
}

void ProofingStripModel::renderPdfPages(const QString& sourcePath, const ProofingMap& map, const QString& dir)
{
    QPdfDocument doc;
    if (doc.load(sourcePath) != QPdfDocument::Error::None || doc.status() != QPdfDocument::Status::Ready)
    {
        LOGW() << "Reprise: proofing band: cannot load source PDF " << sourcePath;
        return;
    }

    const int32_t docPages = doc.pageCount();
    if (docPages <= 0)
    {
        return;
    }

    //! Render exactly the recognised page count (map sheets), taken as the leading pages of the document -- correct for
    //! an "all pages" / from-page-1 import. A non-leading page subrange is a known Phase A simplification (the map does
    //! not record which original page each sheet came from); fall back to every page when the map has no page list.
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

        //! Fit the page into a BAND_PAGE_MAX_SIDE box, preserving aspect, and render straight to that pixel size.
        const double aspect = pts.width() / pts.height();
        int32_t h = BAND_PAGE_MAX_SIDE;
        int32_t w = static_cast<int32_t>(std::lround(static_cast<double>(h) * aspect));
        if (w > BAND_PAGE_MAX_SIDE)
        {
            w = BAND_PAGE_MAX_SIDE;
            h = static_cast<int32_t>(std::lround(static_cast<double>(w) / aspect));
        }

        const QImage rendered = doc.render(i, QSize(std::max<int32_t>(1, w), std::max<int32_t>(1, h)), options);
        if (rendered.isNull())
        {
            LOGW() << "Reprise: proofing band: PDF page " << i << " rendered empty";
            continue;
        }

        const int32_t sheetW = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageWidth : 0;
        const int32_t sheetH = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageHeight : 0;
        appendPage(rendered, dir, i, sheetW, sheetH);
    }

    return;
}

void ProofingStripModel::renderImagePages(const QString& sourcePath, const ProofingMap& map, const QString& dir)
{
    QImageReader reader(sourcePath);

    //! imageCount() FIRST: it primes qtiff's directory count so jumpToNextImage() walks a multi-frame TIFF (see the
    //! multi-frame TIFF idiom in reference_musescore_build_gotchas). For a plain single-frame image it returns 1.
    int32_t frameCount = reader.imageCount();
    if (frameCount <= 0)
    {
        frameCount = 1;
    }

    //! Mirror the PDF branch: show the recognised sheet count, clamped to what the file actually holds.
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
            LOGW() << "Reprise: proofing band: image frame " << i << " could not be read from " << sourcePath;
            break;
        }

        const int32_t sheetW = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageWidth : 0;
        const int32_t sheetH = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageHeight : 0;
        appendPage(frame, dir, i, sheetW, sheetH);
    }

    return;
}

void ProofingStripModel::appendPage(const QImage& image, const QString& dir, int32_t index, int32_t sheetW, int32_t sheetH)
{
    QImage out = image;
    const int32_t longest = std::max(out.width(), out.height());
    if (longest > BAND_PAGE_MAX_SIDE)
    {
        out = (out.width() >= out.height())
              ? out.scaledToWidth(BAND_PAGE_MAX_SIDE, Qt::SmoothTransformation)
              : out.scaledToHeight(BAND_PAGE_MAX_SIDE, Qt::SmoothTransformation);
    }

    const QString path = dir + QString("/sheet%1_%2.png").arg(index).arg(m_generation);
    if (!out.save(path, "PNG"))
    {
        LOGW() << "Reprise: proofing band: failed to write page png " << path;
        return;
    }

    //! Fall back to the rendered dims when the map carries no sheet-image size for this page: the band then scales the
    //! (absent) rects against the render itself, which keeps the page displayable even if follow has nothing to line up.
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

void ProofingStripModel::resubscribeSelection()
{
    //! Destroy + recreate the receiver so the previous notation's selectionChanged subscription is dropped before we bind
    //! the new one (prevents a stale score's selection from driving this band after a switch).
    m_selectionReceiver = std::make_unique<muse::async::Asyncable>();

    //! Any pending hover apply belonged to the previous notation -- drop it so a switch never follows a stale measure.
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

        //! Hover-follow ALONGSIDE selection: the ungated hover path on the input controller emits the measure under the
        //! mouse; we debounce + apply it to the same band follow. Click/selection follow above is unchanged.
        notation->interaction()->hoveredMeasureChanged().onReceive(m_selectionReceiver.get(), [this](int measure)
        {
            scheduleHoverFollow(static_cast<int32_t>(measure));
        });
    }

    updateFollow();

    return;
}

void ProofingStripModel::updateFollow()
{
    int32_t measure = -1;

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (notation && notation->interaction())
    {
        const mu::notation::INotationSelectionPtr sel = notation->interaction()->selection();
        if (sel && !sel->isNone())
        {
            //! startMeasureBase() resolves a single-note selection to its enclosing measure, a range to its first
            //! measure, and a frame to itself. measureIndex() is POSITIONAL (counts only Measures from the first,
            //! pickup = 0), which is exactly how the map's globalMeasureIndex is assigned -- so the two line up directly.
            mu::engraving::MeasureBase* mb = sel->startMeasureBase();
            if (mb)
            {
                measure = mb->measureIndex();
            }
        }
    }

    //! Selection drives the follow directly (measure < 0 when nothing is selected -> clears hasCurrent).
    followMeasure(measure);

    return;
}

void ProofingStripModel::scheduleHoverFollow(int32_t measure)
{
    //! Ignore a hover that resolves to no measure (empty canvas / non-measure, or the mouse left the view) and anything
    //! while no map is held -- the band keeps showing the last followed measure rather than clearing on every stray move.
    if (measure < 0 || !m_hasMap)
    {
        return;
    }

    //! Latch the latest hovered measure and (re)arm the debounce: a fast sweep coalesces to one apply once the mouse
    //! settles. Already showing this measure -> nothing to do (don't re-arm the timer for a no-op).
    if (measure == m_currentMeasure && m_hasCurrent)
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

void ProofingStripModel::followMeasure(int32_t measure)
{
    bool found = false;
    int32_t page = m_currentPageIndex;
    int32_t rx = 0;
    int32_t ry = 0;
    int32_t rw = 0;
    int32_t rh = 0;

    if (measure >= 0 && proofingMapService() && proofingMapService()->hasMap())
    {
        const ProofingMap& map = proofingMapService()->map();
        for (const ProofingMeasureRect& m : map.measures)
        {
            if (m.globalMeasureIndex == measure)
            {
                found = true;
                page = m.pageIndex;
                rx = m.x;
                ry = m.y;
                rw = m.w;
                rh = m.h;
                break;
            }
        }
    }

    const int32_t newPage = found ? page
                          : (m_currentPageIndex >= 0 ? m_currentPageIndex
                                                     : (m_pages.isEmpty() ? -1 : 0));

    const bool unchanged = (measure == m_currentMeasure) && (found == m_hasCurrent) && (newPage == m_currentPageIndex)
                           && (!found || (rx == m_rectX && ry == m_rectY && rw == m_rectW && rh == m_rectH));
    if (unchanged)
    {
        return;
    }

    m_currentMeasure = measure;
    m_hasCurrent = found;
    m_currentPageIndex = newPage;
    if (found)
    {
        m_rectX = rx;
        m_rectY = ry;
        m_rectW = rw;
        m_rectH = rh;
    }

    emit followChanged();

    return;
}

void ProofingStripModel::clearRendered()
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

QString ProofingStripModel::renderDir() const
{
    //! Per-instance subdir so two notation views never trample one another's rendered pages, and the dtor's sweep only
    //! removes this band's own files.
    return QDir::tempPath() + "/reprise-proofing/" + QString::number(reinterpret_cast<quintptr>(this));
}
