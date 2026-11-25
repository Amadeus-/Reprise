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

#include "importpdfcontroller.h"

#include <cstdint>

#include <QByteArray>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQuickWindow>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <QWindow>

#include "global/translation.h"
#include "global/log.h"
#include "global/types/val.h"

#include "async/async.h"

#include "notation/inotation.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationelements.h"
#include "notation/inotationselection.h"
#include "notation/inotationundostack.h"

#include "engraving/dom/score.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/types/constants.h"

#include "reprise/reprisecommands.h"

#include "pdfresolutiondetector.h"

using namespace mu;
using namespace mu::reprise;
using namespace muse;

//! Audiveris (org.audiveris.omr.sheet.SheetStub, constant maxPixelCount) rejects any sheet whose pixel count exceeds
//! this cap and aborts the whole run with "Too large image: N pixels (vs 20,000,000 max)". Confirmed empirically
//! against Audiveris 5.11.0 (a 6000x6000 = 36,000,000-pixel image produced exactly that log line). We mirror the cap
//! so Reprise can block an oversized image BEFORE launching Audiveris.
static constexpr qint64 AUDIVERIS_MAX_PIXELS = 20000000;

//! White-border width (px per side) the always-on image pad adds before Audiveris (the #711 edge-probe fix in
//! AudiverisRunner::padImageToTemp). The pre-flight judges the PADDED pixel count because that is what Audiveris actually
//! sees. Kept in sync with IMAGE_PAD_MARGIN in audiverisrunner.cpp.
static constexpr int IMAGE_PAD_MARGIN = 100;

//! DPI shown (clearly labelled "(assumed)") on the intake dialog's Resolution line when an image carries no real
//! resolution metadata. The user prefers a shown value over a blank; for images DPI is informational anyway (Audiveris
//! recognizes at native pixel resolution). The QML dialog uses this same value for the "(~N dpi, assumed)" suffix. This
//! applies to EVERY image type (PNG/JPG/TIFF/BMP/GIF) whose metadata is truly absent -- including a no-pHYs PNG, which
//! must read "~300 dpi, assumed", never Qt's back-filled 96. "assumed" is the right word for an image (an informational
//! guess); the PDF path's separate "(..., default)" wording means a real applied rasterization tier and is left alone.
static constexpr int DEFAULT_IMAGE_DPI = 300;

namespace {
//! Read cap for the in-house TIFF inspector. Large enough to cover a multi-frame scan whose image file directories may
//! sit near the end of the file; a TIFF larger than this can't be walked safely, so the inspector reports failure and
//! the caller shows the page range (never hides pages).
static constexpr qint64 TIFF_MAX_READ_BYTES = 128LL * 1024 * 1024;

//! Read cap for the PNG/JPEG/BMP metadata byte-scan. Resolution metadata lives in the file HEADER -- a PNG pHYs chunk
//! precedes the first IDAT, a JPEG JFIF/EXIF segment sits in the leading APPn markers, a BMP DIB header is at the very
//! start -- so a few MB always contains it when present; a real scan's pixel data (tens of MB) need never be read.
static constexpr qint64 IMAGE_META_READ_BYTES = 4LL * 1024 * 1024;

//! Little/big-endian 16- and 32-bit readers for the TIFF byte-scan. Callers guarantee the bounds.
static uint32_t rdU16(const QByteArray& d, int64_t pos, bool le)
{
    const uint8_t a = static_cast<uint8_t>(d.at(static_cast<int>(pos)));
    const uint8_t b = static_cast<uint8_t>(d.at(static_cast<int>(pos + 1)));
    return le ? (static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8))
              : (static_cast<uint32_t>(b) | (static_cast<uint32_t>(a) << 8));
}

static uint32_t rdU32(const QByteArray& d, int64_t pos, bool le)
{
    const uint8_t a = static_cast<uint8_t>(d.at(static_cast<int>(pos)));
    const uint8_t b = static_cast<uint8_t>(d.at(static_cast<int>(pos + 1)));
    const uint8_t c = static_cast<uint8_t>(d.at(static_cast<int>(pos + 2)));
    const uint8_t e = static_cast<uint8_t>(d.at(static_cast<int>(pos + 3)));
    return le ? (static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8)
                 | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(e) << 24))
              : ((static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16)
                 | (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(e));
}

//! In-house TIFF inspector: count frames (image file directories) by walking the IFD chain, and read the LARGEST
//! frame's pixel width/height (tags 256/257, max by pixel count across all frames) plus its native resolution (DPI,
//! from tags 282 XResolution / 296 ResolutionUnit, with 283 YResolution as a fallback). The kit now ships qtiff, so
//! QImageReader could supply the same facts (a future cleanup), but this plugin-independent walk is kept because it serves
//! both the page count/resolution info line and the oversized-image pre-flight in one pass (whose Audiveris cap is
//! per-SHEET, i.e. per frame, so the biggest frame is what must be judged). Classic TIFF only (BigTIFF magic 43 -> false).
//! Returns false on any
//! parse failure (not a classic TIFF, truncated, offset out of range) so the caller treats the frame count as unknown.
//! outDpi is the resolved DPI of the reported (largest) frame, or 0 when it cannot be determined (ResolutionUnit none,
//! resolution tags absent, or denominator 0) -- never invented here; the caller then shows a labelled default.
static bool readTiffInfo(const QByteArray& d, int& outFrames, int& outWidth, int& outHeight, int& outDpi)
{
    if (d.size() < 8)
    {
        return false;
    }

    const QByteArray bo = d.left(2);
    bool le = false;
    if (bo == "II")
    {
        le = true;
    }
    else if (bo == "MM")
    {
        le = false;
    }
    else
    {
        return false;
    }

    if (rdU16(d, 2, le) != 42)   // 43 == BigTIFF, not handled here
    {
        return false;
    }

    int64_t off = rdU32(d, 4, le);
    int frames = 0;
    int width = 0;
    int height = 0;

    //! Resolution of the LARGEST frame (the one whose dimensions we report), captured whenever a bigger frame is found.
    //! XResolution / YResolution are TIFF RATIONAL (two uint32: numerator/denominator). ResolutionUnit: 1=none, 2=inch,
    //! 3=cm. 0 == tag not present.
    uint32_t bestXNum = 0;
    uint32_t bestXDen = 0;
    uint32_t bestYNum = 0;
    uint32_t bestYDen = 0;
    uint32_t bestUnit = 0;

    //! Reads a RATIONAL value: its 4-byte value field at eo+8 is an OFFSET to two uint32 (num, den) elsewhere in the
    //! file (a RATIONAL is 8 bytes, which never fits inline). Honors the IFD's byte order. Zeros num/den on any bound miss.
    auto readRational = [&d, le](int64_t eo, uint32_t& num, uint32_t& den)
    {
        const int64_t ro = static_cast<int64_t>(rdU32(d, eo + 8, le));
        if (ro < 0 || ro + 8 > d.size())
        {
            num = 0;
            den = 0;
            return;
        }

        num = rdU32(d, ro, le);
        den = rdU32(d, ro + 4, le);
    };

    //! Guard caps the walk so a malformed / looping IFD chain can never spin forever.
    for (int guard = 0; guard < 100000 && off != 0; ++guard)
    {
        if (off < 0 || off + 2 > d.size())
        {
            return false;
        }

        const uint32_t entries = rdU16(d, off, le);

        //! Read ImageWidth (256) / ImageLength (257) + the resolution tags (282/283/296) for THIS frame, then keep the
        //! dimensions AND resolution of the LARGEST frame (by pixel count). Audiveris caps resolution per SHEET (per
        //! frame), so the biggest frame is what the oversized pre-flight must judge -- not merely the first.
        int fw = 0;
        int fh = 0;
        uint32_t fXNum = 0;
        uint32_t fXDen = 0;
        uint32_t fYNum = 0;
        uint32_t fYDen = 0;
        uint32_t fUnit = 0;
        for (uint32_t i = 0; i < entries; ++i)
        {
            const int64_t eo = off + 2 + static_cast<int64_t>(i) * 12;
            if (eo + 12 > d.size())
            {
                break;
            }

            const uint32_t tag = rdU16(d, eo, le);
            const uint32_t typ = rdU16(d, eo + 2, le);
            if (tag == 256 || tag == 257)
            {
                uint32_t val = 0;
                if (typ == 3)
                {
                    val = rdU16(d, eo + 8, le);        // SHORT
                }
                else if (typ == 4)
                {
                    val = rdU32(d, eo + 8, le);        // LONG
                }

                if (tag == 256)
                {
                    fw = static_cast<int>(val);
                }
                else
                {
                    fh = static_cast<int>(val);
                }
            }
            else if (tag == 282 && typ == 5)           // XResolution (RATIONAL)
            {
                readRational(eo, fXNum, fXDen);
            }
            else if (tag == 283 && typ == 5)           // YResolution (RATIONAL)
            {
                readRational(eo, fYNum, fYDen);
            }
            else if (tag == 296 && typ == 3)           // ResolutionUnit (SHORT)
            {
                fUnit = rdU16(d, eo + 8, le);
            }
        }

        if (static_cast<int64_t>(fw) * static_cast<int64_t>(fh) > static_cast<int64_t>(width) * static_cast<int64_t>(height))
        {
            width = fw;
            height = fh;
            bestXNum = fXNum;
            bestXDen = fXDen;
            bestYNum = fYNum;
            bestYDen = fYDen;
            bestUnit = fUnit;
        }

        const int64_t nextPos = off + 2 + static_cast<int64_t>(entries) * 12;
        if (nextPos + 4 > d.size())
        {
            return false;
        }

        off = rdU32(d, nextPos, le);
        ++frames;
    }

    if (frames < 1)
    {
        return false;
    }

    //! Resolve DPI from the largest frame's XResolution (YResolution as a fallback if X is absent). Only inch (unit 2)
    //! and cm (unit 3) yield a value; unit 1 (none) or a missing unit or a zero denominator leaves DPI unknown (0), and
    //! the caller then shows a clearly-labelled default -- a value is never invented here.
    int dpi = 0;
    if (bestUnit == 2 || bestUnit == 3)
    {
        uint32_t num = bestXNum;
        uint32_t den = bestXDen;
        if (den == 0 || num == 0)
        {
            num = bestYNum;
            den = bestYDen;
        }

        if (den != 0 && num != 0)
        {
            const double perUnit = static_cast<double>(num) / static_cast<double>(den);
            const double perInch = (bestUnit == 3) ? (perUnit * 2.54) : perUnit;
            dpi = static_cast<int>(perInch + 0.5);
        }
    }

    outFrames = frames;
    outWidth = width;
    outHeight = height;
    outDpi = dpi;

    return true;
}

//! In-house PNG resolution reader: byte-scan the chunk stream for the pHYs chunk. A pHYs chunk with unit specifier == 1
//! (metres) and a nonzero pixels-per-unit-X yields the real DPI (round(ppuX * 0.0254)); pHYs absent, or unit == 0
//! (aspect-ratio only, no physical resolution), returns 0 (truly absent). We deliberately do NOT trust
//! QImage::dotsPerMeterX(): this Qt kit BACK-FILLS 96 dpi for a metadata-less PNG, so it cannot tell "measured 96" from
//! "no metadata". pHYs always precedes IDAT, so a small header read contains it whenever it is present.
static int readPngDpi(const QByteArray& d)
{
    static const unsigned char kSig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (d.size() < 8)
    {
        return 0;
    }

    for (int i = 0; i < 8; ++i)
    {
        if (static_cast<uint8_t>(d.at(i)) != kSig[i])
        {
            return 0;
        }
    }

    int64_t pos = 8;
    while (pos + 8 <= d.size())
    {
        const uint32_t len = rdU32(d, pos, false);
        const int64_t typePos = pos + 4;
        const char t0 = d.at(static_cast<int>(typePos));
        const char t1 = d.at(static_cast<int>(typePos + 1));
        const char t2 = d.at(static_cast<int>(typePos + 2));
        const char t3 = d.at(static_cast<int>(typePos + 3));
        const int64_t dataPos = typePos + 4;

        if (t0 == 'p' && t1 == 'H' && t2 == 'Y' && t3 == 's')
        {
            if (dataPos + 9 > d.size())
            {
                return 0;
            }

            const uint32_t ppuX = rdU32(d, dataPos, false);
            const uint8_t unit = static_cast<uint8_t>(d.at(static_cast<int>(dataPos + 8)));
            if (unit == 1 && ppuX > 0)
            {
                return static_cast<int>(static_cast<double>(ppuX) * 0.0254 + 0.5);
            }

            return 0;   // unit 0 == aspect ratio only (no physical DPI)
        }

        if (t0 == 'I' && t1 == 'D' && t2 == 'A' && t3 == 'T')
        {
            return 0;   // reached image data without a pHYs chunk -> no metadata
        }

        //! Advance to the next chunk: 4 (length) + 4 (type) + len (data) + 4 (CRC).
        pos = dataPos + static_cast<int64_t>(len) + 4;
    }

    return 0;
}

//! Parse a mini-TIFF block (the payload of a JPEG EXIF APP1 segment) for IFD0's XResolution (tag 282) + ResolutionUnit
//! (tag 296), returning the resolved DPI or 0 when absent/unparseable. tiffStart points at the "II"/"MM" byte-order mark;
//! all offsets inside the block are relative to it. Only inch (unit 2) and cm (unit 3) yield a value (cm -> inch x 2.54).
static int parseExifDpi(const QByteArray& d, int64_t tiffStart)
{
    if (tiffStart < 0 || tiffStart + 8 > d.size())
    {
        return 0;
    }

    const QByteArray bo = d.mid(static_cast<int>(tiffStart), 2);
    bool le = false;
    if (bo == "II")
    {
        le = true;
    }
    else if (bo == "MM")
    {
        le = false;
    }
    else
    {
        return 0;
    }

    if (rdU16(d, tiffStart + 2, le) != 42)
    {
        return 0;
    }

    const int64_t ifdOff = tiffStart + static_cast<int64_t>(rdU32(d, tiffStart + 4, le));
    if (ifdOff < 0 || ifdOff + 2 > d.size())
    {
        return 0;
    }

    const uint32_t entries = rdU16(d, ifdOff, le);
    uint32_t xNum = 0;
    uint32_t xDen = 0;
    uint32_t unit = 0;
    for (uint32_t i = 0; i < entries; ++i)
    {
        const int64_t eo = ifdOff + 2 + static_cast<int64_t>(i) * 12;
        if (eo + 12 > d.size())
        {
            break;
        }

        const uint32_t tag = rdU16(d, eo, le);
        const uint32_t typ = rdU16(d, eo + 2, le);
        if (tag == 282 && typ == 5)            // XResolution (RATIONAL): value field is an offset to num/den
        {
            const int64_t ro = tiffStart + static_cast<int64_t>(rdU32(d, eo + 8, le));
            if (ro >= 0 && ro + 8 <= d.size())
            {
                xNum = rdU32(d, ro, le);
                xDen = rdU32(d, ro + 4, le);
            }
        }
        else if (tag == 296 && typ == 3)       // ResolutionUnit (SHORT): 1=none, 2=inch, 3=cm (inline)
        {
            unit = rdU16(d, eo + 8, le);
        }
    }

    if ((unit == 2 || unit == 3) && xNum != 0 && xDen != 0)
    {
        const double perUnit = static_cast<double>(xNum) / static_cast<double>(xDen);
        const double perInch = (unit == 3) ? (perUnit * 2.54) : perUnit;
        return static_cast<int>(perInch + 0.5);
    }

    return 0;
}

//! In-house JPEG resolution reader: scan the APPn marker segments (up to SOS) for a real physical resolution. Primary
//! source is the JFIF APP0 density (units 1=dpi / 2=dpcm with a nonzero X density); fallback is the EXIF APP1
//! XResolution/ResolutionUnit. Returns the resolved DPI, or 0 when neither carries one (JFIF units 0 == aspect-ratio
//! only). Standalone markers (SOI/EOI/RSTn/TEM) have no length segment; everything else does (big-endian, includes the
//! 2 length bytes). Matches the TIFF philosophy: a value here is genuinely measured, never Qt's back-filled guess.
static int readJpegDpi(const QByteArray& d)
{
    const int64_t n = d.size();
    if (n < 4)
    {
        return 0;
    }

    if (static_cast<uint8_t>(d.at(0)) != 0xFF || static_cast<uint8_t>(d.at(1)) != 0xD8)
    {
        return 0;
    }

    int exifDpi = 0;
    int64_t pos = 2;
    while (pos + 2 <= n)
    {
        if (static_cast<uint8_t>(d.at(static_cast<int>(pos))) != 0xFF)
        {
            break;
        }

        uint8_t marker = static_cast<uint8_t>(d.at(static_cast<int>(pos + 1)));
        pos += 2;

        //! Skip any fill 0xFF bytes between a marker's prefix and its code.
        while (marker == 0xFF && pos < n)
        {
            marker = static_cast<uint8_t>(d.at(static_cast<int>(pos)));
            ++pos;
        }

        //! Standalone markers (no length segment): SOI/EOI, RST0-7, TEM.
        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
        {
            continue;
        }

        if (marker == 0xDA)   // SOS: entropy-coded data follows; stop header scan
        {
            break;
        }

        if (pos + 2 > n)
        {
            break;
        }

        const int segLen = static_cast<int>(rdU16(d, pos, false));
        if (segLen < 2)
        {
            break;
        }

        const int64_t dataPos = pos + 2;
        const int64_t dataLen = static_cast<int64_t>(segLen) - 2;

        if (marker == 0xE0 && dataPos + 12 <= n)   // APP0 (JFIF)
        {
            if (d.at(static_cast<int>(dataPos)) == 'J' && d.at(static_cast<int>(dataPos + 1)) == 'F'
                && d.at(static_cast<int>(dataPos + 2)) == 'I' && d.at(static_cast<int>(dataPos + 3)) == 'F'
                && d.at(static_cast<int>(dataPos + 4)) == '\0')
            {
                const uint8_t units = static_cast<uint8_t>(d.at(static_cast<int>(dataPos + 7)));
                const uint32_t xden = rdU16(d, dataPos + 8, false);
                if (units == 1 && xden > 0)
                {
                    return static_cast<int>(xden);
                }

                if (units == 2 && xden > 0)
                {
                    return static_cast<int>(static_cast<double>(xden) * 2.54 + 0.5);
                }

                //! units == 0 -> aspect ratio only; keep scanning for an EXIF APP1 that may carry a real resolution.
            }
        }
        else if (marker == 0xE1 && dataPos + 6 <= n)   // APP1 (EXIF)
        {
            if (d.at(static_cast<int>(dataPos)) == 'E' && d.at(static_cast<int>(dataPos + 1)) == 'x'
                && d.at(static_cast<int>(dataPos + 2)) == 'i' && d.at(static_cast<int>(dataPos + 3)) == 'f'
                && d.at(static_cast<int>(dataPos + 4)) == '\0' && d.at(static_cast<int>(dataPos + 5)) == '\0')
            {
                const int found = parseExifDpi(d, dataPos + 6);
                if (found > 0 && exifDpi == 0)
                {
                    exifDpi = found;
                }
            }
        }

        pos = dataPos + dataLen;
    }

    return exifDpi;
}

//! In-house BMP resolution reader: the DIB header's biXPelsPerMeter (pixels per metre, little-endian signed LONG at byte
//! offset 38, present from BITMAPINFOHEADER onward). Nonzero -> real DPI (round(xppm * 0.0254)); zero/absent, or a core
//! header too short to carry it, returns 0 (absent).
static int readBmpDpi(const QByteArray& d)
{
    if (d.size() < 42)
    {
        return 0;
    }

    if (d.at(0) != 'B' || d.at(1) != 'M')
    {
        return 0;
    }

    const uint32_t dibSize = rdU32(d, 14, true);
    if (dibSize < 40)
    {
        return 0;   // BITMAPCOREHEADER (12) has no pixels-per-metre fields
    }

    const int32_t xppm = static_cast<int32_t>(rdU32(d, 38, true));
    if (xppm > 0)
    {
        return static_cast<int>(static_cast<double>(xppm) * 0.0254 + 0.5);
    }

    return 0;
}

//! Dispatch a non-TIFF image's bytes to the matching in-house resolution reader. Returns the genuinely-measured embedded
//! DPI, or 0 when the file carries none (GIF -- and any unrecognised suffix -- has no physical-resolution concept). TIFF
//! resolution is handled separately by readTiffInfo (its IFD walk, kept despite qtiff). suffix is io::suffix (lower-cased, no dot).
static int readImageDpi(const QByteArray& d, const std::string& suffix)
{
    if (suffix == "png")
    {
        return readPngDpi(d);
    }

    if (suffix == "jpg" || suffix == "jpeg")
    {
        return readJpegDpi(d);
    }

    if (suffix == "bmp")
    {
        return readBmpDpi(d);
    }

    return 0;
}
}

void ImportPdfController::init()
{
    m_audiverisRunner = std::make_shared<AudiverisRunner>(iocContext());

    commandDispatcher()->onRequest(this, REPRISE_IMPORT_PDF_COMMAND,
                                   [this](const muse::rcommand::Params& params) { return importPdf(params); });

    //! Read-only MCP/API queries. Always callable (not gated on a running import): they report the latest snapshot.
    commandDispatcher()->onRequest<GetImportProgressCommand>(this, std::function<muse::RetVal<GetImportProgressCommand::Output>(const GetImportProgressCommand&)>(
                                                                 [this](const GetImportProgressCommand&) { return getImportProgress(); }));
    commandDispatcher()->onRequest<GetImportErrorsCommand>(this, std::function<muse::RetVal<GetImportErrorsCommand::Output>(const GetImportErrorsCommand&)>(
                                                               [this](const GetImportErrorsCommand&) { return getImportErrors(); }));
    commandDispatcher()->onRequest<GetProofingMapCommand>(this, std::function<muse::RetVal<GetProofingMapCommand::Output>(const GetProofingMapCommand&)>(
                                                              [this](const GetProofingMapCommand&) { return getProofingMap(); }));

    //! Dev tool: open + capture the intake dialog to a PNG (headless verification of the modal dialog).
    commandDispatcher()->onRequest<CaptureImportDialogCommand>(this, std::function<muse::RetVal<CaptureImportDialogCommand::Output>(const CaptureImportDialogCommand&)>(
                                                                   [this](const CaptureImportDialogCommand& c) { return captureImportDialog(c); }));

    //! OCR language-pack helpers (list / add / remove), sharing the Preferences pack-manager code path.
    commandDispatcher()->onRequest<ListLanguagesCommand>(this, std::function<muse::RetVal<ListLanguagesCommand::Output>(const ListLanguagesCommand&)>(
                                                             [this](const ListLanguagesCommand&) { return listLanguages(); }));
    commandDispatcher()->onRequest<AddLanguageCommand>(this, std::function<muse::RetVal<AddLanguageCommand::Output>(const AddLanguageCommand&)>(
                                                           [this](const AddLanguageCommand& c) { return addLanguage(c); }));
    commandDispatcher()->onRequest<RemoveLanguageCommand>(this, std::function<muse::RetVal<RemoveLanguageCommand::Output>(const RemoveLanguageCommand&)>(
                                                              [this](const RemoveLanguageCommand& c) { return removeLanguage(c); }));

    //! Dev tool: open + capture the Preferences dialog at the Reprise page to a PNG (headless verification).
    commandDispatcher()->onRequest<CapturePreferencesCommand>(this, std::function<muse::RetVal<CapturePreferencesCommand::Output>(const CapturePreferencesCommand&)>(
                                                                  [this](const CapturePreferencesCommand& c) { return capturePreferences(c); }));

    //! Dev "drive" tool: navigate the app shell to a named screen in-process (no foreground, no synthetic input).
    commandDispatcher()->onRequest<NavigateCommand>(this, std::function<muse::RetVal<NavigateCommand::Output>(const NavigateCommand&)>(
                                                        [this](const NavigateCommand& c) { return navigate(c); }));

    //! Dev tool: drive the Two-Pane Proofing follow band -- select a measure by global index and report its map rect.
    commandDispatcher()->onRequest<ProofingDebugGotoCommand>(this, std::function<muse::RetVal<ProofingDebugGotoCommand::Output>(const ProofingDebugGotoCommand&)>(
                                                                 [this](const ProofingDebugGotoCommand& c) { return proofingDebugGoto(c); }));

    //! Dev tool: drive the Two-Pane Proofing FULL DETAIL VIEW magnifier -- select a note by measure/staff/index and
    //! report its B1 key + whether the map holds its head box.
    commandDispatcher()->onRequest<ProofingDebugSelectNoteCommand>(this, std::function<muse::RetVal<ProofingDebugSelectNoteCommand::Output>(const ProofingDebugSelectNoteCommand&)>(
                                                                       [this](const ProofingDebugSelectNoteCommand& c) { return proofingDebugSelectNote(c); }));

    //! Dev tool: set the live Two-Pane Proofing follow band's / magnifier's user zoom (drive + screenshot zoom states).
    commandDispatcher()->onRequest<ProofingDebugSetZoomCommand>(this, std::function<muse::RetVal<ProofingDebugSetZoomCommand::Output>(const ProofingDebugSetZoomCommand&)>(
                                                                    [this](const ProofingDebugSetZoomCommand& c) { return proofingDebugSetZoom(c); }));

    //! Dev tool: drive the bad-timing navigator's one-click Fix -- regularize an under-full measure to its time signature.
    commandDispatcher()->onRequest<ProofingDebugRegularizeCommand>(this, std::function<muse::RetVal<ProofingDebugRegularizeCommand::Output>(const ProofingDebugRegularizeCommand&)>(
                                                                       [this](const ProofingDebugRegularizeCommand& c) { return proofingDebugRegularize(c); }));

    //! Success auto-close of the recognition progress popup. The popup (ImportProgressModel) asks for its own window to
    //! be closed via the shared tracker; we do the actual close + settle from HERE, where the controller safely outlives
    //! the transient popup. Deferred to a fresh event-loop turn so the popup's own call stack (the asking QTimer lambda)
    //! has unwound before its window + view-model are destroyed by forceCloseAndSettle's pump.
    importProgressTracker()->popupCloseRequested().onNotify(this, [this]()
    {
        muse::async::Async::call(this, [this]() { closeProgressPopup(); });
    });

    //! FIX 2: a recognition FAILED. Close the progress popup (settled) and open the dedicated "Recognition failed"
    //! window. Deferred to a fresh event-loop turn (Async::call) so the failing call stack in ImportProgressModel has
    //! unwound before the popup's window + view-model are destroyed by the settled close -- same discipline as the
    //! success auto-close above. The error message + details were just recorded in the tracker snapshot by the model's
    //! reportFinished(false, ...) and are read back here to populate the error dialog.
    importProgressTracker()->errorWindowRequested().onNotify(this, [this]()
    {
        muse::async::Async::call(this, [this]() { showRecognitionError(); });
    });

    //! "Import finished" partial-import notice: some pages were skipped on an otherwise-successful import. Open the dedicated
    //! notice window (the progress popup is already closed by its own success auto-close by now). Deferred to a fresh
    //! event-loop turn (Async::call) for the same stack-unwind discipline as the error/success paths above.
    importProgressTracker()->noticeWindowRequested().onNotify(this, [this]()
    {
        muse::async::Async::call(this, [this]() { showImportNotice(); });
    });
}

muse::Ret ImportPdfController::importPdf(const muse::rcommand::Params& params)
{
    //! Three ways in, distinguished by two params:
    //!   * New-Scan tile / "Import Scan…" menu: no "path"               -> show the picker, then the Options dialog.
    //!   * File > Open / drag-and-drop of a scan: "path" + "interactive"=true -> skip the picker (file already chosen) but
    //!     STILL show the Options dialog (exact parity with the tile).
    //!   * Headless MCP reprise.import-pdf: "path" only (no "interactive") -> skip BOTH the picker and the Options dialog.
    //! "headless" is true only for that last, GUI-less path, so the audiveris-missing / busy feedback below fails fast with
    //! a Ret to a scripted caller but shows a modal to a real user (both interactive entry points).
    const std::string givenPath = params.at("path").toString();
    const bool wantInteractive = params.at("interactive", Val(false)).toBool();
    const bool headless = !givenPath.empty() && !wantInteractive;

    //! FIX 1 (single-import guard, belt-and-suspenders): refuse a SECOND import while one recognition is already active.
    //! The New-Scan command is also greyed out while active (RepriseCommandsState), but this covers any edge path that
    //! still reaches importPdf -- a direct command dispatch, or an MCP reprise.import-pdf call (which bypasses the GUI
    //! action state entirely). snapshot().active is set by launchImport's reportStarted and cleared on finish / error /
    //! cancel, so it is true for exactly the duration of a live run. MCP/headless caller -> a clean busy error;
    //! interactive caller -> a warning, then cancel. Either way NO second popup / Audiveris run is launched.
    if (importProgressTracker()->snapshot().active)
    {
        if (headless)
        {
            return make_ret(Ret::Code::NotSupported, "A recognition is already in progress");
        }

        interactive()->warning(muse::trc("reprise", "Import already in progress"),
                               muse::trc("reprise", "A recognition is already in progress."),
                               { interactive()->buttonData(IInteractive::Button::Ok) });

        return make_ret(Ret::Code::Cancel);
    }

    //! Reprise has exactly one OMR path: local Audiveris. Verify it is present before asking for a file.
    if (m_audiverisRunner->findAudiveris().empty())
    {
        //! Headless caller: fail fast without a modal dialog that would block the thread.
        if (headless)
        {
            return make_ret(Ret::Code::NotSupported, "Audiveris was not found");
        }

        interactive()->warning(muse::trc("reprise", "Audiveris was not found"),
                               muse::trc("reprise", "Install Audiveris or set its path to import scans."),
                               { interactive()->buttonData(IInteractive::Button::Ok) });

        return make_ret(Ret::Code::NotSupported);
    }

    //! Headless / scripted path: a "path" arg WITHOUT "interactive" skips the picker AND the options dialog entirely, so
    //! an MCP/API caller can drive the whole import with no GUI clicks. "pages"/"dpi" default when omitted. (A "path" WITH
    //! "interactive"=true falls through to the interactive section below, which shows the Options dialog for it.)
    if (headless)
    {
        const io::path_t pdfPath(givenPath);
        if (!fileSystem()->exists(pdfPath))
        {
            return make_ret(Ret::Code::UnknownError, "Scan not found: " + givenPath);
        }

        //! Oversized-image pre-flight (image inputs only). Block up front so the MCP/API caller gets a clean error
        //! rather than a raw Audiveris exit code. Also mirror it into the progress tracker so reprise.get-import-errors
        //! surfaces it, since this early return never reaches the popup view-model that normally reports failures.
        const muse::Ret sizeRet = preflightImageSize(pdfPath);
        if (!sizeRet)
        {
            importProgressTracker()->reportStarted(pdfPath.toStdString());
            importProgressTracker()->reportFinished(false, sizeRet.text(), std::string());
            return sizeRet;
        }

        //! Make the intake-dialog show/hide decision observable on the HEADLESS path too (no GUI): this logs the same
        //! derived values the interactive dialog-prep logs -- pageCount / isMultiPage / willShowPageRange / resolution
        //! for an image, or pdfPageCount / confident / willShowPageRange for a PDF. The return value is unused here.
        inspectImage(pdfPath, io::suffix(pdfPath) != "pdf");

        std::string pages = params.at("pages").toString();
        if (pages.empty())
        {
            pages = "all";
        }

        //! No (or non-positive) "dpi" arg -> auto-detect from the PDF, same detector the GUI uses. A clean match
        //! snaps to a tier (200/300/400) so headless and GUI runs land on identical resolutions; a miss falls back
        //! to the stored scanResolution(). Intentionally NOT clamped to 200-400: an explicit "dpi" arg stays raw
        //! for headless agents who want a non-tier resolution.
        int dpi = static_cast<int>(params.at("dpi").toInt());
        if (dpi <= 0)
        {
            const PdfResolutionResult detected = detectPdfResolution(pdfPath);
            if (detected.matched)
            {
                dpi = snapToTier(detected.dpi);
                LOGI() << "Reprise PDF detect (headless): native dpi=" << detected.dpi << " -> tier " << dpi;
            }
            else
            {
                dpi = scanResolution();
                LOGI() << "Reprise PDF detect (headless): no embedded resolution found -> fallback dpi=" << dpi;
            }
        }

        //! Agent/headless-driven run (a "path" arg was supplied): mark the session non-restorable so the project this
        //! import opens never lands in the session-restore list. Otherwise the ~15s job-teardown kill (which never
        //! reaches SessionsManager::deinit()) would leave a leftover path that pops the "previous session quit
        //! unexpectedly" prompt on the next USER launch. The normal GUI import path below is untouched, so a genuine
        //! user quit with a project open still restores as before. Set before the project opens, so ordering is moot.
        sessionsManager()->setIgnoreSession(true);

        //! Recognition options for the headless/MCP path (same values the GUI "Options" panel collects), feeding the
        //! SAME AudiverisRunner constant-building logic. Defaults mirror Audiveris's own (text on, eng, chords off,
        //! lyrics below), so an MCP caller that omits them gets exactly the proven default OCR behavior.
        RecognitionOptions options;
        options.recognizeText = params.at("recognizeText", Val(true)).toBool();
        //! Default to the configured global default OCR language (omr/ocrLanguage) when no explicit "language" arg is
        //! given, so a headless import matches what the GUI dialog would open on.
        const std::string defaultLang = repriseConfiguration()->ocrLanguage();
        const std::string langArg = params.at("language", Val(defaultLang)).toString();
        options.language = langArg.empty() ? QString::fromStdString(defaultLang) : QString::fromStdString(langArg);
        options.chordSymbols = params.at("chordSymbols", Val(false)).toBool();
        options.lyricsAboveStaff = params.at("lyricsAboveStaff", Val(false)).toBool();

        rememberImportPdfDir(pdfPath);
        setScanResolution(dpi);
        launchImport(pdfPath, pages, dpi, options);

        return make_ok();
    }

    //! The scan to import. An interactive File > Open / drag-and-drop pre-chose it ("interactive"=true with a "path"), so
    //! skip the picker and use that path (verifying it still exists); otherwise (the New-Scan tile / "Import Scan…" menu)
    //! show the picker now. Either way the Options dialog below is shown identically -- exact parity between the paths.
    io::path_t pdfPath;
    if (wantInteractive)
    {
        pdfPath = io::path_t(givenPath);
        if (!fileSystem()->exists(pdfPath))
        {
            interactive()->warning(muse::trc("reprise", "Scan not found"),
                                   muse::qtrc("reprise", "The file could not be found:\n%1").arg(pdfPath.toQString()).toStdString(),
                                   { interactive()->buttonData(IInteractive::Button::Ok) });
            return make_ret(Ret::Code::Cancel);
        }
    }
    else
    {
        pdfPath = interactive()->selectOpeningFileSync(muse::trc("reprise", "Import Scan"), defaultImportPdfDir(),
                                                       { muse::trc("reprise", "Scans") + " (*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.bmp *.gif)" });
        if (pdfPath.empty())
        {
            return make_ret(Ret::Code::Cancel);
        }
    }

    //! Remember the folder on a real pick / pre-chosen scan (never on cancel), independent of MuseScore's File > Open folder.
    rememberImportPdfDir(pdfPath);

    //! Auto-detect the PDF's native image resolution BEFORE the dialog opens so we can pre-select a matching quality
    //! tier. A clean detection pre-selects its snapped tier and flags the "matched to your PDF" badge; a miss falls
    //! back to the stored scanResolution() (snapped to a tier) with no badge.
    const PdfResolutionResult detected = detectPdfResolution(pdfPath);
    const int preselectTier = detected.matched ? snapToTier(detected.dpi) : snapToTier(scanResolution());
    if (detected.matched)
    {
        LOGI() << "Reprise PDF detect: native dpi=" << detected.dpi << " -> tier " << preselectTier << " (matched)";
    }
    else
    {
        LOGI() << "Reprise PDF detect: no embedded resolution found -> tier " << preselectTier << " (Standard, no badge)";
    }

    //! A single image (png/jpg/tif/...) has no PDF-resolution tier to pick, so the intake dialog hides that control.
    //! Derive from the picked file's extension (io::suffix is lower-cased, no dot).
    const bool isImage = io::suffix(pdfPath) != "pdf";

    //! Oversized-image pre-flight (image inputs only; PDFs are skipped inside the helper). Block up front with a clear
    //! dialog instead of showing the options dialog and then letting Audiveris abort mid-run with a raw exit code.
    const muse::Ret sizeRet = preflightImageSize(pdfPath);
    if (!sizeRet)
    {
        interactive()->warning(muse::trc("reprise", "Scan too large"), sizeRet.text(),
                               { interactive()->buttonData(IInteractive::Button::Ok) });
        return make_ret(Ret::Code::Cancel);
    }

    //! Inspect the input ONCE (also logs the decision for headless parity): a multi-frame TIFF is multiple sheets, and a
    //! multi-page PDF needs the page range, but a single image and a confidently-single-page PDF do not. inspectImage
    //! returns the final showPageRange decision plus the image's native resolution for the single-image info line.
    const ImageInfo imageInfo = inspectImage(pdfPath, isImage);

    //! Intake dialog: page range (free-form) + a Faster/Standard/Best quality tier. Cancel discards the import. The
    //! controller passes the already-decided showPageRange (single source of truth) plus the image resolution so the
    //! dialog only renders what applies: tier for any PDF, page range per showPageRange, info line for a single image.
    UriQuery query("musescore://reprise/importpdf/options");
    query.addParam("pages", Val(std::string("all")));
    query.addParam("tier", Val(preselectTier));
    query.addParam("matched", Val(detected.matched));
    query.addParam("isImage", Val(isImage));
    query.addParam("showPageRange", Val(imageInfo.showPageRange));
    query.addParam("imageWidth", Val(imageInfo.width));
    query.addParam("imageHeight", Val(imageInfo.height));

    //! Read-only muted-header fields + the "why" caption data. fileName is the picked file's basename; pageCount +
    //! pdfConfident drive the "Pages:" line (a non-confident PDF shows "multiple", never a guess); embeddedDpi is the
    //! image's real resolution metadata if any (0 == omit the dpi suffix); detectedDpi is the PDF's raw detected native
    //! resolution shown in the "why" caption when matched.
    //! Bare filename for the dialog header. QFileInfo (not muse io::filename) because the latter's FileInfo::fileName()
    //! splits on '/' only -- a native Windows backslash path yields a drive-letter-dropped folder path instead of the
    //! basename. QFileInfo handles both separators cross-platform (same helper already used in audiverisrunner).
    query.addParam("fileName", Val(QFileInfo(pdfPath.toQString()).fileName().toStdString()));
    query.addParam("pageCount", Val(imageInfo.pageCount));
    query.addParam("pdfConfident", Val(imageInfo.pdfConfident));
    query.addParam("embeddedDpi", Val(imageInfo.embeddedDpi));
    query.addParam("detectedDpi", Val(detected.dpi));

    //! "Options" panel data: the installed OCR languages (enumerated in C++ from Reprise's tessdata, osd excluded) as a
    //! JSON array of {code, name} for the language dropdown, plus the default option values the panel opens at. The
    //! language defaults to the configured global default (omr/ocrLanguage), not a hardcoded eng.
    query.addParam("languagesJson", Val(buildLanguagesJson()));
    query.addParam("recognizeText", Val(true));
    query.addParam("language", Val(repriseConfiguration()->ocrLanguage()));
    query.addParam("chordSymbols", Val(false));
    query.addParam("lyricsAboveStaff", Val(false));

    //! Harden the normal user close->reopen path too: if a prior Import-Scan dialog of this exact query is somehow still
    //! open on the stack, force it closed and settled before opening again, so a fast cancel->reopen can never overlap.
    //! openSync drives its own modal loop and tears the dialog down before it returns, so this is a no-op (no latency) in
    //! the common case; it is a cheap belt-and-suspenders guard.
    forceCloseAndSettle(query, nullptr);

    const RetVal<Val> dialog = interactive()->openSync(query);
    if (!dialog.ret)
    {
        return make_ret(Ret::Code::Cancel);
    }

    const QVariantMap vals = dialog.val.toQVariant().toMap();
    if (!vals.value("accepted").toBool())
    {
        return make_ret(Ret::Code::Cancel);
    }

    const QString pages = vals.value("pages").toString();
    const int dpi = vals.value("dpi").toInt();

    //! Recognition options from the dialog's "Options" panel. Absent keys fall back to the Audiveris-default values, so
    //! an older/edge dialog state can never turn OCR off by accident. These feed the SAME AudiverisRunner constant
    //! logic as the headless path (both converge on launchImport -> the progress popup -> AudiverisRunner::start).
    RecognitionOptions options;
    options.recognizeText = vals.contains("recognizeText") ? vals.value("recognizeText").toBool() : true;
    const QString langChoice = vals.value("language").toString();
    options.language = langChoice.trimmed().isEmpty() ? QString("eng") : langChoice.trimmed();
    options.chordSymbols = vals.value("chordSymbols").toBool();
    options.lyricsAboveStaff = vals.value("lyricsAboveStaff").toBool();

    //! Remember the chosen resolution so the dialog defaults to it next time.
    setScanResolution(dpi);

    launchImport(pdfPath, pages.toStdString(), dpi, options);

    return make_ok();
}

void ImportPdfController::launchImport(const muse::io::path_t& pdfPath, const std::string& pages, int dpi,
                                      const RecognitionOptions& options)
{
    //! Seed the shared progress tracker up front so an immediate reprise.get-import-progress poll already sees an
    //! active run (the popup's view-model takes over the detailed progress once it loads).
    importProgressTracker()->reportStarted(pdfPath.toStdString());

    //! Launch the recognition progress popup (non-blocking). Its view-model owns the async Audiveris run and the
    //! import of the produced MusicXML. Progress + Cancel live there. The
    //! recognition options ride along as query params -> the popup's root properties -> ImportProgressModel::load()
    //! -> AudiverisRunner::start(), which is the single place the '-constant' switches are built (DRY with headless).
    UriQuery progressQuery("musescore://reprise/importpdf/progress");
    progressQuery.addParam("pdfPath", Val(pdfPath.toQString().toStdString()));
    progressQuery.addParam("pages", Val(pages));
    progressQuery.addParam("dpi", Val(dpi));
    progressQuery.addParam("recognizeText", Val(options.recognizeText));
    progressQuery.addParam("language", Val(options.language.toStdString()));
    progressQuery.addParam("chordSymbols", Val(options.chordSymbols));
    progressQuery.addParam("lyricsAboveStaff", Val(options.lyricsAboveStaff));

    interactive()->open(progressQuery);
}

std::string ImportPdfController::buildLanguagesJson() const
{
    QJsonArray arr;

    if (m_audiverisRunner)
    {
        const std::vector<std::pair<std::string, std::string> > langs = m_audiverisRunner->installedOcrLanguages();
        for (const std::pair<std::string, std::string>& lang : langs)
        {
            QJsonObject obj;
            obj["code"] = QString::fromStdString(lang.first);
            obj["name"] = QString::fromStdString(lang.second);
            arr.append(obj);
        }
    }

    //! Never hand the dialog an empty list -- fall back to English so the dropdown always has at least one entry
    //! (e.g. if the shipped tessdata folder could not be read for some reason).
    if (arr.isEmpty())
    {
        QJsonObject obj;
        obj["code"] = "eng";
        obj["name"] = "English";
        arr.append(obj);
    }

    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)).toStdString();
}

muse::Ret ImportPdfController::preflightImageSize(const muse::io::path_t& path) const
{
    //! PDFs are rasterised by Audiveris at a chosen DPI (the quality tier), so their on-disk content has no fixed
    //! pixel count to pre-flight here. Only direct image inputs have fixed pixel dimensions.
    if (io::suffix(path) == "pdf")
    {
        return make_ok();
    }

    qint64 maxPixels = 0;

    //! TIFF dimensions still come from the in-house, plugin-independent IFD walk, which reports the LARGEST frame's pixel
    //! count (Audiveris's cap is per-sheet / per-frame). The kit now ships qtiff so QImageReader COULD read a TIFF here too
    //! (a future cleanup), but readTiffInfo also yields the page count + DPI the intake dialog needs, so it is kept. As of
    //! 0.12.11 TIFF is padded like any other image (padTiffToTemp), so judge the PADDED pixel count Audiveris will see --
    //! falling back to the original count when the border alone would exceed the cap (the runner then skips the pad for
    //! that frame), consistent with the non-TIFF branch below.
    const std::string suffix = io::suffix(path);
    const bool isTiff = suffix == "tif" || suffix == "tiff";
    if (isTiff)
    {
        QByteArray tiffData;
        QFile file(path.toQString());
        if (file.open(QIODevice::ReadOnly))
        {
            tiffData = file.read(TIFF_MAX_READ_BYTES);
            file.close();
        }

        int tiffFrames = 0;
        int tiffWidth = 0;
        int tiffHeight = 0;
        int tiffDpi = 0;   // unused here (pre-flight judges pixels, not DPI)
        const bool tiffOk = readTiffInfo(tiffData, tiffFrames, tiffWidth, tiffHeight, tiffDpi);
        if (tiffOk)
        {
            const qint64 origPixels = static_cast<qint64>(tiffWidth) * static_cast<qint64>(tiffHeight);
            const qint64 paddedPixels = static_cast<qint64>(tiffWidth + 2 * IMAGE_PAD_MARGIN)
                                        * static_cast<qint64>(tiffHeight + 2 * IMAGE_PAD_MARGIN);
            maxPixels = paddedPixels <= AUDIVERIS_MAX_PIXELS ? paddedPixels : origPixels;
        }

        LOGI() << "Reprise: image pre-flight (TIFF) largest frame " << tiffWidth << "x" << tiffHeight << " = "
               << maxPixels << " padded pixels, frames=" << tiffFrames << ", tiffOk=" << (tiffOk ? "true" : "false")
               << (maxPixels > AUDIVERIS_MAX_PIXELS ? " -> blocked oversized" : " -> passed") << ": "
               << path.toQString();
    }
    else
    {
        //! Non-TIFF image: these formats DO have a Qt plugin, so read dimensions via QImageReader. A multi-frame image
        //! is several sheets, each of which Audiveris checks independently; iterate the frames and let the largest
        //! decide. size() reads the dimensions from the header without decoding the pixels (so this stays cheap and
        //! never has to load the very image we are trying to reject). A frame whose size can't be read is skipped.
        QImageReader reader(path.toQString());
        int frames = reader.imageCount();
        if (frames < 1)
        {
            frames = 1;
        }

        for (int i = 0; i < frames; ++i)
        {
            reader.jumpToImage(i);
            const QSize size = reader.size();
            if (size.isValid())
            {
                //! Judge the PADDED pixel count Audiveris will actually see -- the always-on image pad adds a white border
                //! of IMAGE_PAD_MARGIN px on every side. BUT if the border alone would push an otherwise-OK frame past the
                //! cap, the runner skips the pad for that frame (feeds it unpadded), so fall back to the original pixel
                //! count then: this blocks only a scan that is genuinely too large on its own, never one our own border
                //! inflated over the limit. (TIFF takes the same padded judgment in its own branch above.)
                const qint64 origPixels = static_cast<qint64>(size.width()) * static_cast<qint64>(size.height());
                const qint64 paddedPixels = static_cast<qint64>(size.width() + 2 * IMAGE_PAD_MARGIN)
                                            * static_cast<qint64>(size.height() + 2 * IMAGE_PAD_MARGIN);
                const qint64 effectivePixels = paddedPixels <= AUDIVERIS_MAX_PIXELS ? paddedPixels : origPixels;
                if (effectivePixels > maxPixels)
                {
                    maxPixels = effectivePixels;
                }
            }
        }
    }

    if (maxPixels > AUDIVERIS_MAX_PIXELS)
    {
        const QString message = muse::qtrc("reprise",
                                           "This scan is too large to recognize (%1 megapixels). The maximum is "
                                           "20 megapixels (20,000,000 pixels). Please use a lower-resolution scan.")
                                .arg(QString::number(double(maxPixels) / 1000000.0, 'f', 1));
        LOGW() << "Reprise: image pre-flight blocked oversized scan: " << maxPixels << " pixels (vs "
               << AUDIVERIS_MAX_PIXELS << " max): " << path.toQString();
        return make_ret(Ret::Code::UnknownError, message.toStdString());
    }

    return make_ok();
}

ImportPdfController::ImageInfo ImportPdfController::inspectImage(const muse::io::path_t& path, bool isImage) const
{
    ImageInfo info;
    info.isImage = isImage;

    if (!isImage)
    {
        //! PDF: count pages via QPdfDocument (authoritative for all PDFs, incl. compressed-xref-stream files), with
        //! the in-house byte-scan as the fallback. Show the page range UNLESS we can CONFIDENTLY determine a single
        //! page; any uncertainty (QPdfDocument failed AND the byte-scan could not resolve it) keeps it shown so pages
        //! are never silently dropped.
        const PdfPageCountResult pc = detectPdfPageCount(path);
        info.pageCount = pc.pageCount;
        info.pdfConfident = pc.confident;
        const bool confidentSinglePage = pc.confident && pc.pageCount == 1;
        info.showPageRange = !confidentSinglePage;

        LOGI() << "Reprise intake inspect: input=PDF pdfPageCount=" << pc.pageCount
               << " confident=" << (pc.confident ? "true" : "false")
               << " willShowPageRange=" << (info.showPageRange ? "true" : "false");

        return info;
    }

    //! Image frame count + native resolution. The kit now ships qtiff, so QImageReader reads a TIFF too; we still count
    //! frames two ways and take the larger -- QImageReader's jumpToNextImage() (PNG/JPG/GIF/TIFF/...) and the in-house TIFF
    //! IFD-chain walk (readTiffInfo, kept for its page-count + DPI read) -- so a plugin hiccup can never silently hide a
    //! page. jumpToImage() is deliberately NOT used: in Qt 6 it returns false even for frame 0 of a perfectly readable
    //! image, so it always under-counts (the multi-frame pad path relies on this same gotcha).
    int rawImageCount = 0;
    int qtWidth = 0;
    int qtHeight = 0;
    {
        QImageReader reader(path.toQString());
        rawImageCount = reader.imageCount();
        const QSize size = reader.size();
        if (size.isValid())
        {
            qtWidth = size.width();
            qtHeight = size.height();
        }
    }

    int nextCount = 0;
    {
        QImageReader reader(path.toQString());
        if (reader.canRead())
        {
            nextCount = 1;
            while (reader.jumpToNextImage())
            {
                ++nextCount;
            }
        }
    }

    //! In-house TIFF read (kept despite qtiff now being present): walk the IFD chain for the real frame count + first-frame dimensions.
    const std::string suffix = io::suffix(path);
    const bool isTiff = suffix == "tif" || suffix == "tiff";
    int tiffFrames = 0;
    int tiffWidth = 0;
    int tiffHeight = 0;
    int tiffDpi = 0;
    bool tiffOk = false;
    if (isTiff)
    {
        QByteArray tiffData;
        QFile file(path.toQString());
        if (file.open(QIODevice::ReadOnly))
        {
            tiffData = file.read(TIFF_MAX_READ_BYTES);
            file.close();
        }

        tiffOk = readTiffInfo(tiffData, tiffFrames, tiffWidth, tiffHeight, tiffDpi);
    }

    //! Did EITHER method actually read this image? A TIFF whose IFD walk failed AND an empty reader count is genuinely unknown.
    const bool frameCountKnown = nextCount >= 1 || tiffOk;

    int reliable = nextCount;
    if (tiffFrames > reliable)
    {
        reliable = tiffFrames;
    }
    if (reliable < 1)
    {
        reliable = 1;
    }

    info.width = qtWidth > 0 ? qtWidth : (tiffOk ? tiffWidth : 0);
    info.height = qtHeight > 0 ? qtHeight : (tiffOk ? tiffHeight : 0);

    //! Embedded DPI for the header's resolution line, determined by PARSING THE FILE BYTES -- not by trusting Qt. For a
    //! non-TIFF image we byte-scan for real resolution metadata (PNG pHYs chunk, JPEG JFIF/EXIF density, BMP
    //! biXPelsPerMeter; GIF has none). We deliberately do NOT use QImage::dotsPerMeterX(): this Qt kit BACK-FILLS 96 dpi
    //! for a PNG with no pHYs chunk, so dotsPerMeterX() cannot distinguish "measured 96" from "no metadata" and would
    //! show a bogus (~96 dpi) as if measured. For TIFF the in-house IFD walk reads the RATIONAL tags
    //! (282/283/296). In BOTH cases embeddedDpi stays 0 when no real metadata is present; the dialog then shows the
    //! clearly-labelled "assumed" default (never a silently-invented measurement). embeddedDpi > 0 always means a value
    //! genuinely read from the file.
    if (!isTiff)
    {
        QByteArray imgData;
        QFile imgFile(path.toQString());
        if (imgFile.open(QIODevice::ReadOnly))
        {
            imgData = imgFile.read(IMAGE_META_READ_BYTES);
            imgFile.close();
        }

        info.embeddedDpi = readImageDpi(imgData, suffix);
    }
    else if (tiffOk && tiffDpi > 0)
    {
        info.embeddedDpi = tiffDpi;
    }

    if (isTiff && !frameCountKnown)
    {
        //! Could not determine the TIFF frame count by either method -> SHOW the page range (safe: never hide pages),
        //! mirroring the PDF "uncertain -> show" fallback. pageCount 0 == unknown.
        info.pageCount = 0;
        info.showPageRange = true;
    }
    else
    {
        info.pageCount = reliable;
        info.showPageRange = reliable > 1;
    }

    //! Resolved DPI the dialog's Resolution line will show: the measured embeddedDpi when present, else the labelled
    //! "assumed" default (300, uniform across every image type). dpiSource proves (headlessly, from the log) which path
    //! fired -- measured (real metadata found in the file) vs assumed (none present). For images it is measured|assumed.
    const bool dpiMeasured = info.embeddedDpi > 0;
    const int resolvedDpi = dpiMeasured ? info.embeddedDpi : DEFAULT_IMAGE_DPI;

    LOGI() << "Reprise intake inspect: input=image rawImageCount=" << rawImageCount
           << " nextCount=" << nextCount << " tiffOk=" << (tiffOk ? "true" : "false")
           << " tiffFrames=" << tiffFrames
           << " pageCount=" << info.pageCount << " isMultiPage=" << (info.pageCount > 1 ? "true" : "false")
           << " willShowPageRange=" << (info.showPageRange ? "true" : "false")
           << " resolution=" << info.width << "x" << info.height
           << " embeddedDpi=" << info.embeddedDpi
           << " resolvedDpi=" << resolvedDpi
           << " dpiSource=" << (dpiMeasured ? "measured" : "assumed");

    return info;
}

muse::RetVal<GetImportProgressCommand::Output> ImportPdfController::getImportProgress() const
{
    GetImportProgressCommand::Output output;

    const ImportProgressSnapshot snap = importProgressTracker()->snapshot();
    output.active = snap.active;
    output.status = snap.status;
    output.phase = snap.phase;
    output.percent = snap.percent;
    output.page = snap.page;
    output.totalPages = snap.totalPages;
    output.pdfPath = snap.pdfPath;

    return muse::RetVal<GetImportProgressCommand::Output>::make_ok(output);
}

muse::RetVal<GetImportErrorsCommand::Output> ImportPdfController::getImportErrors() const
{
    GetImportErrorsCommand::Output output;

    const ImportProgressSnapshot snap = importProgressTracker()->snapshot();
    output.hasError = snap.hasError;
    output.error = snap.error;
    output.details = snap.details;

    return muse::RetVal<GetImportErrorsCommand::Output>::make_ok(output);
}

muse::RetVal<GetProofingMapCommand::Output> ImportPdfController::getProofingMap() const
{
    GetProofingMapCommand::Output output;

    if (!proofingMapService()->hasMap())
    {
        output.hasMap = false;
        output.pages = "[]";
        output.measures = "[]";
        output.notes = "[]";
        return muse::RetVal<GetProofingMapCommand::Output>::make_ok(output);
    }

    const ProofingMap& map = proofingMapService()->map();
    output.hasMap = true;
    output.sourcePath = map.sourcePath;
    output.measureCount = map.measureCount;
    output.pageCount = static_cast<int>(map.pages.size());
    output.noteCount = static_cast<int>(map.notes.size());

    QJsonArray pages;
    for (const ProofingPageInfo& p : map.pages)
    {
        QJsonObject obj;
        obj["index"] = p.index;
        obj["w"] = p.imageWidth;
        obj["h"] = p.imageHeight;
        obj["dpi"] = p.dpi;
        pages.append(obj);
    }
    output.pages = QString::fromUtf8(QJsonDocument(pages).toJson(QJsonDocument::Compact)).toStdString();

    QJsonArray measures;
    for (const ProofingMeasureRect& m : map.measures)
    {
        QJsonObject obj;
        obj["gm"] = m.globalMeasureIndex;
        obj["page"] = m.pageIndex;
        obj["x"] = m.x;
        obj["y"] = m.y;
        obj["w"] = m.w;
        obj["h"] = m.h;
        measures.append(obj);
    }
    output.measures = QString::fromUtf8(QJsonDocument(measures).toJson(QJsonDocument::Compact)).toStdString();

    QJsonArray notes;
    for (const ProofingNoteBox& b : map.notes)
    {
        QJsonObject obj;
        obj["gm"] = b.key.globalMeasureIndex;
        obj["staff"] = b.key.staffIdx;
        obj["tick"] = b.key.tickInMeasure;
        obj["voice"] = b.key.voice;
        obj["step"] = b.key.step;
        obj["octave"] = b.key.octave;
        obj["page"] = b.pageIndex;
        obj["x"] = b.x;
        obj["y"] = b.y;
        obj["w"] = b.w;
        obj["h"] = b.h;
        notes.append(obj);
    }
    output.notes = QString::fromUtf8(QJsonDocument(notes).toJson(QJsonDocument::Compact)).toStdString();

    return muse::RetVal<GetProofingMapCommand::Output>::make_ok(output);
}

std::string ImportPdfController::installedPacksJson() const
{
    QJsonArray arr;
    for (const LanguagePackInfo& pack : languagePackService()->installedPacks())
    {
        QJsonObject obj;
        obj["code"] = QString::fromStdString(pack.code);
        obj["name"] = QString::fromStdString(pack.name);
        arr.append(obj);
    }

    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)).toStdString();
}

muse::RetVal<ListLanguagesCommand::Output> ImportPdfController::listLanguages() const
{
    ListLanguagesCommand::Output output;

    output.installedJson = installedPacksJson();

    QJsonArray available;
    for (const LanguagePackInfo& pack : languagePackService()->availablePacks())
    {
        QJsonObject obj;
        obj["code"] = QString::fromStdString(pack.code);
        obj["name"] = QString::fromStdString(pack.name);
        available.append(obj);
    }
    output.availableJson = QString::fromUtf8(QJsonDocument(available).toJson(QJsonDocument::Compact)).toStdString();

    output.defaultLanguage = repriseConfiguration()->ocrLanguage();

    return muse::RetVal<ListLanguagesCommand::Output>::make_ok(output);
}

muse::RetVal<AddLanguageCommand::Output> ImportPdfController::addLanguage(const AddLanguageCommand& cmd)
{
    AddLanguageCommand::Output output;

    const muse::Ret ret = languagePackService()->addOrUpdatePack(cmd.code);
    output.ok = ret;
    if (!ret)
    {
        output.error = ret.text();
    }
    output.installedJson = installedPacksJson();

    return muse::RetVal<AddLanguageCommand::Output>::make_ok(output);
}

muse::RetVal<RemoveLanguageCommand::Output> ImportPdfController::removeLanguage(const RemoveLanguageCommand& cmd)
{
    RemoveLanguageCommand::Output output;

    const muse::Ret ret = languagePackService()->removePack(cmd.code);
    output.ok = ret;
    if (!ret)
    {
        output.error = ret.text();
    }
    output.installedJson = installedPacksJson();

    return muse::RetVal<RemoveLanguageCommand::Output>::make_ok(output);
}

void ImportPdfController::forceCloseAndSettle(const muse::UriQuery& query, QQuickWindow* known) const
{
    //! Bounded GUI-thread event pump.
    auto pump = [](int ms)
    {
        QEventLoop loop;
        QTimer pumpTimer;
        pumpTimer.setSingleShot(true);
        QObject::connect(&pumpTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        pumpTimer.start(ms);
        loop.exec();
    };

    //! QPointer goes null exactly when the window object is destroyed, so it is the true "fully torn down" signal.
    QPointer<QQuickWindow> guard(known);

    const int timeoutMs = 5000;
    bool acted = false;

    //! Phase 1: close the window directly until it leaves the interactive stack. interactive()->close(query) is a no-op
    //! for a fire-and-forget async-opened dialog (verified from the log: isOpened stays true through a full pump), so we
    //! close the QQuickWindow itself -- that fires Hide -> WindowView::onHidden -> closed -> Interactive::onClose, which
    //! pops it off m_stack synchronously.
    //! Gate on the URI-level isOpened (params IGNORED), NOT the full UriQuery: a dialog is registered on the interactive
    //! stack under the EXACT query it was opened with -- params included -- but a caller may close by a reconstructed query
    //! that omits them (closeProgressPopup rebuilds a bare "importpdf/progress" while the popup was opened with pdfPath /
    //! dpi / recognition-option params). UriQuery::operator== compares params too, so a full-query isOpened() misses the
    //! registered popup and the close never fires (0.12.13: that left the modeless popup lingering after Done). Matching on
    //! the URI is sufficient here -- each of these dialog URIs is single-instance -- and keeps the close exactly-once: it
    //! stops the instant the object leaves the stack, so Interactive::onClose always finds a valid objectId (no SIGSEGV).
    QElapsedTimer t1;
    t1.start();
    while (t1.elapsed() < timeoutMs && interactive()->isOpened(query.uri()).val)
    {
        acted = true;
        QQuickWindow* w = !guard.isNull() ? guard.data() : qobject_cast<QQuickWindow*>(interactive()->topWindow());
        if (w)
        {
            w->close();
        }

        pump(30);
    }

    //! Phase 2: when we hold the window pointer, wait for the deferred QML obj.destroy() to actually destroy it, so no
    //! stale teardown can re-enter Interactive::onClose during the NEXT open (the original second-call SIGSEGV).
    QElapsedTimer t2;
    t2.start();
    while (!guard.isNull() && t2.elapsed() < timeoutMs)
    {
        acted = true;
        pump(30);
    }

    //! Only drain the queued onClosed callback when we actually closed something, so the common case (nothing of ours
    //! open) adds no latency.
    if (acted)
    {
        pump(60);
    }
}

void ImportPdfController::closeProgressPopup()
{
    //! Close the recognition progress popup on the success auto-close path (see init()'s popupCloseRequested subscription
    //! and ImportProgressModel's masterNotationChanged handler). The popup was opened fire-and-forget with
    //! interactive()->open(progressQuery) in launchImport(); it registered its own QQuickWindow via the tracker. We close
    //! that specific window + settle it. forceCloseAndSettle closes the window ONLY while the dialog is still on the
    //! interactive stack (isOpened(query) true), so the resulting Interactive::onClose always finds a valid objectId --
    //! never the double-delivery that the old self-close (root.hide() racing the project-open stack churn) tripped into a
    //! SIGSEGV. Passing the known window makes it precise (it does not touch the just-opened notation window), and the
    //! pump-until-destroyed drains any racing teardown before returning.
    QQuickWindow* win = qobject_cast<QQuickWindow*>(importProgressTracker()->progressWindow());

    //! Guard: a null handle means the popup is already gone (closed by some other path, QPointer auto-nulled). Do NOT
    //! fall through to forceCloseAndSettle(query, nullptr) -- with no known window it would act on interactive()->topWindow(),
    //! which by now is the just-opened NOTATION window. There is simply nothing to close here, so return.
    if (!win)
    {
        return;
    }

    LOGI() << "Reprise: auto-closing the recognition progress popup (settled close of its own window)";

    const UriQuery progressQuery("musescore://reprise/importpdf/progress");
    forceCloseAndSettle(progressQuery, win);

    //! Clear the stale handle (the window is destroyed now); the next import registers a fresh one.
    importProgressTracker()->setProgressWindow(nullptr);
}

void ImportPdfController::showRecognitionError()
{
    //! FIX 2: a recognition failed. First close + settle the progress popup (it is still open showing its last running
    //! state), reusing the SAME proven forceCloseAndSettle path as the success auto-close so there is no SIGSEGV on the
    //! handoff. closeProgressPopup is a no-op if the popup is already gone (QPointer auto-nulled). Then open the dedicated
    //! "Recognition failed" window with the message + details captured in the snapshot by the model's reportFinished.
    const ImportProgressSnapshot snap = importProgressTracker()->snapshot();

    closeProgressPopup();

    UriQuery query("musescore://reprise/importpdf/error");
    query.addParam("message", Val(snap.error));
    query.addParam("details", Val(snap.details));
    query.addParam("logFilePath", Val(snap.logFilePath));

    //! Harden against an edge re-entry: force any prior error window of this URI closed + settled before opening again.
    forceCloseAndSettle(query, nullptr);

    interactive()->open(query);
}

void ImportPdfController::showImportNotice()
{
    //! Some pages were skipped (or the survivors were split) on an otherwise-successful import. DISMISS the progress popup
    //! first (settled), regardless of the close-after-import checkbox, THEN open the standalone "Import finished" notice --
    //! so the notice is the SINGLE window: never stacked on top of a still-open progress popup (the unticked-checkbox
    //! stacking bug), and never racing a separate checkbox-governed auto-close of this same popup (the ticked-checkbox
    //! suppression bug, which tore the notice down). This is the exact ordered close-then-open showRecognitionError uses.
    //! closeProgressPopup is a no-op if the popup is already gone. The notice is populated from the app-lifetime snapshot
    //! the model filled via reportPartialImport (the per-page report + multi-movement flag), which survives the popup
    //! view-model's teardown -- so the snapshot is captured by value BEFORE the close pumps the popup to destruction.
    const ImportProgressSnapshot snap = importProgressTracker()->snapshot();

    closeProgressPopup();

    UriQuery query("musescore://reprise/importpdf/notice");
    query.addParam("skippedReport", Val(snap.partialSkippedReport));
    query.addParam("multiMovement", Val(snap.partialMultiMovement));
    query.addParam("textDropped", Val(snap.partialTextDropped));
    query.addParam("genericParts", Val(snap.partialGenericParts));

    //! Harden against an edge re-entry: force any prior notice window of this URI closed + settled before opening again.
    forceCloseAndSettle(query, nullptr);

    interactive()->open(query);
}

muse::RetVal<CaptureImportDialogCommand::Output> ImportPdfController::captureImportDialog(const CaptureImportDialogCommand& cmd)
{
    using RV = muse::RetVal<CaptureImportDialogCommand::Output>;

    CaptureImportDialogCommand::Output out;

    if (cmd.path.empty())
    {
        out.error = "no path given";
        return RV::make_ok(out);
    }

    const io::path_t filePath(cmd.path);
    if (!fileSystem()->exists(filePath))
    {
        out.error = "scan not found: " + cmd.path;
        return RV::make_ok(out);
    }

    //! Writable Reprise screenshots dir: <userAppDataPath>/screenshots (same convention + fixed overwrite file as the
    //! reprise.capture-screenshot tool), with an optional per-call override.
    const io::path_t dir = globalConfiguration()->userAppDataPath() + "/screenshots";
    fileSystem()->makePath(dir);
    const io::path_t absPath = cmd.outPath.empty() ? (dir + "/mcp-import-dialog.png") : io::path_t(cmd.outPath);

    //! An oversized image never reaches the intake dialog in the interactive flow (it is blocked up front), so there is
    //! nothing to capture -- report the same reason rather than render a dialog the user would never see.
    const muse::Ret sizeRet = preflightImageSize(filePath);
    if (!sizeRet)
    {
        out.error = "scan too large: " + sizeRet.text();
        return RV::make_ok(out);
    }

    //! Run the SAME dialog-prep the interactive path uses so the rendered dialog is identical to what a user sees.
    const bool isImage = io::suffix(filePath) != "pdf";
    const PdfResolutionResult detected = detectPdfResolution(filePath);
    const int preselectTier = detected.matched ? snapToTier(detected.dpi) : snapToTier(scanResolution());
    const ImageInfo imageInfo = inspectImage(filePath, isImage);

    //! Build the identical UriQuery the interactive path passes to openSync (see importPdf()).
    UriQuery query("musescore://reprise/importpdf/options");
    query.addParam("pages", Val(std::string("all")));
    query.addParam("tier", Val(preselectTier));
    query.addParam("matched", Val(detected.matched));
    query.addParam("isImage", Val(isImage));
    query.addParam("showPageRange", Val(imageInfo.showPageRange));
    query.addParam("imageWidth", Val(imageInfo.width));
    query.addParam("imageHeight", Val(imageInfo.height));
    //! Bare filename for the dialog header -- QFileInfo (handles both separators), not muse io::filename (splits on '/'
    //! only, so a native Windows backslash path would show a drive-letter-dropped folder path). Matches importPdf().
    query.addParam("fileName", Val(QFileInfo(filePath.toQString()).fileName().toStdString()));
    query.addParam("pageCount", Val(imageInfo.pageCount));
    query.addParam("pdfConfident", Val(imageInfo.pdfConfident));
    query.addParam("embeddedDpi", Val(imageInfo.embeddedDpi));
    query.addParam("detectedDpi", Val(detected.dpi));

    //! Same "Options" panel data the interactive path passes, so the captured dialog renders the language dropdown and
    //! the option defaults identically to what a user sees (language defaults to the configured omr/ocrLanguage).
    query.addParam("languagesJson", Val(buildLanguagesJson()));
    query.addParam("recognizeText", Val(true));
    query.addParam("language", Val(repriseConfiguration()->ocrLanguage()));
    query.addParam("chordSymbols", Val(false));
    query.addParam("lyricsAboveStaff", Val(false));

    //! Dev capture only: optionally open with the Options disclosure expanded, so the expanded panel can be grabbed
    //! (the capture tool renders the dialog's default state without interaction). The shipped dialog default stays
    //! collapsed -- this param just overrides the initial state for the screenshot.
    query.addParam("optionsExpanded", Val(cmd.expandOptions));

    //! Guard the reopen race: if a PRIOR capture call's dialog is somehow still open on the stack (same query), force it
    //! closed and fully settled before opening again, so this open never overlaps a lingering predecessor. No-op (no
    //! delay) on the first call, when nothing matching is open.
    forceCloseAndSettle(query, nullptr);

    //! Open the dialog ASYNC (never openSync): openSync would block this GUI-thread handler inside the dialog's nested
    //! modal loop. The async open returns immediately; we drive our OWN nested event loop below to let the dialog window
    //! be created, exposed and rendered -- then grab it. The returned promise is intentionally discarded (fire-and-forget,
    //! same pattern as launchImport): we force-close the dialog ourselves, so its accept/cancel result is never needed.
    interactive()->open(query);

    //! Local nested-event-loop pump. Qt supports nested processEvents/exec; this lets the posted QML-open + expose events
    //! run without returning control all the way out of the handler.
    auto spin = [](int ms)
    {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(ms);
        loop.exec();
    };

    //! Wait (bounded) for the dialog's QQuickWindow to exist, be the top window, and be exposed (so grabWindow() renders
    //! real content). isOpened(query) confirms OUR dialog is on the interactive stack, so topWindow() returns it.
    QQuickWindow* dlg = nullptr;
    QElapsedTimer timer;
    timer.start();
    const int timeoutMs = 5000;
    while (timer.elapsed() < timeoutMs)
    {
        spin(60);

        if (!interactive()->isOpened(query).val)
        {
            continue;
        }

        QWindow* tw = interactive()->topWindow();
        QQuickWindow* qw = qobject_cast<QQuickWindow*>(tw);
        if (qw && qw->isVisible())
        {
            qw->raise();
            qw->requestActivate();
            if (qw->isExposed())
            {
                dlg = qw;
                break;
            }
        }
    }

    if (!dlg)
    {
        //! Never exposed in time -- force-close whatever may have opened (the top dialog for this query) and fully settle
        //! it, so a leftover does not race a later reopen, then report a clean failure.
        forceCloseAndSettle(query, nullptr);
        out.error = "import dialog did not become visible in time";
        return RV::make_ok(out);
    }

    //! A couple more pumps so the first frame is fully rendered before the grab.
    spin(120);

    QImage img = dlg->grabWindow();

    //! Close the dialog and FULLY settle it before returning -- the core of the second-call-crash fix. Closing the
    //! grabbed QQuickWindow directly (forceCloseAndSettle) pops it off the interactive stack and drains its deferred
    //! teardown, so the NEXT capture call opens from a clean state. (interactive()->close(query) does NOT close this
    //! fire-and-forget async-opened dialog -- it would linger on the stack and the reopen would race onClose -> SIGSEGV.)
    forceCloseAndSettle(query, dlg);

    if (img.isNull())
    {
        out.error = "grab failed (dialog window not renderable?)";
        return RV::make_ok(out);
    }

    if (!img.save(absPath.toQString(), "PNG"))
    {
        out.error = "failed to save PNG to " + absPath.toStdString();
        return RV::make_ok(out);
    }

    out.ok = true;
    out.path = absPath.toStdString();
    return RV::make_ok(out);
}

muse::RetVal<CapturePreferencesCommand::Output> ImportPdfController::capturePreferences(const CapturePreferencesCommand& cmd)
{
    using RV = muse::RetVal<CapturePreferencesCommand::Output>;

    CapturePreferencesCommand::Output out;

    //! Writable Reprise screenshots dir (same convention + fixed overwrite file as the other capture tools), with an
    //! optional per-call override.
    const io::path_t dir = globalConfiguration()->userAppDataPath() + "/screenshots";
    fileSystem()->makePath(dir);
    const io::path_t absPath = cmd.outPath.empty() ? (dir + "/mcp-preferences.png") : io::path_t(cmd.outPath);

    //! Open the Preferences dialog ON the Reprise page: the dialog's QML root has a currentPageId property the URI
    //! param maps to, so it loads straight to our page (where the language-pack UI lives). Async open (never openSync),
    //! same as captureImportDialog, so this GUI-thread handler is not blocked inside the dialog's modal loop.
    UriQuery query("muse://preferences");
    query.addParam("currentPageId", Val(std::string("reprise")));

    forceCloseAndSettle(query, nullptr);

    interactive()->open(query);

    auto spin = [](int ms)
    {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(ms);
        loop.exec();
    };

    //! Wait (bounded) for the Preferences dialog window to exist, be top, and be exposed before grabbing.
    QQuickWindow* dlg = nullptr;
    QElapsedTimer timer;
    timer.start();
    const int timeoutMs = 8000;
    while (timer.elapsed() < timeoutMs)
    {
        spin(80);

        if (!interactive()->isOpened(query).val)
        {
            continue;
        }

        QWindow* tw = interactive()->topWindow();
        QQuickWindow* qw = qobject_cast<QQuickWindow*>(tw);
        if (qw && qw->isVisible())
        {
            qw->raise();
            qw->requestActivate();
            if (qw->isExposed())
            {
                dlg = qw;
                break;
            }
        }
    }

    if (!dlg)
    {
        forceCloseAndSettle(query, nullptr);
        out.error = "preferences dialog did not become visible in time";
        return RV::make_ok(out);
    }

    //! A few more pumps so the page content (and our section) is fully laid out before the grab.
    spin(200);

    QImage img = dlg->grabWindow();

    forceCloseAndSettle(query, dlg);

    if (img.isNull())
    {
        out.error = "grab failed (preferences window not renderable?)";
        return RV::make_ok(out);
    }

    if (!img.save(absPath.toQString(), "PNG"))
    {
        out.error = "failed to save PNG to " + absPath.toStdString();
        return RV::make_ok(out);
    }

    out.ok = true;
    out.path = absPath.toStdString();
    return RV::make_ok(out);
}

muse::RetVal<NavigateCommand::Output> ImportPdfController::navigate(const NavigateCommand& cmd)
{
    using RV = muse::RetVal<NavigateCommand::Output>;

    NavigateCommand::Output out;

    //! Resolve the named screen to its app-shell URI. Home-page sections mirror AppshellCommandsController::openExtensions
    //! ("musescore://home?section=plugins"); the section names ("scores"/"plugins"/"learn") are defined in HomePage.qml.
    //! The notation page URI is registered in appshellmodule.cpp (also appshelltypes.h NOTATION_URI).
    std::string uri;
    if (cmd.target == "scans")
    {
        uri = "musescore://home?section=scores";
    }
    else if (cmd.target == "plugins")
    {
        uri = "musescore://home?section=plugins";
    }
    else if (cmd.target == "learn")
    {
        uri = "musescore://home?section=learn";
    }
    else if (cmd.target == "home")
    {
        uri = "musescore://home";
    }
    else if (cmd.target == "notation")
    {
        uri = "musescore://notation";
    }
    else
    {
        out.error = "unknown target '" + cmd.target + "' (expected 'scans', 'plugins', 'learn', 'home' or 'notation')";
        return RV::make_ok(out);
    }

    //! In-process navigation on the Qt GUI thread -- no foreground activation, no synthetic input. Fire-and-forget async
    //! open (the returned promise is intentionally discarded): the app-shell page swap needs no result here.
    interactive()->open(UriQuery(uri));

    out.ok = true;
    out.page = uri;
    return RV::make_ok(out);
}

muse::RetVal<ProofingDebugGotoCommand::Output> ImportPdfController::proofingDebugGoto(const ProofingDebugGotoCommand& cmd)
{
    using RV = muse::RetVal<ProofingDebugGotoCommand::Output>;

    ProofingDebugGotoCommand::Output out;
    out.measure = cmd.measure;

    if (cmd.measure < 0)
    {
        out.error = "measure index must be >= 0";
        return RV::make_ok(out);
    }

    const notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (!notation)
    {
        out.error = "no current notation (open or import a score first)";
        return RV::make_ok(out);
    }

    engraving::Score* score = notation->elements() ? notation->elements()->msScore() : nullptr;
    if (!score)
    {
        out.error = "current notation has no score";
        return RV::make_ok(out);
    }

    //! Walk to the Nth Measure positionally (pickup/anacrusis = index 0) -- the exact convention ProofingMapBuilder uses
    //! to assign globalMeasureIndex and MeasureBase::measureIndex() uses to read it back, so the three stay aligned.
    engraving::Measure* m = score->firstMeasure();
    int32_t idx = 0;
    while (m && idx < cmd.measure)
    {
        m = m->nextMeasure();
        ++idx;
    }

    if (!m)
    {
        out.error = "measure index out of range (score has fewer measures)";
        return RV::make_ok(out);
    }

    //! Select the measure. This fires the live selectionChanged the follow band observes, so the band scrolls to +
    //! highlights the matching original measure -- exactly the behaviour a headless capture then verifies.
    notation->interaction()->select({ m }, engraving::SelectType::SINGLE);

    //! Scroll the EDITOR viewport to the measure too (as the navigator's jumpTo does), so this dev tool exercises the full
    //! select + showItem path -- without this the on-screen notation canvas keeps a stale frame on a programmatic select.
    notation->interaction()->showItem(m);
    out.ok = true;

    //! Report the map rect for this global index, so the returned geometry can be checked against the band capture AND
    //! against reprise.get-proofing-map (proving notation current-measure index == map globalMeasureIndex).
    if (proofingMapService()->hasMap())
    {
        const ProofingMap& map = proofingMapService()->map();
        out.measureCount = map.measureCount;
        for (const ProofingMeasureRect& r : map.measures)
        {
            if (r.globalMeasureIndex == cmd.measure)
            {
                out.found = true;
                out.gm = r.globalMeasureIndex;
                out.page = r.pageIndex;
                out.x = r.x;
                out.y = r.y;
                out.w = r.w;
                out.h = r.h;
                break;
            }
        }
    }

    return RV::make_ok(out);
}

muse::RetVal<ProofingDebugRegularizeCommand::Output> ImportPdfController::proofingDebugRegularize(const ProofingDebugRegularizeCommand& cmd)
{
    using RV = muse::RetVal<ProofingDebugRegularizeCommand::Output>;

    ProofingDebugRegularizeCommand::Output out;
    out.measure = cmd.measure;

    if (cmd.measure < 0)
    {
        out.error = "measure index must be >= 0";
        return RV::make_ok(out);
    }

    const notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (!notation || !notation->undoStack())
    {
        out.error = "no current notation (open or import a score first)";
        return RV::make_ok(out);
    }

    engraving::Score* score = notation->elements() ? notation->elements()->msScore() : nullptr;
    if (!score)
    {
        out.error = "current notation has no score";
        return RV::make_ok(out);
    }

    //! Walk to the Nth Measure positionally (pickup = 0) -- the same convention as proofingDebugGoto / the navigator.
    engraving::Measure* m = score->firstMeasure();
    int32_t idx = 0;
    while (m && idx < cmd.measure)
    {
        m = m->nextMeasure();
        ++idx;
    }

    if (!m)
    {
        out.error = "measure index out of range (score has fewer measures)";
        return RV::make_ok(out);
    }

    out.ok = true;
    out.before = m->ticks().toString().toStdString();
    out.timesig = m->timesig().toString().toStdString();

    //! SAFE-ONLY, mirroring BadTimingNavigatorModel::regularize: extend ONLY an under-full bar. An over-full / exactly-
    //! nominal measure is refused (no mutation) so adjustToLen can never truncate notes here.
    if (m->ticks() < m->timesig())
    {
        notation->undoStack()->prepareChanges(muse::TranslatableString("reprise", "Regularize measure"));
        m->adjustToLen(m->timesig());
        notation->undoStack()->commitChanges();

        out.regularized = true;

        //! Re-resolve the measure at the same tick after the edit (adjustToLen rebuilds measures) to report the new length.
        engraving::Measure* after = score->tick2measure(m->tick());
        out.after = (after ? after->ticks() : m->ticks()).toString().toStdString();

        if (notation->interaction() && after)
        {
            notation->interaction()->select({ after }, engraving::SelectType::SINGLE);
            notation->interaction()->showItem(after);
        }
    }
    else
    {
        //! Not under-full: unchanged. after == before. (An over-full bar is left for the user to fix by hand.)
        out.after = out.before;
    }

    return RV::make_ok(out);
}

muse::RetVal<ProofingDebugSelectNoteCommand::Output> ImportPdfController::proofingDebugSelectNote(const ProofingDebugSelectNoteCommand& cmd)
{
    using RV = muse::RetVal<ProofingDebugSelectNoteCommand::Output>;

    ProofingDebugSelectNoteCommand::Output out;
    out.measure = cmd.measure;
    out.staff = cmd.staff;
    out.index = cmd.index;

    if (cmd.measure < 0 || cmd.staff < 0 || cmd.index < 0)
    {
        out.error = "measure, staff and index must be >= 0";
        return RV::make_ok(out);
    }

    const notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (!notation)
    {
        out.error = "no current notation (open or import a score first)";
        return RV::make_ok(out);
    }

    engraving::Score* score = notation->elements() ? notation->elements()->msScore() : nullptr;
    if (!score)
    {
        out.error = "current notation has no score";
        return RV::make_ok(out);
    }

    //! Walk to the Nth Measure positionally (pickup/anacrusis = index 0), matching ProofingMapBuilder's gm convention.
    engraving::Measure* m = score->firstMeasure();
    int32_t idx = 0;
    while (m && idx < cmd.measure)
    {
        m = m->nextMeasure();
        ++idx;
    }

    if (!m)
    {
        out.error = "measure index out of range (score has fewer measures)";
        return RV::make_ok(out);
    }

    //! Collect every note on the requested staff in this measure, in tick (segment) order then within-chord order --
    //! the same stable order a user would arrow through, so `index` is reproducible.
    const int32_t firstTrack = cmd.staff * static_cast<int32_t>(engraving::VOICES);
    const int32_t lastTrack = firstTrack + static_cast<int32_t>(engraving::VOICES) - 1;

    std::vector<engraving::Note*> notes;
    for (engraving::Segment* seg = m->first(engraving::SegmentType::ChordRest); seg;
         seg = seg->next(engraving::SegmentType::ChordRest))
    {
        for (int32_t track = firstTrack; track <= lastTrack; ++track)
        {
            engraving::EngravingItem* el = seg->element(track);
            if (el && el->isChord())
            {
                for (engraving::Note* n : engraving::toChord(el)->notes())
                {
                    notes.push_back(n);
                }
            }
        }
    }

    out.noteCount = static_cast<int>(notes.size());

    if (cmd.index >= static_cast<int32_t>(notes.size()))
    {
        out.error = "note index out of range for that measure+staff";
        return RV::make_ok(out);
    }

    engraving::Note* note = notes[cmd.index];
    if (!note || !note->chord() || !note->chord()->measure())
    {
        out.error = "selected note has no chord/measure";
        return RV::make_ok(out);
    }

    //! Select the note. This fires the live selectionChanged the floating magnifier observes, so it zooms + highlights
    //! the matching original head -- exactly what a headless capture then verifies.
    notation->interaction()->select({ note }, engraving::SelectType::SINGLE);
    out.ok = true;
    out.found = true;

    //! Derive the Phase B1 runtime key straight from the Note (the magnifier does the identical derivation). staffIdx is
    //! read from the Note (so a cross-staff note reports its true staff, not the requested one).
    const engraving::Measure* nm = note->chord()->measure();
    const int32_t staffIdx = static_cast<int32_t>(note->staffIdx());
    const int32_t tickInMeasure = (note->tick() - nm->tick()).ticks();
    const int32_t step = engraving::tpc2step(note->tpc());
    const int32_t octave = note->octave();

    out.gm = nm->measureIndex();
    out.staff = staffIdx;
    out.tick = tickInMeasure;
    out.step = step;
    out.octave = octave;
    out.voice = static_cast<int>(note->voice());

    static const char* k_step[7] = { "C", "D", "E", "F", "G", "A", "B" };
    out.noteName = ((step >= 0 && step < 7) ? std::string(k_step[step]) : std::string("?")) + std::to_string(octave);

    //! Report whether the map holds a head box for this key (+ its geometry), or fall back to the measure rect -- the
    //! same decision the magnifier makes, so the capture and this output agree on note vs measure-level.
    if (proofingMapService()->hasMap())
    {
        const ProofingNoteBox* box = proofingMapService()->noteBox(out.gm, staffIdx, tickInMeasure, step, octave);
        if (box)
        {
            out.hasBox = true;
            out.isFallback = false;
            out.page = box->pageIndex;
            out.x = box->x;
            out.y = box->y;
            out.w = box->w;
            out.h = box->h;
        }
        else
        {
            out.hasBox = false;
            out.isFallback = true;
            const ProofingMap& map = proofingMapService()->map();
            for (const ProofingMeasureRect& r : map.measures)
            {
                if (r.globalMeasureIndex == out.gm)
                {
                    out.page = r.pageIndex;
                    out.x = r.x;
                    out.y = r.y;
                    out.w = r.w;
                    out.h = r.h;
                    break;
                }
            }
        }
    }

    return RV::make_ok(out);
}

muse::RetVal<ProofingDebugSetZoomCommand::Output> ImportPdfController::proofingDebugSetZoom(const ProofingDebugSetZoomCommand& cmd)
{
    using RV = muse::RetVal<ProofingDebugSetZoomCommand::Output>;

    ProofingDebugSetZoomCommand::Output out;

    //! Resolve which live view-model to drive. The band + magnifier models self-register with IProofingViewControl on
    //! their load(), so a panel that is not currently open (no proofing view instantiated) resolves to nullptr.
    QObject* model = nullptr;
    if (cmd.panel == "band")
    {
        model = proofingViewControl()->bandModel();
    }
    else if (cmd.panel == "magnifier")
    {
        model = proofingViewControl()->magnifierModel();
    }
    else
    {
        out.error = "unknown panel '" + cmd.panel + "' (expected 'band' or 'magnifier')";
        return RV::make_ok(out);
    }

    if (!model)
    {
        out.error = "no " + cmd.panel + " proofing panel is open";
        return RV::make_ok(out);
    }

    //! Drive the model's userZoom Q_PROPERTY through the Qt property system: its WRITE accessor (setUserZoom) clamps the
    //! value to [0.5, 4.0] and emits userZoomChanged (the QML re-scales + re-centers). Reading the property straight back
    //! yields the CLAMPED applied value, so a caller that asked for an out-of-range factor sees exactly what the panel
    //! now shows -- no concrete-type coupling to the QML view-model needed.
    model->setProperty("userZoom", QVariant(cmd.factor));
    out.zoom = model->property("userZoom").toDouble();
    out.ok = true;

    return RV::make_ok(out);
}

muse::io::path_t ImportPdfController::defaultImportPdfDir() const
{
    const io::path_t dir = repriseConfiguration()->lastImportPdfDir();
    //! The stored value is only usable if it is a real ABSOLUTE directory. A non-absolute value (e.g. "." left by a
    //! relative/MCP import) resolves against the process CWD (build.release) -- never the user's folder -- so treat it as
    //! empty and fall through to Documents. QFileInfo handles BOTH separators (muse io::* splits on '/' only).
    //! We deliberately DO NOT gate on fileSystem()->exists() here: QFileInfo::exists() returns false for a OneDrive
    //! Files-On-Demand DEHYDRATED placeholder directory (reparse-point target not synchronously resolvable) even while the
    //! folder is present and navigable, which wrongly reset the picker to Documents every launch. Pass any absolute
    //! remembered dir straight to the native dialog -- it opens there when reachable and falls back on its own for a
    //! genuinely-deleted path. (Supersedes the 0.13.1 exists() hard-gate; keeps the 0.12.14 absolute-path pin.)
    if (!dir.empty() && QFileInfo(dir.toQString()).isAbsolute())
    {
        return dir;
    }

    //! Empty/first-run fallback: the user's Scores folder -- IProjectConfiguration::userProjectsPath(), the SAME location
    //! imported scores auto-save to as .repr (importprogressmodel.cpp) and the same default File->Open uses. So "New Scan"
    //! opens where the user's projects live, not Documents. userProjectsPath() resolves per-OS (Documents/<appName>/Scores)
    //! and is never the process CWD; it returns its registered default when unset, so it is effectively never empty.
    return projectConfiguration()->userProjectsPath();
}

void ImportPdfController::rememberImportPdfDir(const muse::io::path_t& pdfPath)
{
    if (pdfPath.empty())
    {
        return;
    }

    repriseConfiguration()->setLastImportPdfDir(io::dirpath(pdfPath));
}

int ImportPdfController::scanResolution() const
{
    return repriseConfiguration()->scanResolution();
}

void ImportPdfController::setScanResolution(int dpi)
{
    repriseConfiguration()->setScanResolution(dpi);
}
