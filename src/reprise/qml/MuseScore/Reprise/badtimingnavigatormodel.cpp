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

#include "badtimingnavigatormodel.h"

#include <algorithm>

#include <QCursor>
#include <QPointF>
#include <QQuickItem>
#include <QRectF>
#include <QTimer>

#include "global/translation.h"

#include "notation/inotation.h"
#include "notation/inotationelements.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationundostack.h"

#include "engraving/types/constants.h"
#include "engraving/types/fraction.h"
#include "engraving/types/types.h"

#include "engraving/dom/chordrest.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"

#include "log.h"

using namespace mu::reprise;

BadTimingNavigatorModel::BadTimingNavigatorModel(QObject* parent)
    : QAbstractListModel(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: nothing that resolves the IoC context may run here. iocCtxForQmlObject(this) is lazy (it reads qmlContext(this),
    //! attached only AFTER this ctor returns); touching projectSession() now trips the "QQmlContext is not set" assert and
    //! crashes on first use. All context-dependent setup is deferred to load().
}

BadTimingNavigatorModel::~BadTimingNavigatorModel() = default;

void BadTimingNavigatorModel::load()
{
    if (!m_inited)
    {
        m_inited = true;

        //! Re-point the undo-stack + selection subscriptions whenever the user switches to a different score/part.
        projectSession()->currentNotationChanged().onNotify(this, [this]()
        {
            resubscribe();
        });
    }

    resubscribe();

    return;
}

void BadTimingNavigatorModel::resubscribe()
{
    //! Destroy + recreate the receiver so the previous notation's undo-stack notification is dropped before we bind the new
    //! one (prevents a stale score's edits from re-walking this model after a switch).
    m_notationReceiver = std::make_unique<muse::async::Asyncable>();

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (notation && notation->undoStack())
    {
        //! Live refresh: any edit / undo / redo fires stackChanged() -> re-walk, so a fixed measure's row disappears and a
        //! newly-broken measure appears. The walk is read-only and cheap (a few Fraction adds per measure x staff).
        notation->undoStack()->stackChanged().onNotify(m_notationReceiver.get(), [this]()
        {
            rebuild();
        });
    }

    rebuild();

    return;
}

void BadTimingNavigatorModel::rebuild()
{
    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    mu::engraving::Score* score = (notation && notation->elements()) ? notation->elements()->msScore() : nullptr;

    beginResetModel();

    //! ONE read-only walk shared with the dock controller (scanScoreForBadTiming), so the worklist rows below and the
    //! panel's show/hide decision can never disagree. The walk mutates nothing.
    m_entries = scanScoreForBadTiming(score);
    m_hasScore = (score != nullptr && score->firstMeasure() != nullptr);

    //! Compute the per-row "safe to regularize" flags from the LIVE measures (not the row's found/expected, though they
    //! agree for a WrongLength row): a one-click Fix is offered ONLY for an UNDER-full bar (ticks < timesig), where
    //! adjustToLen(timesig) extends with rests. Every other row (over-full, exactly-nominal, corrupted repeat) stays
    //! false so the button is disabled and can never truncate notes. Resolved positionally, matching jumpTo/the scan.
    m_canRegularize.assign(m_entries.size(), false);
    if (score)
    {
        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            mu::engraving::Measure* measure = score->firstMeasure();
            for (int32_t k = 0; k < m_entries[i].measureIndex0 && measure; ++k)
            {
                measure = measure->nextMeasure();
            }

            if (measure)
            {
                m_canRegularize[i] = measure->ticks() < measure->timesig();
            }
        }
    }

    endResetModel();

    emit countChanged();

    return;
}

void BadTimingNavigatorModel::jumpTo(int row)
{
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
    {
        return;
    }

    const BadTimingProblem& entry = m_entries[static_cast<size_t>(row)];

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (!notation || !notation->interaction() || !notation->elements())
    {
        return;
    }

    mu::engraving::Score* score = notation->elements()->msScore();
    if (!score)
    {
        return;
    }

    //! Resolve the Nth measure positionally (pickup = 0), matching measureIndex0 / the proofing map's globalMeasureIndex.
    mu::engraving::Measure* measure = score->firstMeasure();
    for (int32_t i = 0; i < entry.measureIndex0 && measure; ++i)
    {
        measure = measure->nextMeasure();
    }

    if (!measure)
    {
        return;
    }

    //! MEASURE-LEVEL entry (WrongLength carries staffIdx == -1): select the WHOLE MEASURE across EVERY staff, not just one
    //! chord/rest. Passing the Measure* itself with SINGLE routes through Score::selectSingle, which for a Measure target
    //! early-returns into doSelect(..., RANGE) over 0..nstaves() (engraving/dom/score.cpp) -- a full-measure range selection.
    //! This is the exact path the reprise.proofing-debug-goto dev tool uses, verified to visibly select + scroll the whole
    //! bar. showItem(measure) then scrolls the editor viewport to it.
    if (entry.staffIdx < 0)
    {
        //! clearSelection() FIRST so a RE-click of the already-selected measure still produces a selectionChanged -> the
        //! follow band re-centres on it. Without the clear, re-selecting the same measure is a no-op in muse (no
        //! selectionChanged fires), so after an editor hover moved the band to another measure, clicking the SAME row again
        //! would not bring the band back (the reported bug). muse selectionChanged is a synchronous Notification, so the
        //! clear->select pair settles within this call and renders as a single frame on the target measure (no flicker).
        notation->interaction()->clearSelection();
        notation->interaction()->select({ measure }, mu::notation::SelectType::SINGLE);
        notation->interaction()->showItem(measure);

        return;
    }

    //! PER-STAFF entry: select that staff's first chord/rest so the magnifier's note-follow lands on the right staff; fall
    //! back to any chord/rest in the measure, then to the measure frame itself.
    const mu::engraving::staff_idx_t staffIdx = static_cast<mu::engraving::staff_idx_t>(entry.staffIdx);

    mu::engraving::EngravingItem* target = measure->firstChordRest(staffIdx * mu::engraving::VOICES);
    if (!target)
    {
        for (mu::engraving::voice_idx_t v = 0; v < mu::engraving::VOICES && !target; ++v)
        {
            target = measure->firstChordRest(staffIdx * mu::engraving::VOICES + v);
        }
    }

    if (!target)
    {
        target = measure;
    }

    //! clearSelection() first (see the measure-level branch above) so re-clicking the same per-staff row re-centres the band
    //! after an editor hover moved it -- a re-select of the already-selected target is otherwise a no-op (no selectionChanged).
    notation->interaction()->clearSelection();
    notation->interaction()->select({ target }, mu::notation::SelectType::REPLACE, staffIdx);
    notation->interaction()->showItem(target);

    return;
}

void BadTimingNavigatorModel::regularize(int row)
{
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
    {
        return;
    }

    //! DATA-SAFETY GUARD (defense in depth): only an UNDER-full bar may be regularized. m_canRegularize was computed on
    //! the last walk; refuse here if it says no, so even a stray/queued call cannot reach adjustToLen on an over-full bar.
    if (row >= static_cast<int>(m_canRegularize.size()) || !m_canRegularize[static_cast<size_t>(row)])
    {
        return;
    }

    const BadTimingProblem& entry = m_entries[static_cast<size_t>(row)];

    //! Capture the positional measure index NOW, before any mutation: commitChanges() below fires stackChanged -> rebuild()
    //! SYNCHRONOUSLY, which reassigns m_entries and dangles `entry`. We schedule the post-fix cursor warp from HERE (not from
    //! QML after the call) for exactly this reason -- by the time regularize() returns, the row is gone from the worklist, so
    //! a QML-side warp-by-row would miss. The measure index itself is stable across this duration-only edit.
    const int32_t targetMeasureIndex0 = entry.measureIndex0;

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (!notation || !notation->undoStack() || !notation->interaction() || !notation->elements())
    {
        return;
    }

    mu::engraving::Score* score = notation->elements()->msScore();
    if (!score)
    {
        return;
    }

    //! Resolve the Nth measure positionally (pickup = 0), the same convention as jumpTo / the scan / the proofing map.
    mu::engraving::Measure* measure = score->firstMeasure();
    for (int32_t i = 0; i < entry.measureIndex0 && measure; ++i)
    {
        measure = measure->nextMeasure();
    }

    if (!measure)
    {
        return;
    }

    //! FINAL live re-check against the real measure (m_canRegularize can be a frame stale between the click and here), so
    //! adjustToLen is reached ONLY while the bar is genuinely under-full -> it can only EXTEND, never shorten/delete.
    if (!(measure->ticks() < measure->timesig()))
    {
        return;
    }

    //! Extend the bar to its nominal time signature. adjustToLen(appendRestsIfNecessary = true) fills the freed beats with
    //! RESTS -- we never guess the missing note; the user replaces the rest. One undoable step, the exact mechanism (and
    //! undo-stack wrapping) MeasurePropertiesDialog uses (src/notationscene/widgets/measureproperties.cpp).
    notation->undoStack()->prepareChanges(muse::TranslatableString("reprise", "Regularize measure"));
    measure->adjustToLen(measure->timesig());
    notation->undoStack()->commitChanges();

    //! Select the whole regularized measure so the proofing view lands on it. commitChanges fires stackChanged, which
    //! re-walks this model: the now-correct bar drops out of the worklist and its red highlight clears automatically.
    notation->interaction()->select({ measure }, mu::notation::SelectType::SINGLE);
    notation->interaction()->showItem(measure);

    //! Warp the cursor onto the just-regularized measure in the editor (deferred so it lands on the settled scroll from the
    //! showItem above), the same courtesy as a navigator double-click. Scheduled by the captured measure index, not the row.
    //! FLASH cue: the measure is no longer a problem (its red overlay has cleared), so the warp lands with a transient
    //! accent-blue flash box marking the just-fixed bar rather than pulsing a (now-absent) red overlay.
    scheduleWarp(targetMeasureIndex0, WarpCue::Flash);

    return;
}

void BadTimingNavigatorModel::warpCursorToMeasure(int row)
{
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
    {
        return;
    }

    //! Resolve the row to its positional measure index immediately (while the row is valid) and schedule the deferred warp.
    //! PULSE cue: a double-click targets a measure that is still a problem, so the warp lands by pulsing its red overlay.
    scheduleWarp(m_entries[static_cast<size_t>(row)].measureIndex0, WarpCue::Pulse);

    return;
}

void BadTimingNavigatorModel::warpCursorToMeasureIndex(int globalMeasureIndex)
{
    if (globalMeasureIndex < 0)
    {
        return;
    }

    //! Navigate-to-a-problem semantics (the 0.18.0-redesign hook): PULSE the measure's red overlay like a double-click.
    scheduleWarp(static_cast<int32_t>(globalMeasureIndex), WarpCue::Pulse);

    return;
}

void BadTimingNavigatorModel::scheduleWarp(int32_t measureIndex0, WarpCue cue)
{
    m_pendingWarpMeasureIndex0 = measureIndex0;
    m_pendingWarpCue = cue;

    if (!m_warpTimer)
    {
        m_warpTimer = new QTimer(this);
        m_warpTimer->setSingleShot(true);
        connect(m_warpTimer, &QTimer::timeout, this, [this]()
        {
            performWarp();
        });
    }

    //! Short settle delay: showItem's scroll + the model select are synchronous (the matrix is already final when the caller
    //! returns), but a small deferral lets the click handler unwind and any queued relayout/repaint settle before we read the
    //! measure's on-screen rect. performWarp recomputes the geometry at fire time, so the warp uses the settled position.
    m_warpTimer->start(60);

    return;
}

void BadTimingNavigatorModel::performWarp()
{
    const int32_t measureIndex0 = m_pendingWarpMeasureIndex0;
    const WarpCue cue = m_pendingWarpCue;
    m_pendingWarpMeasureIndex0 = -1;
    m_pendingWarpCue = WarpCue::None;

    if (measureIndex0 < 0)
    {
        return;
    }

    //! The registered editor paint view (notationscene AbstractNotationPaintView), reached as a bare QObject* through the
    //! proofing bridge so this module needs no notationscene dependency. Absent (not yet registered / page torn down) -> skip.
    QObject* viewObject = proofingViewControl() ? proofingViewControl()->editorView() : nullptr;
    QQuickItem* view = qobject_cast<QQuickItem*>(viewObject);
    if (!view || !view->isVisible())
    {
        return;
    }

    const qreal viewW = view->width();
    const qreal viewH = view->height();
    if (viewW <= 0.0 || viewH <= 0.0)
    {
        return;
    }

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    mu::engraving::Score* score = (notation && notation->elements()) ? notation->elements()->msScore() : nullptr;
    if (!score)
    {
        return;
    }

    //! Resolve the Nth measure positionally (pickup = 0), the same convention as jumpTo / regularize / the scan.
    mu::engraving::Measure* measure = score->firstMeasure();
    for (int32_t i = 0; i < measureIndex0 && measure; ++i)
    {
        measure = measure->nextMeasure();
    }

    if (!measure)
    {
        return;
    }

    //! The measure's canvas (LOGICAL) bounding rect -- the same rect the editor red-highlight overlay maps. Not laid out
    //! yet / empty -> nothing to aim at, skip rather than warp to a garbage point.
    const muse::RectF canvasRect = measure->canvasBoundingRect();
    const QRectF logicalRect(canvasRect.x(), canvasRect.y(), canvasRect.width(), canvasRect.height());
    if (logicalRect.isEmpty())
    {
        return;
    }

    //! Map LOGICAL -> view px via the paint view's Q_INVOKABLE fromLogicalRect (meta-call: no concrete AbstractNotationPaintView
    //! type needed here, mirroring how the set-zoom handler drives the models through the Qt meta-object system).
    QRectF viewRect;
    const bool mapped = QMetaObject::invokeMethod(view, "fromLogicalRect", Qt::DirectConnection,
                                                  Q_RETURN_ARG(QRectF, viewRect), Q_ARG(QRectF, logicalRect));
    if (!mapped)
    {
        return;
    }

    //! Aim at the measure centre, CLAMPED into the editor viewport (with a small inset) so a measure that showItem left only
    //! partially on-screen still warps to a point inside the editor, never off it.
    constexpr qreal inset = 6.0;
    QPointF centerViewPx = viewRect.center();
    centerViewPx.setX(std::clamp(centerViewPx.x(), inset, viewW - inset));
    centerViewPx.setY(std::clamp(centerViewPx.y(), inset, viewH - inset));

    //! View px -> GLOBAL screen coords (QQuickItem::mapToGlobal is multi-monitor aware), then warp the OS cursor. The real
    //! cursor landing over the bar fires the editor's hover-follow, which re-syncs the band + magnifier to the measure.
    const QPointF globalPos = view->mapToGlobal(centerViewPx);
    QCursor::setPos(globalPos.toPoint());

    //! FOCUS CUE: now that the pointer has landed on the measure, ask the editor-side cue sink (ProblemMeasureHighlightModel,
    //! which owns the editor overlays) to draw the cue -- synchronously here, before the warp's own hover-move event is
    //! processed, so the cue's target selection/overlay state is the settled one we just warped to. PULSE modulates the still-
    //! present red problem overlay (double-click); FLASH latches the just-fixed measure's rect and shows a transient accent-blue
    //! box (post-regularize). Reached by meta-call through the bridge (no concrete type), mirroring the fromLogicalRect idiom above.
    if (cue != WarpCue::None)
    {
        QObject* cueSink = proofingViewControl() ? proofingViewControl()->editorCueSink() : nullptr;
        if (cueSink)
        {
            if (cue == WarpCue::Pulse)
            {
                QMetaObject::invokeMethod(cueSink, "requestPulse", Qt::DirectConnection);
            }
            else if (cue == WarpCue::Flash)
            {
                QMetaObject::invokeMethod(cueSink, "requestFlashForSelection", Qt::DirectConnection);
            }
        }
    }

    return;
}

int BadTimingNavigatorModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
    {
        return 0;
    }

    return static_cast<int>(m_entries.size());
}

QVariant BadTimingNavigatorModel::data(const QModelIndex& index, int role) const
{
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
    {
        return QVariant();
    }

    const BadTimingProblem& e = m_entries[static_cast<size_t>(row)];

    switch (role)
    {
    case DisplayRole:
        return rowText(e);
    case MeasureNumberRole:
        return e.measureNumber1;
    case StaffRole:
        //! staffIdx < 0 marks a measure-level entry (WrongLength) -> show the staff cell as an em dash.
        return e.staffIdx < 0 ? QVariant(QString::fromUtf8("\xE2\x80\x94")) : QVariant(e.staffIdx + 1);
    case KindRole:
        return kindLabel(e.kind);
    case FoundRole:
        return e.found.toString().toQString();
    case ExpectedRole:
        return e.expected.toString().toQString();
    case AccentRole:
        return kindAccent(e.kind);
    case DetailRole:
        return detailText(e);
    case CanRegularizeRole:
        return row < static_cast<int>(m_canRegularize.size()) ? m_canRegularize[static_cast<size_t>(row)] : false;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> BadTimingNavigatorModel::roleNames() const
{
    return {
        { DisplayRole, "display" },
        { MeasureNumberRole, "measureNumber" },
        { StaffRole, "staffNumber" },
        { KindRole, "kindLabel" },
        { FoundRole, "found" },
        { ExpectedRole, "expected" },
        { AccentRole, "accentColor" },
        { DetailRole, "detail" },
        { CanRegularizeRole, "canRegularize" }
    };
}

QString BadTimingNavigatorModel::rowText(const BadTimingProblem& e) const
{
    //! Measure-level entry (staffIdx < 0, e.g. WrongLength): no staff column -- length is a property of the whole bar. The
    //! visible row is PLAIN ("Too many/few beats"); the exact ratio is the hover tooltip (detailText), never on the row.
    if (e.staffIdx < 0)
    {
        //: Measure-level bad-timing navigator row, e.g. "m. 37  —  Too many beats" (%1 = measure number, %2 = plain problem).
        return muse::qtrc("reprise", "m. %1  \xE2\x80\x94  %2")
               .arg(e.measureNumber1)
               .arg(plainKindLabel(e));
    }

    //: Bad-timing navigator row, e.g. "m. 5  staff 1  —  Too few beats" (%1 = measure number, %2 = staff number,
    //: %3 = plain problem).
    return muse::qtrc("reprise", "m. %1  staff %2  \xE2\x80\x94  %3")
           .arg(e.measureNumber1)
           .arg(e.staffIdx + 1)
           .arg(plainKindLabel(e));
}

QString BadTimingNavigatorModel::detailText(const BadTimingProblem& e) const
{
    //: Hover tooltip for a bad-timing row -- the exact technical kind + duration ratio, e.g. "Wrong length  138/128 of 4/4"
    //: (%1 = problem kind, %2 = found duration, %3 = expected duration). Kept OFF the visible row (the nominal can be a wrong
    //: OMR read), surfaced only on hover for a power user.
    return muse::qtrc("reprise", "%1  %2 of %3")
           .arg(kindLabel(e.kind))
           .arg(e.found.toString().toQString(), e.expected.toString().toQString());
}

QString BadTimingNavigatorModel::plainKindLabel(const BadTimingProblem& e) const
{
    switch (e.kind)
    {
    case BadTimingKind::CorruptedRepeat:
        return muse::qtrc("reprise", "Repeat problem");
    case BadTimingKind::Incomplete:
        //! voice 1 does not fill the bar -- underfull.
        return muse::qtrc("reprise", "Too few beats");
    case BadTimingKind::VoiceTooLong:
        //! a voice runs past the bar -- overfull.
        return muse::qtrc("reprise", "Too many beats");
    case BadTimingKind::WrongLength:
        //! The bar's actual length disagrees with its nominal: longer than nominal -> too many beats, shorter -> too few.
        return e.found > e.expected ? muse::qtrc("reprise", "Too many beats")
                                    : muse::qtrc("reprise", "Too few beats");
    }

    return muse::qtrc("reprise", "Timing problem");
}

QString BadTimingNavigatorModel::kindLabel(BadTimingKind kind) const
{
    switch (kind)
    {
    case BadTimingKind::Incomplete:
        return muse::qtrc("reprise", "Incomplete");
    case BadTimingKind::VoiceTooLong:
        return muse::qtrc("reprise", "Voice too long");
    case BadTimingKind::CorruptedRepeat:
        return muse::qtrc("reprise", "Corrupted repeat");
    case BadTimingKind::WrongLength:
        return muse::qtrc("reprise", "Wrong length");
    }

    return QString();
}

QString BadTimingNavigatorModel::kindAccent(BadTimingKind kind) const
{
    switch (kind)
    {
    case BadTimingKind::Incomplete:
        return QStringLiteral("#d08b1f");       //! amber -- a bar that is short
    case BadTimingKind::VoiceTooLong:
        return QStringLiteral("#e06c2e");       //! orange -- a voice runs past the bar
    case BadTimingKind::CorruptedRepeat:
        return QStringLiteral("#c0392b");       //! red -- a structural corruption
    case BadTimingKind::WrongLength:
        return QStringLiteral("#7a52c7");       //! violet -- the bar length itself disagrees with the time signature
    }

    return QStringLiteral("#d08b1f");
}
