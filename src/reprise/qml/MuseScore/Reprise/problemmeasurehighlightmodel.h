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
#include <qqmlintegration.h>

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "project/iprojectsession.h"

#include "internal/badtimingcheck.h"

namespace mu::reprise {
//! View-model behind the Reprise EDITOR problem-measure RED HIGHLIGHT. When the current notation selection lands on a
//! bad-timing PROBLEM measure (the SAME read-only walk that feeds the navigator + the proofing band/magnifier), this
//! exposes that measure's LOGICAL (canvas) bounding rect so an overlay Rectangle in NotationPage.qml can draw a red box
//! over it in the notation editor -- the editor counterpart to the band/magnifier red. It is a pure QML scene-graph
//! overlay (NOT drawn by the C++ notation canvas), so it renders the instant the selection changes AND is capturable in a
//! headless grabWindow session (unlike a canvas-paint decoration, which the headless canvas does not repaint on a
//! programmatic selection). The QML maps the logical rect to view pixels via the paint view's fromLogicalRect(), re-reading
//! it on matrixChanged so the box stays glued to the measure while the user pans/zooms.
//!
//! One-way + read-only: it never moves the cursor and never mutates the score. It re-walks on undo-stack changes (so the
//! red clears the instant the measure is fixed) and re-points its subscriptions when the user switches score/part.
class ProblemMeasureHighlightModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    //! true when the current selection resolves to a problem measure -> the QML overlay is shown over logicalRect.
    Q_PROPERTY(bool active READ active NOTIFY changed)

    //! The highlighted measure's bounding rect in LOGICAL (canvas) coordinates. The QML turns this into view px with the
    //! paint view's fromLogicalRect(). Valid only while active.
    Q_PROPERTY(double logicalX READ logicalX NOTIFY changed)
    Q_PROPERTY(double logicalY READ logicalY NOTIFY changed)
    Q_PROPERTY(double logicalW READ logicalW NOTIFY changed)
    Q_PROPERTY(double logicalH READ logicalH NOTIFY changed)

    QML_ELEMENT

    muse::ContextInject<mu::project::IProjectSession> projectSession = { this };

public:
    explicit ProblemMeasureHighlightModel(QObject* parent = nullptr);
    ~ProblemMeasureHighlightModel() override;

    bool active() const { return m_active; }
    double logicalX() const { return m_logicalX; }
    double logicalY() const { return m_logicalY; }
    double logicalW() const { return m_logicalW; }
    double logicalH() const { return m_logicalH; }

    //! Called from the overlay's Component.onCompleted: subscribe (once) to notation/selection/undo-stack changes and do
    //! the first resolve.
    Q_INVOKABLE void load();

signals:
    void changed();

private:
    //! Drop + rebind the selection + undo-stack subscriptions for the CURRENT notation (at load and on every switch), then
    //! re-walk the problem set and re-resolve. Mirrors the proofing models' resubscribe idiom.
    void resubscribe();

    //! Re-walk the current score read-only (scanScoreForBadTiming) into the problem-measure set, then re-resolve so a
    //! fix/undo updates the red live.
    void rebuildProblemSet();

    //! Recompute active + the logical rect from the current selection against the cached problem set, and emit changed()
    //! if anything flipped. Safe with no selection / no score (clears active).
    void resolve();

    bool m_inited = false;

    //! POSITIONAL indices (pickup = 0) of every measure the walk flags; the resolve checks the selected measure against it.
    std::set<int32_t> m_problemMeasures;

    bool m_active = false;
    double m_logicalX = 0.0;
    double m_logicalY = 0.0;
    double m_logicalW = 0.0;
    double m_logicalH = 0.0;

    //! Receiver for the CURRENT notation's selection + undo-stack channels; destroyed+recreated on every notation switch so
    //! a previous notation's notifications never drive this model (the proofing-models idiom).
    std::unique_ptr<muse::async::Asyncable> m_notationReceiver;
};
}
