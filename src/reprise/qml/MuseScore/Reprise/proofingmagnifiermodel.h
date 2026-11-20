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

#include "internal/iproofingmapservice.h"

class QImage;

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

    QML_ELEMENT

    muse::GlobalInject<mu::reprise::IProofingMapService> proofingMapService;
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

    //! Called from the panel's Component.onCompleted: subscribe to map + notation changes (once) and render + follow.
    Q_INVOKABLE void load();

signals:
    void changed();
    void followChanged();

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

    //! Drop + rebuild the per-notation selection subscription (at load and on every notation switch), then follow once.
    void resubscribeSelection();

    //! Recompute currentPageIndex + the box to magnify from the current notation's selection against the held map, and
    //! emit followChanged. Safe to call with no selection / no map (clears hasCurrent).
    void updateFollow();

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

    //! Receiver for the CURRENT notation's selectionChanged; reset (destroyed+recreated) on every notation switch so a
    //! previous notation's subscription never lingers (the NotationSwitchListModel per-notation-receiver idiom).
    std::unique_ptr<muse::async::Asyncable> m_selectionReceiver;
};
}
