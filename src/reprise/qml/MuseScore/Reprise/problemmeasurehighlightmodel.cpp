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

#include "problemmeasurehighlightmodel.h"

#include <cmath>

#include "notation/inotation.h"
#include "notation/inotationelements.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationselection.h"
#include "notation/inotationundostack.h"

#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/score.h"

#include "log.h"

using namespace mu::reprise;

ProblemMeasureHighlightModel::ProblemMeasureHighlightModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: nothing that resolves the IoC context may run here. iocCtxForQmlObject(this) is lazy (it reads qmlContext(this),
    //! attached only AFTER this ctor returns); touching projectSession() now trips the "QQmlContext is not set" assert and
    //! crashes on first use. All context-dependent setup is deferred to load().
}

ProblemMeasureHighlightModel::~ProblemMeasureHighlightModel()
{
    //! Drop the editor-view registration if this model published one, so the bridge never hands out a dangling view after
    //! the page is torn down. unregisterEditorView only clears the slot when it still points at our view, so a newer page's
    //! registration is never stepped on.
    if (!m_editorView.isNull() && proofingViewControl())
    {
        proofingViewControl()->unregisterEditorView(m_editorView.data());
    }
}

void ProblemMeasureHighlightModel::setEditorView(QObject* view)
{
    if (m_editorView.data() == view)
    {
        return;
    }

    if (!m_editorView.isNull() && proofingViewControl())
    {
        proofingViewControl()->unregisterEditorView(m_editorView.data());
    }

    m_editorView = view;

    if (view && proofingViewControl())
    {
        proofingViewControl()->registerEditorView(view);
    }

    return;
}

void ProblemMeasureHighlightModel::load()
{
    if (!m_inited)
    {
        m_inited = true;

        //! Re-point the selection + undo-stack subscriptions whenever the user switches to a different score/part.
        projectSession()->currentNotationChanged().onNotify(this, [this]()
        {
            resubscribe();
        });
    }

    resubscribe();

    return;
}

void ProblemMeasureHighlightModel::resubscribe()
{
    //! Destroy + recreate the receiver so the previous notation's subscriptions are dropped before we bind the new ones.
    m_notationReceiver = std::make_unique<muse::async::Asyncable>();

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (notation && notation->interaction())
    {
        notation->interaction()->selectionChanged().onNotify(m_notationReceiver.get(), [this]()
        {
            resolve();
        });
    }

    if (notation && notation->undoStack())
    {
        //! Any edit / undo / redo re-walks the score, so a measure the user has just fixed stops being highlighted the
        //! instant its timing resolves (and a newly-broken one that is selected turns red).
        notation->undoStack()->stackChanged().onNotify(m_notationReceiver.get(), [this]()
        {
            rebuildProblemSet();
        });
    }

    rebuildProblemSet();
    resolve();

    return;
}

void ProblemMeasureHighlightModel::rebuildProblemSet()
{
    m_problemMeasures.clear();

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    const mu::engraving::Score* score = (notation && notation->elements()) ? notation->elements()->msScore() : nullptr;

    for (const BadTimingProblem& e : scanScoreForBadTiming(score))
    {
        m_problemMeasures.insert(e.measureIndex0);
    }

    //! A re-walk can flip the selected measure's status without the selection changing, so re-resolve + emit.
    resolve();

    return;
}

void ProblemMeasureHighlightModel::resolve()
{
    bool active = false;
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (notation && notation->interaction())
    {
        const mu::notation::INotationSelectionPtr sel = notation->interaction()->selection();
        if (sel && !sel->isNone())
        {
            //! startMeasureBase() resolves a single-note selection to its enclosing measure, a range to its first measure.
            //! measureIndex() is POSITIONAL (pickup = 0) -- the same key scanScoreForBadTiming uses -- so the two line up.
            mu::engraving::MeasureBase* mb = sel->startMeasureBase();
            if (mb && mb->isMeasure())
            {
                const mu::engraving::Measure* measure = mu::engraving::toMeasure(mb);
                if (m_problemMeasures.find(measure->measureIndex()) != m_problemMeasures.end())
                {
                    //! The Measure's canvas bbox spans the full system height (all staves) at the measure's horizontal
                    //! extent -- exactly the whole-bar region to tint red, in the LOGICAL coords fromLogicalRect() expects.
                    const muse::RectF r = measure->canvasBoundingRect();
                    active = true;
                    x = r.x();
                    y = r.y();
                    w = r.width();
                    h = r.height();
                }
            }
        }
    }

    auto same = [](double a, double b) { return std::abs(a - b) < 0.01; };
    const bool unchanged = (active == m_active)
                           && (!active || (same(x, m_logicalX) && same(y, m_logicalY)
                                           && same(w, m_logicalW) && same(h, m_logicalH)));
    if (unchanged)
    {
        return;
    }

    m_active = active;
    m_logicalX = x;
    m_logicalY = y;
    m_logicalW = w;
    m_logicalH = h;

    emit changed();

    return;
}
