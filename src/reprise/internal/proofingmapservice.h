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

        //! Arm the one-shot ownership claim: the next onDocumentLoaded() binds this map to the document that is opening.
        m_claimPending = true;
        m_ownerPath.clear();

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
        m_claimPending = false;
        m_ownerPath.clear();
        m_mapChanged.notify();
    }

    void onDocumentLoaded(const std::string& documentPath) override
    {
        //! A map set just before this load (OMR import / .repr reopen) claims ownership of the document now loading.
        if (m_claimPending)
        {
            m_ownerPath = documentPath;
            m_claimPending = false;

            return;
        }

        //! No fresh map for this load: if a map is still held from a DIFFERENT document, it is stale -> drop it so the
        //! band + magnifier reset to the empty state rather than showing the previous document's scan.
        if (m_map.valid && m_ownerPath != documentPath)
        {
            clear();
        }

        return;
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

    //! Document-ownership for the held map (see onDocumentLoaded): m_claimPending is armed by setMap() and consumed by the
    //! first following load, which records that document's path in m_ownerPath. A later load of any other document drops it.
    bool m_claimPending = false;
    std::string m_ownerPath;
};
}
