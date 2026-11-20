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
    }

private:
    ProofingMap m_map;
};
}
