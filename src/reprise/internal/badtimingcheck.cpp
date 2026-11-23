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

#include "badtimingcheck.h"

#include "engraving/types/constants.h"
#include "engraving/types/types.h"

#include "engraving/dom/chordrest.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"

namespace mu::reprise {
//! Append the problem rows for one measure x staff (replicates the sanityCheckLocal branches, NO mutation).
//! This MUST match Score::sanityCheckLocal exactly, because the navigator stands in for the corruption dialog it
//! suppresses (Part F): anything sanityCheck flags, the dialog would have reported, so the navigator has to surface
//! it too or information is lost. NO pickup/irregular (ticks != timesig) exclusion: sanityCheck does NOT exclude
//! those, and it already passes a correctly-filled pickup naturally (voices[0] == its own short ticks, so the
//! Incomplete test below does not fire). A blanket ticks-!=-timesig skip would instead HIDE real corruptions that
//! live in irregular measures -- e.g. hardtimes-a4 measure 37, whose ticks (138/128) are irregular yet whose voice 3
//! overflows (1109/1024 > 1104/1024): a true problem.
static void scanMeasure(const mu::engraving::Measure* measure, int32_t measureIndex0, std::vector<BadTimingProblem>& out)
{
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
            BadTimingProblem e;
            e.measureIndex0 = measureIndex0;
            e.measureNumber1 = measureNumber1;
            e.staffIdx = static_cast<int32_t>(staffIdx);
            e.kind = BadTimingKind::CorruptedRepeat;
            e.found = voices[0];
            e.expected = mLen;
            out.push_back(e);
        }

        //! Incomplete: voice 1 does not fill the measure.
        if (voices[0] != mLen)
        {
            BadTimingProblem e;
            e.measureIndex0 = measureIndex0;
            e.measureNumber1 = measureNumber1;
            e.staffIdx = static_cast<int32_t>(staffIdx);
            e.kind = BadTimingKind::Incomplete;
            e.found = voices[0];
            e.expected = mLen;
            out.push_back(e);
        }

        //! Voice too long: a voice beyond the first holds more than the measure length.
        for (mu::engraving::voice_idx_t v = 1; v < mu::engraving::VOICES; ++v)
        {
            if (voices[v] > mLen)
            {
                BadTimingProblem e;
                e.measureIndex0 = measureIndex0;
                e.measureNumber1 = measureNumber1;
                e.staffIdx = static_cast<int32_t>(staffIdx);
                e.kind = BadTimingKind::VoiceTooLong;
                e.voice = static_cast<int32_t>(v);
                e.found = voices[v];
                e.expected = mLen;
                out.push_back(e);
            }
        }
    }

    return;
}

std::vector<BadTimingProblem> scanScoreForBadTiming(const mu::engraving::Score* score)
{
    std::vector<BadTimingProblem> out;

    if (!score)
    {
        return out;
    }

    int32_t measureIndex0 = 0;
    for (const mu::engraving::Measure* m = score->firstMeasure(); m; m = m->nextMeasure())
    {
        scanMeasure(m, measureIndex0, out);
        ++measureIndex0;
    }

    return out;
}
}
