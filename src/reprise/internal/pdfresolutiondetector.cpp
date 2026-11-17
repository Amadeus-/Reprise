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

#include "pdfresolutiondetector.h"

#include <cstdlib>

#include <QByteArray>
#include <QFile>
#include <QtPdf/QPdfDocument>

using namespace mu::reprise;

namespace {
//! Read cap: enough to cover the first page's objects on a typical scanned PDF without slurping a huge file.
static constexpr int64_t MAX_READ_BYTES = 4 * 1024 * 1024;

//! Sane embedded-resolution bounds. Anything outside this is treated as a parse artefact, not a real scan dpi.
static constexpr int32_t MIN_SANE_DPI = 50;
static constexpr int32_t MAX_SANE_DPI = 1200;

//! Window (bytes) searched around a "/Subtype /Image" marker for that image dict's /Width and /Height.
static constexpr int64_t IMAGE_DICT_WINDOW = 4096;

//! Read cap for page counting: large enough to cover a whole typical scanned PDF so an uncompressed "/Type /Page" scan
//! is complete. A file bigger than this cannot be counted confidently (a page object could live past the cap), so the
//! page counter reports not-confident and the caller shows the page range.
static constexpr int64_t MAX_PAGECOUNT_READ_BYTES = 64 * 1024 * 1024;

//! True for PDF whitespace or any PDF delimiter -- used as a token boundary so "/Page" is not matched inside "/PageX"
//! and "/Pages" is told apart from "/Page".
static bool isPdfDelimiterOrWs(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0'
           || c == '/' || c == '<' || c == '>' || c == '[' || c == ']' || c == '(' || c == ')'
           || c == '{' || c == '}' || c == '%';
}

//! Skip PDF whitespace (incl. NUL and form-feed) forward from pos.
static int64_t skipWs(const QByteArray& data, int64_t pos)
{
    while (pos < data.size())
    {
        const char c = data.at(static_cast<int>(pos));
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0')
        {
            ++pos;
            continue;
        }

        break;
    }

    return pos;
}

//! Parse a PDF number (optionally signed / fractional) starting at pos. Returns false if no digits are present.
static bool parseNumber(const QByteArray& data, int64_t pos, double& outValue, int64_t& outEnd)
{
    const int64_t start = pos;
    if (pos < data.size() && (data.at(static_cast<int>(pos)) == '+' || data.at(static_cast<int>(pos)) == '-'))
    {
        ++pos;
    }

    bool anyDigit = false;
    while (pos < data.size())
    {
        const char c = data.at(static_cast<int>(pos));
        if (c >= '0' && c <= '9')
        {
            anyDigit = true;
            ++pos;
            continue;
        }

        if (c == '.')
        {
            ++pos;
            continue;
        }

        break;
    }

    if (!anyDigit)
    {
        return false;
    }

    outValue = QByteArray(data.constData() + start, static_cast<int>(pos - start)).toDouble();
    outEnd = pos;

    return true;
}

//! Read the integer immediately following a key (after whitespace), where the key occurs within [searchFrom, searchTo).
static bool intAfterKey(const QByteArray& data, const QByteArray& key, int64_t searchFrom, int64_t searchTo, int32_t& outValue)
{
    const int from = static_cast<int>(searchFrom < 0 ? 0 : searchFrom);
    const int to = static_cast<int>(searchTo > data.size() ? data.size() : searchTo);
    const int idx = data.indexOf(key, from);
    if (idx < 0 || idx >= to)
    {
        return false;
    }

    const int64_t pos = skipWs(data, idx + key.size());
    double value = 0.0;
    int64_t end = 0;
    if (!parseNumber(data, pos, value, end))
    {
        return false;
    }

    outValue = static_cast<int32_t>(value + 0.5);

    return true;
}

//! First /MediaBox [ llx lly urx ury ] -> page width in points (urx - llx). False if not found / malformed.
static bool parseFirstMediaBoxWidthPts(const QByteArray& data, double& outWidthPts)
{
    const int idx = data.indexOf("/MediaBox");
    if (idx < 0)
    {
        return false;
    }

    int64_t pos = skipWs(data, idx + 9);   // 9 == len("/MediaBox")
    if (pos >= data.size() || data.at(static_cast<int>(pos)) != '[')
    {
        return false;
    }

    ++pos;   // past '['

    double nums[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int i = 0; i < 4; ++i)
    {
        pos = skipWs(data, pos);
        double v = 0.0;
        int64_t end = 0;
        if (!parseNumber(data, pos, v, end))
        {
            return false;
        }

        nums[i] = v;
        pos = end;
    }

    const double width = nums[2] - nums[0];
    if (width <= 0.0)
    {
        return false;
    }

    outWidthPts = width;

    return true;
}

//! First image XObject (a /Subtype whose next token is /Image) -> its /Width in pixels. False if none parses cleanly.
static bool parseFirstImagePixelWidth(const QByteArray& data, int32_t& outPixelWidth)
{
    int from = 0;
    while (true)
    {
        const int sub = data.indexOf("/Subtype", from);
        if (sub < 0)
        {
            return false;
        }

        const int64_t tok = skipWs(data, sub + 8);   // 8 == len("/Subtype")
        if (tok + 6 <= data.size() && QByteArray(data.constData() + tok, 6) == "/Image")
        {
            const int64_t winStart = sub > IMAGE_DICT_WINDOW ? sub - IMAGE_DICT_WINDOW : 0;
            const int64_t winEnd = sub + IMAGE_DICT_WINDOW;
            int32_t w = 0;
            int32_t h = 0;
            if (intAfterKey(data, "/Width", winStart, winEnd, w)
                && intAfterKey(data, "/Height", winStart, winEnd, h)
                && w > 0 && h > 0)
            {
                outPixelWidth = w;

                return true;
            }
        }

        from = sub + 8;
    }
}

//! Byte offset just AFTER the "obj" keyword of indirect object `objNum` (an "<objNum> <gen> obj" header). Any
//! generation is accepted; a token boundary before the number stops objNum 4 from matching inside "114 0 obj". Returns
//! the LAST such header, so an incrementally-updated object resolves to its current revision. -1 if none is found.
static int64_t findObjectStart(const QByteArray& data, int32_t objNum)
{
    const QByteArray needle = QByteArray::number(objNum);
    int from = 0;
    int64_t best = -1;
    while (true)
    {
        const int idx = data.indexOf(needle, from);
        if (idx < 0)
        {
            break;
        }

        from = idx + needle.size();

        //! Token boundary before the number: otherwise objNum 4 would match inside "114 0 obj".
        if (idx > 0 && !isPdfDelimiterOrWs(data.at(idx - 1)))
        {
            continue;
        }

        int64_t pos = idx + needle.size();
        const char sep = pos < data.size() ? data.at(static_cast<int>(pos)) : 'x';
        if (!(sep == ' ' || sep == '\t' || sep == '\r' || sep == '\n' || sep == '\f' || sep == '\0'))
        {
            continue;
        }

        pos = skipWs(data, pos);
        double gen = 0.0;
        int64_t end = 0;
        if (!parseNumber(data, pos, gen, end))
        {
            continue;
        }

        pos = skipWs(data, end);
        if (pos + 3 <= data.size() && QByteArray(data.constData() + static_cast<int>(pos), 3) == "obj")
        {
            best = pos + 3;
        }
    }

    return best;
}

//! Parse the page-tree root's inline /Count starting at searchFrom. Returns false when /Count is absent, non-positive,
//! or an INDIRECT reference ("/Count N G R") which this shallow reader will not chase -- the caller then stays
//! unconfident and shows the page range.
static bool parseInlineCount(const QByteArray& data, int64_t searchFrom, int32_t& outCount)
{
    const int idx = data.indexOf("/Count", static_cast<int>(searchFrom));
    if (idx < 0)
    {
        return false;
    }

    int64_t pos = skipWs(data, idx + 6);   // 6 == len("/Count")
    double value = 0.0;
    int64_t end = 0;
    if (!parseNumber(data, pos, value, end))
    {
        return false;
    }

    //! Indirect "/Count N G R": a second integer then 'R' after the first number means N was an object number, not the
    //! count. We do not resolve indirection -> treat as uncertain.
    const int64_t look = skipWs(data, end);
    double gen = 0.0;
    int64_t genEnd = 0;
    if (parseNumber(data, look, gen, genEnd))
    {
        const int64_t rpos = skipWs(data, genEnd);
        if (rpos < data.size() && data.at(static_cast<int>(rpos)) == 'R')
        {
            return false;
        }
    }

    const int32_t c = static_cast<int32_t>(value + 0.5);
    if (c < 1)
    {
        return false;
    }

    outCount = c;

    return true;
}
}

PdfResolutionResult mu::reprise::detectPdfResolution(const muse::io::path_t& pdfPath)
{
    PdfResolutionResult result;   // dpi = 300, matched = false

    QFile file(pdfPath.toQString());
    if (!file.open(QIODevice::ReadOnly))
    {
        return result;
    }

    const QByteArray data = file.read(MAX_READ_BYTES);
    file.close();
    if (data.isEmpty())
    {
        return result;
    }

    double widthPts = 0.0;
    int32_t pixelWidth = 0;
    if (!parseFirstMediaBoxWidthPts(data, widthPts) || !parseFirstImagePixelWidth(data, pixelWidth))
    {
        return result;
    }

    const double widthInches = widthPts / 72.0;
    if (widthInches <= 0.0)
    {
        return result;
    }

    const int32_t dpi = static_cast<int32_t>((static_cast<double>(pixelWidth) / widthInches) + 0.5);
    if (dpi < MIN_SANE_DPI || dpi > MAX_SANE_DPI)
    {
        return result;
    }

    result.dpi = dpi;
    result.matched = true;

    return result;
}

int32_t mu::reprise::snapToTier(int32_t dpi)
{
    static const int32_t TIERS[3] = { 200, 300, 400 };

    int32_t best = TIERS[0];
    int32_t bestDist = std::abs(dpi - TIERS[0]);
    for (int i = 1; i < 3; ++i)
    {
        const int32_t dist = std::abs(dpi - TIERS[i]);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = TIERS[i];
        }
    }

    return best;
}

PdfPageCountResult mu::reprise::detectPdfPageCount(const muse::io::path_t& pdfPath)
{
    PdfPageCountResult result;   // pageCount = 0, confident = false

    //! PRIMARY: QPdfDocument is the AUTHORITATIVE page counter for ALL PDFs, including the PDF-1.5+
    //! compressed-xref-stream / object-stream files (the common "edited + re-saved" case) that the byte-scan
    //! fallback below cannot read. load() of a LOCAL file is synchronous -- QtPdf reads the whole document and
    //! pageCount() is valid the moment load() returns -- so no event loop pumping or async handling is needed
    //! even though QPdfDocument is a QObject (constructed on the stack here). On success (Error::None, status
    //! Ready, pageCount > 0) this is the true count and we are confident. Any failure -- file not found, invalid
    //! format, password-protected, or a non-positive page count -- FALLS THROUGH to the byte-scan, which still
    //! enforces the safe contract (confident single page hides the range; any uncertainty shows it).
    {
        QPdfDocument doc;
        if (doc.load(pdfPath.toQString()) == QPdfDocument::Error::None
            && doc.status() == QPdfDocument::Status::Ready
            && doc.pageCount() > 0)
        {
            result.pageCount = doc.pageCount();
            result.confident = true;

            return result;
        }
    }

    //! FALLBACK (byte-scan): QPdfDocument could not load this file (or reported no pages). Resolve the page count
    //! from the page-tree root's /Count via a no-dependency plaintext byte-scan. It can only read uncompressed
    //! page trees, so a compressed object stream or any unresolvable chain link leaves confident == false and the
    //! caller shows the page range -- pages are never silently dropped.
    QFile file(pdfPath.toQString());
    if (!file.open(QIODevice::ReadOnly))
    {
        return result;
    }

    const int64_t fileSize = file.size();
    const QByteArray data = file.read(MAX_PAGECOUNT_READ_BYTES);
    file.close();
    if (data.isEmpty())
    {
        return result;
    }

    //! File larger than our read cap: a page object could live past what we read, so we cannot be sure it is a single
    //! page. Stay unconfident -> the caller shows the page range (safe: never hides pages).
    if (fileSize > static_cast<int64_t>(data.size()))
    {
        return result;
    }

    //! Compressed object streams (PDF 1.5+) hide page objects from a naive byte scan, so we cannot count reliably.
    if (data.indexOf("/ObjStm") >= 0)
    {
        return result;
    }

    //! Resolve the AUTHORITATIVE page count from the page-tree root: active (last) trailer -> /Root (document catalog)
    //! -> /Pages (page-tree root node) -> /Count. Taking the LAST trailer and the LAST revision of each object makes an
    //! incrementally-updated PDF resolve to its CURRENT revision, so stale/duplicate page objects left behind by
    //! Acrobat-style incremental saves do NOT inflate the count (the old "/Type /Page" leaf scan over-counted those).
    //! Any step we cannot resolve from the plaintext bytes -- no trailer, no /Root, no /Pages, no /Count, or an INDIRECT
    //! "/Count N G R" we will not chase -- leaves confident == false so the caller SHOWS the page range (never drops).
    const int trailerIdx = data.lastIndexOf("trailer");
    if (trailerIdx < 0)
    {
        return result;
    }

    const int rootKeyIdx = data.lastIndexOf("/Root");
    if (rootKeyIdx < 0)
    {
        return result;
    }

    //! /Root <catalogObj> <gen> R -- intAfterKey yields the first integer, i.e. the catalog's object number.
    int32_t catalogObjNum = 0;
    if (!intAfterKey(data, "/Root", rootKeyIdx, data.size(), catalogObjNum) || catalogObjNum < 1)
    {
        return result;
    }

    const int64_t catalogStart = findObjectStart(data, catalogObjNum);
    if (catalogStart < 0)
    {
        return result;
    }

    //! Catalog: /Pages <pagesObj> <gen> R -- the first /Pages key inside the catalog object is the page-tree root ref.
    int32_t pagesObjNum = 0;
    if (!intAfterKey(data, "/Pages", catalogStart, data.size(), pagesObjNum) || pagesObjNum < 1)
    {
        return result;
    }

    const int64_t pagesStart = findObjectStart(data, pagesObjNum);
    if (pagesStart < 0)
    {
        return result;
    }

    //! Page-tree root dict: /Count <total>. Must be an inline integer; an indirect "/Count N G R" is NOT chased.
    int32_t count = 0;
    if (!parseInlineCount(data, pagesStart, count))
    {
        return result;
    }

    result.pageCount = count;
    result.confident = true;

    return result;
}
