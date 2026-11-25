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
#include "notation/inotationelements.h"
#include "notation/inotationundostack.h"
#include "notation/notationtypes.h"

#include "engraving/dom/chordrest.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/score.h"

#include "internal/badtimingcheck.h"

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
    //! Drop the live-model registration so the dev reprise.proofing-debug-set-zoom handler never drives a destroyed band
    //! (the control also holds us as a QPointer, so this is belt-and-braces). Guarded in case the IoC is already gone.
    if (proofingViewControl())
    {
        proofingViewControl()->unregisterBand(this);
    }

    clearRendered();
}

void ProofingStripModel::load()
{
    if (!m_inited)
    {
        m_inited = true;

        //! Register this live band with the proofing view-control bridge so the dev reprise.proofing-debug-set-zoom tool
        //! can drive our userZoom headlessly. Unregistered in the dtor (the control also holds us as a QPointer).
        if (proofingViewControl())
        {
            proofingViewControl()->registerBand(this);
        }

        //! 0.18.2: seed this band's zoom from the per-document view-state service (a .repr opened before this panel finished
        //! loading may already hold the restored band zoom) and re-apply it whenever the service's state is replaced (.repr
        //! open) or reset (document switch). applyRestoredZoom sets userZoom WITHOUT pushing back or marking the project unsaved
        //! (a restore is not a user edit). The followed measure is restored separately, in maybeSelectFirstOnOpen (it needs a
        //! live notation + selection, not just a zoom scalar). Subscribed once, app-lifetime. See IProofingViewStateService.
        if (proofingViewStateService())
        {
            applyRestoredZoom(proofingViewStateService()->viewState().bandZoom);
            proofingViewStateService()->viewStateChanged().onNotify(this, [this]()
            {
                if (proofingViewStateService())
                {
                    applyRestoredZoom(proofingViewStateService()->viewState().bandZoom);
                }
            });
        }

        //! Hover-follow throttle window (~50ms). scheduleHoverFollow() applies the FIRST move of a burst immediately (leading
        //! edge) and opens this window; moves arriving during the window are latched and applied here when it elapses
        //! (trailing edge). A continuous sweep thus updates at a steady ~20 Hz instead of only after the mouse fully stops --
        //! which was the old "sometimes slow, sometimes snappy" behaviour of a restart-every-move debounce. The per-apply cost
        //! is just a scroll-offset + highlight move (the page PNGs are rendered once at import, not per hover), so ~20 Hz is
        //! cheap. Single-shot; owned by this (QObject parent).
        m_hoverDebounce = new QTimer(this);
        m_hoverDebounce->setSingleShot(true);
        m_hoverDebounce->setInterval(50);
        connect(m_hoverDebounce, &QTimer::timeout, this, [this]()
        {
            //! Trailing edge: apply the latest measure latched during the window (if any). Do NOT reopen the window here --
            //! the next move re-arms the leading edge, which keeps the steady cadence without busy-spinning when idle.
            if (m_pendingHoverMeasure >= 0)
            {
                const int32_t measure = m_pendingHoverMeasure;
                m_pendingHoverMeasure = -1;
                followMeasure(measure);
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
    m_inRebuild = true;

    clearRendered();
    m_pages.clear();
    m_pdfDoc.reset();
    m_sourcePath.clear();
    m_renderDir.clear();
    m_sourceIsPdf = false;
    m_pageCount = 0;

    const auto svc = proofingMapService();
    if (svc && svc->hasMap())
    {
        const ProofingMap& map = svc->map();
        ++m_generation;

        m_renderDir = renderDir();
        QDir().mkpath(m_renderDir);

        m_sourcePath = QString::fromStdString(map.sourcePath);
        m_sourceIsPdf = m_sourcePath.endsWith(".pdf", Qt::CaseInsensitive);

        //! Open the source + size placeholders only; the page pixels render lazily (ensurePageRendered) one page at a time.
        prepareLazyPages(map);
    }

    //! Render ONLY the first page now so the band shows immediately; the rest render on demand as the follow reaches them.
    //! Gate hasMap on that first page actually rendering -- an unreadable source should not leave an empty band hanging
    //! above the score (preserves the pre-lazy "advertise once at least one page rendered" contract).
    const bool firstOk = (m_pageCount > 0) && ensurePageRendered(0);
    if (!firstOk)
    {
        m_pages.clear();
        m_pageCount = 0;
        m_pdfDoc.reset();
    }

    m_hasMap = m_pageCount > 0;

    //! Default the band to the first page until a selection drives the follow; -1 when there is nothing to show.
    m_currentPageIndex = (m_pageCount > 0) ? 0 : -1;
    m_hasCurrent = false;
    m_currentMeasure = -1;

    m_inRebuild = false;

    emit changed();

    //! Recompute the highlight for whatever is currently selected against the fresh map.
    updateFollow();

    //! FIRST-MEASURE-ON-OPEN: if the map arrived while a notation is already current (e.g. a .repr reopen, or the band
    //! re-instantiated over an open score), seed the selection now so the panels populate without a hover. No-op when the
    //! notation is not yet open -- resubscribeSelection() retries the instant it becomes current.
    maybeSelectFirstOnOpen();

    return;
}

void ProofingStripModel::prepareLazyPages(const ProofingMap& map)
{
    int32_t pageCount = 0;

    if (m_sourceIsPdf)
    {
        m_pdfDoc = std::make_unique<QPdfDocument>();
        if (m_pdfDoc->load(m_sourcePath) != QPdfDocument::Error::None || m_pdfDoc->status() != QPdfDocument::Status::Ready)
        {
            LOGW() << "Reprise: proofing band: cannot load source PDF " << m_sourcePath;
            m_pdfDoc.reset();

            return;
        }

        const int32_t docPages = m_pdfDoc->pageCount();
        if (docPages <= 0)
        {
            m_pdfDoc.reset();

            return;
        }

        //! Recognised page count (map sheets), taken as the leading pages -- correct for an "all pages" / from-page-1
        //! import; fall back to every page when the map has no page list.
        pageCount = map.pages.empty() ? docPages : static_cast<int32_t>(map.pages.size());
        pageCount = std::min(pageCount, docPages);
    }
    else
    {
        QImageReader reader(m_sourcePath);

        //! imageCount() FIRST: it primes qtiff's directory count so jumpToNextImage() walks a multi-frame TIFF (see the
        //! multi-frame TIFF idiom in reference_musescore_build_gotchas). For a plain single-frame image it returns 1.
        int32_t frameCount = reader.imageCount();
        if (frameCount <= 0)
        {
            frameCount = 1;
        }

        pageCount = map.pages.empty() ? frameCount : static_cast<int32_t>(map.pages.size());
        pageCount = std::min(pageCount, frameCount);
    }

    //! One placeholder per page: the Audiveris sheet-image dims are known from the map now; the rendered PNG url + pixel
    //! size are filled lazily by ensurePageRendered the first time the band actually displays that page.
    for (int32_t i = 0; i < pageCount; ++i)
    {
        const int32_t sheetW = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageWidth : 0;
        const int32_t sheetH = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageHeight : 0;

        QVariantMap entry;
        entry["url"] = QString();
        entry["width"] = 0;
        entry["height"] = 0;
        entry["sheetWidth"] = sheetW;
        entry["sheetHeight"] = sheetH;
        m_pages.append(entry);
    }

    m_pageCount = pageCount;

    return;
}

bool ProofingStripModel::ensurePageRendered(int32_t index)
{
    if (index < 0 || index >= m_pageCount || index >= static_cast<int32_t>(m_pages.size()))
    {
        return false;
    }

    QVariantMap entry = m_pages.at(index).toMap();
    if (!entry.value("url").toString().isEmpty())
    {
        return true;   //! already rendered (cached)
    }

    const QImage img = renderPageImage(index);
    if (img.isNull())
    {
        return false;
    }

    const QVariantMap filled = savePageEntry(img, index, entry.value("sheetWidth").toInt(), entry.value("sheetHeight").toInt());
    if (filled.isEmpty())
    {
        return false;
    }

    m_pages[index] = filled;

    //! Filling a placeholder changes the pages list; a bound QML Image must re-read. During rebuild the single emit at the
    //! end covers it (avoids a mid-rebuild emit against half-set state); on the on-demand (follow) path, emit now.
    if (!m_inRebuild)
    {
        emit changed();
    }

    return true;
}

QImage ProofingStripModel::renderPageImage(int32_t index) const
{
    if (index < 0 || index >= m_pageCount)
    {
        return QImage();
    }

    if (m_sourceIsPdf)
    {
        if (!m_pdfDoc)
        {
            return QImage();
        }

        const QSizeF pts = m_pdfDoc->pagePointSize(index);
        if (pts.width() <= 0.0 || pts.height() <= 0.0)
        {
            return QImage();
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

        QPdfDocumentRenderOptions options;
        const QImage rendered = m_pdfDoc->render(index, QSize(std::max<int32_t>(1, w), std::max<int32_t>(1, h)), options);
        if (rendered.isNull())
        {
            LOGW() << "Reprise: proofing band: PDF page " << index << " rendered empty";
        }

        return rendered;
    }

    //! Image source: re-open the reader and walk to frame `index` (single-frame files return frame 0). imageCount() first
    //! primes qtiff's directory count so jumpToNextImage() walks a multi-frame TIFF (the multi-frame TIFF idiom).
    QImageReader reader(m_sourcePath);
    (void)reader.imageCount();
    for (int32_t i = 0; i < index; ++i)
    {
        if (!reader.jumpToNextImage())
        {
            return QImage();
        }
    }

    const QImage frame = reader.read();
    if (frame.isNull())
    {
        LOGW() << "Reprise: proofing band: image frame " << index << " could not be read from " << m_sourcePath;
    }

    return frame;
}

QVariantMap ProofingStripModel::savePageEntry(const QImage& image, int32_t index, int32_t sheetW, int32_t sheetH) const
{
    QImage out = image;
    const int32_t longest = std::max(out.width(), out.height());
    if (longest > BAND_PAGE_MAX_SIDE)
    {
        out = (out.width() >= out.height())
              ? out.scaledToWidth(BAND_PAGE_MAX_SIDE, Qt::SmoothTransformation)
              : out.scaledToHeight(BAND_PAGE_MAX_SIDE, Qt::SmoothTransformation);
    }

    const QString path = m_renderDir + QString("/sheet%1_%2.png").arg(index).arg(m_generation);
    if (!out.save(path, "PNG"))
    {
        LOGW() << "Reprise: proofing band: failed to write page png " << path;

        return QVariantMap();
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

    return entry;
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

        //! Keep the problem-measure set (and thus the red highlight) live: any edit / undo / redo re-walks the score, so a
        //! measure the user has just fixed stops being drawn red the instant its timing resolves (and a newly-broken one
        //! turns red). The walk is the cheap read-only scanScoreForBadTiming the navigator already uses.
        if (notation->undoStack())
        {
            notation->undoStack()->stackChanged().onNotify(m_selectionReceiver.get(), [this]()
            {
                rebuildProblemSet();
            });
        }
    }

    //! Build the problem set for the now-current notation BEFORE the first follow so the highlight colour is right on the
    //! opening selection (maybeSelectFirstOnOpen drives updateFollow below).
    rebuildProblemSet();

    updateFollow();

    //! FIRST-MEASURE-ON-OPEN: this fires on currentNotationChanged (a fresh import or a .repr reopen making its score
    //! current) -- the reliable "notation is now open" signal. Seed the first-note selection so both panels populate with no
    //! hover. Guarded to once per map generation + skips when a selection already exists.
    maybeSelectFirstOnOpen();

    return;
}

void ProofingStripModel::maybeSelectFirstOnOpen()
{
    //! Run only once per held map, and only while a map IS held (nothing to proof otherwise).
    if (!m_hasMap || m_autoSelectGeneration == m_generation)
    {
        return;
    }

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (!notation || !notation->interaction())
    {
        //! Notation not open yet (e.g. map set just before a fresh import's score opens) -- resubscribeSelection() retries
        //! this the instant currentNotationChanged fires for the opened score.
        return;
    }

    //! Mark done for THIS map before touching the selection: select() emits selectionChanged -> updateFollow() re-enters, and
    //! the guard above must already be set so we never recurse into a second auto-select.
    m_autoSelectGeneration = m_generation;

    const mu::notation::INotationSelectionPtr sel = notation->interaction()->selection();
    if (sel && !sel->isNone())
    {
        //! The user (or a restored .repr) already has a selection -- leave it; the follow is already populated from it.
        return;
    }

    //! 0.18.2: restore the FOLLOWED MEASURE saved in the .repr view state (set by doLoadRepr) instead of always landing on
    //! measure 1. A valid saved index (>=0) selects that measure's first chord/rest (SINGLE -> band + magnifier follow it, same
    //! as a user click); an out-of-range index or an empty measure falls through to the first-note default below. The resulting
    //! selectionChanged drives updateFollow(), which re-records the (unchanged) followed measure -- harmless.
    const int32_t savedMeasure = proofingViewStateService() ? proofingViewStateService()->viewState().followedMeasure : -1;
    if (savedMeasure >= 0)
    {
        mu::engraving::Score* score = notation->elements() ? notation->elements()->msScore() : nullptr;
        mu::engraving::Measure* measure = score ? score->firstMeasure() : nullptr;
        for (int32_t i = 0; i < savedMeasure && measure; ++i)
        {
            measure = measure->nextMeasure();
        }

        if (measure)
        {
            mu::engraving::ChordRest* cr = measure->firstChordRest(0);
            if (cr)
            {
                notation->interaction()->select({ cr }, mu::notation::SelectType::SINGLE);
            }
            else
            {
                notation->interaction()->select({ measure }, mu::notation::SelectType::SINGLE);
            }

            return;
        }
    }

    //! Select the first navigable element (the first note/rest of measure 1 -- Navigation::firstElement, the same target as
    //! Ctrl+Home / "go to beginning"). Does NOT require note-input mode. The resulting selectionChanged drives updateFollow()
    //! here and the magnifier's follow, so both panels populate on the first note of measure 1.
    notation->interaction()->select(mu::notation::SelectionTarget::FirstItem);

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

    //! 0.18.2: remember the followed (selected) measure in the per-document view-state service so a Save captures it and a
    //! reopen lands here instead of measure 1. syncFollowedMeasure ignores a negative (no selection), so a transient null
    //! selection cannot clobber the restored target; it does NOT mark the project unsaved (non-dirtying, like the zoom). Only
    //! the SELECTION path (this updateFollow) records it -- hover-follow goes through followMeasure() and must not change it.
    if (proofingViewStateService())
    {
        proofingViewStateService()->syncFollowedMeasure(measure);
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

    //! Already showing this measure -> nothing to do (and clear any stale pending so the trailing edge doesn't re-apply it).
    if (measure == m_currentMeasure && m_hasCurrent)
    {
        m_pendingHoverMeasure = -1;
        return;
    }

    //! LEADING EDGE: the first move of a burst (no window open) applies IMMEDIATELY so the band feels instant, then opens the
    //! throttle window. Subsequent moves within the window are latched (below) and applied by the trailing timeout -- giving a
    //! steady update rate during a continuous sweep instead of the old apply-only-after-the-mouse-stops lag.
    if (m_hoverDebounce && !m_hoverDebounce->isActive())
    {
        m_pendingHoverMeasure = -1;
        followMeasure(measure);
        m_hoverDebounce->start();

        return;
    }

    //! Inside the open window: latch the latest hovered measure for the trailing apply. Do NOT restart the window (that would
    //! be the old sliding debounce that never fires mid-sweep); the fixed window keeps the cadence steady.
    m_pendingHoverMeasure = measure;

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

    //! LAZY: render the page we are about to show if it has not been rendered yet, so the bound QML Image has a url the
    //! instant followChanged fires (ensurePageRendered emits changed() when it fills, so pages[newPage] is ready first).
    if (newPage >= 0)
    {
        ensurePageRendered(newPage);
    }

    //! Red-vs-blue: is the measure we are now following one the bad-timing walk flags? Decides the highlight colour in QML.
    updateCurrentProblemFlag();

    emit followChanged();

    return;
}

void ProofingStripModel::updateCurrentProblemFlag()
{
    m_currentMeasureIsProblem = (m_currentMeasure >= 0) && (m_problemMeasures.count(m_currentMeasure) > 0);

    return;
}

void ProofingStripModel::rebuildProblemSet()
{
    m_problemMeasures.clear();

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    const mu::engraving::Score* score = (notation && notation->elements()) ? notation->elements()->msScore() : nullptr;

    for (const BadTimingProblem& e : scanScoreForBadTiming(score))
    {
        m_problemMeasures.insert(e.measureIndex0);
    }

    //! A re-walk (undo/redo/edit) can flip the current measure's status without the follow itself changing, so recompute and
    //! fire followChanged when it flips -- that is the signal the QML highlight colour binds to.
    const bool was = m_currentMeasureIsProblem;
    updateCurrentProblemFlag();
    if (was != m_currentMeasureIsProblem)
    {
        emit followChanged();
    }

    return;
}

void ProofingStripModel::setUserZoom(double zoom)
{
    //! 0.18.1: min band zoom lowered 0.5 -> 0.25 (max 4.0 unchanged) so the follow band can zoom further OUT. Mirrors the
    //! QML wheel-handler clamp in ProofingStrip.qml -- both sites must agree or one re-clamps the other.
    const double clamped = std::clamp(zoom, 0.25, 4.0);
    if (qFuzzyCompare(clamped, m_userZoom))
    {
        return;
    }

    m_userZoom = clamped;
    emit userZoomChanged();

    //! 0.18.2: this is the USER path (QML wheel / the dev set-zoom tool). Push the new zoom into the per-document view-state
    //! service (silent -- no re-hydrate) so the NEXT Save of the .repr captures it. It deliberately does NOT mark the project
    //! unsaved: a zoom is a view preference, not a document edit, so merely zooming must not raise a "save changes?" prompt
    //! (same non-dirtying policy applied to the navigator collapse/ignore). The RESTORE path (applyRestoredZoom) also never
    //! dirties. Consequence: the zoom persists when the user saves the document; it does not force a save on its own.
    if (proofingViewStateService())
    {
        proofingViewStateService()->syncBandZoom(clamped);
    }

    return;
}

void ProofingStripModel::applyRestoredZoom(double zoom)
{
    //! RESTORE path (see header): set the band zoom from the service WITHOUT pushing back or marking the project unsaved.
    const double clamped = std::clamp(zoom, 0.25, 4.0);
    if (qFuzzyCompare(clamped, m_userZoom))
    {
        return;
    }

    m_userZoom = clamped;
    emit userZoomChanged();

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
