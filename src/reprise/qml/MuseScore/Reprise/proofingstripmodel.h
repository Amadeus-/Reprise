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

#include <QObject>
#include <QString>
#include <QVariantList>
#include <qqmlintegration.h>

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "internal/iproofingmapservice.h"

class QImage;

namespace mu::reprise {
//! View-model behind the Reprise Two-Pane Proofing TOP STRIP (Phase A2): the view-only band above the notation editor
//! that renders the ORIGINAL scanned page(s) the current score was recognised from. Reads the latest import's
//! ProofingMap (sourcePath + per-page dims) from IProofingMapService, renders each page once to a PNG in a per-instance
//! temp dir (PDF via QPdfDocument, images via QImageReader), and exposes file:// URLs + aspect dims for the QML
//! Repeater. Re-renders whenever the service's map changes (a fresh import). A2 is DISPLAY ONLY -- it does not consume
//! the measure rects and wires no follow / highlight / score interaction (that is Phase A3).
class ProofingStripModel : public QObject, public muse::async::Asyncable
{
    Q_OBJECT

    //! true once at least one source page rendered from the current map -> the strip should be shown.
    Q_PROPERTY(bool hasMap READ hasMap NOTIFY changed)
    //! One entry per rendered page, left-to-right: { url: "file:///...png", width: <px>, height: <px> }. width/height
    //! are the rendered pixel size, used by the delegate to size each page to the strip height preserving aspect.
    Q_PROPERTY(QVariantList pages READ pages NOTIFY changed)

    QML_ELEMENT

    muse::GlobalInject<mu::reprise::IProofingMapService> proofingMapService;

public:
    explicit ProofingStripModel(QObject* parent = nullptr);
    ~ProofingStripModel() override;

    bool hasMap() const { return m_hasMap; }
    QVariantList pages() const { return m_pages; }

    //! Called from the strip's Component.onCompleted: subscribe to map changes (once) and render the current map.
    Q_INVOKABLE void load();

signals:
    void changed();

private:
    //! Re-render the strip from whatever the service currently holds (clears the previous render first).
    void rebuild();

    //! Render the recognised pages of a source PDF via QPdfDocument.
    void renderPdfPages(const QString& sourcePath, const ProofingMap& map, const QString& dir);

    //! Render the recognised frames of a source image (single-frame, or multi-frame TIFF via the frame-walk idiom).
    void renderImagePages(const QString& sourcePath, const ProofingMap& map, const QString& dir);

    //! Scale (if needed), write <dir>/sheetN_gen.png, and append its { url, width, height } entry to m_pages.
    void appendPage(const QImage& image, const QString& dir, int32_t index);

    //! Delete the PNGs written by the previous render (this instance's temp subdir only).
    void clearRendered();

    //! Per-instance temp subdir holding this strip's rendered page PNGs.
    QString renderDir() const;

    bool m_inited = false;
    bool m_hasMap = false;
    QVariantList m_pages;
    int32_t m_generation = 0;
};
}
