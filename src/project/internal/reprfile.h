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
};

//! Serialize a ProofingMap to the proofingmap.json byte form. sourcePath is intentionally OMITTED (it is machine-local
//! and re-derived on open to the extracted original).
muse::ByteArray serializeProofingMap(const reprise::ProofingMap& map);

//! Parse proofingmap.json bytes into `out` (out.sourcePath left empty; out.valid set true on success). Returns false
//! on malformed / non-object input.
bool parseProofingMap(const muse::ByteArray& bytes, reprise::ProofingMap& out);

//! Assemble a complete .repr ZIP into a byte array, ready to write to disk atomically. `importJson`, when non-empty,
//! must be a JSON object; it is embedded verbatim as manifest.import.
muse::ByteArray assembleRepr(const muse::ByteArray& scoreMscz, const muse::ByteArray& original,
                             const std::string& originalFileName, const std::string& originalSuffix,
                             const std::string& importJson, const reprise::ProofingMap& map,
                             const muse::ByteArray& proofingScan, const std::string& proofingScanSuffix,
                             const std::vector<double>& skewAnglesDeg);

//! Read + parse a .repr bundle from disk into `out`. The embedded original is NOT extracted to disk here. Returns a
//! non-ok Ret (with a user-facing message) when the file cannot be opened, is missing a required entry, or its manifest
//! major version is newer than this build supports.
muse::Ret readRepr(const muse::io::path_t& path, ReprBundle& out);
}
