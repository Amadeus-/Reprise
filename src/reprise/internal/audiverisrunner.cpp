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

#include "audiverisrunner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMarginsF>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPoint>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRect>
#include <QRegularExpression>
#include <QSize>
#include <QSizeF>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTransform>
#include <QtPdf/QPdfDocument>
#include <QtPdf/QPdfDocumentRenderOptions>

#include "global/translation.h"
#include "global/log.h"

using namespace muse;
using namespace mu::reprise;

//! Ordered Audiveris per-sheet steps. Used only to turn the current step into an approximate within-sheet
//! fraction for the progress bar; unknown steps simply hold the last position. Kept lenient on naming.
static const QStringList STEP_ORDER = {
    "LOAD", "BINARY", "SCALE", "STEM_SCALE", "GRID", "HEADERS", "STAVES",
    "LEDGERS", "HEADS", "STEMS", "STEM_SEEDS", "BEAMS", "FLAGS", "RESTS",
    "KEYS", "CLEFS", "TIME", "MEASURES", "CHORDS", "CURVES", "TEXTS",
    "SYMBOLS", "RHYTHMS", "LINKS", "PAGE"
};

//! Audiveris/PDFBox-matching PDF render options: grayscale + anti-aliasing OFF (text/image/path). Shared by the #711
//! PDF pad (buildPaddedPdf) and the PART 1 deskew render so both reproduce the same pixel grid PDFBox would at the run DPI.
static QPdfDocumentRenderOptions pdfGrayAaOffOptions()
{
    QPdfDocumentRenderOptions options;
    options.setRenderFlags(QPdfDocumentRenderOptions::RenderFlag::Grayscale
                           | QPdfDocumentRenderOptions::RenderFlag::TextAliased
                           | QPdfDocumentRenderOptions::RenderFlag::ImageAliased
                           | QPdfDocumentRenderOptions::RenderFlag::PathAliased);

    return options;
}

//! Audiveris's per-sheet input cap (org.audiveris.omr.sheet.SheetStub constant maxPixelCount): a sheet whose pixel count
//! exceeds this is rejected with "Too large image". ONE shared copy for this TU, uniquely named (NOT the AUDIVERIS_MAX_PIXELS
//! / IMAGE_PAD_MARGIN that importpdfcontroller.cpp defines at file scope) so the Unity build -- which concatenates both .cpp
//! into a single TU -- sees no duplicate-symbol clash. The deskew DPI clamp and the image/TIFF/deskew pads all read these.
static constexpr int64_t kSheetMaxPixels = 20000000;

//! The fixed white border the image, TIFF, and deskew pads add on EACH side (the #711 edge-probe-crash margin): a staff
//! touching the raster edge trips AreaUtil.verticalCore, and the maintainer workaround is to composite onto a white margin.
static constexpr int32_t kPadMargin = 100;

//! Safety target the deskew render clamps to, held a little below kSheetMaxPixels so the rotation bounding-box growth and the
//! margin rounding slop that deskew adds AFTER the projection can never push the final straightened sheet back over the cap.
static constexpr int64_t kDeskewSafetyPixels = 19500000;

AudiverisRunner::~AudiverisRunner() = default;

muse::io::path_t AudiverisRunner::findAudiveris() const
{
    //! A user-set path wins (decision: a configured path overrides detection), as long as it still exists.
    const io::path_t configured = repriseConfiguration()->audiverisPath();
    if (!configured.empty() && QFileInfo::exists(configured.toQString()))
    {
        return configured;
    }

    //! Otherwise the C#-matched detection search order (PATH names, then the standard install folders).
    return repriseConfiguration()->detectAudiveris();
}

muse::io::path_t AudiverisRunner::findRepriseTessdata() const
{
    //! Resolved at RUNTIME from the running executable's directory, never a build-time path, so the deployed
    //! standalone build and the dev build each find the tessdata folder shipped beside their own Reprise.exe.
    const QDir exeDir(QCoreApplication::applicationDirPath());
    const QDir dir(exeDir.absoluteFilePath("tessdata"));
    if (dir.exists() && !dir.entryList(QStringList() << "*.traineddata", QDir::Files).isEmpty())
    {
        return io::path_t(QDir::cleanPath(dir.absolutePath()));
    }

    return io::path_t();
}

std::vector<std::pair<std::string, std::string> > AudiverisRunner::installedOcrLanguages() const
{
    //! Delegate to the single owner of language-pack enumeration (ILanguagePackService): it enumerates the
    //! *.traineddata packs in Reprise's shipped tessdata folder (osd excluded) and maps each code to a friendly
    //! display name. This keeps the intake-dialog dropdown, the Preferences pack manager, and the MCP helpers all
    //! reading the exact same installed list. Returned here as (code, name) pairs for buildLanguagesJson().
    std::vector<std::pair<std::string, std::string> > result;
    for (const LanguagePackInfo& pack : languagePackService()->installedPacks())
    {
        result.push_back({ pack.code, pack.name });
    }

    return result;
}

muse::io::path_t AudiverisRunner::omrPath() const
{
    if (m_outDir.isEmpty())
    {
        return io::path_t();
    }

    //! Prefer the .omr whose base matches the actual input fed to Audiveris (the padded copy for an image/PDF retry, else
    //! the original); locateOutput falls back to any *.omr under the output dir when the exact name is not found.
    const std::string base = QFileInfo(m_pdfPath.toQString()).completeBaseName().toStdString();
    return locateOutput(io::path_t(m_outDir), base, "omr");
}

muse::io::path_t AudiverisRunner::locateOutput(const muse::io::path_t& dir, const std::string& base, const std::string& ext) const
{
    const QString wanted = QString::fromStdString(base) + "." + QString::fromStdString(ext);
    const QString glob = "*." + QString::fromStdString(ext);

    //! Prefer an exact <base>.<ext> match, anywhere under the output dir.
    QString firstAny;
    QDirIterator it(dir.toQString(), QStringList() << glob, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        if (QFileInfo(path).fileName().compare(wanted, Qt::CaseInsensitive) == 0)
        {
            return io::path_t(path);
        }

        if (firstAny.isEmpty())
        {
            firstAny = path;
        }
    }

    return firstAny.isEmpty() ? io::path_t() : io::path_t(firstAny);
}

std::vector<muse::io::path_t> AudiverisRunner::locateAllOutputs(const muse::io::path_t& dir, const std::string& ext) const
{
    const QString glob = "*." + QString::fromStdString(ext);

    QStringList paths;
    QDirIterator it(dir.toQString(), QStringList() << glob, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        paths << it.next();
    }

    //! Ascending by path so the "first" movement is deterministic (Audiveris names split movements with an ordered suffix).
    paths.sort();

    std::vector<io::path_t> result;
    result.reserve(static_cast<size_t>(paths.size()));
    for (const QString& p : paths)
    {
        result.push_back(io::path_t(p));
    }

    return result;
}

QList<QPair<int, QString> > AudiverisRunner::skippedPages() const
{
    //! QMap iterates in ascending key order, so the list is already sorted by sheet number.
    QList<QPair<int, QString> > result;
    for (auto it = m_invalidSheetReasons.constBegin(); it != m_invalidSheetReasons.constEnd(); ++it)
    {
        result.append(qMakePair(it.key(), it.value()));
    }

    return result;
}

bool AudiverisRunner::isQtDecodableImage(const std::string& suffix)
{
    //! SINGLE-frame raster formats this Qt kit has an image plugin for and that Audiveris reads as one raster sheet, so the
    //! single-image pad (padImageToTemp) is correct for them. TIFF is NOT here: it is a multi-frame container handled by
    //! padTiffToTemp, which preserves every page. GIF is decodable; an animated GIF is never a music scan, and QImage then
    //! simply loads its first frame, which pads fine.
    return suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "bmp" || suffix == "gif";
}

muse::io::path_t AudiverisRunner::padImageToTemp(const muse::io::path_t& imagePath) const
{
    QImage src(imagePath.toQString());
    if (src.isNull())
    {
        //! Unexpected for a format we claimed decodable -> feed the original unpadded (graceful, never crash).
        LOGW() << "Reprise: image pad: could not decode " << imagePath.toQString() << "; feeding Audiveris the original.";
        return io::path_t();
    }

    //! Audiveris's per-sheet pixel cap and the fixed image border, kept in sync with preflightImageSize (which applies the
    //! matching skip-the-pad fallback). Aliased to the shared file-scope constants so the cap/border value lives in ONE
    //! place; the aliases keep the Unity build clash-free (the file-scope names differ from importpdfcontroller.cpp's).
    constexpr int64_t kMaxPixels = kSheetMaxPixels;
    constexpr int32_t kMargin = kPadMargin;

    //! Judge the PADDED pixel count against Audiveris's per-sheet cap; if the border alone would push it over, skip the
    //! pad (feed the original) rather than trading the #711 crash for Audiveris's own oversize rejection.
    const int64_t paddedW = static_cast<int64_t>(src.width()) + 2 * kMargin;
    const int64_t paddedH = static_cast<int64_t>(src.height()) + 2 * kMargin;
    if (paddedW * paddedH > kMaxPixels)
    {
        LOGI() << "Reprise: image pad skipped (padded " << paddedW << "x" << paddedH
               << " exceeds the 20 MP cap); feeding Audiveris the original.";
        return io::path_t();
    }

    //! Composite centered onto a white canvas at 1:1 (no rescale). RGB32 keeps the write lossless; grayscale content maps
    //! exactly, so only a white border is added and OMR quality is unchanged. Preserve any resolution metadata so Audiveris
    //! still reads the image at its native scale.
    QImage canvas(static_cast<int32_t>(paddedW), static_cast<int32_t>(paddedH), QImage::Format_RGB32);
    canvas.fill(Qt::white);
    if (src.dotsPerMeterX() > 0)
    {
        canvas.setDotsPerMeterX(src.dotsPerMeterX());
    }
    if (src.dotsPerMeterY() > 0)
    {
        canvas.setDotsPerMeterY(src.dotsPerMeterY());
    }

    {
        QPainter painter(&canvas);
        painter.drawImage(kMargin, kMargin, src);
    }

    const QString outPath = QDir(m_outDir).absoluteFilePath(
        QFileInfo(imagePath.toQString()).completeBaseName() + "-padded.png");
    if (!canvas.save(outPath, "PNG"))
    {
        LOGW() << "Reprise: image pad: could not write padded image " << outPath << "; feeding Audiveris the original.";
        return io::path_t();
    }

    LOGI() << "Reprise: image pad: " << src.width() << "x" << src.height() << " -> " << paddedW << "x" << paddedH
           << " (margin " << kMargin << " px) -> " << outPath;

    return io::path_t(outPath);
}

muse::io::path_t AudiverisRunner::padTiffToTemp(const muse::io::path_t& imagePath) const
{
    //! Frame count FIRST: QImageReader::imageCount() runs qtiff's ensureHaveDirectoryCount(), which both returns the real
    //! page count AND populates the handler's directoryCount. Populating it is REQUIRED -- without it jumpToNextImage()
    //! fails its bounds check immediately and the walk stops after frame 0 (observed on this kit). jumpToImage() is NOT
    //! used (unreliable here via QImageReader -- the inspectImage gotcha), but jumpToNextImage() works after a read() once
    //! the count is known (traced in qtiff's qtiffhandler.cpp).
    QImageReader reader(imagePath.toQString());
    const int32_t frameCount = reader.imageCount();

    //! Count unreadable (unexpected now the kit ships qtiff) -> feed the ORIGINAL unpadded TIFF, which Audiveris reads
    //! natively. Never crash, never drop a page.
    if (frameCount < 1)
    {
        LOGW() << "Reprise: TIFF pad: could not read a frame count from " << imagePath.toQString()
               << "; feeding Audiveris the original.";
        return io::path_t();
    }

    //! A single-frame TIFF is just an image: take the proven single-image pad (a padded PNG at native scale), exactly like
    //! any other raster format. The multi-page PDF path below is only for a TIFF that genuinely holds more than one page.
    if (frameCount == 1)
    {
        return padImageToTemp(imagePath);
    }

    //! Decode EVERY frame in document order: frame 0 is current after imageCount(), and jumpToNextImage() advances one
    //! directory before each subsequent read(). Any seek/decode miss -> feed the ORIGINAL unpadded TIFF so a partial or
    //! reordered page set is never produced.
    std::vector<QImage> frames;
    frames.reserve(static_cast<size_t>(frameCount));
    for (int32_t i = 0; i < frameCount; ++i)
    {
        if (i > 0 && !reader.jumpToNextImage())
        {
            LOGW() << "Reprise: TIFF pad: could not advance to frame " << i << " of " << imagePath.toQString()
                   << "; feeding Audiveris the original to avoid dropping a page.";
            return io::path_t();
        }

        const QImage frame = reader.read();
        if (frame.isNull())
        {
            LOGW() << "Reprise: TIFF pad: could not decode frame " << i << " of " << imagePath.toQString()
                   << "; feeding Audiveris the original to avoid dropping a page.";
            return io::path_t();
        }

        frames.push_back(frame);
    }

    //! Per-sheet pixel cap and the fixed image border, matching padImageToTemp (TIFF frames are native rasters like any other
    //! image, so a fixed 100 px border -- not a DPI-scaled one -- is the right pad). Aliased to the shared file-scope
    //! constants so the value lives in ONE place while the Unity build stays clash-free.
    constexpr int64_t kMaxPixels = kSheetMaxPixels;
    constexpr int32_t kMargin = kPadMargin;

    //! Qt 6's qtiff handler cannot WRITE a multi-page TIFF (its write() opens a fresh handle per call with no
    //! TIFFWriteDirectory/append), so re-emit the padded frames as a single multi-page PDF, which Audiveris reads
    //! identically. Size each page so paddedW px at the run DPI maps 1:1 (page = paddedW*72/dpi points); Audiveris then
    //! rasterizes the PDF back at pdfResolution=dpi to the same pixel grid, so the DPI cancels and native pixels survive.
    const int32_t dpi = m_dpi > 0 ? m_dpi : 300;

    const QString outPath = QDir(m_outDir).absoluteFilePath(
        QFileInfo(imagePath.toQString()).completeBaseName() + "-padded.pdf");

    QPdfWriter writer(outPath);
    writer.setResolution(dpi);
    //! Zero page margins so the painter origin is the page corner and the whole page is paintable (the image fills it 1:1).
    writer.setPageMargins(QMarginsF(0.0, 0.0, 0.0, 0.0));

    QPainter painter;
    bool painting = false;
    for (const QImage& frame : frames)
    {
        //! Per-frame cap (mirrors preflightImageSize): pad the frame unless the border alone would push it past Audiveris's
        //! per-sheet cap, in which case draw it unpadded (no margin) so this frame is still carried rather than rejected.
        const int64_t paddedW = static_cast<int64_t>(frame.width()) + 2 * kMargin;
        const int64_t paddedH = static_cast<int64_t>(frame.height()) + 2 * kMargin;
        const bool padThisFrame = paddedW * paddedH <= kMaxPixels;
        const int32_t canvasW = padThisFrame ? static_cast<int32_t>(paddedW) : frame.width();
        const int32_t canvasH = padThisFrame ? static_cast<int32_t>(paddedH) : frame.height();

        QImage canvas(canvasW, canvasH, QImage::Format_RGB32);
        canvas.fill(Qt::white);
        {
            QPainter cp(&canvas);
            cp.drawImage(padThisFrame ? kMargin : 0, padThisFrame ? kMargin : 0, frame);
        }

        //! One PDF page sized to the canvas raster at this DPI (device px == canvasW/canvasH), so the image draws 1:1.
        const QPageSize ps(QSizeF(canvasW * 72.0 / static_cast<double>(dpi), canvasH * 72.0 / static_cast<double>(dpi)),
                           QPageSize::Point);
        writer.setPageSize(ps);
        if (!painting)
        {
            if (!painter.begin(&writer))
            {
                LOGW() << "Reprise: TIFF pad: could not open QPdfWriter at " << outPath
                       << "; feeding Audiveris the original.";
                return io::path_t();
            }
            painting = true;
        }
        else
        {
            writer.newPage();
        }

        painter.drawImage(QPoint(0, 0), canvas);
    }

    if (painting)
    {
        painter.end();
    }

    LOGI() << "Reprise: TIFF pad: re-emitted " << frames.size() << " frame(s) with a " << kMargin
           << " px white margin as a multi-page PDF at " << dpi << " dpi -> " << outPath;

    return io::path_t(outPath);
}

muse::io::path_t AudiverisRunner::buildPaddedPdf(const muse::io::path_t& pdfPath, int32_t dpi) const
{
    QPdfDocument doc;
    if (doc.load(pdfPath.toQString()) != QPdfDocument::Error::None || doc.status() != QPdfDocument::Status::Ready)
    {
        LOGW() << "Reprise: PDF pad: could not load " << pdfPath.toQString() << " for re-rasterization.";
        return io::path_t();
    }

    const int32_t pageCount = doc.pageCount();
    if (pageCount < 1)
    {
        LOGW() << "Reprise: PDF pad: " << pdfPath.toQString() << " reports no pages.";
        return io::path_t();
    }

    //! Match Audiveris/PDFBox rasterization: grayscale + anti-aliasing OFF (text/image/path), at the SAME DPI the run
    //! used -- never a downgrade -- so the retry reproduces the original page content with only a white border added.
    const QPdfDocumentRenderOptions options = pdfGrayAaOffOptions();

    //! PDF pages: a DPI-scaled margin (at least 100 px) so the border is proportionate at any resolution.
    const int32_t margin = std::max<int32_t>(100, static_cast<int32_t>(std::lround(static_cast<double>(dpi) / 3.0)));

    const QString outPath = QDir(m_outDir).absoluteFilePath(
        QFileInfo(pdfPath.toQString()).completeBaseName() + "-padded.pdf");

    QPdfWriter writer(outPath);
    writer.setResolution(dpi);
    //! Zero page margins so the painter's origin is the page corner and the full page is paintable (the image fills it 1:1).
    writer.setPageMargins(QMarginsF(0.0, 0.0, 0.0, 0.0));

    QPainter painter;
    bool painting = false;
    for (int32_t page = 0; page < pageCount; ++page)
    {
        //! Target raster size at the run DPI (PDF points are 1/72 inch): matches PDFBox renderImageWithDPI(page, dpi).
        const QSizeF pts = doc.pagePointSize(page);
        const QSize pagePx(static_cast<int32_t>(std::lround(pts.width() * static_cast<double>(dpi) / 72.0)),
                           static_cast<int32_t>(std::lround(pts.height() * static_cast<double>(dpi) / 72.0)));

        const QImage rendered = doc.render(page, pagePx, options);
        if (rendered.isNull())
        {
            LOGW() << "Reprise: PDF pad: page " << page << " rendered empty; aborting pad.";
            if (painting)
            {
                painter.end();
            }
            return io::path_t();
        }

        //! Composite centered onto a white canvas with the margin, 1:1 (no rescale) -> content is the rendered page plus a
        //! border only, so OMR quality is unchanged (and the retry only ever runs after an actual crash).
        const int32_t paddedW = rendered.width() + 2 * margin;
        const int32_t paddedH = rendered.height() + 2 * margin;
        QImage canvas(paddedW, paddedH, QImage::Format_RGB32);
        canvas.fill(Qt::white);
        {
            QPainter cp(&canvas);
            cp.drawImage(margin, margin, rendered);
        }

        //! One PDF page sized to the padded raster at this DPI (device px == paddedW/paddedH), so the image draws 1:1.
        const QPageSize ps(QSizeF(paddedW * 72.0 / static_cast<double>(dpi), paddedH * 72.0 / static_cast<double>(dpi)),
                           QPageSize::Point);
        writer.setPageSize(ps);
        if (!painting)
        {
            if (!painter.begin(&writer))
            {
                LOGW() << "Reprise: PDF pad: could not open QPdfWriter at " << outPath;
                return io::path_t();
            }
            painting = true;
        }
        else
        {
            writer.newPage();
        }

        painter.drawImage(QPoint(0, 0), canvas);
    }

    if (painting)
    {
        painter.end();
    }

    LOGI() << "Reprise: PDF pad: re-emitted " << pageCount << " page(s) with a " << margin
           << " px white margin at " << dpi << " dpi -> " << outPath;

    return io::path_t(outPath);
}

double AudiverisRunner::detectSkewAngle(const QImage& input)
{
    if (input.isNull())
    {
        return 0.0;
    }

    //! Downsample the working copy to <=~1000 px on the longest side (fast transform -- this is a measurement, not output).
    QImage img = input;
    const int32_t longest = std::max(img.width(), img.height());
    if (longest > 1000)
    {
        img = (img.width() >= img.height()) ? img.scaledToWidth(1000, Qt::FastTransformation)
                                            : img.scaledToHeight(1000, Qt::FastTransformation);
    }

    img = img.convertToFormat(QImage::Format_Grayscale8);
    const int32_t w = img.width();
    const int32_t h = img.height();
    if (w < 8 || h < 8)
    {
        return 0.0;
    }

    //! Binarize to an ink mask and collect the ink pixel coordinates, centered on the image middle. A fixed threshold is
    //! fine for a white-paper scan (background ~255, ink ~0); the centered coords let each candidate angle project the
    //! ink onto a rotated vertical axis with a plain 2D rotation.
    constexpr uint8_t kInkThreshold = 140;
    const double cx = w / 2.0;
    const double cy = h / 2.0;

    std::vector<float> ix;
    std::vector<float> iy;
    ix.reserve(100000);
    iy.reserve(100000);
    for (int32_t y = 0; y < h; ++y)
    {
        const uint8_t* line = img.constScanLine(y);
        for (int32_t x = 0; x < w; ++x)
        {
            if (line[x] < kInkThreshold)
            {
                ix.push_back(static_cast<float>(x - cx));
                iy.push_back(static_cast<float>(y - cy));
            }
        }
    }

    //! Too little ink (a blank/near-blank page) -> do not chase noise; report level.
    if (ix.size() < 500)
    {
        return 0.0;
    }

    //! Cap the sample so a mostly-dark page stays bounded in work per candidate angle.
    size_t stride = 1;
    if (ix.size() > 300000)
    {
        stride = ix.size() / 300000 + 1;
    }

    constexpr double kPi = 3.14159265358979323846;

    double bestVar = -1.0;
    double bestAngle = 0.0;
    std::vector<double> hist(static_cast<size_t>(h), 0.0);
    for (int32_t a = -30; a <= 30; ++a)
    {
        const double deg = a * 0.2;
        const double rad = deg * kPi / 180.0;
        const double s = std::sin(rad);
        const double c = std::cos(rad);

        std::fill(hist.begin(), hist.end(), 0.0);
        double count = 0.0;
        for (size_t k = 0; k < ix.size(); k += stride)
        {
            //! y' = x*sin + y*cos (the QTransform().rotate(deg) convention), binned into the row histogram.
            const double ry = static_cast<double>(ix[k]) * s + static_cast<double>(iy[k]) * c + cy;
            const int32_t row = static_cast<int32_t>(std::lround(ry));
            if (row >= 0 && row < h)
            {
                hist[static_cast<size_t>(row)] += 1.0;
                count += 1.0;
            }
        }

        if (count <= 0.0)
        {
            continue;
        }

        //! Variance of the row-sum profile: staff lines concentrate into few rows (high variance) when level.
        const double mean = count / static_cast<double>(h);
        double var = 0.0;
        for (int32_t r = 0; r < h; ++r)
        {
            const double d = hist[static_cast<size_t>(r)] - mean;
            var += d * d;
        }
        var /= static_cast<double>(h);

        if (var > bestVar)
        {
            bestVar = var;
            bestAngle = deg;
        }
    }

    //! Dead-band no-op, and clamp away a wild result (never apply a wild angle).
    if (std::fabs(bestAngle) < 0.2)
    {
        return 0.0;
    }
    if (std::fabs(bestAngle) > 6.0)
    {
        return 0.0;
    }

    return bestAngle;
}

QImage AudiverisRunner::deskewImage(const QImage& src, double angleDeg)
{
    if (src.isNull())
    {
        return src;
    }

    //! Same fixed white border as the single-image pad (padImageToTemp), so a straightened page keeps the #711 edge-crash
    //! protection a staff touching the raster edge would otherwise trip. The shared file-scope kPadMargin is the single
    //! source for this border; the deskew DPI clamp in deskewStep() projects the SAME value so the two always agree.
    constexpr int32_t kMargin = kPadMargin;

    //! Rotate on an ARGB copy so the exposed corners come out TRANSPARENT (not opaque black); compositing the rotated
    //! image onto a white canvas then shows white in those corners instead of the black-corner artifact.
    const QImage argb = src.convertToFormat(QImage::Format_ARGB32);
    const QImage rotated = argb.transformed(QTransform().rotate(angleDeg), Qt::SmoothTransformation);

    QImage canvas(rotated.width() + 2 * kMargin, rotated.height() + 2 * kMargin, QImage::Format_RGB32);
    canvas.fill(Qt::white);
    {
        QPainter painter(&canvas);
        painter.drawImage(kMargin, kMargin, rotated);
    }

    return canvas;
}

bool AudiverisRunner::beginDeskew(const muse::io::path_t& input, const QString& pages, int dpi)
{
    //! Fresh chunked state per call.
    m_deskewPdf.reset();
    m_deskewSourceFrames.clear();
    m_deskewImages.clear();
    m_deskewAngles.clear();
    m_deskewPages.clear();
    m_deskewRotatedAny = false;
    m_deskewIsPdf = false;
    m_deskewDpi = dpi > 0 ? dpi : 300;
    m_deskewInputPath = input.toQString();
    m_deskewOutDir.clear();

    const std::string suffix = io::suffix(input);
    int32_t sourcePageCount = 0;

    if (suffix == "pdf")
    {
        auto doc = std::make_unique<QPdfDocument>();
        if (doc->load(input.toQString()) != QPdfDocument::Error::None || doc->status() != QPdfDocument::Status::Ready)
        {
            LOGW() << "Reprise: deskew: could not load PDF " << input.toQString() << "; skipping deskew.";
            return false;
        }

        sourcePageCount = doc->pageCount();
        if (sourcePageCount < 1)
        {
            LOGW() << "Reprise: deskew: " << input.toQString() << " reports no pages; skipping deskew.";
            return false;
        }

        m_deskewPdf = std::move(doc);
        m_deskewIsPdf = true;
    }
    else
    {
        //! A raster image: decode every frame now (a single frame for the usual case; a multi-page TIFF yields several).
        //! Pre-decoding keeps deskewStep() uniform with the PDF path and avoids a fragile per-step re-seek.
        QImageReader reader(input.toQString());
        int32_t frameCount = reader.imageCount();
        if (frameCount < 1)
        {
            frameCount = 1;   //! some formats report 0 frames -> treat as a single image
        }

        for (int32_t f = 0; f < frameCount; ++f)
        {
            if (f > 0 && !reader.jumpToNextImage())
            {
                break;
            }

            QImage frame = reader.read();
            if (frame.isNull())
            {
                if (f == 0)
                {
                    //! Reader could not decode frame 0 -> last-resort plain load (covers formats QImageReader stumbles on).
                    const QImage one(input.toQString());
                    if (!one.isNull())
                    {
                        m_deskewSourceFrames.push_back(one);
                    }
                }
                break;
            }

            m_deskewSourceFrames.push_back(frame);
        }

        if (m_deskewSourceFrames.empty())
        {
            LOGW() << "Reprise: deskew: could not decode image " << input.toQString() << "; skipping deskew.";
            return false;
        }

        sourcePageCount = static_cast<int32_t>(m_deskewSourceFrames.size());
    }

    //! Resolve the SELECTED pages (expandSheets gives 1-based sheet numbers; empty -> every page). Map to 0-based source
    //! indices, dropping any out-of-range request so a bad range can never index past the real page count.
    const std::vector<int> sheets = expandSheets(pages);
    if (sheets.empty())
    {
        for (int32_t p = 0; p < sourcePageCount; ++p)
        {
            m_deskewPages.push_back(p);
        }
    }
    else
    {
        for (int sheet : sheets)
        {
            const int32_t idx = sheet - 1;
            if (idx >= 0 && idx < sourcePageCount)
            {
                m_deskewPages.push_back(idx);
            }
        }
    }

    if (m_deskewPages.empty())
    {
        m_deskewPdf.reset();
        m_deskewSourceFrames.clear();
        return false;
    }

    //! A persistent temp dir for the straightened PDF (survives until the process exits, like the recognition temp dir);
    //! start() creates its OWN output dir, so this is separate.
    QTemporaryDir tempDir(QDir::tempPath() + "/reprise-deskew-XXXXXX");
    tempDir.setAutoRemove(false);
    if (!tempDir.isValid())
    {
        LOGW() << "Reprise: deskew: could not create a temp output directory; skipping deskew.";
        m_deskewPdf.reset();
        m_deskewSourceFrames.clear();
        m_deskewPages.clear();
        return false;
    }

    m_deskewOutDir = tempDir.path();
    m_deskewImages.reserve(m_deskewPages.size());
    m_deskewAngles.reserve(m_deskewPages.size());

    LOGI() << "Reprise: deskew: armed " << m_deskewPages.size() << " selected page(s) from " << input.toQString()
           << " (dpi=" << m_deskewDpi << ")";

    return true;
}

void AudiverisRunner::deskewStep(int i)
{
    const int32_t total = static_cast<int32_t>(m_deskewPages.size());
    if (i < 0 || i >= total)
    {
        return;
    }

    //! Render the i-th selected source page to a QImage at the run DPI (grayscale, AA off -- the same grid PDFBox uses).
    QImage rendered;
    if (m_deskewIsPdf && m_deskewPdf)
    {
        const int32_t page = m_deskewPages[static_cast<size_t>(i)];
        const QSizeF pts = m_deskewPdf->pagePointSize(page);
        const QSize px(static_cast<int32_t>(std::lround(pts.width() * static_cast<double>(m_deskewDpi) / 72.0)),
                       static_cast<int32_t>(std::lround(pts.height() * static_cast<double>(m_deskewDpi) / 72.0)));
        rendered = m_deskewPdf->render(page, px, pdfGrayAaOffOptions());
    }
    else
    {
        const int32_t idx = m_deskewPages[static_cast<size_t>(i)];
        if (idx >= 0 && idx < static_cast<int32_t>(m_deskewSourceFrames.size()))
        {
            rendered = m_deskewSourceFrames[static_cast<size_t>(idx)];
        }
    }

    //! Measure this page's own skew; rotate only past the dead-band, else keep the page as rasterized (angle 0.0). The
    //! kept page is still carried into the assembly so page count/order is preserved whenever ANY page was rotated.
    double angle = 0.0;
    QImage result = rendered;
    if (!rendered.isNull())
    {
        angle = detectSkewAngle(rendered);
        if (std::fabs(angle) >= 0.2)
        {
            //! DESKEW OVERSIZE CLAMP. deskewImage() rotates the page (which GROWS its bounding box) and then adds a fixed
            //! kPadMargin-px white border on EACH side (the #711 edge-crash margin). On a borderline scan that rotated+padded
            //! sheet can tip past Audiveris's 20 MP per-sheet cap (kSheetMaxPixels) and the page is rejected "Too large image"
            //! -- even though the SAME page imports fine WITHOUT deskew (observed: hardtimes-a4.pdf, ~15.5 MP flat at 400 dpi,
            //! deskewed to 3916x5155 = 20.19 MP). The rotation growth is the LARGER contributor, so project AFTER the angle is
            //! known: take the rotated bounding box PLUS the 2*kPadMargin border, and only when that exceeds the safety target
            //! (kDeskewSafetyPixels, held below the cap so the fixed-margin rounding slop that scaling leaves can't push the
            //! final sheet back over) scale the render DOWN by sqrt(target / projected) -- i.e. a lower effective render DPI
            //! for THIS page only. A page whose rotated+padded size is already under the target keeps its full resolution (no
            //! regression). finishDeskew() sizes the straightened PDF page from the resulting pixel grid at m_deskewDpi, so the
            //! clamped grid still maps 1:1 for both the Audiveris run and the PART 2 proofing render.
            constexpr double kPi = 3.14159265358979323846;
            const double rad = std::fabs(angle) * kPi / 180.0;
            const double cosA = std::cos(rad);
            const double sinA = std::sin(rad);
            const double rotW = rendered.width() * cosA + rendered.height() * sinA;
            const double rotH = rendered.width() * sinA + rendered.height() * cosA;
            const int64_t projected = static_cast<int64_t>(std::llround(rotW + 2 * kPadMargin))
                                      * static_cast<int64_t>(std::llround(rotH + 2 * kPadMargin));
            if (projected > kDeskewSafetyPixels)
            {
                const double factor = std::sqrt(static_cast<double>(kDeskewSafetyPixels) / static_cast<double>(projected));
                const int32_t newW = std::max<int32_t>(1, static_cast<int32_t>(std::floor(rendered.width() * factor)));
                const int32_t effectiveDpi = std::max<int32_t>(1, static_cast<int32_t>(std::lround(m_deskewDpi * factor)));
                LOGI() << "Reprise: deskew oversize clamp: page " << (i + 1) << " rotated+padded projects " << projected
                       << " px (> " << kDeskewSafetyPixels << " target); scaling the " << m_deskewDpi
                       << " dpi render down to width " << newW << " px (effective ~" << effectiveDpi
                       << " dpi) to stay under the 20 MP cap.";
                rendered = rendered.scaledToWidth(newW, Qt::SmoothTransformation);
            }

            result = deskewImage(rendered, angle);
            m_deskewRotatedAny = true;
        }
        else
        {
            angle = 0.0;
        }
    }

    m_deskewImages.push_back(result);
    m_deskewAngles.push_back(angle);

    //! Single-page scan -> no "page N of M" makes sense, so say "Straightening the scan..."; multi-page keeps the
    //! "Straightening page N of M..." form. Both are already localised at source (the model mirrors stageHuman verbatim).
    const QString stageHuman = (total == 1)
                               ? muse::qtrc("reprise", "Straightening the scan...")
                               : muse::qtrc("reprise", "Straightening page %1 of %2...").arg(i + 1).arg(total);
    emit preprocessing(i + 1, total, stageHuman);
}

muse::io::path_t AudiverisRunner::finishDeskew()
{
    //! Regression gate: if NO page was actually rotated, feed Audiveris the ORIGINAL untouched (byte-identical no-op).
    //! Angles are retained for PART 2; only the heavy source/page buffers are released here.
    if (!m_deskewRotatedAny)
    {
        m_deskewPdf.reset();
        m_deskewSourceFrames.clear();
        m_deskewImages.clear();
        return io::path_t();
    }

    if (m_deskewImages.empty() || m_deskewOutDir.isEmpty())
    {
        m_deskewPdf.reset();
        m_deskewSourceFrames.clear();
        m_deskewImages.clear();
        return io::path_t();
    }

    const int32_t dpi = m_deskewDpi > 0 ? m_deskewDpi : 300;
    const QString base = QFileInfo(m_deskewInputPath).completeBaseName();
    const QString outPath = QDir(m_deskewOutDir).absoluteFilePath(base + "-deskewed.pdf");

    //! Assemble the straightened pages into ONE multi-page PDF, mirroring buildPaddedPdf's assembly: each page sized so
    //! its pixel raster maps 1:1 at the run DPI (page = px*72/dpi points), drawn 1:1, order preserved.
    QPdfWriter writer(outPath);
    writer.setResolution(dpi);
    writer.setPageMargins(QMarginsF(0.0, 0.0, 0.0, 0.0));

    QPainter painter;
    bool painting = false;
    for (const QImage& img : m_deskewImages)
    {
        //! A page that failed to render earlier (unexpected) becomes a blank white Letter page so the page count/order is
        //! still preserved rather than silently dropped.
        QImage page = img;
        if (page.isNull())
        {
            page = QImage(static_cast<int32_t>(std::lround(8.5 * dpi)), static_cast<int32_t>(std::lround(11.0 * dpi)),
                          QImage::Format_RGB32);
            page.fill(Qt::white);
        }

        //! Composite onto an opaque white canvas so any stray alpha can never show through on the PDF page.
        const int32_t cw = page.width();
        const int32_t ch = page.height();
        QImage canvas(cw, ch, QImage::Format_RGB32);
        canvas.fill(Qt::white);
        {
            QPainter cp(&canvas);
            cp.drawImage(0, 0, page);
        }

        const QPageSize ps(QSizeF(cw * 72.0 / static_cast<double>(dpi), ch * 72.0 / static_cast<double>(dpi)),
                           QPageSize::Point);
        writer.setPageSize(ps);
        if (!painting)
        {
            if (!painter.begin(&writer))
            {
                LOGW() << "Reprise: deskew: could not open QPdfWriter at " << outPath << "; skipping deskew.";
                m_deskewPdf.reset();
                m_deskewSourceFrames.clear();
                m_deskewImages.clear();
                return io::path_t();
            }

            painting = true;
        }
        else
        {
            writer.newPage();
        }

        painter.drawImage(QPoint(0, 0), canvas);
    }

    if (painting)
    {
        painter.end();
    }

    //! Release the heavy buffers; keep m_deskewAngles for PART 2 provenance.
    m_deskewPdf.reset();
    m_deskewSourceFrames.clear();
    m_deskewImages.clear();

    LOGI() << "Reprise: deskew: re-emitted " << m_deskewAngles.size() << " straightened page(s) at " << dpi
           << " dpi -> " << outPath;

    return io::path_t(outPath);
}

std::vector<int> AudiverisRunner::expandSheets(const QString& pages) const
{
    //! "all"/empty -> every page (no -sheets argument). Otherwise expand comma lists and dashed ranges into
    //! explicit 1-based sheet numbers; Audiveris '-sheets' accepts a space-separated int[] (e.g. "1 4-5").
    std::vector<int> sheets;

    const QString trimmed = pages.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("all", Qt::CaseInsensitive) == 0)
    {
        return sheets;
    }

    const QStringList tokens = trimmed.split(',', Qt::SkipEmptyParts);
    for (const QString& rawToken : tokens)
    {
        const QString token = rawToken.trimmed();
        if (token.isEmpty())
        {
            continue;
        }

        const int dash = token.indexOf('-');
        if (dash > 0)
        {
            bool okLo = false;
            bool okHi = false;
            const int lo = token.left(dash).trimmed().toInt(&okLo);
            const int hi = token.mid(dash + 1).trimmed().toInt(&okHi);
            if (okLo && okHi && lo >= 1 && hi >= lo)
            {
                for (int n = lo; n <= hi; ++n)
                {
                    sheets.push_back(n);
                }
            }
        }
        else
        {
            bool ok = false;
            const int n = token.toInt(&ok);
            if (ok && n >= 1)
            {
                sheets.push_back(n);
            }
        }
    }

    return sheets;
}

std::vector<std::string> AudiverisRunner::buildTextConstants(const RecognitionOptions& options) const
{
    std::vector<std::string> constants;

    auto addConstant = [&constants](const std::string& assignment)
    {
        constants.push_back("-constant");
        constants.push_back(assignment);
    };

    //! Master OFF: skip the whole TEXTS/OCR step (notes-only, faster). Nothing else text-related applies when OFF.
    if (!options.recognizeText)
    {
        addConstant("org.audiveris.omr.text.tesseract.TesseractOCR.useOCR=false");
        return constants;
    }

    //! Master ON (default): pass NO constant when a sub-option is at its Audiveris default. Audiveris's own defaults
    //! are lyrics on, chords off, lyrics below the staff, language eng -- so the default path emits an empty list and
    //! the proven default OCR behavior is left exactly as-is (no redundant constants clutter the command).
    if (options.chordSymbols)
    {
        addConstant("org.audiveris.omr.sheet.ProcessingSwitches.chordNames=true");
    }

    if (options.lyricsAboveStaff)
    {
        addConstant("org.audiveris.omr.sheet.ProcessingSwitches.lyricsAboveStaff=true");
    }

    //! Language: only when the chosen code differs from Audiveris's own default (eng). Passing eng explicitly is
    //! harmless but redundant, so it is omitted. A '+'-joined multi-language spec (e.g. "eng+deu") passes through as-is.
    const QString lang = options.language.trimmed();
    if (!lang.isEmpty() && lang.compare("eng", Qt::CaseInsensitive) != 0)
    {
        addConstant("org.audiveris.omr.text.Language.defaultSpecification=" + lang.toStdString());
    }

    return constants;
}

bool AudiverisRunner::isRunning() const
{
    return m_process != nullptr;
}

bool AudiverisRunner::start(const muse::io::path_t& pdfPath, const QString& pages, int dpi, const RecognitionOptions& options)
{
    const io::path_t audiverisExe = findAudiveris();
    if (audiverisExe.empty())
    {
        return false;
    }

    m_pdfPath = pdfPath;
    m_pages = pages;
    m_dpi = dpi;
    m_options = options;
    m_cancelled = false;
    m_sawArrayIndexCrash = false;
    m_sawNoStaves = false;
    m_sawLowInterline = false;
    m_sawTooLarge = false;
    m_invalidSheetReasons.clear();
    m_multiMovement = false;
    //! NOTE: m_reExportedOmr is deliberately NOT reset here -- the skip-invalid re-export re-enters the QProcess machinery
    //! (its own connections), and the guard must survive so the re-export runs at most once per import (like m_retriedWithPad).
    m_totalSheets = -1;
    m_currentSheet = -1;
    m_requestedSheetCount = 0;
    m_totalLocked = false;
    m_maxSheet = 0;
    m_maxPercent = -1;
    m_currentStep.clear();
    m_stdoutBuffer.clear();
    m_tail.clear();
    m_fullLog.clear();
    m_logFilePath.clear();
    //! NOTE: m_retriedWithPad is deliberately NOT reset here -- the PDF retry re-enters start(), and the guard must
    //! survive that re-entry so the pad is applied at most once per import.

    //! A persistent temp dir: outputs (.mxl, .omr) must survive the run. Durable copy is a later step.
    QTemporaryDir tempDir(QDir::tempPath() + "/reprise-omr-XXXXXX");
    tempDir.setAutoRemove(false);
    if (!tempDir.isValid())
    {
        //! Deferred so the caller can connect() to finished() before it fires.
        QMetaObject::invokeMethod(this, [this]()
        {
            emit finished(false, QString(), muse::qtrc("reprise", "Could not create a temporary output directory"), QString());
        }, Qt::QueuedConnection);
        return true;
    }

    m_outDir = tempDir.path();

    //! Always-on image pad (Audiveris #711 edge-probe crash): a staff touching the raster edge trips AreaUtil.verticalCore
    //! with an ArrayIndexOutOfBoundsException. The maintainer workaround is a white margin, so for any QtGui-decodable
    //! raster image we composite it centered on a larger white canvas and feed Audiveris THAT padded copy instead. TIFF now
    //! pads too (the kit ships qtiff) via padTiffToTemp, which preserves every frame of a multi-page TIFF. PDFs take the
    //! retry-on-crash pad in onFinished(). On any pad failure or an over-cap border, the pad helper returns empty and we
    //! feed the original unchanged (never crash, never drop a page).
    const std::string inputSuffix = io::suffix(pdfPath);
    if (inputSuffix == "tif" || inputSuffix == "tiff")
    {
        const io::path_t padded = padTiffToTemp(pdfPath);
        if (!padded.empty())
        {
            m_pdfPath = padded;
        }
    }
    else if (isQtDecodableImage(inputSuffix))
    {
        const io::path_t padded = padImageToTemp(pdfPath);
        if (!padded.empty())
        {
            m_pdfPath = padded;
        }
    }

    std::vector<std::string> args = {
        "-batch",
        "-save",
        "-export",
        //! Scan resolution. Runtime-confirmed effect on the rasterised image (e.g. =400 -> 3305x4677 vs 300-default 2479x3508).
        "-constant", "org.audiveris.omr.image.ImageLoading.pdfResolution=" + std::to_string(dpi),
        //! Force a SINGLE .mxl export. Audiveris treats an INDENTED system as the start of a new movement and then emits one
        //! .mxl per movement; Reprise assumes a single .mxl and imports only the first -> a mis-import. Disabling indentation
        //! detection makes Audiveris emit one combined export. ALWAYS applied -- every PDF and image import, independent of the
        //! text/OCR options (so it sits here beside pdfResolution, not in buildTextConstants()).
        "-constant", "org.audiveris.omr.sheet.ProcessingSwitches.indentations=false",
        "-output", m_outDir.toStdString()
    };

    //! Per-import recognition options -> Audiveris '-constant' switches. Both the GUI and headless/MCP paths reach
    //! here with a RecognitionOptions, so this one call is the single source of truth for the text constants. Master
    //! ON at defaults contributes nothing (preserving Audiveris's own proven defaults); master OFF adds useOCR=false.
    const std::vector<std::string> textConstants = buildTextConstants(options);
    args.insert(args.end(), textConstants.begin(), textConstants.end());

    //! Page selection. '-sheets' takes a space-separated int[]; keep it immediately before '--' so args4j
    //! stops consuming numbers at the separator. Empty list -> omit entirely -> Audiveris reads every page.
    const std::vector<int> sheets = expandSheets(pages);
    m_requestedSheetCount = static_cast<int>(sheets.size());

    //! When a page range was requested we already know EXACTLY how many sheets Audiveris will process, so take that
    //! as the authoritative total up front and lock it -- no parsing, and immune to any stray "N sheet(s)" line.
    if (m_requestedSheetCount > 0)
    {
        m_totalSheets = m_requestedSheetCount;
        m_totalLocked = true;
    }

    if (!sheets.empty())
    {
        args.push_back("-sheets");
        for (int sheet : sheets)
        {
            args.push_back(std::to_string(sheet));
        }
    }

    args.push_back("--");
    //! m_pdfPath is the ACTUAL input fed to Audiveris -- the padded PNG when the image pad above substituted it, else the
    //! original path. onFinished() reads the same member for the output base name and the PDF retry check.
    args.push_back(m_pdfPath.toQString().toStdString());

    QStringList qargs;
    for (const std::string& a : args)
    {
        qargs << QString::fromStdString(a);
    }

    m_process = new QProcess(this);
    m_process->setProgram(audiverisExe.toQString());
    m_process->setArguments(qargs);

    //! Point Tesseract at its training data. Reprise SHIPS its own legacy+LSTM tessdata beside the executable and it
    //! is the SOLE authoritative source: Audiveris inits Tesseract in LEGACY mode, which the system LSTM-only "fast"
    //! eng.traineddata cannot satisfy (legacy init fails -> no OCR -> no lyrics/titles), so only the self-contained
    //! legacy data Reprise ships + manages is ever used. If the shipped folder is somehow missing we log a clear
    //! error rather than silently falling back to a wrong system model (which would produce garbage OCR).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const io::path_t tessdata = findRepriseTessdata();
    if (!tessdata.empty())
    {
        env.insert("TESSDATA_PREFIX", tessdata.toQString());
        LOGI() << "Reprise: TESSDATA_PREFIX set to " << tessdata.toQString();
    }
    else
    {
        LOGW() << "Reprise: shipped tessdata folder (<exeDir>/tessdata) is missing or empty; OCR will not work. "
                  "Reprise ships and manages this data -- reinstall or redeploy to restore it.";
    }
    m_process->setProcessEnvironment(env);

    connect(m_process, &QProcess::readyReadStandardOutput, this, &AudiverisRunner::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &AudiverisRunner::onReadyReadStandardError);
    connect(m_process, &QProcess::errorOccurred, this, &AudiverisRunner::onErrorOccurred);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onFinished(exitCode); });

    LOGI() << "Reprise: running Audiveris: " << audiverisExe.toQString() << " on " << pdfPath.toQString()
           << " (dpi=" << dpi << ", sheets=" << (sheets.empty() ? std::string("all") : std::to_string(sheets.size())) << ")";

    //! Make the recognition options observable on the headless path (no GUI). The assembled text '-constant' args also
    //! appear on the full command line logged below; this summary line states the resolved option values directly.
    QStringList constantArgsForLog;
    for (const std::string& c : textConstants)
    {
        constantArgsForLog << QString::fromStdString(c);
    }
    LOGI() << "Reprise: recognition options: recognizeText=" << (options.recognizeText ? "true" : "false")
           << " language=" << options.language
           << " chordSymbols=" << (options.chordSymbols ? "true" : "false")
           << " lyricsAboveStaff=" << (options.lyricsAboveStaff ? "true" : "false")
           << " -> text constants: " << (constantArgsForLog.isEmpty() ? QString("(none)") : constantArgsForLog.join(' '));

    LOGI() << "[audiveris] command: \"" << audiverisExe.toQString() << "\" " << qargs.join(' ');

    //! Kick an initial indeterminate tick so the popup shows an animated bar immediately.
    QMetaObject::invokeMethod(this, [this]()
    {
        emit progressChanged(-1, -1, -1, muse::qtrc("reprise", "Starting Audiveris..."));
    }, Qt::QueuedConnection);

    m_process->start();
    return true;
}

bool AudiverisRunner::startOmrReExport(const muse::io::path_t& omrPath)
{
    const io::path_t audiverisExe = findAudiveris();
    if (audiverisExe.empty() || omrPath.empty())
    {
        return false;
    }

    //! One-shot guard: mark the re-export BEFORE launching so onFinished() routes its outcome through the re-export branch,
    //! and so a re-export that itself fails is never re-exported again. Keep m_fullLog + m_invalidSheetReasons (the notice
    //! reads the latter); clear only the short error tail so it reflects the re-export pass.
    m_reExportedOmr = true;
    m_tail.clear();

    //! Minimal second pass: re-export the ALREADY-recognised .omr book. No -save (the book is already saved), no -sheets and
    //! no pdfResolution (the book holds the recognised sheets; the failed stubs simply have nothing to export, so they are
    //! skipped naturally). indentations=false keeps the surviving systems combined into one export, matching start().
    std::vector<std::string> args = {
        "-batch",
        "-export",
        "-constant", "org.audiveris.omr.sheet.ProcessingSwitches.indentations=false",
        "-output", m_outDir.toStdString(),
        "--",
        omrPath.toQString().toStdString()
    };

    QStringList qargs;
    for (const std::string& a : args)
    {
        qargs << QString::fromStdString(a);
    }

    m_process = new QProcess(this);
    m_process->setProgram(audiverisExe.toQString());
    m_process->setArguments(qargs);

    //! Same TESSDATA_PREFIX setup as start() (export can still touch OCR data); mirror it exactly.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const io::path_t tessdata = findRepriseTessdata();
    if (!tessdata.empty())
    {
        env.insert("TESSDATA_PREFIX", tessdata.toQString());
    }
    m_process->setProcessEnvironment(env);

    //! Same signal wiring as start(), so progress/log streaming and onFinished() behave identically for this pass.
    connect(m_process, &QProcess::readyReadStandardOutput, this, &AudiverisRunner::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &AudiverisRunner::onReadyReadStandardError);
    connect(m_process, &QProcess::errorOccurred, this, &AudiverisRunner::onErrorOccurred);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onFinished(exitCode); });

    LOGI() << "[audiveris] re-export command: \"" << audiverisExe.toQString() << "\" " << qargs.join(' ');

    m_process->start();
    return true;
}

void AudiverisRunner::cancel()
{
    m_cancelled = true;

    if (m_process)
    {
        //! Disconnect first so the kill does not fire onFinished()/onErrorOccurred() into the model.
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
}

void AudiverisRunner::onReadyReadStandardOutput()
{
    if (!m_process)
    {
        return;
    }

    m_stdoutBuffer += QString::fromLocal8Bit(m_process->readAllStandardOutput());

    int nl = -1;
    while ((nl = m_stdoutBuffer.indexOf('\n')) >= 0)
    {
        QString line = m_stdoutBuffer.left(nl);
        m_stdoutBuffer.remove(0, nl + 1);
        line.remove('\r');
        if (!line.trimmed().isEmpty())
        {
            LOGI() << "[audiveris] " << line;
            parseLine(line);
            pushTail(line);
            emit outputLine(line);
        }
    }
}

void AudiverisRunner::onReadyReadStandardError()
{
    if (!m_process)
    {
        return;
    }

    const QString text = QString::fromLocal8Bit(m_process->readAllStandardError());
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString& raw : lines)
    {
        QString line = raw;
        line.remove('\r');
        if (!line.trimmed().isEmpty())
        {
            LOGW() << "[audiveris:stderr] " << line;
            pushTail(line);
            emit outputLine(line);
        }
    }
}

void AudiverisRunner::parseLine(const QString& line)
{
    bool changed = false;

    //! Total sheets. The book prints its sheet count ONCE ("... N sheets in <pdf>"), before any per-sheet
    //! processing, so the authoritative total is the FIRST "N sheets" line that is not itself a per-sheet-tagged
    //! status line (those carry a "#<n>]" tag). We lock on that first accept, so a later stray "<n> sheet(s)"
    //! line can never overwrite it -- that clobber is what previously drove the total down to 1 ("page 3 of 1").
    //! Skipped entirely once the total is already locked (e.g. from a requested page range).
    static const QRegularExpression reSheetTag("#\\d+\\]");
    if (!m_totalLocked && !reSheetTag.match(line).hasMatch())
    {
        static const QRegularExpression reTotal("(\\d+)\\s+sheets?\\b", QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch mTotal = reTotal.match(line);
        if (mTotal.hasMatch())
        {
            const int total = mTotal.captured(1).toInt();
            if (total > 0)
            {
                m_totalSheets = total;
                m_totalLocked = true;
                changed = true;
            }
        }
    }

    //! Current sheet: the LEADING logger tag "LEVEL [<book>#<N>] ...". Anchored to that first bracket on purpose: a
    //! multi-sheet run also prints "Book reaching PAGE on sheets:[#1#2]" (empty leading tag) BEFORE any sheet starts, and an
    //! unanchored "#<n>]" search matched its trailing "#2]" -> "page 2 of 2" + a near-full bar from the first moment.
    static const QRegularExpression reSheet("^\\s*\\w+\\s+\\[[^\\]]*#(\\d+)\\]");
    const QRegularExpressionMatch mSheet = reSheet.match(line);
    if (mSheet.hasMatch())
    {
        const int sheet = mSheet.captured(1).toInt();
        if (sheet > 0 && sheet != m_currentSheet)
        {
            m_currentSheet = sheet;
            changed = true;
        }
    }

    //! Current step: "StepMonitoring ... | STEP".
    if (line.contains("StepMonitoring"))
    {
        const int bar = line.lastIndexOf('|');
        if (bar >= 0)
        {
            const QString step = line.mid(bar + 1).trimmed().section(' ', 0, 0).toUpper();
            if (!step.isEmpty() && step != m_currentStep)
            {
                m_currentStep = step;
                changed = true;
            }
        }
    }

    if (changed)
    {
        //! Clamp the current sheet to the known total so we never report current > total ("page 3 of 1").
        int sheet = m_currentSheet;
        if (m_totalSheets > 0 && sheet > m_totalSheets)
        {
            sheet = m_totalSheets;
        }

        //! Monotonic: recognition only moves forward, so never let the reported sheet / percent go backwards.
        if (sheet > m_maxSheet)
        {
            m_maxSheet = sheet;
        }
        sheet = m_maxSheet;

        int percent = computePercent(sheet);
        if (percent > m_maxPercent)
        {
            m_maxPercent = percent;
        }
        percent = m_maxPercent;

        emit progressChanged(percent, sheet, m_totalSheets, humanStage(m_currentStep));
    }
}

int AudiverisRunner::computePercent(int currentSheet) const
{
    if (m_totalSheets <= 0)
    {
        return -1;
    }

    const int sheet = currentSheet > 0 ? currentSheet : 1;

    double withinSheet = 0.0;
    const int stepIndex = STEP_ORDER.indexOf(m_currentStep);
    if (stepIndex >= 0 && !STEP_ORDER.isEmpty())
    {
        withinSheet = double(stepIndex + 1) / double(STEP_ORDER.size());
    }

    double fraction = (double(sheet - 1) + withinSheet) / double(m_totalSheets);
    if (fraction < 0.0)
    {
        fraction = 0.0;
    }
    if (fraction > 1.0)
    {
        fraction = 1.0;
    }

    return int(fraction * 100.0 + 0.5);
}

QString AudiverisRunner::humanStage(const QString& rawStep) const
{
    if (rawStep.isEmpty())
    {
        return muse::qtrc("reprise", "Recognizing...");
    }

    //! Friendly labels for the common Audiveris steps; unknown steps fall back to the raw name, lowercased.
    if (rawStep == "LOAD")       { return muse::qtrc("reprise", "loading the page..."); }
    if (rawStep == "BINARY")     { return muse::qtrc("reprise", "cleaning up the image..."); }
    if (rawStep == "SCALE" || rawStep == "STEM_SCALE") { return muse::qtrc("reprise", "measuring the staves..."); }
    if (rawStep == "GRID" || rawStep == "STAVES" || rawStep == "HEADERS") { return muse::qtrc("reprise", "detecting staves..."); }
    if (rawStep == "LEDGERS")    { return muse::qtrc("reprise", "reading ledger lines..."); }
    if (rawStep == "HEADS")      { return muse::qtrc("reprise", "reading notes..."); }
    if (rawStep == "STEMS" || rawStep == "STEM_SEEDS") { return muse::qtrc("reprise", "finding stems..."); }
    if (rawStep == "BEAMS" || rawStep == "FLAGS") { return muse::qtrc("reprise", "reading beams..."); }
    if (rawStep == "RESTS")      { return muse::qtrc("reprise", "reading rests..."); }
    if (rawStep == "KEYS")       { return muse::qtrc("reprise", "reading key signatures..."); }
    if (rawStep == "CLEFS")      { return muse::qtrc("reprise", "reading clefs..."); }
    if (rawStep == "TIME")       { return muse::qtrc("reprise", "reading time signatures..."); }
    if (rawStep == "MEASURES")   { return muse::qtrc("reprise", "finding measures..."); }
    if (rawStep == "CHORDS")     { return muse::qtrc("reprise", "reading chords..."); }
    if (rawStep == "CURVES")     { return muse::qtrc("reprise", "reading slurs and ties..."); }
    if (rawStep == "TEXTS")      { return muse::qtrc("reprise", "reading text and lyrics..."); }
    if (rawStep == "SYMBOLS")    { return muse::qtrc("reprise", "reading symbols..."); }
    if (rawStep == "RHYTHMS")    { return muse::qtrc("reprise", "working out the rhythm..."); }
    if (rawStep == "LINKS")      { return muse::qtrc("reprise", "connecting everything up..."); }
    if (rawStep == "PAGE")       { return muse::qtrc("reprise", "finishing the page..."); }

    return rawStep.toLower() + "...";
}

void AudiverisRunner::pushTail(const QString& line)
{
    //! Detect the Audiveris #711 edge-probe signature on EVERY line (stdout + stderr both route here). The error tail is a
    //! short ring that can scroll the signature out before the process exits, so the PDF retry decision reads this sticky
    //! flag instead of re-scanning the tail. Set-once per run; reset at the top of start().
    if (!m_sawArrayIndexCrash && line.contains("ArrayIndexOutOfBoundsException"))
    {
        m_sawArrayIndexCrash = true;
    }

    //! Audiveris's own failure signatures, scanned once each over every line, so a non-zero exit can be turned into a
    //! specific, user-friendly message (no staves -> cover/title page; too-low interline / too-large image -> DPI advice).
    //! The global m_saw* flags still drive the all-invalid failure message; the per-sheet reason map (recorded below) drives
    //! the skip-invalid re-export + the "Import finished" notice's skipped-page list.
    QString reason;
    if (line.contains("No regularly spaced lines found") || line.contains("No system found"))
    {
        m_sawNoStaves = true;
        reason = "no-music";
    }
    else if (line.contains("too low interline"))
    {
        m_sawLowInterline = true;
        reason = "low-res";
    }
    else if (line.contains("Too large image"))
    {
        m_sawTooLarge = true;
        reason = "too-large";
    }

    //! Record the per-sheet reason the FIRST time a given sheet fails. The reason line itself carries the leading logger tag
    //! "LEVEL [<book>#<N>] ..." (confirmed against captured Audiveris logs), so read the sheet number straight from it; the
    //! untagged continuation lines (ExecutionException / "Caused by:") fall back to the last sheet parseLine saw. First
    //! reason per sheet wins (contains() guard), so a later untagged line never overwrites the correctly-tagged one.
    if (!reason.isEmpty())
    {
        static const QRegularExpression reSheetReason("\\[[^\\]]*#(\\d+)\\]");
        const QRegularExpressionMatch mSheet = reSheetReason.match(line);
        const int sheet = mSheet.hasMatch() ? mSheet.captured(1).toInt() : m_currentSheet;
        if (sheet > 0 && !m_invalidSheetReasons.contains(sheet))
        {
            m_invalidSheetReasons.insert(sheet, reason);
        }
    }

    //! Accumulate the COMPLETE output (never truncated) for the error window + its on-disk copy.
    m_fullLog << line;

    //! Small ring of the most recent output lines, surfaced in the popup's error state.
    static const int MAX_TAIL = 16;
    m_tail << line;
    while (m_tail.size() > MAX_TAIL)
    {
        m_tail.removeFirst();
    }
}

QString AudiverisRunner::writeFullLogToFile()
{
    if (m_outDir.isEmpty() || m_fullLog.isEmpty())
    {
        return QString();
    }

    const QString path = QDir(m_outDir).absoluteFilePath("reprise-audiveris.log");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        LOGW() << "Reprise: could not write Audiveris log to " << path;
        return QString();
    }

    f.write(m_fullLog.join('\n').toUtf8());
    f.close();

    m_logFilePath = path;
    return path;
}

void AudiverisRunner::onErrorOccurred()
{
    if (m_cancelled || !m_process)
    {
        return;
    }

    const QString err = m_process->errorString();
    const QString tail = m_tail.join('\n');
    writeFullLogToFile();

    m_process->deleteLater();
    m_process = nullptr;

    emit finished(false, QString(), muse::qtrc("reprise", "Audiveris could not be started.") + " " + err, tail);
}

void AudiverisRunner::onFinished(int exitCode)
{
    if (m_cancelled)
    {
        return;
    }

    //! Drain anything still buffered before we judge the outcome.
    onReadyReadStandardOutput();
    onReadyReadStandardError();

    LOGI() << "[audiveris] exited with code " << exitCode;

    const QString tail = m_tail.join('\n');
    writeFullLogToFile();

    if (m_process)
    {
        m_process->deleteLater();
        m_process = nullptr;
    }

    //! Skip-invalid re-export finish (handled FIRST, before the exit-code branch): this run was the minimal second pass that
    //! re-exported the .omr book with the failed pages dropped. Its exit code is not authoritative (a partial export can
    //! still report non-zero), so judge it purely by whether MusicXML was produced. Multiple .mxl means the survivor pages
    //! were non-contiguous and Audiveris split them into movements -> flag m_multiMovement so the notice explains the gap.
    if (m_reExportedOmr)
    {
        std::vector<io::path_t> outputs = locateAllOutputs(io::path_t(m_outDir), "mxl");
        if (outputs.empty())
        {
            outputs = locateAllOutputs(io::path_t(m_outDir), "xml");
        }

        if (outputs.empty())
        {
            LOGE() << "Reprise: skip-invalid re-export produced no MusicXML under " << m_outDir;
            emit finished(false, QString(),
                          muse::qtrc("reprise", "Audiveris could not import the remaining pages after skipping the unreadable ones."),
                          tail);
            return;
        }

        m_multiMovement = outputs.size() > 1;
        LOGI() << "Reprise: skip-invalid re-export produced " << outputs.size() << " movement(s), skipped "
               << m_invalidSheetReasons.size() << " page(s)";
        emit finished(true, outputs.front().toQString(), QString(), tail);
        return;
    }

    if (exitCode != 0)
    {
        //! PDF retry-on-crash for Audiveris #711: when a PDF run crashes with the edge-probe signature
        //! (ArrayIndexOutOfBoundsException) and we have not padded yet, re-rasterize the PDF to padded pages and re-run
        //! ONCE on the padded copy. Guarded by m_retriedWithPad so a padded re-run that still fails is reported, never
        //! re-padded. Images never reach here (they are padded up front in start()), so m_pdfPath is the original PDF.
        if (!m_retriedWithPad && m_sawArrayIndexCrash && io::suffix(m_pdfPath) == "pdf")
        {
            m_retriedWithPad = true;
            LOGW() << "Reprise: Audiveris #711 edge-probe crash detected on " << m_pdfPath.toQString()
                   << "; retrying once on a white-margin-padded copy.";

            const io::path_t paddedPdf = buildPaddedPdf(m_pdfPath, m_dpi);
            if (!paddedPdf.empty())
            {
                //! FIX 1: surface the recovery pass BEFORE re-entering start(). The #711 pad-retry is a full SECOND
                //! Audiveris run; without this the progress bar restarting looks like a silent duplicate to the user.
                emit retryingAfterError();

                //! Re-launch an identical run on the padded PDF. start() swaps m_pdfPath and resets per-run state, but
                //! leaves m_retriedWithPad set, so this cannot loop.
                start(paddedPdf, m_pages, m_dpi, m_options);
                return;
            }

            LOGW() << "Reprise: PDF pad failed; reporting the original Audiveris failure.";
        }

        //! Skip-invalid re-export: some (but maybe not all) pages failed with a known signature. Rather than failing the
        //! whole import, launch a minimal second pass that re-exports the already-recognised .omr book with the failed pages
        //! dropped, so the valid pages still import. Ordered AFTER the #711 pad-retry (a crash is worth a padded re-run
        //! first) and guarded by m_reExportedOmr so it runs at most once. Only when we actually recorded per-sheet reasons.
        if (!m_reExportedOmr && !m_invalidSheetReasons.isEmpty())
        {
            const io::path_t omr = omrPath();
            if (!omr.empty())
            {
                LOGW() << "Reprise: " << m_invalidSheetReasons.size() << " page(s) failed recognition; re-exporting the "
                       << "recognised pages from " << omr.toQString() << " with the unreadable ones skipped.";
                if (startOmrReExport(omr))
                {
                    return;
                }
            }
        }

        LOGE() << "Reprise: Audiveris exited with code " << exitCode;

        QString msg;
        if (m_sawNoStaves)
        {
            msg = muse::qtrc("reprise", "No music was found on this page -- it looks like a cover or title page. Choose the pages that contain music and try again.");
        }
        else if (m_sawLowInterline)
        {
            msg = muse::qtrc("reprise", "This page's resolution is too low for the staff size. Try a higher resolution scan (around 300 DPI), or use a scan that has one score page per page of the PDF/image.");
        }
        else if (m_sawTooLarge)
        {
            msg = muse::qtrc("reprise", "The scan image is too large to process at this quality. Try a lower resolution (DPI) or fewer pages.");
        }
        else
        {
            msg = muse::qtrc("reprise", "Audiveris failed to recognise the scan (exit code %1).").arg(exitCode);
        }

        emit finished(false, QString(), msg, tail);
        return;
    }

    const std::string base = QFileInfo(m_pdfPath.toQString()).completeBaseName().toStdString();

    io::path_t output = locateOutput(io::path_t(m_outDir), base, "mxl");
    if (output.empty())
    {
        output = locateOutput(io::path_t(m_outDir), base, "xml");
    }

    if (output.empty())
    {
        LOGE() << "Reprise: Audiveris produced no .mxl/.xml under " << m_outDir;
        emit finished(false, QString(), muse::qtrc("reprise", "Audiveris produced no MusicXML output."), tail);
        return;
    }

    LOGI() << "Reprise: Audiveris output located: " << output.toQString();
    emit finished(true, output.toQString(), QString(), tail);
}
