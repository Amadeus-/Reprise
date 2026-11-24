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
#include "async/notification.h"

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

//! A RUNTIME-COMPUTABLE identity for one recognised NOTE (Reprise Two-Pane Proofing, Phase B). B2's floating magnifier
//! computes this key from the live SELECTED engraving Note and asks the service for its head box to zoom + highlight.
//! Every field is derivable at runtime from a mu::engraving::Note:
//!   globalMeasureIndex == note->chord()->measure()->measureIndex()         (positional, pickup = 0; same as the rects)
//!   staffIdx           == note->staffIdx()                                  (global 0-based staff index)
//!   tickInMeasure      == (note->tick() - measure->tick()).ticks()         (MuseScore 480-per-quarter ticks)
//!   step / octave      == tpc2step(note->tpc()) / note->octave()           (WRITTEN diatonic pitch; accidental IGNORED)
//!   voice              == note->voice()                                     (ADVISORY ONLY -- see noteBox())
//! The match is on (globalMeasureIndex, staffIdx, tickInMeasure, step, octave); that tuple is unique per note (two
//! distinct pitches at one tick differ in step/octave; an exact dup would be the same pixel head anyway).
struct ProofingNoteKey
{
    int32_t globalMeasureIndex = 0;   //! 0-based positional global measure index (pickup/anacrusis = 0)
    int32_t staffIdx = 0;             //! global 0-based staff index across the whole score
    int32_t tickInMeasure = 0;        //! ticks from the measure start (MuseScore division = 480 per quarter)
    int32_t step = 0;                 //! diatonic step 0..6 == C,D,E,F,G,A,B (letter only; alter/accidental ignored)
    int32_t octave = 0;               //! scientific octave (middle C = C4 = octave 4)
    int32_t voice = 0;                //! 0-based engraving voice -- carried for B2/diagnostics, NOT part of the match
};

//! The pixel head box of ONE recognised note on its scan page, keyed by a runtime-computable note identity. x/y/w/h are
//! the Audiveris <head> <bounds> in the SAME sheet-image pixel space as ProofingMeasureRect (so a note box always lies
//! within its measure's rect). A note with NO trusted head (grace note -- Audiveris drops graces; or a head Audiveris
//! never recognised) gets NO entry here at all; B2 then falls back to the parent chord's / measure's box.
struct ProofingNoteBox
{
    ProofingNoteKey key;
    int32_t pageIndex = 0;            //! 0-based page this head sits on (ProofingPageInfo::index)
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
};

//! The complete scan<->score coordinate map captured at OMR import time (Reprise Two-Pane Proofing, Phase A + B).
//! Built from the Audiveris .omr book and keyed by GLOBAL MEASURE INDEX (positional, 0-based) so the top strip and
//! (later) the magnifier can line the original scan up with wherever the user is in the re-engraved score. Phase B1
//! adds `notes`: a per-NOTE head box keyed by a runtime-computable identity (ProofingNoteKey) matched from the same
//! .omr plus the pre-strip Audiveris MusicXML (the authoritative note order).
struct ProofingMap
{
    bool valid = false;                          //! true once a map has been built and set
    std::string sourcePath;                      //! the original scanned PDF / image the score was recognised from
    int32_t measureCount = 0;                    //! number of measures in the map (== measures.size())
    std::vector<ProofingPageInfo> pages;         //! per-page image dimensions + DPI, 0-based by index
    std::vector<ProofingMeasureRect> measures;   //! one rect per measure, in global measure order
    std::vector<ProofingNoteBox> notes;          //! one head box per matched note (unmatched/grace notes omitted)
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

    //! The per-note head box for a runtime note identity, or nullptr when none is held (grace / untrusted / unmatched
    //! note, or no map) -- B2 then falls back to the measure box. The match uses (gm, staffIdx, tickInMeasure, step,
    //! octave); `voice` is deliberately NOT part of the lookup (it is advisory on the stored key). The returned pointer
    //! is valid until the next setMap()/clear().
    virtual const ProofingNoteBox* noteBox(int32_t gm, int32_t staffIdx, int32_t tickInMeasure, int32_t step,
                                            int32_t octave) const = 0;

    //! Drop the current map (e.g. an import that produced no usable geometry).
    virtual void clear() = 0;

    //! Reprise: tell the service which document just became the loaded project, so a stale map from a PREVIOUS document is
    //! never shown over a newly-opened one. Called at the end of every successful project load. Ownership model: each
    //! setMap() arms a one-shot "claim" -- the FIRST onDocumentLoaded() after it binds the held map to that document
    //! (so an OMR import, which sets the map just before opening its MusicXML, and a .repr reopen both keep their map).
    //! A later onDocumentLoaded() for a DIFFERENT document finds no pending claim and a foreign owner, so it drops the
    //! map (fires mapChanged -> the band + magnifier reset to the empty "import a scan" state). Opening a plain score, a
    //! non-OMR import, or a .repr with no map therefore clears the previous document's scan instead of leaving it on screen.
    virtual void onDocumentLoaded(const std::string& documentPath) = 0;

    //! Fires whenever the held map is replaced (setMap) or dropped (clear). The Two-Pane Proofing top strip
    //! (Phase A2) subscribes so it renders/refreshes/hides the moment a fresh import's map arrives, with no
    //! dependency on notation-change timing.
    virtual muse::async::Notification mapChanged() const = 0;
};
}
