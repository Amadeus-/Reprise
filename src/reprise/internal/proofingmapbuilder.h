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

#include <QString>

#include "iproofingmapservice.h"

namespace mu::reprise {
//! Builds the Reprise Two-Pane Proofing coordinate map (ProofingMap) from an Audiveris .omr book. The .omr is a ZIP of
//! book.xml + per-sheet sheet#N/sheet#N.xml; each sheet xml carries <picture width height> (the sheet-image pixel size)
//! and, per <system>, a sequence of <stack left right ...> measure columns plus <part><staff><lines> staff-line points.
//! A measure's rect is x=[stack.left, stack.right], y-span = the full vertical extent of every staff in that system.
//! The GLOBAL MEASURE INDEX is assigned POSITIONALLY (sheet 1->N, within a sheet system top->bottom, within a system
//! stacks left->right, which is document order) -- NOT from the per-sheet stack id -- so it matches the score's measure
//! order and includes the pickup/anacrusis as index 0. See memory reference_reprise_omr_coordinate_map_sources.
class ProofingMapBuilder
{
public:
    //! Parse the .omr at omrPath and return the coordinate map. sourcePath = the original scanned PDF/image (stored in
    //! the map, not read here); dpi = the import/raster resolution stamped on every page. Returns a map with valid=false
    //! on ANY failure (unreadable .omr, no sheets, parse error) -- capture is best-effort and never blocks the import.
    static ProofingMap buildFromOmr(const QString& omrPath, const QString& sourcePath, int32_t dpi);

    //! Dump the whole map to the log (header line + one line per page + one line per measure), for the Phase A1
    //! verification gate. The per-measure line format is exactly: "proofing-map: gm=<i> page=<p> rect=<x>,<y>,<w>,<h>".
    static void logMap(const ProofingMap& map);
};
}
