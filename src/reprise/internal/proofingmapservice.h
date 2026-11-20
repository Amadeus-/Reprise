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
#include <unordered_map>

#include "iproofingmapservice.h"

namespace mu::reprise {
//! NOTE: Trivial in-memory holder for the latest import's proofing coordinate map. All access is on the Qt UI thread
//! (the import flow writes it as recognition finishes; the command handler + the proofing UI read it), so no locking
//! is needed. Mirrors ImportProgressTracker's single-owner app-lifetime pattern.
class ProofingMapService : public IProofingMapService
{
public:
    ProofingMapService() = default;

    void setMap(const ProofingMap& map) override
    {
        m_map = map;
        m_map.valid = true;
        rebuildNoteIndex();
        m_mapChanged.notify();
    }

    bool hasMap() const override
    {
        return m_map.valid;
    }

    const ProofingMap& map() const override
    {
        return m_map;
    }

    void clear() override
    {
        m_map = ProofingMap();
        m_noteIndex.clear();
        m_mapChanged.notify();
    }

    muse::async::Notification mapChanged() const override
    {
        return m_mapChanged;
    }

    const ProofingNoteBox* noteBox(int32_t gm, int32_t staffIdx, int32_t tickInMeasure, int32_t step,
                                   int32_t octave) const override
    {
        const auto it = m_noteIndex.find(packNoteKey(gm, staffIdx, tickInMeasure, step, octave));
        if (it == m_noteIndex.end())
        {
            return nullptr;
        }

        return &m_map.notes[it->second];
    }

private:
    //! Pack the match tuple (gm, staffIdx, tickInMeasure, step, octave) into one 64-bit key. Field widths are generous
    //! for any real score (gm < 2^20, staff < 2^8, tick < 2^16 -- one 4/4 bar is 1920 ticks, step 0..6, octave biased by
    //! +8 into 0..15). `voice` is intentionally excluded so B2 need not reproduce MuseScore's voice assignment.
    static uint64_t packNoteKey(int32_t gm, int32_t staffIdx, int32_t tickInMeasure, int32_t step, int32_t octave)
    {
        const uint64_t g = static_cast<uint64_t>(gm & 0xFFFFF);
        const uint64_t s = static_cast<uint64_t>(staffIdx & 0xFF);
        const uint64_t t = static_cast<uint64_t>(tickInMeasure & 0xFFFF);
        const uint64_t p = static_cast<uint64_t>(step & 0xF);
        const uint64_t o = static_cast<uint64_t>((octave + 8) & 0xF);

        return (g << 32) | (s << 24) | (t << 8) | (p << 4) | o;
    }

    void rebuildNoteIndex()
    {
        m_noteIndex.clear();
        m_noteIndex.reserve(m_map.notes.size() * 2 + 1);
        for (size_t i = 0; i < m_map.notes.size(); ++i)
        {
            const ProofingNoteKey& k = m_map.notes[i].key;
            m_noteIndex[packNoteKey(k.globalMeasureIndex, k.staffIdx, k.tickInMeasure, k.step, k.octave)]
                = static_cast<int32_t>(i);
        }
    }

    ProofingMap m_map;
    std::unordered_map<uint64_t, int32_t> m_noteIndex;
    muse::async::Notification m_mapChanged;
};
}
