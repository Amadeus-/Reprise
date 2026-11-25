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
#include <set>

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlintegration.h>

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "project/iprojectsession.h"
#include "notation/inotationinteraction.h"

#include "internal/iproofingmapservice.h"
#include "internal/iproofingviewcontrol.h"
#include "internal/iproofingviewstateservice.h"
#include "internal/badtimingcheck.h"

class QImage;
class QTimer;
class QPdfDocument;

namespace mu::reprise {
//! View-model behind the Reprise Two-Pane Proofing FULL DETAIL VIEW -- the floating "Full detail view" MAGNIFIER (Phase
//! B2). It is the view-only, note-level zoom that makes PhotoScore-style proofing work: a whole page shrunk to screen
//! size is too small to read individual notes, so this shows just the spot being checked, magnified, with the selected
//! note highlighted on the ORIGINAL scan. It reads the latest import's ProofingMap from IProofingMapService and renders
//! each source page once to a PNG in a per-instance temp dir, at a HIGHER resolution than the A3 following band (so the
//! magnified crop stays crisp). It then observes the current notation's selection: on every selection change it finds
//! the selected engraving Note, derives the Phase B1 runtime key (globalMeasureIndex / staffIdx / tickInMeasure / step /
//! octave) straight from that Note, and asks the service for the note's head box. Found -> it exposes that box (in
//! sheet-image pixels) + its page and the QML draws a SOLID accent highlight centred + magnified over the note. No box
//! (a grace note -- Audiveris drops graces -- or an untrusted head) -> it FALLS BACK to the note's MEASURE rect and
//! flags isFallback so the QML draws a DASHED measure-level marker (visibly a fallback, not a false note-point). A
//! measure/range selection with no single note likewise shows the measure rect as a dashed fallback (so driving the
//! A3 debug-goto measure tool still lights the magnifier). One-way: the magnifier never moves the score cursor.
class ProofingMagnifierModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    //! true once at least one source page rendered from the current map -> the panel has something to show.
    Q_PROPERTY(bool hasMap READ hasMap NOTIFY changed)
    //! One entry per rendered page, left-to-right: { url: "file:///...png", width, height, sheetWidth, sheetHeight }.
    //! width/height are the rendered PNG pixel size; sheetWidth/sheetHeight are the Audiveris sheet-image pixel space the
    //! note/measure boxes live in (the QML maps a sheet-pixel box to the rendered image via width / sheetWidth).
    Q_PROPERTY(QVariantList pages READ pages NOTIFY changed)

    //! 0-based index into pages of the page the current note/measure sits on (-1 when nothing is followed yet).
    Q_PROPERTY(int currentPageIndex READ currentPageIndex NOTIFY followChanged)
    //! true when the current selection resolves to a box to magnify (a note head box OR a measure-rect fallback).
    Q_PROPERTY(bool hasCurrent READ hasCurrent NOTIFY followChanged)
    //! true when the shown box is the MEASURE-rect fallback (grace / untrusted / unmatched note, or a measure selection)
    //! rather than a precise note head box -- the QML draws a dashed measure-level marker instead of a solid note box.
    Q_PROPERTY(bool isFallback READ isFallback NOTIFY followChanged)

    //! The box to centre + magnify, in its page's SHEET-IMAGE pixel space (valid only while hasCurrent). For a matched
    //! note it is the head box; for a fallback it is the whole measure rect.
    Q_PROPERTY(int boxX READ boxX NOTIFY followChanged)
    Q_PROPERTY(int boxY READ boxY NOTIFY followChanged)
    Q_PROPERTY(int boxW READ boxW NOTIFY followChanged)
    Q_PROPERTY(int boxH READ boxH NOTIFY followChanged)

    //! The followed global measure index (0-based, pickup = 0), or -1 when nothing is followed. Shown in the header.
    Q_PROPERTY(int currentMeasure READ currentMeasure NOTIFY followChanged)
    //! A short human-readable pitch label for the followed note (e.g. "C4"), empty on a measure-level fallback. Header.
    Q_PROPERTY(QString currentNoteName READ currentNoteName NOTIFY followChanged)

    //! true when the currently-followed measure is a bad-timing PROBLEM measure (it appears in scanScoreForBadTiming, the
    //! SAME read-only walk that feeds the navigator) -> the magnifier draws its note/measure marker RED instead of the normal
    //! accent blue, so a problem measure is visually distinct while proofing. Refreshed on every follow + undo-stack change.
    Q_PROPERTY(bool currentMeasureIsProblem READ currentMeasureIsProblem NOTIFY followChanged)

    //! User-driven zoom factor (mouse-wheel), multiplied onto the magnifier's effective zoom. 1.0 = the auto-chosen
    //! note/measure zoom; the setter clamps to [0.5, 4.0]. A Q_PROPERTY (not a plain QML property) so it survives a model
    //! rebuild and can later be driven by a dev tool; the crop + overlays already derive from `zoom`, so this just scales.
    Q_PROPERTY(double userZoom READ userZoom WRITE setUserZoom NOTIFY userZoomChanged)

    QML_ELEMENT

    muse::GlobalInject<mu::reprise::IProofingMapService> proofingMapService;
    muse::GlobalInject<mu::reprise::IProofingViewControl> proofingViewControl;
    muse::GlobalInject<mu::reprise::IProofingViewStateService> proofingViewStateService;
    muse::ContextInject<mu::project::IProjectSession> projectSession = { this };

public:
    explicit ProofingMagnifierModel(QObject* parent = nullptr);
    ~ProofingMagnifierModel() override;

    bool hasMap() const { return m_hasMap; }
    QVariantList pages() const { return m_pages; }

    int currentPageIndex() const { return m_currentPageIndex; }
    bool hasCurrent() const { return m_hasCurrent; }
    bool isFallback() const { return m_isFallback; }
    int boxX() const { return m_boxX; }
    int boxY() const { return m_boxY; }
    int boxW() const { return m_boxW; }
    int boxH() const { return m_boxH; }
    int currentMeasure() const { return m_currentMeasure; }
    QString currentNoteName() const { return m_currentNoteName; }
    bool currentMeasureIsProblem() const { return m_currentMeasureIsProblem; }
    double userZoom() const { return m_userZoom; }

    //! Clamp zoom to [0.5, 4.0] and, if it changed, store it + emit userZoomChanged (the QML multiplies it onto the
    //! effective magnification). The Q_PROPERTY WRITE accessor for model.userZoom.
    void setUserZoom(double zoom);

    //! Called from the panel's Component.onCompleted: subscribe to map + notation changes (once) and render + follow.
    Q_INVOKABLE void load();

signals:
    void changed();
    void followChanged();
    void userZoomChanged();

private:
    //! 0.18.2: apply a zoom RESTORED from the per-document zoom service (.repr open / document switch) WITHOUT pushing it back
    //! to the service or marking the project unsaved -- a restore is not a user edit (unlike setUserZoom, which does both).
    //! Still clamps to [0.5, 4.0] and emits userZoomChanged so the magnifier re-scales.
    void applyRestoredZoom(double zoom);

    //! Coalesce + DEFER the heavy render to the next event-loop turn instead of running it inline. The proofing map's
    //! mapChanged() is a SYNCHRONOUS notify fired from inside NotationProject::doLoadRepr on the UI thread; rebuilding
    //! inline there renders every page (QPdfDocument load + per-page render + PNG write) and freezes the .repr open. A
    //! zero-delay single-shot lets doLoadRepr return and the editor become interactive FIRST, then the magnifier renders
    //! with its placeholder showing. The DockPanel's Loader (active: root.visible) already means this model exists ONLY
    //! while the panel is actually shown, so a collapsed/unseen magnifier never subscribes and never renders. m_rebuildScheduled
    //! coalesces a burst of notifies into one render.
    void scheduleRebuild();

    //! Re-point the magnifier at whatever the service currently holds (clears the previous render first). LAZY: this only
    //! opens the source + sizes m_pages with one PLACEHOLDER entry per recognised page and renders the FIRST page; the rest
    //! render on demand the first time the follow lands on them (ensurePageRendered). Rendering + PNG-encoding every page
    //! here -- at the magnifier's HIGHER resolution -- was the dominant cost that froze a multi-page .repr open, and the
    //! magnifier only ever displays ONE page at a time. Then recompute the follow against the fresh map.
    void rebuild();

    //! Open the source (PDF via QPdfDocument kept alive for on-demand page renders; images re-opened per page) and fill
    //! m_pages with one placeholder entry per recognised page. Sets m_pageCount; leaves every entry's url empty.
    void prepareLazyPages(const ProofingMap& map);

    //! Render page `index` to a PNG and fill its m_pages entry if not already done (cached by a non-empty url). Returns true
    //! when the page has a rendered PNG after the call. Emits changed() when it fills a page so a bound QML Image re-reads.
    bool ensurePageRendered(int32_t index);

    //! Render a single source page to a QImage at the magnifier resolution (PDF via m_pdfDoc; image via QImageReader
    //! frame-walk). sheetW/sheetH (the native scan dims for this page) cap the render side so it never upscales. Null on error.
    QImage renderPageImage(int32_t index, int32_t sheetW, int32_t sheetH) const;

    //! Scale (if needed), write <dir>/sheetN_gen.png, and return its { url, width, height, sheetWidth, sheetHeight } entry
    //! (empty map on failure).
    QVariantMap savePageEntry(const QImage& image, int32_t index, int32_t sheetW, int32_t sheetH) const;

    //! Delete the PNGs written by the previous render (this instance's temp subdir only).
    void clearRendered();

    //! Per-instance temp subdir holding this magnifier's rendered page PNGs.
    QString renderDir() const;

    //! Drop + rebuild the per-notation selection + hover subscriptions (at load and on every notation switch), then follow.
    void resubscribeSelection();

    //! Recompute currentPageIndex + the box to magnify from the current notation's SELECTION against the held map, and emit
    //! followChanged. Safe to call with no selection / no map (clears hasCurrent). Click/selection path (unchanged).
    void updateFollow();

    //! Shared tail of the selection + hover paths: diff the resolved target against the current state and, if anything
    //! changed, store it + emit followChanged. found == false clears hasCurrent (keeps the current page). When found, a
    //! fallback == true target is the dashed MEASURE rect (noteName is blanked); otherwise a solid note head box.
    void applyFollow(bool found, bool fallback, int32_t page, int32_t gm,
                     int32_t bx, int32_t by, int32_t bw, int32_t bh, const QString& noteName);

    //! Re-walk the current score read-only (scanScoreForBadTiming -- the SAME source the navigator uses) into the problem-
    //! measure set, then recompute the current-measure problem flag and emit followChanged if it flipped. Called on notation
    //! switch + on every undo-stack change, so fixing a measure drops its red marker the instant the walk no longer flags it.
    void rebuildProblemSet();

    //! Recompute m_currentMeasureIsProblem from m_currentMeasure against the cached problem set (no emit; the caller emits).
    void updateCurrentProblemFlag();

    //! Drive the magnifier from a NOTE key (the B1 positional key): look up its head box and, if found, magnify it (solid);
    //! on a box MISS fall back to the note's MEASURE rect (dashed). Shared by the selection path and the hover path.
    void followNoteKey(int32_t gm, int32_t staffIdx, int32_t tickInMeasure, int32_t step, int32_t octave,
                       const QString& noteName);

    //! Drive the magnifier from a global measure index alone: show that measure's rect as a dashed, measure-level marker
    //! (the "rest / between notes / measure selection" fallback). gm < 0 or no matching rect clears hasCurrent.
    void followMeasureOnly(int32_t gm);

    //! Hover-follow entries (subscribed ALONGSIDE selectionChanged). A LEADING-edge + fixed-window throttle: the measure
    //! channel (which fires first) only latches; the note channel (fires last) applies the first move of a burst immediately
    //! then throttles the rest to one apply per window, so a continuous sweep updates at a steady rate instead of only after
    //! the mouse stops. applyHover() picks note-level when a note is hovered, else the measure-level fallback; a leave (no
    //! note AND measure < 0) holds the last view rather than blanking it.
    void scheduleHoverNote(const mu::notation::HoveredNoteInfo& note);
    void scheduleHoverMeasure(int32_t measure);
    void applyHover();

    bool m_inited = false;
    bool m_hasMap = false;
    QVariantList m_pages;
    int32_t m_generation = 0;

    //! LAZY-RENDER state (set in rebuild, used by ensurePageRendered for on-demand per-page rendering). The magnifier
    //! renders each page the first time the follow lands on it, not all up front -- so a multi-page .repr opens fast.
    QString m_sourcePath;                       //! the proofing source (deskewed render if present, else the original scan)
    QString m_renderDir;                        //! this magnifier's per-instance temp subdir for the rendered page PNGs
    bool m_sourceIsPdf = false;                 //! true -> render via m_pdfDoc; false -> QImageReader frame-walk
    int32_t m_pageCount = 0;                     //! recognised page count (== m_pages.size() once prepared)
    std::unique_ptr<QPdfDocument> m_pdfDoc;     //! kept alive across on-demand renders for a PDF source (null for images)
    bool m_inRebuild = false;                   //! guards ensurePageRendered from emitting changed() mid-rebuild (one emit at the end)

    //! true between a scheduleRebuild() and the deferred render firing, so a burst of mapChanged / load notifies coalesces
    //! into a single render rather than queuing one per notify.
    bool m_rebuildScheduled = false;

    int32_t m_currentPageIndex = -1;
    bool m_hasCurrent = false;
    bool m_isFallback = false;
    int32_t m_boxX = 0;
    int32_t m_boxY = 0;
    int32_t m_boxW = 0;
    int32_t m_boxH = 0;
    int32_t m_currentMeasure = -1;
    QString m_currentNoteName;

    //! POSITIONAL indices (pickup = 0) of every measure the bad-timing walk currently flags, so applyFollow can colour a
    //! followed problem measure red in O(log n). Rebuilt by rebuildProblemSet on notation switch + undo-stack change.
    std::set<int32_t> m_problemMeasures;
    //! true when m_currentMeasure is in m_problemMeasures (the QML binds currentMeasureIsProblem to pick red vs accent blue).
    bool m_currentMeasureIsProblem = false;

    //! User-driven magnifier zoom (mouse-wheel). 1.0 = the auto-chosen note/measure zoom; clamped to [0.5, 4.0].
    double m_userZoom = 1.0;

    //! Receiver for the CURRENT notation's selectionChanged + hovered note/measure channels; reset (destroyed+recreated) on
    //! every notation switch so a previous notation's subscriptions never linger (the NotationSwitchListModel idiom).
    std::unique_ptr<muse::async::Asyncable> m_selectionReceiver;

    //! Debounce for hover-follow: a fast mouse sweep fires many hovered-note/measure notifications, so the magnifier applies
    //! only the latest ~100ms after the mouse settles (mirrors the following band's debounce). Owned by this; one-shot.
    QTimer* m_hoverDebounce = nullptr;
    //! Latest hovered note (invalid = cursor not over a note) and latest hovered measure (-1 = none / mouse left), latched
    //! for the debounce timer. applyHover() prefers the note when valid, else the measure fallback, else holds the last.
    mu::notation::HoveredNoteInfo m_pendingHoverNote;
    int32_t m_pendingHoverMeasure = -1;
};
}
