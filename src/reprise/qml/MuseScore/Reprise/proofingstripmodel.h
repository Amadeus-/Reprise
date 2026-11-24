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

#pragma once

#include <cstdint>
#include <memory>

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlintegration.h>

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "project/iprojectsession.h"

#include "internal/iproofingmapservice.h"
#include "internal/iproofingviewcontrol.h"

class QImage;
class QTimer;
class QPdfDocument;

namespace mu::reprise {
//! View-model behind the Reprise Two-Pane Proofing READABLE FOLLOWING BAND (Phase A3): the view-only band above the
//! notation editor that renders the ORIGINAL scanned page(s) the current score was recognised from, at a READABLE size,
//! and AUTO-FOLLOWS the score. It reads the latest import's ProofingMap (sourcePath + per-page dims + per-measure rects)
//! from IProofingMapService and renders each page once to a PNG in a per-instance temp dir (PDF via QPdfDocument, images
//! via QImageReader). It then observes the current notation's selection: on every selection change it derives the
//! selected element's GLOBAL MEASURE INDEX (positional, pickup = 0 -- MeasureBase::measureIndex(), which matches the map's
//! gm), looks up that measure's {pageIndex, rect} in the map, and exposes currentPageIndex + the rect (in sheet-image
//! pixels) so the QML band can switch to that page, scroll the matching system into view, and draw a highlight box over
//! the current measure. In ADDITION to selection follow, the band also HOVER-FOLLOWS: it subscribes to the notation's
//! hoveredMeasureChanged (ungated hover from NotationViewInputController) and, debounced ~100ms, drives the same follow
//! from the measure under the mouse (only when a map exists; a hover that resolves to no measure is ignored). One-way: the
//! band never moves the score cursor. Note-level highlight + the magnifier (selection-driven, not hover) are Phase B.
class ProofingStripModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    //! true once at least one source page rendered from the current map -> the strip should be shown.
    Q_PROPERTY(bool hasMap READ hasMap NOTIFY changed)
    //! One entry per rendered page, left-to-right: { url: "file:///...png", width, height, sheetWidth, sheetHeight }.
    //! width/height are the rendered PNG pixel size; sheetWidth/sheetHeight are the Audiveris sheet-image pixel space the
    //! measure rects live in (they share the page aspect, so the band scales a rect by bandWidth / sheetWidth).
    Q_PROPERTY(QVariantList pages READ pages NOTIFY changed)

    //! 0-based index into pages of the page the current measure sits on (-1 when nothing is followed yet). The band binds
    //! its Image source to pages[currentPageIndex].
    Q_PROPERTY(int currentPageIndex READ currentPageIndex NOTIFY followChanged)
    //! true when the current selection maps to a measure rect in the held map (the band draws its highlight + scrolls).
    Q_PROPERTY(bool hasCurrent READ hasCurrent NOTIFY followChanged)
    //! The followed measure's pixel rect in its page's SHEET-IMAGE space (valid only while hasCurrent).
    Q_PROPERTY(int rectX READ rectX NOTIFY followChanged)
    Q_PROPERTY(int rectY READ rectY NOTIFY followChanged)
    Q_PROPERTY(int rectW READ rectW NOTIFY followChanged)
    Q_PROPERTY(int rectH READ rectH NOTIFY followChanged)
    //! The followed global measure index (0-based, pickup = 0), or -1 when nothing is selected. Shown in the header.
    Q_PROPERTY(int currentMeasure READ currentMeasure NOTIFY followChanged)

    //! User-driven zoom factor for the band (mouse-wheel follow-centered zoom). 1.0 = fit-to-width; the setter clamps to
    //! [0.5, 4.0]. A Q_PROPERTY (not a plain QML property) so it survives a model rebuild and can later be driven by a dev
    //! tool; QML binds to model.userZoom and folds it into imgScale + the page width.
    Q_PROPERTY(double userZoom READ userZoom WRITE setUserZoom NOTIFY userZoomChanged)

    QML_ELEMENT

    muse::GlobalInject<mu::reprise::IProofingMapService> proofingMapService;
    muse::GlobalInject<mu::reprise::IProofingViewControl> proofingViewControl;
    muse::ContextInject<mu::project::IProjectSession> projectSession = { this };

public:
    explicit ProofingStripModel(QObject* parent = nullptr);
    ~ProofingStripModel() override;

    bool hasMap() const { return m_hasMap; }
    QVariantList pages() const { return m_pages; }

    int currentPageIndex() const { return m_currentPageIndex; }
    bool hasCurrent() const { return m_hasCurrent; }
    int rectX() const { return m_rectX; }
    int rectY() const { return m_rectY; }
    int rectW() const { return m_rectW; }
    int rectH() const { return m_rectH; }
    int currentMeasure() const { return m_currentMeasure; }
    double userZoom() const { return m_userZoom; }

    //! Clamp zoom to [0.5, 4.0] and, if it changed, store it + emit userZoomChanged (the QML folds it into imgScale and
    //! re-centers the followed measure). The Q_PROPERTY WRITE accessor for model.userZoom.
    void setUserZoom(double zoom);

    //! Called from the band's Component.onCompleted: subscribe to map + notation changes (once) and render + follow.
    Q_INVOKABLE void load();

signals:
    void changed();
    void followChanged();
    void userZoomChanged();

private:
    //! Re-point the band at whatever the service currently holds (clears the previous render first). LAZY: this only opens
    //! the source + sizes m_pages with one PLACEHOLDER entry per recognised page (geometry known from the map, pixels not
    //! rendered yet) and renders the FIRST page so the band shows immediately; the remaining pages render on demand the
    //! first time the follow lands on them (ensurePageRendered). This keeps a multi-page .repr open fast -- rendering +
    //! PNG-encoding every page here was the ~N x per-page cost that froze the UI on open, and the band only ever displays
    //! ONE page at a time, so pre-rendering the rest was pure waste. Then recompute the follow against the fresh map.
    void rebuild();

    //! Open the source (PDF via QPdfDocument kept alive for on-demand page renders; images re-opened per page) and fill
    //! m_pages with one placeholder entry per recognised page. Sets m_pageCount; leaves every entry's url empty.
    void prepareLazyPages(const ProofingMap& map);

    //! Render page `index` to a PNG and fill its m_pages entry if not already done (cached by a non-empty url). Returns true
    //! when the page has a rendered PNG after the call. Emits changed() when it fills a page so a bound QML Image re-reads.
    bool ensurePageRendered(int32_t index);

    //! Render a single source page to a QImage (PDF via the held m_pdfDoc; image via QImageReader frame-walk). Null on error.
    QImage renderPageImage(int32_t index) const;

    //! Scale (if needed), write <dir>/sheetN_gen.png, and return its { url, width, height, sheetWidth, sheetHeight } entry
    //! (empty map on failure). sheetW/sheetH are the Audiveris sheet-image dims for this page (0 -> fall back to render dims).
    QVariantMap savePageEntry(const QImage& image, int32_t index, int32_t sheetW, int32_t sheetH) const;

    //! Delete the PNGs written by the previous render (this instance's temp subdir only).
    void clearRendered();

    //! Per-instance temp subdir holding this strip's rendered page PNGs.
    QString renderDir() const;

    //! Drop + rebuild the per-notation selection subscription (called at load and whenever the current notation changes),
    //! then recompute the follow once for the now-current selection.
    void resubscribeSelection();

    //! Recompute currentPageIndex + the highlight rect from the current notation's selection against the held map, and
    //! emit followChanged. Safe to call with no selection / no map (clears hasCurrent).
    void updateFollow();

    //! Drive the band follow from a single POSITIONAL global measure index (pickup = 0): look it up in the held map and,
    //! if changed, update currentPageIndex + the highlight rect and emit followChanged. Shared tail of the selection path
    //! (updateFollow) and the hover path (the debounce timer). measure < 0 clears hasCurrent (selection cleared).
    void followMeasure(int32_t measure);

    //! Hover-follow entry (subscribed ALONGSIDE selectionChanged): a LEADING-edge + fixed-window throttle -- the first move
    //! applies immediately (snappy), then further moves within the window are latched and applied once per window (the
    //! trailing timeout), so a continuous sweep updates at a steady rate instead of only after the mouse fully stops.
    //! Ignores measure < 0 (empty canvas / non-measure) and no-map.
    void scheduleHoverFollow(int32_t measure);

    //! FIRST-MEASURE-ON-OPEN: as soon as BOTH the proofing map is held AND a notation is open, select the first note of
    //! measure 1 so selectionChanged fires and both proofing panels populate with NO hover required. Runs once per map
    //! generation and never overrides an existing selection; this is the ONE deliberate place the band drives the cursor.
    void maybeSelectFirstOnOpen();

    bool m_inited = false;
    bool m_hasMap = false;
    QVariantList m_pages;
    int32_t m_generation = 0;

    //! LAZY-RENDER state (set in rebuild, used by ensurePageRendered for on-demand per-page rendering). The band renders
    //! each page the first time the follow lands on it, not all up front -- so a multi-page .repr opens in one page's time.
    QString m_sourcePath;                       //! the proofing source (deskewed render if present, else the original scan)
    QString m_renderDir;                        //! this band's per-instance temp subdir for the rendered page PNGs
    bool m_sourceIsPdf = false;                 //! true -> render via m_pdfDoc; false -> QImageReader frame-walk
    int32_t m_pageCount = 0;                     //! recognised page count (== m_pages.size() once prepared)
    std::unique_ptr<QPdfDocument> m_pdfDoc;     //! kept alive across on-demand renders for a PDF source (null for images)
    bool m_inRebuild = false;                   //! guards ensurePageRendered from emitting changed() mid-rebuild (one emit at the end)

    int32_t m_currentPageIndex = -1;
    bool m_hasCurrent = false;
    int32_t m_rectX = 0;
    int32_t m_rectY = 0;
    int32_t m_rectW = 0;
    int32_t m_rectH = 0;
    int32_t m_currentMeasure = -1;

    //! User-driven band zoom (mouse-wheel). 1.0 = fit-to-width; clamped to [0.5, 4.0] by setUserZoom.
    double m_userZoom = 1.0;

    //! Receiver for the CURRENT notation's selectionChanged + hoveredMeasureChanged; reset (destroyed+recreated) on every
    //! notation switch so a previous notation's subscriptions never linger (the NotationSwitchListModel per-notation idiom).
    std::unique_ptr<muse::async::Asyncable> m_selectionReceiver;

    //! Debounce for hover-follow: a fast mouse sweep across measures fires many hoveredMeasureChanged notifications, so the
    //! band only applies the latest ~100ms after the mouse settles (prevents page/scroll thrash). Owned by this; one-shot.
    QTimer* m_hoverDebounce = nullptr;
    //! The most recent measure the mouse hovered (>= 0), latched for the debounce timer to apply; -1 = nothing pending.
    int32_t m_pendingHoverMeasure = -1;

    //! The map generation the FIRST-MEASURE-ON-OPEN auto-select has already run for (-1 = none yet). Guards it to once per
    //! map so switching scores / selecting elsewhere never re-yanks the cursor back to measure 1.
    int32_t m_autoSelectGeneration = -1;
};
}
