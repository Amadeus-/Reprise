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
    //! the map, not read here); dpi = the import/raster resolution stamped on every page. When rawMusicXmlPath is given
    //! (the pre-strip Audiveris MusicXML, .mxl or .xml), the per-NOTE head boxes (ProofingMap::notes) are ALSO built, by
    //! matching each pitched MusicXML note to its Audiveris <head> pixel box (Phase B1; see buildNoteBoxes). Returns a
    //! map with valid=false on ANY failure of the measure-rect parse (unreadable .omr, no sheets); the note-box build is
    //! independently best-effort (an empty notes vector on failure) and never affects valid. Capture never blocks import.
    static ProofingMap buildFromOmr(const QString& omrPath, const QString& sourcePath, int32_t dpi,
                                    const QString& rawMusicXmlPath = QString());

    //! Build ProofingMap::notes: one head box per recognised note, keyed by a runtime-computable identity. The .omr
    //! supplies per-<head> pixel bounds + the chord/voice/measure structure; the pre-strip Audiveris MusicXML supplies
    //! the authoritative note order, pitch, voice, part/staff layout and in-measure tick (the ScanGeometryStripper
    //! preserves every musical value byte-for-byte, so this order == the opened score's). Each note is joined to a head
    //! DIATONICALLY (step+octave, accidental ignored) within its (global-measure, part, voice) cell, trying the part's
    //! staff clefs for hand-crossing, consuming same-pitch duplicates in onset order. Grace notes and notes with no
    //! trusted head get no box. Appends to map.notes; leaves it empty on any failure. See the validated matcher in
    //! memory reference_reprise_omr_coordinate_map_sources.
    static void buildNoteBoxes(const QString& omrPath, const QString& rawMusicXmlPath, ProofingMap& map);

    //! Dump the whole map to the log (header line + one line per page + one line per measure + one line per note), for
    //! the verification gate. The per-measure line is exactly: "proofing-map: gm=<i> page=<p> rect=<x>,<y>,<w>,<h>"; the
    //! per-note line: "proofing-note: gm=<i> staff=<s> tick=<t> v=<v> pitch=<step><oct> page=<p> box=<x>,<y>,<w>,<h>".
    static void logMap(const ProofingMap& map);
};
}
