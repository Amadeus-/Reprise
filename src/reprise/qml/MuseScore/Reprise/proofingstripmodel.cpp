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

#include "proofingstripmodel.h"

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QtPdf/QPdfDocument>
#include <QtPdf/QPdfDocumentRenderOptions>

#include "log.h"

using namespace mu::reprise;

namespace {
//! Longest-side pixel budget for a rendered strip page. Large enough to stay legible when the user drags the strip
//! taller, small enough that a whole book renders quickly and the temp PNGs stay modest. The magnifier (Phase B) will
//! render at full resolution on demand -- the strip never needs the native pixel grid.
constexpr int32_t STRIP_PAGE_MAX_SIDE = 1600;
}

ProofingStripModel::ProofingStripModel(QObject* parent)
    : QObject(parent)
{
}

ProofingStripModel::~ProofingStripModel()
{
    clearRendered();
}

void ProofingStripModel::load()
{
    if (!m_inited)
    {
        m_inited = true;

        //! Re-render whenever a fresh import replaces (or clears) the held map, independent of notation-change timing.
        proofingMapService()->mapChanged().onNotify(this, [this]()
        {
            rebuild();
        });
    }

    rebuild();

    return;
}

void ProofingStripModel::rebuild()
{
    clearRendered();
    m_pages.clear();

    const auto svc = proofingMapService();
    if (svc && svc->hasMap())
    {
        const ProofingMap& map = svc->map();
        ++m_generation;

        const QString dir = renderDir();
        QDir().mkpath(dir);

        const QString source = QString::fromStdString(map.sourcePath);
        if (source.endsWith(".pdf", Qt::CaseInsensitive))
        {
            renderPdfPages(source, map, dir);
        }
        else
        {
            renderImagePages(source, map, dir);
        }
    }

    //! Only advertise the strip once at least one page actually rendered -- an unreadable source should not leave an
    //! empty band hanging above the score.
    m_hasMap = !m_pages.isEmpty();
    emit changed();

    return;
}

void ProofingStripModel::renderPdfPages(const QString& sourcePath, const ProofingMap& map, const QString& dir)
{
    QPdfDocument doc;
    if (doc.load(sourcePath) != QPdfDocument::Error::None || doc.status() != QPdfDocument::Status::Ready)
    {
        LOGW() << "Reprise: proofing strip: cannot load source PDF " << sourcePath;
        return;
    }

    const int32_t docPages = doc.pageCount();
    if (docPages <= 0)
    {
        return;
    }

    //! Render exactly the recognised page count (map sheets), taken as the leading pages of the document -- correct for
    //! an "all pages" / from-page-1 import. A non-leading page subrange is a known Phase A simplification (the map does
    //! not record which original page each sheet came from); fall back to every page when the map has no page list.
    int32_t pageCount = map.pages.empty() ? docPages : static_cast<int32_t>(map.pages.size());
    pageCount = std::min(pageCount, docPages);

    QPdfDocumentRenderOptions options;

    for (int32_t i = 0; i < pageCount; ++i)
    {
        const QSizeF pts = doc.pagePointSize(i);
        if (pts.width() <= 0.0 || pts.height() <= 0.0)
        {
            continue;
        }

        //! Fit the page into a STRIP_PAGE_MAX_SIDE box, preserving aspect, and render straight to that pixel size.
        const double aspect = pts.width() / pts.height();
        int32_t h = STRIP_PAGE_MAX_SIDE;
        int32_t w = static_cast<int32_t>(std::lround(static_cast<double>(h) * aspect));
        if (w > STRIP_PAGE_MAX_SIDE)
        {
            w = STRIP_PAGE_MAX_SIDE;
            h = static_cast<int32_t>(std::lround(static_cast<double>(w) / aspect));
        }

        const QImage rendered = doc.render(i, QSize(std::max<int32_t>(1, w), std::max<int32_t>(1, h)), options);
        if (rendered.isNull())
        {
            LOGW() << "Reprise: proofing strip: PDF page " << i << " rendered empty";
            continue;
        }

        appendPage(rendered, dir, i);
    }

    return;
}

void ProofingStripModel::renderImagePages(const QString& sourcePath, const ProofingMap& map, const QString& dir)
{
    QImageReader reader(sourcePath);

    //! imageCount() FIRST: it primes qtiff's directory count so jumpToNextImage() walks a multi-frame TIFF (see the
    //! multi-frame TIFF idiom in reference_musescore_build_gotchas). For a plain single-frame image it returns 1.
    int32_t frameCount = reader.imageCount();
    if (frameCount <= 0)
    {
        frameCount = 1;
    }

    //! Mirror the PDF branch: show the recognised sheet count, clamped to what the file actually holds.
    int32_t pageCount = map.pages.empty() ? frameCount : static_cast<int32_t>(map.pages.size());
    pageCount = std::min(pageCount, frameCount);

    for (int32_t i = 0; i < pageCount; ++i)
    {
        if (i > 0 && !reader.jumpToNextImage())
        {
            break;
        }

        const QImage frame = reader.read();
        if (frame.isNull())
        {
            LOGW() << "Reprise: proofing strip: image frame " << i << " could not be read from " << sourcePath;
            break;
        }

        appendPage(frame, dir, i);
    }

    return;
}

void ProofingStripModel::appendPage(const QImage& image, const QString& dir, int32_t index)
{
    QImage out = image;
    const int32_t longest = std::max(out.width(), out.height());
    if (longest > STRIP_PAGE_MAX_SIDE)
    {
        out = (out.width() >= out.height())
              ? out.scaledToWidth(STRIP_PAGE_MAX_SIDE, Qt::SmoothTransformation)
              : out.scaledToHeight(STRIP_PAGE_MAX_SIDE, Qt::SmoothTransformation);
    }

    const QString path = dir + QString("/sheet%1_%2.png").arg(index).arg(m_generation);
    if (!out.save(path, "PNG"))
    {
        LOGW() << "Reprise: proofing strip: failed to write page png " << path;
        return;
    }

    QVariantMap entry;
    entry["url"] = QUrl::fromLocalFile(path).toString();
    entry["width"] = out.width();
    entry["height"] = out.height();
    m_pages.append(entry);

    return;
}

void ProofingStripModel::clearRendered()
{
    QDir dir(renderDir());
    if (!dir.exists())
    {
        return;
    }

    const QStringList files = dir.entryList(QStringList() << "*.png", QDir::Files);
    for (const QString& file : files)
    {
        dir.remove(file);
    }

    return;
}

QString ProofingStripModel::renderDir() const
{
    //! Per-instance subdir so two notation views never trample one another's rendered pages, and the dtor's sweep only
    //! removes this strip's own files.
    return QDir::tempPath() + "/reprise-proofing/" + QString::number(reinterpret_cast<quintptr>(this));
}
