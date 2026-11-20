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
#include <climits>
#include <utility>
#include <vector>

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
                const int64_t y = r.intAttribute("y");
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
}

ProofingMap ProofingMapBuilder::buildFromOmr(const QString& omrPath, const QString& sourcePath, int32_t dpi)
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

    return map;
}

void ProofingMapBuilder::logMap(const ProofingMap& map)
{
    LOGI() << "proofing-map: source=" << QString::fromStdString(map.sourcePath)
           << " measureCount=" << map.measureCount << " pages=" << static_cast<int>(map.pages.size());

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
}
