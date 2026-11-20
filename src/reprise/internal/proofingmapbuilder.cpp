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

#include "proofingmapbuilder.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <QFile>
#include <QRegularExpression>
#include <QString>

#include "global/io/path.h"
#include "global/types/bytearray.h"
#include "global/serialization/zipreader.h"
#include "global/serialization/xmlstreamreader.h"
#include "global/log.h"

using namespace muse;
using namespace mu::reprise;

namespace {
//! One sheet-xml entry inside the .omr ZIP, paired with its 1-based sheet number (parsed from "sheet#N/sheet#N.xml")
//! so the sheets can be ordered 1->N regardless of the ZIP's own entry order.
struct SheetEntry
{
    int32_t number = 0;
    std::string path;
};

//! Parse ONE sheet xml: fill `page` (image dimensions) and append this sheet's measure rects to `measures`, assigning
//! each a running GLOBAL measure index via `nextGlobalIndex` (document order == measure order). Within a <system> the
//! <stack> columns come BEFORE the <part><staff><lines> points, so the stacks are buffered and flushed at </system>,
//! by which point the system's full staff y-extent is known. Returns the number of measures appended.
int32_t parseSheet(const ByteArray& bytes, int32_t sheetIndex, int32_t dpi,
                   ProofingPageInfo& page, std::vector<ProofingMeasureRect>& measures, int32_t& nextGlobalIndex)
{
    page = ProofingPageInfo();
    page.index = sheetIndex;
    page.dpi = dpi;

    //! Per-system buffers, reset at each <system>.
    std::vector<std::pair<int32_t, int32_t> > stacks;   //! (left, right) for every measure column, in document order
    int64_t yMin = LLONG_MAX;
    int64_t yMax = LLONG_MIN;
    bool inStaff = false;
    bool inStaffLines = false;

    int32_t appended = 0;

    auto flushSystem = [&]()
    {
        if (!stacks.empty())
        {
            const bool haveY = (yMin <= yMax);
            const int32_t sy = haveY ? static_cast<int32_t>(yMin) : 0;
            const int32_t sh = haveY ? static_cast<int32_t>(yMax - yMin) : 0;

            for (const std::pair<int32_t, int32_t>& s : stacks)
            {
                ProofingMeasureRect m;
                m.globalMeasureIndex = nextGlobalIndex++;
                m.pageIndex = sheetIndex;
                m.x = s.first;
                m.y = sy;
                m.w = s.second - s.first;
                m.h = sh;
                measures.push_back(m);
                ++appended;
            }
        }

        stacks.clear();
        yMin = LLONG_MAX;
        yMax = LLONG_MIN;
        inStaff = false;
        inStaffLines = false;
    };

    XmlStreamReader r(bytes);
    while (!r.atEnd())
    {
        const XmlStreamReader::TokenType t = r.readNext();

        if (t == XmlStreamReader::StartElement)
        {
            const AsciiStringView n = r.name();
            if (n == "picture")
            {
                page.imageWidth = r.intAttribute("width");
                page.imageHeight = r.intAttribute("height");
            }
            else if (n == "system")
            {
                //! Defensive: flush any un-flushed previous system before starting a new one.
                flushSystem();
            }
            else if (n == "stack")
            {
                stacks.push_back({ r.intAttribute("left"), r.intAttribute("right") });
            }
            else if (n == "staff")
            {
                inStaff = true;
            }
            else if (n == "lines" && inStaff)
            {
                inStaffLines = true;
            }
            else if (n == "point" && inStaff && inStaffLines)
            {
                //! Audiveris emits staff-line point coordinates as DECIMALS at higher DPI (e.g. y="1149.5"); intAttribute
                //! would fail the decimal and yield 0, collapsing every measure rect to y=0,h=0. Read as double + round.
                const int64_t y = static_cast<int64_t>(std::llround(r.doubleAttribute("y")));
                yMin = std::min(yMin, y);
                yMax = std::max(yMax, y);
            }
        }
        else if (t == XmlStreamReader::EndElement)
        {
            const AsciiStringView n = r.name();
            if (n == "system")
            {
                flushSystem();
            }
            else if (n == "staff")
            {
                inStaff = false;
                inStaffLines = false;
            }
            else if (n == "lines")
            {
                inStaffLines = false;
            }
        }
    }

    //! Defensive: a malformed sheet that never closed its last <system> still contributes its buffered measures.
    flushSystem();

    return appended;
}

// ===========================================================================
// Phase B1: per-NOTE head boxes. Ports the validated Audiveris .omr <head> <-> MusicXML <note> matcher (spike
// omr_match_spike.py) into C++. The .omr gives per-<head> pixel bounds + the chord/voice/measure structure; the
// pre-strip Audiveris MusicXML gives the authoritative note order, pitch, part/staff layout and in-measure tick.
// ===========================================================================

//! One recognised note head from the .omr: its staff-position (the <head pitch=> integer, NOT a real pitch), the
//! within-part staff it was detected on, and its pixel box + 0-based sheet index.
struct OmrHead
{
    int32_t position = 0;
    int32_t staff = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    int32_t sheet = 0;
    bool hasBounds = false;
    bool used = false;
};

//! One pitched MusicXML note in document (== recognised-score) order, with everything the matcher + the runtime key
//! need. `dia` is the diatonic index (octave*7 + step 0..6), used to join a note to its head ignoring the accidental.
struct MxlNote
{
    int32_t gm = 0;
    int32_t partNum = 0;
    int32_t staff = 1;
    int32_t voice = 1;
    int32_t tickInMeasure = 0;
    int32_t octave = 0;
    int32_t step = 0;     //! 0..6 == C,D,E,F,G,A,B
    int32_t dia = 0;      //! octave*7 + step
};

using CellKey = std::tuple<int32_t, int32_t, int32_t>;   //! (globalMeasureIndex, partNum, voice)
using ClefKey = std::tuple<int32_t, int32_t, int32_t>;   //! (partNum, within-part staff, globalMeasureIndex)

//! 0..6 for C,D,E,F,G,A,B; -1 for anything else.
int32_t stepIndex(char c)
{
    switch (c)
    {
    case 'C': return 0;
    case 'D': return 1;
    case 'E': return 2;
    case 'F': return 3;
    case 'G': return 4;
    case 'A': return 5;
    case 'B': return 6;
    default:  return -1;
    }
}

//! Diatonic index (octave*7 + step) of the pitch that sits on a clef sign's reference line (G->G4, F->F3, C->C4).
int32_t clefRefDia(char sign)
{
    switch (sign)
    {
    case 'G': return 4 * 7 + 4;   //! G4
    case 'F': return 3 * 7 + 3;   //! F3
    case 'C': return 4 * 7 + 0;   //! C4
    default:  return 4 * 7 + 4;   //! default treble
    }
}

//! Convert an Audiveris head staff-position to a diatonic index under a given clef. Audiveris position counts steps
//! from the middle staff line (line 3), positive downward; each staff line == 2 diatonic steps.
int32_t omrPosToDia(int32_t position, char sign, int32_t line)
{
    const int32_t middleLineDia = clefRefDia(sign) + (3 - line) * 2;

    return middleLineDia - position;
}

//! True when the centre of box b lies inside box hc (the geometric chord->head containment fallback).
bool centroidIn(const OmrHead& b, const std::array<int32_t, 4>& hc)
{
    const double cx = b.x + b.w / 2.0;
    const double cy = b.y + b.h / 2.0;

    return cx >= hc[0] && cx <= hc[0] + hc[2] && cy >= hc[1] && cy <= hc[1] + hc[3];
}

//! Parse ONE .omr sheet xml and append its resolved note heads to `cellHeads`, keyed by (globalMeasureIndex, partNum,
//! voice) in onset order. Returns the number of distinct local measures on the sheet (to advance the global base). The
//! sheet carries: an <inters> block of <head id pitch staff><bounds/></head> and <head-chord id><bounds/></head-chord>
//! definitions; <relation source target><containment/></relation> links chord->head; and a page->system->part->measure
//! ->voice->slots->entry->value(chord,status) structure whose BEGIN values are the onsets in order.
int32_t parseOmrSheet(const ByteArray& bytes, int32_t sheetIndex, int32_t sheetBase,
                      std::map<CellKey, std::vector<OmrHead> >& cellHeads)
{
    std::map<std::string, OmrHead> heads;
    std::set<std::string> headChords;
    std::map<std::string, std::array<int32_t, 4> > hcBounds;
    std::vector<std::pair<std::string, std::string> > containment;   //! (chord id, head id)
    std::set<int32_t> localIds;

    //! One resolved onset: (partNum, local measure id, voice, chord id) in document order (== onset order per voice).
    struct StructEntry
    {
        int32_t partNum = 0;
        int32_t localMeasure = 0;
        int32_t voice = 0;
        std::string chordId;
    };
    std::vector<StructEntry> structEntries;

    std::string curHeadId;
    std::string curHcId;
    std::string relSrc;
    std::string relTgt;
    bool wantHeadBounds = false;
    bool wantHcBounds = false;
    bool relContain = false;

    int32_t curPart = -1;
    int32_t curMeasure = -1;
    int32_t curVoice = -1;
    int32_t curOnset = 0;

    XmlStreamReader r(bytes);
    while (!r.atEnd())
    {
        const XmlStreamReader::TokenType t = r.readNext();

        if (t == XmlStreamReader::StartElement)
        {
            const AsciiStringView n = r.name();
            if (n == "head")
            {
                curHeadId = r.attribute("id").toStdString();
                OmrHead hd;
                hd.position = r.intAttribute("pitch");
                hd.staff = r.intAttribute("staff", 1);
                hd.sheet = sheetIndex;
                heads[curHeadId] = hd;
                wantHeadBounds = true;
                wantHcBounds = false;
            }
            else if (n == "head-chord")
            {
                curHcId = r.attribute("id").toStdString();
                headChords.insert(curHcId);
                wantHcBounds = true;
                wantHeadBounds = false;
            }
            else if (n == "bounds")
            {
                if (wantHeadBounds && !curHeadId.empty())
                {
                    OmrHead& hd = heads[curHeadId];
                    hd.x = r.intAttribute("x");
                    hd.y = r.intAttribute("y");
                    hd.w = r.intAttribute("w");
                    hd.h = r.intAttribute("h");
                    hd.hasBounds = true;
                    wantHeadBounds = false;
                }
                else if (wantHcBounds && !curHcId.empty())
                {
                    hcBounds[curHcId] = { r.intAttribute("x"), r.intAttribute("y"), r.intAttribute("w"), r.intAttribute("h") };
                    wantHcBounds = false;
                }
            }
            else if (n == "relation")
            {
                relSrc = r.attribute("source").toStdString();
                relTgt = r.attribute("target").toStdString();
                relContain = false;
            }
            else if (n == "containment")
            {
                relContain = true;
            }
            else if (n == "part")
            {
                curPart = r.intAttribute("id", -1);
            }
            else if (n == "measure")
            {
                curMeasure = r.intAttribute("id", -1);
                if (curMeasure >= 0)
                {
                    localIds.insert(curMeasure);
                }
            }
            else if (n == "voice")
            {
                curVoice = r.intAttribute("id", -1);
                curOnset = 0;
            }
            else if (n == "value")
            {
                //! A slot value inside a voice -- distinguished from the top <value path="GRAY.png"/> by its status attr.
                if (curVoice >= 0 && curMeasure >= 0 && r.hasAttribute("status")
                    && r.asciiAttribute("status") == "BEGIN")
                {
                    ++curOnset;
                    StructEntry e;
                    e.partNum = curPart;
                    e.localMeasure = curMeasure;
                    e.voice = curVoice;
                    e.chordId = r.attribute("chord").toStdString();
                    structEntries.push_back(std::move(e));
                }
            }
        }
        else if (t == XmlStreamReader::EndElement)
        {
            const AsciiStringView n = r.name();
            if (n == "head")
            {
                curHeadId.clear();
                wantHeadBounds = false;
            }
            else if (n == "head-chord")
            {
                curHcId.clear();
                wantHcBounds = false;
            }
            else if (n == "relation")
            {
                if (relContain && !relSrc.empty() && !relTgt.empty())
                {
                    containment.push_back({ relSrc, relTgt });
                }
                relSrc.clear();
                relTgt.clear();
                relContain = false;
            }
            else if (n == "voice")
            {
                curVoice = -1;
            }
        }
    }

    //! Positional local-measure numbering: ids may start at 0 or 1 and may have gaps, so map sorted id -> 0..k-1.
    std::map<int32_t, int32_t> id2pos;
    {
        int32_t pos = 0;
        for (int32_t id : localIds)
        {
            id2pos[id] = pos++;
        }
    }

    //! chord id -> head ids, from <containment> relations whose source is a head-chord and target a head.
    std::map<std::string, std::vector<std::string> > chordToHeads;
    for (const std::pair<std::string, std::string>& c : containment)
    {
        if (headChords.count(c.first) && heads.count(c.second))
        {
            chordToHeads[c.first].push_back(c.second);
        }
    }

    //! Resolve each onset's chord to its heads (relation first, geometric containment as fallback) and append in order.
    for (const StructEntry& e : structEntries)
    {
        const auto itPos = id2pos.find(e.localMeasure);
        if (itPos == id2pos.end())
        {
            continue;
        }
        const int32_t gm = sheetBase + itPos->second;

        std::vector<std::string> hids;
        const auto itCh = chordToHeads.find(e.chordId);
        if (itCh != chordToHeads.end())
        {
            hids = itCh->second;
        }
        else
        {
            const auto itHc = hcBounds.find(e.chordId);
            if (itHc != hcBounds.end())
            {
                for (const std::pair<const std::string, OmrHead>& hp : heads)
                {
                    if (hp.second.hasBounds && centroidIn(hp.second, itHc->second))
                    {
                        hids.push_back(hp.first);
                    }
                }
            }
        }

        std::vector<OmrHead>& cell = cellHeads[CellKey(gm, e.partNum, e.voice)];
        for (const std::string& hid : hids)
        {
            const auto itH = heads.find(hid);
            if (itH != heads.end() && itH->second.hasBounds)
            {
                cell.push_back(itH->second);
            }
        }
    }

    return static_cast<int32_t>(localIds.size());
}

//! Extract the MusicXML bytes from a produced file: a .mxl is a ZIP (the rootfile named in META-INF/container.xml);
//! anything else is read verbatim. Mirrors ImportProgressModel::recognizedPartCount / MuseScore's own extractRootfile.
ByteArray readMusicXmlBytes(const QString& path)
{
    const io::path_t p(path);
    if (muse::io::suffix(p) == "mxl")
    {
        ZipReader zip(p);
        if (!zip.exists() || zip.hasError())
        {
            return ByteArray();
        }

        const ByteArray container = zip.fileData("META-INF/container.xml");
        std::string rootfile;
        XmlStreamReader e(container);
        while (e.readNextStartElement())
        {
            if (e.name() != "container")
            {
                break;
            }
            while (e.readNextStartElement())
            {
                if (e.name() != "rootfiles")
                {
                    break;
                }
                while (e.readNextStartElement())
                {
                    if (e.name() == "rootfile")
                    {
                        rootfile = e.attribute("full-path").toStdString();
                        break;
                    }
                }
            }
        }

        if (rootfile.empty())
        {
            return ByteArray();
        }

        return zip.fileData(rootfile);
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        return ByteArray();
    }
    const QByteArray all = f.readAll();
    f.close();

    return ByteArray::fromQByteArray(all);
}

//! Parse the pre-strip Audiveris MusicXML: fill `notes` (every pitched, non-grace note in document order), `clefBy`
//! (forward-filled clef per (partNum,staff,gm)), `partStaves` (staff count per partNum) and `partFirstStaff` (global
//! 0-based first staff index per partNum, from the <part-list> order + per-part <staves>). The note order here is the
//! order MuseScore re-imports (the strip preserves every musical value), so the runtime key computed per note matches
//! the opened score.
void parseRawMusicXml(const ByteArray& bytes, std::vector<MxlNote>& notes, std::map<ClefKey, std::pair<char, int32_t> >& clefBy,
                      std::map<int32_t, int32_t>& partStaves, std::map<int32_t, int32_t>& partFirstStaff)
{
    //! Part-list order first, to derive each part's global first-staff index once staff counts are known.
    std::vector<int32_t> partOrder;
    std::map<int32_t, int32_t> stavesByPart;   //! partNum -> <staves> (default 1)

    int32_t curPartNum = -1;
    int32_t gm = -1;
    int32_t curDivisions = 1;
    std::map<int32_t, std::pair<char, int32_t> > curClef;   //! within-part staff -> (sign, line)

    //! Measure time cursor (divisions), measure-relative; reset per measure.
    int32_t cursorDiv = 0;
    int32_t prevOnsetDiv = 0;

    //! Current note accumulator.
    bool inNote = false;
    bool inPartList = false;
    bool inClef = false;
    int32_t clefNumber = 1;
    char clefSign = 0;
    int32_t clefLine = 0;
    bool backupActive = false;
    bool forwardActive = false;

    bool isChord = false;
    bool isGrace = false;
    bool isRest = false;
    bool pitchPresent = false;
    int32_t nStep = -1;
    int32_t nOctave = 0;
    int32_t nDuration = 0;
    int32_t nVoice = 1;
    int32_t nStaff = 1;

    XmlStreamReader r(bytes);
    while (!r.atEnd())
    {
        const XmlStreamReader::TokenType t = r.readNext();

        if (t == XmlStreamReader::StartElement)
        {
            const AsciiStringView n = r.name();
            if (n == "part-list")
            {
                inPartList = true;
            }
            else if (n == "score-part" && inPartList)
            {
                const std::string pid = r.attribute("id").toStdString();
                const int32_t pnum = (pid.size() > 1 && pid[0] == 'P') ? std::atoi(pid.c_str() + 1) : std::atoi(pid.c_str());
                partOrder.push_back(pnum);
                if (!stavesByPart.count(pnum))
                {
                    stavesByPart[pnum] = 1;
                }
            }
            else if (n == "part")
            {
                const std::string pid = r.attribute("id").toStdString();
                curPartNum = (pid.size() > 1 && pid[0] == 'P') ? std::atoi(pid.c_str() + 1) : std::atoi(pid.c_str());
                gm = -1;
                curDivisions = 1;
                curClef.clear();
            }
            else if (n == "measure" && curPartNum >= 0)
            {
                ++gm;
                cursorDiv = 0;
                prevOnsetDiv = 0;
                //! Forward-fill: this measure inherits the running clef for every staff until a <clef> overrides it.
                for (const std::pair<const int32_t, std::pair<char, int32_t> >& cc : curClef)
                {
                    clefBy[ClefKey(curPartNum, cc.first, gm)] = cc.second;
                }
            }
            else if (n == "divisions")
            {
                curDivisions = r.readInt();
                if (curDivisions <= 0)
                {
                    curDivisions = 1;
                }
            }
            else if (n == "staves")
            {
                const int32_t s = r.readInt();
                stavesByPart[curPartNum] = (s > 0) ? s : 1;
            }
            else if (n == "clef")
            {
                inClef = true;
                clefNumber = r.intAttribute("number", 1);
                clefSign = 0;
                clefLine = 0;
            }
            else if (n == "sign" && inClef)
            {
                const std::string s = r.readText().toStdString();
                clefSign = s.empty() ? 0 : s[0];
            }
            else if (n == "line" && inClef)
            {
                clefLine = r.readInt();
            }
            else if (n == "backup")
            {
                backupActive = true;
            }
            else if (n == "forward")
            {
                forwardActive = true;
            }
            else if (n == "note")
            {
                inNote = true;
                isChord = false;
                isGrace = false;
                isRest = false;
                pitchPresent = false;
                nStep = -1;
                nOctave = 0;
                nDuration = 0;
                nVoice = 1;
                nStaff = 1;
            }
            else if (n == "chord" && inNote)
            {
                isChord = true;
            }
            else if (n == "grace" && inNote)
            {
                isGrace = true;
            }
            else if (n == "rest" && inNote)
            {
                isRest = true;
            }
            else if (n == "pitch" && inNote)
            {
                pitchPresent = true;
            }
            else if (n == "step" && inNote)
            {
                const std::string s = r.readText().toStdString();
                nStep = s.empty() ? -1 : stepIndex(s[0]);
            }
            else if (n == "octave" && inNote)
            {
                nOctave = r.readInt();
            }
            else if (n == "voice" && inNote)
            {
                nVoice = r.readInt();
            }
            else if (n == "staff" && inNote)
            {
                nStaff = r.readInt();
            }
            else if (n == "duration")
            {
                const int32_t d = r.readInt();
                if (backupActive)
                {
                    cursorDiv -= d;
                }
                else if (forwardActive)
                {
                    cursorDiv += d;
                }
                else if (inNote)
                {
                    nDuration = d;
                }
            }
        }
        else if (t == XmlStreamReader::EndElement)
        {
            const AsciiStringView n = r.name();
            if (n == "part-list")
            {
                inPartList = false;
            }
            else if (n == "clef")
            {
                if (clefSign != 0 && clefLine > 0)
                {
                    curClef[clefNumber] = { clefSign, clefLine };
                    clefBy[ClefKey(curPartNum, clefNumber, gm)] = { clefSign, clefLine };
                }
                inClef = false;
            }
            else if (n == "backup")
            {
                backupActive = false;
            }
            else if (n == "forward")
            {
                forwardActive = false;
            }
            else if (n == "note")
            {
                const int32_t onsetDiv = isChord ? prevOnsetDiv : cursorDiv;
                const bool pitched = pitchPresent && !isRest && nStep >= 0;

                if (pitched && !isGrace)
                {
                    MxlNote mn;
                    mn.gm = gm;
                    mn.partNum = curPartNum;
                    mn.staff = nStaff;
                    mn.voice = nVoice;
                    mn.octave = nOctave;
                    mn.step = nStep;
                    mn.dia = nOctave * 7 + nStep;
                    mn.tickInMeasure = (curDivisions > 0) ? (onsetDiv * 480) / curDivisions : 0;
                    notes.push_back(mn);
                }

                //! A non-chord element (note or rest) occupies a new onset and then advances the measure cursor; a grace
                //! note consumes no time; a chord member shares the previous onset and does not advance.
                if (!isChord)
                {
                    prevOnsetDiv = cursorDiv;
                    if (!isGrace)
                    {
                        cursorDiv += nDuration;
                    }
                }

                inNote = false;
            }
        }
    }

    //! Global first-staff index per part, in part-list order (parts MuseScore imports in this order).
    partStaves = stavesByPart;
    int32_t acc = 0;
    for (int32_t pnum : partOrder)
    {
        partFirstStaff[pnum] = acc;
        const auto it = stavesByPart.find(pnum);
        acc += (it != stavesByPart.end() ? it->second : 1);
    }
}
}

ProofingMap ProofingMapBuilder::buildFromOmr(const QString& omrPath, const QString& sourcePath, int32_t dpi,
                                             const QString& rawMusicXmlPath)
{
    ProofingMap map;
    map.sourcePath = sourcePath.toStdString();

    if (omrPath.isEmpty())
    {
        return map;
    }

    const io::path_t omrFilePath(omrPath);
    ZipReader zip(omrFilePath);
    if (!zip.exists() || zip.hasError())
    {
        LOGW() << "Reprise: proofing-map: cannot open .omr book " << omrPath;
        return map;
    }

    //! Collect the per-sheet xml entries (sheet#N/sheet#N.xml), capturing N so they can be ordered 1->N independent of
    //! the ZIP's stored entry order. book.xml and the BINARY.png images are skipped.
    static const QRegularExpression reSheet("sheet#(\\d+)\\.xml$");
    std::vector<SheetEntry> sheets;
    for (const ZipReader::FileInfo& fi : zip.fileInfoList())
    {
        if (!fi.isFile)
        {
            continue;
        }

        const QString p = QString::fromStdString(fi.filePath.toStdString());
        const QRegularExpressionMatch mt = reSheet.match(p);
        if (mt.hasMatch())
        {
            SheetEntry e;
            e.number = mt.captured(1).toInt();
            e.path = fi.filePath.toStdString();
            sheets.push_back(std::move(e));
        }
    }

    if (sheets.empty())
    {
        LOGW() << "Reprise: proofing-map: no sheet xml entries in " << omrPath;
        return map;
    }

    std::sort(sheets.begin(), sheets.end(), [](const SheetEntry& a, const SheetEntry& b) { return a.number < b.number; });

    int32_t nextGlobalIndex = 0;
    for (size_t i = 0; i < sheets.size(); ++i)
    {
        const ByteArray bytes = zip.fileData(sheets[i].path);
        if (bytes.empty())
        {
            LOGW() << "Reprise: proofing-map: empty sheet xml " << QString::fromStdString(sheets[i].path);
            continue;
        }

        ProofingPageInfo page;
        parseSheet(bytes, static_cast<int32_t>(i), dpi, page, map.measures, nextGlobalIndex);
        map.pages.push_back(page);
    }

    map.measureCount = static_cast<int32_t>(map.measures.size());
    map.valid = !map.measures.empty();

    //! Phase B1: per-note head boxes, from the same .omr plus the pre-strip MusicXML. Independently best-effort -- a
    //! failure here just leaves notes empty and never changes `valid` (the measure rects / band stay usable).
    if (!rawMusicXmlPath.isEmpty())
    {
        buildNoteBoxes(omrPath, rawMusicXmlPath, map);
    }

    return map;
}

void ProofingMapBuilder::buildNoteBoxes(const QString& omrPath, const QString& rawMusicXmlPath, ProofingMap& map)
{
    if (omrPath.isEmpty() || rawMusicXmlPath.isEmpty())
    {
        return;
    }

    //! --- 1. Parse the .omr: resolved note heads per (globalMeasureIndex, partNum, voice) cell, in onset order. ---
    const io::path_t omrFilePath(omrPath);
    ZipReader zip(omrFilePath);
    if (!zip.exists() || zip.hasError())
    {
        LOGW() << "Reprise: proofing-notes: cannot open .omr book " << omrPath;
        return;
    }

    static const QRegularExpression reSheet("sheet#(\\d+)\\.xml$");
    std::vector<SheetEntry> sheets;
    for (const ZipReader::FileInfo& fi : zip.fileInfoList())
    {
        if (!fi.isFile)
        {
            continue;
        }
        const QString p = QString::fromStdString(fi.filePath.toStdString());
        const QRegularExpressionMatch mt = reSheet.match(p);
        if (mt.hasMatch())
        {
            SheetEntry e;
            e.number = mt.captured(1).toInt();
            e.path = fi.filePath.toStdString();
            sheets.push_back(std::move(e));
        }
    }

    if (sheets.empty())
    {
        return;
    }
    std::sort(sheets.begin(), sheets.end(), [](const SheetEntry& a, const SheetEntry& b) { return a.number < b.number; });

    std::map<CellKey, std::vector<OmrHead> > cellHeads;
    int32_t sheetBase = 0;
    for (size_t i = 0; i < sheets.size(); ++i)
    {
        const ByteArray bytes = zip.fileData(sheets[i].path);
        if (bytes.empty())
        {
            continue;
        }
        sheetBase += parseOmrSheet(bytes, static_cast<int32_t>(i), sheetBase, cellHeads);
    }

    //! --- 2. Parse the pre-strip MusicXML: pitched notes in order + clef/part-staff layout. ---
    const ByteArray xml = readMusicXmlBytes(rawMusicXmlPath);
    if (xml.empty())
    {
        LOGW() << "Reprise: proofing-notes: cannot read MusicXML " << rawMusicXmlPath;
        return;
    }

    std::vector<MxlNote> notes;
    std::map<ClefKey, std::pair<char, int32_t> > clefBy;
    std::map<int32_t, int32_t> partStaves;
    std::map<int32_t, int32_t> partFirstStaff;
    parseRawMusicXml(xml, notes, clefBy, partStaves, partFirstStaff);

    if (notes.empty())
    {
        LOGW() << "Reprise: proofing-notes: MusicXML held no pitched notes";
        return;
    }

    //! --- 3. Match: assign each note the first unused head in its cell whose diatonic pitch matches under any of the
    //! part's staff clefs (handles hand-crossing); notes appear in onset order so same-pitch duplicates consume in
    //! order. A note with no matching head gets no box (untrusted -> B2 falls back to the measure box). ---
    int32_t matched = 0;
    for (const MxlNote& mn : notes)
    {
        const auto itCell = cellHeads.find(CellKey(mn.gm, mn.partNum, mn.voice));
        if (itCell == cellHeads.end())
        {
            continue;
        }

        const int32_t staffCount = [&]()
        {
            const auto it = partStaves.find(mn.partNum);
            return (it != partStaves.end() && it->second > 0) ? it->second : 1;
        }();

        OmrHead* hit = nullptr;
        for (OmrHead& head : itCell->second)
        {
            if (head.used)
            {
                continue;
            }
            bool diaMatch = false;
            for (int32_t s = 1; s <= staffCount && !diaMatch; ++s)
            {
                const auto itClef = clefBy.find(ClefKey(mn.partNum, s, mn.gm));
                if (itClef != clefBy.end() && omrPosToDia(head.position, itClef->second.first, itClef->second.second) == mn.dia)
                {
                    diaMatch = true;
                }
            }
            if (diaMatch)
            {
                hit = &head;
                break;
            }
        }

        if (!hit)
        {
            continue;
        }
        hit->used = true;

        const int32_t firstStaff = [&]()
        {
            const auto it = partFirstStaff.find(mn.partNum);
            return (it != partFirstStaff.end()) ? it->second : 0;
        }();

        ProofingNoteBox box;
        box.key.globalMeasureIndex = mn.gm;
        box.key.staffIdx = firstStaff + (mn.staff - 1);
        box.key.tickInMeasure = mn.tickInMeasure;
        box.key.step = mn.step;
        box.key.octave = mn.octave;
        box.key.voice = (mn.voice - 1) % 4;   //! advisory engraving voice (Audiveris 1..4 -> staff1, 5..8 -> staff2)
        if (box.key.voice < 0)
        {
            box.key.voice = 0;
        }
        box.pageIndex = hit->sheet;
        box.x = hit->x;
        box.y = hit->y;
        box.w = hit->w;
        box.h = hit->h;
        map.notes.push_back(box);
        ++matched;
    }

    LOGI() << "Reprise: proofing-notes: matched " << matched << " of " << static_cast<int>(notes.size())
           << " pitched notes to head boxes";

    return;
}

void ProofingMapBuilder::logMap(const ProofingMap& map)
{
    LOGI() << "proofing-map: source=" << QString::fromStdString(map.sourcePath)
           << " measureCount=" << map.measureCount << " pages=" << static_cast<int>(map.pages.size())
           << " notes=" << static_cast<int>(map.notes.size());

    for (const ProofingPageInfo& p : map.pages)
    {
        LOGI() << "proofing-map: page=" << p.index << " image=" << p.imageWidth << "x" << p.imageHeight
               << " dpi=" << p.dpi;
    }

    for (const ProofingMeasureRect& m : map.measures)
    {
        LOGI() << "proofing-map: gm=" << m.globalMeasureIndex << " page=" << m.pageIndex
               << " rect=" << m.x << "," << m.y << "," << m.w << "," << m.h;
    }

    static const char STEP_LETTER[7] = { 'C', 'D', 'E', 'F', 'G', 'A', 'B' };
    for (const ProofingNoteBox& b : map.notes)
    {
        const char letter = (b.key.step >= 0 && b.key.step < 7) ? STEP_LETTER[b.key.step] : '?';
        LOGI() << "proofing-note: gm=" << b.key.globalMeasureIndex << " staff=" << b.key.staffIdx
               << " tick=" << b.key.tickInMeasure << " v=" << b.key.voice
               << " pitch=" << QString(QChar(letter)) << b.key.octave << " page=" << b.pageIndex
               << " box=" << b.x << "," << b.y << "," << b.w << "," << b.h;
    }
}
