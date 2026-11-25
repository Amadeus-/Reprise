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

#include <string>
#include <vector>

#include "global/types/ret.h"
#include "global/types/bytearray.h"
#include "global/io/path.h"

#include "reprise/internal/iproofingmapservice.h"
#include "reprise/internal/inavigatorstateservice.h"
#include "reprise/internal/iproofingviewstateservice.h"

namespace mu::project {
//! Reprise .repr custom project container (Phase C persistence). A .repr is a flat ZIP bundling four fixed entries:
//! the editable score (score.mscz -- a real standalone .mscz, ZIP-in-ZIP), the original scan (original.<ext>, verbatim),
//! the scan<->score proofing coordinate map (proofingmap.json, a 1:1 serialization of reprise::ProofingMap) and a
//! manifest (manifest.json: format version + import provenance). See handoffs\Reprise-ReprFormat-Design.md. These free
//! functions own all .repr (de)serialization so NotationProject's load/save branches stay thin.

//! The container suffix (no dot). Defined once here; referenced by the project load/save + the open/save dialogs.
inline const std::string REPRISE_PROJECT_SUFFIX = "repr";

//! The current .repr manifest MAJOR version. A bundle whose formatVersion exceeds this is refused on open (clear
//! message); unknown fields on the same major are ignored, so later minors can add entries without breaking old builds.
inline constexpr int REPR_FORMAT_VERSION = 1;

//! The fixed entry names inside the bundle (original.<ext> keeps the import source's extension, recorded in the manifest).
inline const std::string REPR_ENTRY_MANIFEST = "manifest.json";
inline const std::string REPR_ENTRY_SCORE = "score.mscz";
inline const std::string REPR_ENTRY_PROOFINGMAP = "proofingmap.json";
//! Optional (0.16.5+): the deskewed render Audiveris actually consumed, stored ALONGSIDE the verbatim original.<ext> so a
//! reopen renders the straightened band the serialized boxes align to. Written only when a page was deskewed; absence tolerated.
inline const std::string REPR_ENTRY_PROOFINGSCAN = "proofing-scan";
//! Optional (0.18.0+): the bad-timing navigator's per-record collapse/ignore state (a 1:1 serialization of
//! reprise::NavigatorStateMap). Written only when at least one record is non-default; absence tolerated (all records default).
inline const std::string REPR_ENTRY_NAVIGATORSTATE = "navigatorstate.json";
//! Optional (0.18.2+): the proofing per-document VIEW STATE (followed measure + band + magnifier zoom), a 1:1 serialization of
//! reprise::ProofingViewState. Absence tolerated (an older bundle, or one never navigated/zoomed) -> the models use their
//! defaults (followedMeasure -1 == first-note-on-open, band 0.5, magnifier 1.0).
inline const std::string REPR_ENTRY_VIEWSTATE = "viewstate.json";

//! Everything read back out of a .repr bundle. The score + original bytes are raw (the caller writes score.mscz to a
//! memory buffer for MscReader and extracts the original to a real temp path); map.sourcePath is left empty here and the
//! caller repoints it at the extracted scan before setMap().
struct ReprBundle
{
    muse::ByteArray scoreMscz;        //! the embedded editable score (a complete, standalone .mscz)
    muse::ByteArray original;         //! the original scan bytes, byte-for-byte
    std::string originalSuffix;       //! e.g. "pdf" (from the manifest; backfilled from the entry name if absent)
    std::string originalFileName;     //! e.g. "waltz.pdf" (from the manifest)
    std::string importJson;           //! the raw manifest "import" object (compact JSON), re-emitted on save; may be empty
    muse::ByteArray proofingScan;     //! optional deskewed render bytes (empty when the source was not deskewed)
    std::string proofingScanSuffix;   //! e.g. "pdf" (from the manifest proofingScan object; backfilled from the entry name)
    bool hasProofingScan = false;     //! true when a non-empty proofing-scan.<ext> was present in the bundle
    std::vector<double> skewAnglesDeg; //! optional per-page deskew angles (provenance only; empty when absent)
    int formatVersion = 0;
    bool hasMap = false;
    reprise::ProofingMap map;         //! sourcePath left empty here -- the caller repoints it to the extracted scan path
    bool hasNavigatorState = false;   //! true when a navigatorstate.json entry was present (0.18.0+ bundles)
    reprise::NavigatorStateMap navigatorState; //! bad-timing navigator per-record collapse/ignore state (empty when absent)
    bool hasViewState = false;        //! true when a viewstate.json entry was present (0.18.2+ bundles)
    reprise::ProofingViewState viewState; //! followed measure + band/magnifier zoom (left at defaults when absent)
};

//! Serialize a ProofingMap to the proofingmap.json byte form. sourcePath is intentionally OMITTED (it is machine-local
//! and re-derived on open to the extracted original).
muse::ByteArray serializeProofingMap(const reprise::ProofingMap& map);

//! Parse proofingmap.json bytes into `out` (out.sourcePath left empty; out.valid set true on success). Returns false
//! on malformed / non-object input.
bool parseProofingMap(const muse::ByteArray& bytes, reprise::ProofingMap& out);

//! Serialize the bad-timing navigator per-record state to the navigatorstate.json byte form (a {"records":[...]} object;
//! all-default records are omitted). An empty map serializes to an empty records array.
muse::ByteArray serializeNavigatorState(const reprise::NavigatorStateMap& states);

//! Parse navigatorstate.json bytes into `out`. Returns false on malformed / non-object input; unknown fields are ignored
//! and all-default records are dropped so the parsed map stays minimal.
bool parseNavigatorState(const muse::ByteArray& bytes, reprise::NavigatorStateMap& out);

//! Serialize the proofing view state to the viewstate.json byte form (a {"followedMeasure":n,"bandZoom":x,"magnifierZoom":y} object).
muse::ByteArray serializeViewState(const reprise::ProofingViewState& state);

//! Parse viewstate.json bytes into `out`. Returns false on malformed / non-object input; a MISSING key leaves that field at
//! its default (followedMeasure -1 / bandZoom 0.5 / magnifierZoom 1.0) -- guarded so a release build never derefs a missing key.
bool parseViewState(const muse::ByteArray& bytes, reprise::ProofingViewState& out);

//! Assemble a complete .repr ZIP into a byte array, ready to write to disk atomically. `importJson`, when non-empty,
//! must be a JSON object; it is embedded verbatim as manifest.import.
muse::ByteArray assembleRepr(const muse::ByteArray& scoreMscz, const muse::ByteArray& original,
                             const std::string& originalFileName, const std::string& originalSuffix,
                             const std::string& importJson, const reprise::ProofingMap& map,
                             const muse::ByteArray& proofingScan, const std::string& proofingScanSuffix,
                             const std::vector<double>& skewAnglesDeg,
                             const reprise::NavigatorStateMap& navigatorState,
                             const reprise::ProofingViewState& viewState);

//! Read + parse a .repr bundle from disk into `out`. The embedded original is NOT extracted to disk here. Returns a
//! non-ok Ret (with a user-facing message) when the file cannot be opened, is missing a required entry, or its manifest
//! major version is newer than this build supports.
muse::Ret readRepr(const muse::io::path_t& path, ReprBundle& out);

//! Read ONLY the original scan entry out of a .repr bundle (manifest for its suffix/file name + the original.<ext> bytes),
//! WITHOUT decoding score.mscz / proofing-scan / proofingmap. Used by the deferred-original save path (0.16.15): opening a
//! deskewed .repr no longer extracts the original to a temp file, so a later Save re-reads just the original straight from
//! the source bundle. Returns non-ok when the file cannot be opened; `out` is left empty when no original entry is present.
muse::Ret readReprOriginal(const muse::io::path_t& path, muse::ByteArray& out, std::string& outSuffix, std::string& outFileName);
}
