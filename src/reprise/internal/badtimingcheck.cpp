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
//! Is this irregular (ticks != timesig) measure a LEGIT short bar that must NOT be flagged as "wrong length"?
//! Two cases, both using MuseScore's own semantics so a real pickup is never surfaced:
//!   1. A leading pickup / section anacrusis -- Measure::isAnacrusis() is true for a short measure that opens the score
//!      or follows a frame/break. This is MuseScore's definition of a pickup, so it covers the title-frame case and
//!      multi-section pickups too.
//!   2. A cleanly-paired completing FINAL bar -- the last measure of the score, shortened so that pickup + final == one
//!      full bar (the classic anacrusis completion). Only excluded when that pairing is trivially provable, so a stray
//!      irregular last bar is still flagged.
static bool isExcludedIrregularMeasure(const mu::engraving::Measure* measure)
{
    if (measure->isAnacrusis())
    {
        return true;
    }

    if (measure->nextMeasure() == nullptr)
    {
        const mu::engraving::Measure* first = measure->score()->firstMeasure();
        if (first && first != measure && first->isAnacrusis()
            && (measure->ticks() + first->ticks() == measure->timesig()))
        {
            return true;
        }
    }

    return false;
}

//! Append the problem rows for one measure (replicates the sanityCheckLocal per-staff branches, NO mutation, PLUS a
//! Reprise-only measure-level WRONG-LENGTH check).
//!
//! The per-staff branches MUST match Score::sanityCheckLocal, because the navigator stands in for the corruption dialog
//! it suppresses (Part F): anything sanityCheck flags, the dialog would have reported, so the navigator has to surface
//! it too or information is lost. Those branches do NOT exclude pickups/irregular bars, and a correctly-filled pickup
//! passes them naturally (voices[0] == its own short ticks, so the Incomplete test does not fire).
//!
//! The ADDED measure-level WrongLength check (ticks != timesig) is the signal that actually catches Audiveris rhythm
//! mis-reads: those come out as INTERNALLY-CONSISTENT irregular measures (each voice fills the bar's own odd length), so
//! the per-staff checks show 0 on a fresh import even though the bar is wrong. We exclude a legit pickup/completing final
//! bar (isExcludedIrregularMeasure) so a real anacrusis is never flagged. DEDUPE: a wrong-length measure is also, by
//! definition, internally irregular (e.g. hardtimes-a4 m.37: ticks 138/128 AND a voice overflowing 1104/1024), so when
//! the WrongLength row fires we SKIP the per-staff voice rows for that same bar -- one coherent entry per measure, not a
//! confusing WrongLength + VoiceTooLong pair pointing at the same place.
static void scanMeasure(const mu::engraving::Measure* measure, int32_t measureIndex0, std::vector<BadTimingProblem>& out)
{
    const mu::engraving::Fraction mLen = measure->ticks();
    const mu::engraving::Fraction mSig = measure->timesig();
    const int32_t measureNumber1 = measureIndex0 + 1;

    //! Measure-level WRONG LENGTH: actual length != time-signature nominal. One per-measure entry (staff "—"), and it
    //! absorbs the per-staff voice rows for this bar (dedupe) so a single coherent row stands for the whole measure.
    if (mLen != mSig && !isExcludedIrregularMeasure(measure))
    {
        BadTimingProblem e;
        e.measureIndex0 = measureIndex0;
        e.measureNumber1 = measureNumber1;
        e.staffIdx = -1;
        e.kind = BadTimingKind::WrongLength;
        e.found = mLen;
        e.expected = mSig;
        out.push_back(e);

        return;
    }

    const size_t endStaff = measure->score()->staves().size();

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
