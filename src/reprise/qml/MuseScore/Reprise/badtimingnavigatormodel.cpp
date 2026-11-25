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
