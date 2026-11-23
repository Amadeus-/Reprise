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
#include <vector>

#include "engraving/types/fraction.h"

namespace mu::engraving {
class Score;
}

namespace mu::reprise {
//! The kind of timing problem a measure x staff exhibits. Mirrors the branches of Score::sanityCheckLocal
//! (src/engraving/dom/check.cpp) that the Reprise read-only bad-timing walk replicates.
enum class BadTimingKind : int32_t
{
    Incomplete = 0,     //! voice 1 (voices[0]) does not fill the measure length
    VoiceTooLong,       //! a voice > 1 holds more than the measure length
    CorruptedRepeat     //! a measure-repeat group whose repeat element is missing
};

//! One problem measure x staff. measureIndex0 is the POSITIONAL 0-based index (pickup = 0), matching the proofing
//! map's globalMeasureIndex and the magnifier header, so a click lines the whole proofing view up. found / expected
//! are the measured vs. expected durations (rendered as fraction strings by the navigator model).
struct BadTimingProblem
{
    int32_t measureIndex0 = 0;
    int32_t measureNumber1 = 0;
    int32_t staffIdx = 0;
    BadTimingKind kind = BadTimingKind::Incomplete;
    int32_t voice = 0;
    mu::engraving::Fraction found;
    mu::engraving::Fraction expected;
};

//! THE single source of truth for "which measures have bad timing". A READ-ONLY walk of the whole score that
//! replicates Score::sanityCheckLocal (check.cpp) WITHOUT mutation (the real sanity check rewrites full-measure-rest
//! ticks + per-measure corrupted flags, so it must never be called for this). Safe with a null score (returns empty).
//!
//! Consumed by the navigator model (BadTimingNavigatorModel): it renders one worklist row per entry, and the row count
//! drives whether the host magnifier panel shows or collapses the "Timing problems" section. Driving both the rows and
//! the section's visibility from ONE walk means they can never disagree (no "section shown but empty").
std::vector<BadTimingProblem> scanScoreForBadTiming(const mu::engraving::Score* score);
}
