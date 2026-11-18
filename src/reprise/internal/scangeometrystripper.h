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

#include <QByteArray>
#include <QString>

namespace mu::reprise {
//! Post-processes an Audiveris-produced MusicXML before it is opened, stripping the SCAN GEOMETRY Audiveris bakes in
//! (per-measure widths, system/page breaks, absolute pixel positions, page/system/staff layout, slur bezier control
//! points) so MuseScore engraves its OWN layout instead of mirroring the scan -- which otherwise yields stretched
//! sparse bars and uneven spacing. It also de-duplicates the scan-positioned title <credit>s (which overprint the
//! metadata-built title frame as a doubled/overlapping title) and promotes the real title into <work><work-title>.
//! ALL musical and textual content (notes, rests, durations, pitches, voices, directions, lyrics, slurs, ties,
//! part-list, and the kept metadata) is preserved verbatim.
class ScanGeometryStripper
{
public:
    //! Strip the MusicXML at mxlOrXmlPath (a .mxl ZIP or a plain .xml/.musicxml), writing the cleaned plain MusicXML
    //! to a SIBLING file (next to the input, i.e. in the same temp working folder) and returning its absolute path.
    //! Returns an EMPTY string on ANY failure (unreadable input, parse error, unexpected structure, write failure);
    //! the caller must then open the ORIGINAL file unmodified, so a layout nicety can never block an import.
    static QString stripToSibling(const QString& mxlOrXmlPath);

    //! Core transform: take raw MusicXML bytes, return the stripped bytes. Empty QByteArray on failure. Exposed
    //! separately so the transform can be exercised without any file IO.
    static QByteArray stripBytes(const QByteArray& xml);

private:
    //! The "Gt" credits (texts == "Gt", roughly five of them on pages 2-10) are organ "Great"-manual registration
    //! marks Audiveris mis-captured as page credits -- not title-block text. DEFAULT true drops them so they do not
    //! clutter the title frame; flip to false to keep them as credits. Isolated here as a single clearly-labelled
    //! toggle because it is the one credit-policy choice that is a judgement call rather than a scan artifact.
    static constexpr bool kStripOrganManualCredits = true;
};
}
