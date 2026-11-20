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
#include <string>
#include <vector>

#include "modularity/imoduleinterface.h"

namespace mu::reprise {
//! One recognised page (sheet) of the original scan: its 0-based index, the Audiveris sheet-image pixel dimensions
//! (the space every measure rect is expressed in), and the DPI the sheet was rasterised / recognised at.
struct ProofingPageInfo
{
    int32_t index = 0;          //! 0-based sheet index (sheet #1 -> 0)
    int32_t imageWidth = 0;     //! sheet-image width in pixels (Audiveris <picture width>)
    int32_t imageHeight = 0;    //! sheet-image height in pixels (Audiveris <picture height>)
    int32_t dpi = 0;            //! the import/raster DPI (0 when unknown)
};

//! The pixel bounding box of ONE measure column on its scan page. x/y/w/h are in that page's sheet-image pixel space
//! (ProofingPageInfo::imageWidth x imageHeight). The box spans the measure column's x-bounds and the full vertical
//! extent of every staff in the measure's system (so a grand-staff measure covers both staves).
struct ProofingMeasureRect
{
    int32_t globalMeasureIndex = 0;   //! 0-based running index across all pages (anacrusis/pickup is index 0)
    int32_t pageIndex = 0;            //! 0-based page this measure sits on (ProofingPageInfo::index)
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
};

//! The complete scan<->score coordinate map captured at OMR import time (Reprise Two-Pane Proofing, Phase A).
//! Built from the Audiveris .omr book and keyed by GLOBAL MEASURE INDEX (positional, 0-based) so the top strip and
//! (later) the magnifier can line the original scan up with wherever the user is in the re-engraved score.
struct ProofingMap
{
    bool valid = false;                          //! true once a map has been built and set
    std::string sourcePath;                      //! the original scanned PDF / image the score was recognised from
    int32_t measureCount = 0;                    //! number of measures in the map (== measures.size())
    std::vector<ProofingPageInfo> pages;         //! per-page image dimensions + DPI, 0-based by index
    std::vector<ProofingMeasureRect> measures;   //! one rect per measure, in global measure order
};

//! NOTE: Single app-lifetime holder of the LATEST import's proofing coordinate map, exported globally. The import
//! flow (ImportProgressModel) builds the map from the Audiveris .omr the instant recognition succeeds -- while the
//! temp dir still exists -- and sets it here; the read-only reprise.get-proofing-map MCP query and (later, in Phase
//! A2/A3) the top strip + follow loop read it back. A1 holds exactly one current map (the last import); score-scoped
//! association + persistence are Phase C (D7).
class IProofingMapService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IProofingMapService)

public:
    virtual ~IProofingMapService() = default;

    //! Store the captured map (marks it valid). Replaces any previous map (one current map at a time in Phase A).
    virtual void setMap(const ProofingMap& map) = 0;

    //! Whether a valid map is currently held.
    virtual bool hasMap() const = 0;

    //! The current map (valid == false when none has been set).
    virtual const ProofingMap& map() const = 0;

    //! Drop the current map (e.g. an import that produced no usable geometry).
    virtual void clear() = 0;
};
}
