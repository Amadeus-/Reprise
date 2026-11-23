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

    m_entries.clear();
    m_hasScore = (score != nullptr && score->firstMeasure() != nullptr);

    if (score)
    {
        int32_t measureIndex0 = 0;
        for (const mu::engraving::Measure* m = score->firstMeasure(); m; m = m->nextMeasure())
        {
            scanMeasure(m, measureIndex0);
            ++measureIndex0;
        }
    }

    endResetModel();

    emit countChanged();

    return;
}

void BadTimingNavigatorModel::scanMeasure(const mu::engraving::Measure* measure, int32_t measureIndex0)
{
    //! Replicates Score::sanityCheckLocal (check.cpp) READ-ONLY: sum each voice's actual ticks per staff and compare against
    //! the measure length. NO mutation (the real check rewrites full-measure-rest ticks + corrupted flags -- forbidden here).
    //! This MUST match sanityCheck exactly, because the navigator stands in for the corruption dialog it suppresses (Part F):
    //! anything sanityCheck flags, the dialog would have reported, so the navigator has to surface it too or information is
    //! lost. NO pickup/irregular (ticks != timesig) exclusion: sanityCheck does NOT exclude those, and it already passes a
    //! correctly-filled pickup naturally (voices[0] == its own short ticks, so the Incomplete test below does not fire). A
    //! blanket ticks-!=-timesig skip would instead HIDE real corruptions that live in irregular measures -- e.g. hardtimes-a4
    //! measure 37, whose ticks (138/128) are irregular yet whose voice 3 overflows (1109/1024 > 1104/1024): a true problem.
    const mu::engraving::Fraction mLen = measure->ticks();

    const size_t endStaff = measure->score()->staves().size();
    const int32_t measureNumber1 = measureIndex0 + 1;

    for (size_t staffIdx = 0; staffIdx < endStaff; ++staffIdx)
    {
        mu::engraving::Fraction voices[mu::engraving::VOICES];

        for (const mu::engraving::Segment* s = measure->first(mu::engraving::SegmentType::ChordRest); s;
             s = s->next(mu::engraving::SegmentType::ChordRest))
        {
            for (mu::engraving::voice_idx_t v = 0; v < mu::engraving::VOICES; ++v)
            {
                mu::engraving::EngravingItem* element = s->element(staffIdx * mu::engraving::VOICES + v);
                if (!element)
                {
                    continue;
                }

                voices[v] += mu::engraving::toChordRest(element)->actualTicks();
            }
        }

        //! Corrupted measure-repeat group (the repeat element went missing) -- a genuine corruption worth surfacing.
        if (measure->isMeasureRepeatGroup(staffIdx) && measure->measureRepeatElement(staffIdx) == nullptr)
        {
            BadTimingEntry e;
            e.measureIndex0 = measureIndex0;
            e.measureNumber1 = measureNumber1;
            e.staffIdx = static_cast<int32_t>(staffIdx);
            e.kind = Kind::CorruptedRepeat;
            e.found = voices[0].toString().toQString();
            e.expected = mLen.toString().toQString();
            m_entries.push_back(e);
        }

        //! Incomplete: voice 1 does not fill the measure.
        if (voices[0] != mLen)
        {
            BadTimingEntry e;
            e.measureIndex0 = measureIndex0;
            e.measureNumber1 = measureNumber1;
            e.staffIdx = static_cast<int32_t>(staffIdx);
            e.kind = Kind::Incomplete;
            e.found = voices[0].toString().toQString();
            e.expected = mLen.toString().toQString();
            m_entries.push_back(e);
        }

        //! Voice too long: a voice beyond the first holds more than the measure length.
        for (mu::engraving::voice_idx_t v = 1; v < mu::engraving::VOICES; ++v)
        {
            if (voices[v] > mLen)
            {
                BadTimingEntry e;
                e.measureIndex0 = measureIndex0;
                e.measureNumber1 = measureNumber1;
                e.staffIdx = static_cast<int32_t>(staffIdx);
                e.kind = Kind::VoiceTooLong;
                e.voice = static_cast<int32_t>(v);
                e.found = voices[v].toString().toQString();
                e.expected = mLen.toString().toQString();
                m_entries.push_back(e);
            }
        }
    }

    return;
}

void BadTimingNavigatorModel::jumpTo(int row)
{
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
    {
        return;
    }

    const BadTimingEntry& entry = m_entries[static_cast<size_t>(row)];

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

    const mu::engraving::staff_idx_t staffIdx = static_cast<mu::engraving::staff_idx_t>(entry.staffIdx);

    //! Prefer the first chord/rest of the problem staff so the selection (and thus the magnifier's note follow) lands on
    //! that staff; fall back to any chord/rest in the measure, then to the measure frame itself.
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

    notation->interaction()->select({ target }, mu::notation::SelectType::REPLACE, staffIdx);
    notation->interaction()->showItem(target);

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

    const BadTimingEntry& e = m_entries[static_cast<size_t>(row)];

    switch (role)
    {
    case DisplayRole:
        return rowText(e);
    case MeasureNumberRole:
        return e.measureNumber1;
    case StaffRole:
        return e.staffIdx + 1;
    case KindRole:
        return kindLabel(e.kind);
    case FoundRole:
        return e.found;
    case ExpectedRole:
        return e.expected;
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
        { ExpectedRole, "expected" }
    };
}

QString BadTimingNavigatorModel::rowText(const BadTimingEntry& e) const
{
    //: Bad-timing navigator row, e.g. "m. 5  staff 1  Incomplete  3/4 of 4/4" (%1 = measure number, %2 = staff number,
    //: %3 = problem kind, %4 = found duration, %5 = expected duration).
    return muse::qtrc("reprise", "m. %1  staff %2  %3  %4 of %5")
           .arg(e.measureNumber1)
           .arg(e.staffIdx + 1)
           .arg(kindLabel(e.kind))
           .arg(e.found, e.expected);
}

QString BadTimingNavigatorModel::kindLabel(Kind kind) const
{
    switch (kind)
    {
    case Kind::Incomplete:
        return muse::qtrc("reprise", "Incomplete");
    case Kind::VoiceTooLong:
        return muse::qtrc("reprise", "Voice too long");
    case Kind::CorruptedRepeat:
        return muse::qtrc("reprise", "Corrupted repeat");
    }

    return QString();
}
