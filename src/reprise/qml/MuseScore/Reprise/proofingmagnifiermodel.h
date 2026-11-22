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
#include <qqmlintegration.h>

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "project/iprojectsession.h"
#include "notation/inotationinteraction.h"

#include "internal/iproofingmapservice.h"
#include "internal/iproofingviewcontrol.h"

class QImage;
class QTimer;

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

    //! User-driven zoom factor (mouse-wheel), multiplied onto the magnifier's effective zoom. 1.0 = the auto-chosen
    //! note/measure zoom; the setter clamps to [0.5, 4.0]. A Q_PROPERTY (not a plain QML property) so it survives a model
    //! rebuild and can later be driven by a dev tool; the crop + overlays already derive from `zoom`, so this just scales.
    Q_PROPERTY(double userZoom READ userZoom WRITE setUserZoom NOTIFY userZoomChanged)

    QML_ELEMENT

    muse::GlobalInject<mu::reprise::IProofingMapService> proofingMapService;
    muse::GlobalInject<mu::reprise::IProofingViewControl> proofingViewControl;
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
    //! Re-render from whatever the service currently holds (clears the previous render first), then recompute the follow.
    void rebuild();

    //! Render the recognised pages of a source PDF via QPdfDocument, at the magnifier's higher resolution.
    void renderPdfPages(const QString& sourcePath, const ProofingMap& map, const QString& dir);

    //! Render the recognised frames of a source image (single-frame, or multi-frame TIFF via the frame-walk idiom).
    void renderImagePages(const QString& sourcePath, const ProofingMap& map, const QString& dir);

    //! Scale (if needed), write <dir>/sheetN_gen.png, and append its { url, width, height, sheetWidth, sheetHeight }.
    void appendPage(const QImage& image, const QString& dir, int32_t index, int32_t sheetW, int32_t sheetH);

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

    //! Drive the magnifier from a NOTE key (the B1 positional key): look up its head box and, if found, magnify it (solid);
    //! on a box MISS fall back to the note's MEASURE rect (dashed). Shared by the selection path and the hover path.
    void followNoteKey(int32_t gm, int32_t staffIdx, int32_t tickInMeasure, int32_t step, int32_t octave,
                       const QString& noteName);

    //! Drive the magnifier from a global measure index alone: show that measure's rect as a dashed, measure-level marker
    //! (the "rest / between notes / measure selection" fallback). gm < 0 or no matching rect clears hasCurrent.
    void followMeasureOnly(int32_t gm);

    //! Hover-follow entries (subscribed ALONGSIDE selectionChanged). Latch the latest hovered note / measure and (re)arm the
    //! debounce so a fast sweep applies only once the mouse settles. applyHover() picks note-level when a note is hovered,
    //! else the measure-level fallback; a leave (no note AND measure < 0) holds the last view rather than blanking it.
    void scheduleHoverNote(const mu::notation::HoveredNoteInfo& note);
    void scheduleHoverMeasure(int32_t measure);
    void applyHover();

    bool m_inited = false;
    bool m_hasMap = false;
    QVariantList m_pages;
    int32_t m_generation = 0;

    int32_t m_currentPageIndex = -1;
    bool m_hasCurrent = false;
    bool m_isFallback = false;
    int32_t m_boxX = 0;
    int32_t m_boxY = 0;
    int32_t m_boxW = 0;
    int32_t m_boxH = 0;
    int32_t m_currentMeasure = -1;
    QString m_currentNoteName;

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
