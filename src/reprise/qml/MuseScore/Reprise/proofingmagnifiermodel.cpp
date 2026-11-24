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

#include "proofingmagnifiermodel.h"

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtPdf/QPdfDocument>
#include <QtPdf/QPdfDocumentRenderOptions>

#include "notation/inotation.h"
#include "notation/inotationinteraction.h"
#include "notation/inotationselection.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurebase.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"

#include "log.h"

using namespace mu::reprise;

namespace {
//! Longest-side pixel budget for a rendered magnifier page. The magnifier shows a zoomed CROP of a single spot, so it
//! renders higher than the A3 following band (BAND_PAGE_MAX_SIDE = 2600): big enough that a magnified note head stays
//! crisp (~345 dpi for US Letter), bounded so a whole book still renders quickly and one decoded page stays modest in
//! memory. The scan itself is the detail ceiling (a 300-450 dpi scan), so beyond this the magnifier simply upscales the
//! native scan pixels -- exactly as PhotoScore does; you cannot invent detail the scan never captured.
constexpr int32_t MAGNIFIER_PAGE_MAX_SIDE = 4000;
}

ProofingMagnifierModel::ProofingMagnifierModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    //! NOTE: nothing that resolves the IoC context may run here. iocCtxForQmlObject(this) is lazy (it reads
    //! qmlContext(this), attached only AFTER this ctor returns); touching projectSession() / iocContext() now trips the
    //! "QQmlContext is not set" assert and crashes on first use. All context-dependent setup is deferred to load().
}

ProofingMagnifierModel::~ProofingMagnifierModel()
{
    //! Drop the live-model registration so the dev reprise.proofing-debug-set-zoom handler never drives a destroyed
    //! magnifier (the control also holds us as a QPointer, so this is belt-and-braces). Guarded if the IoC is already gone.
    if (proofingViewControl())
    {
        proofingViewControl()->unregisterMagnifier(this);
    }

    clearRendered();
}

void ProofingMagnifierModel::load()
{
    if (!m_inited)
    {
        m_inited = true;

        //! Register this live magnifier with the proofing view-control bridge so the dev reprise.proofing-debug-set-zoom
        //! tool can drive our userZoom headlessly. Unregistered in the dtor (the control also holds us as a QPointer).
        if (proofingViewControl())
        {
            proofingViewControl()->registerMagnifier(this);
        }

        //! Hover-follow throttle window (~50ms), mirroring the following band. scheduleHoverNote() applies the FIRST move of a
        //! burst immediately (leading edge) and opens this window; moves arriving during the window are latched and applied
        //! here when it elapses (trailing edge). A continuous sweep thus updates at a steady ~20 Hz instead of only after the
        //! mouse fully stops -- the old "sometimes slow, sometimes snappy" behaviour of a restart-every-move debounce. The
        //! per-apply cost is just a sourceClipRect re-crop of the already-rendered page PNG (rendered once at import, cached),
        //! so ~20 Hz is cheap. Single-shot; owned by this (QObject parent).
        m_hoverDebounce = new QTimer(this);
        m_hoverDebounce->setSingleShot(true);
        m_hoverDebounce->setInterval(50);
        connect(m_hoverDebounce, &QTimer::timeout, this, [this]()
        {
            //! Trailing edge: apply the latest note/measure latched during the window. Do NOT reopen the window -- the next
            //! move re-arms the leading edge, keeping the cadence steady without busy-spinning when idle.
            applyHover();
        });

        //! Re-render whenever a fresh import replaces (or clears) the held map, independent of notation-change timing.
        //! DEFERRED (not inline): mapChanged() is a synchronous notify fired from inside NotationProject::doLoadRepr on the
        //! UI thread, so rendering here inline would freeze the .repr open. scheduleRebuild() posts the render to the next
        //! event-loop turn so the open finishes first and the editor is interactive before the magnifier renders.
        proofingMapService()->mapChanged().onNotify(this, [this]()
        {
            scheduleRebuild();
        });

        //! Re-point the selection subscription whenever the user switches to a different score/part.
        projectSession()->currentNotationChanged().onNotify(this, [this]()
        {
            resubscribeSelection();
        });
    }

    //! Defer the initial render too (panel first shown): scheduleRebuild() keeps the first paint off whatever triggered the
    //! panel's creation, so the placeholder shows immediately and the render follows on the next event-loop turn. Selection
    //! wiring is cheap (no render) and stays synchronous so follow is ready the moment pages exist.
    scheduleRebuild();
    resubscribeSelection();

    return;
}

void ProofingMagnifierModel::scheduleRebuild()
{
    if (m_rebuildScheduled)
    {
        return;
    }

    m_rebuildScheduled = true;

    //! Context object `this`: Qt auto-cancels the pending single-shot if this model is destroyed first (the DockPanel's
    //! Loader destroys it when the panel is hidden), so the deferred render never fires on a dead magnifier.
    QTimer::singleShot(0, this, [this]()
    {
        m_rebuildScheduled = false;
        rebuild();
    });

    return;
}

void ProofingMagnifierModel::rebuild()
{
    m_inRebuild = true;

    clearRendered();
    m_pages.clear();
    m_pdfDoc.reset();
    m_sourcePath.clear();
    m_renderDir.clear();
    m_sourceIsPdf = false;
    m_pageCount = 0;

    const auto svc = proofingMapService();
    if (svc && svc->hasMap())
    {
        const ProofingMap& map = svc->map();
        ++m_generation;

        m_renderDir = renderDir();
        QDir().mkpath(m_renderDir);

        m_sourcePath = QString::fromStdString(map.sourcePath);
        m_sourceIsPdf = m_sourcePath.endsWith(".pdf", Qt::CaseInsensitive);

        //! Open the source + size placeholders only; the page pixels render lazily (ensurePageRendered) one page at a time.
        prepareLazyPages(map);
    }

    //! Render ONLY the first page now so the panel shows immediately; the rest render on demand as the follow reaches them.
    //! Gate hasMap on that first page actually rendering (preserves the pre-lazy "advertise once at least one page rendered").
    const bool firstOk = (m_pageCount > 0) && ensurePageRendered(0);
    if (!firstOk)
    {
        m_pages.clear();
        m_pageCount = 0;
        m_pdfDoc.reset();
    }

    m_hasMap = m_pageCount > 0;

    m_currentPageIndex = (m_pageCount > 0) ? 0 : -1;
    m_hasCurrent = false;
    m_isFallback = false;
    m_currentMeasure = -1;
    m_currentNoteName.clear();

    m_inRebuild = false;

    emit changed();

    //! Recompute the magnified box for whatever is currently selected against the fresh map.
    updateFollow();

    return;
}

void ProofingMagnifierModel::prepareLazyPages(const ProofingMap& map)
{
    int32_t pageCount = 0;

    if (m_sourceIsPdf)
    {
        m_pdfDoc = std::make_unique<QPdfDocument>();
        if (m_pdfDoc->load(m_sourcePath) != QPdfDocument::Error::None || m_pdfDoc->status() != QPdfDocument::Status::Ready)
        {
            LOGW() << "Reprise: proofing magnifier: cannot load source PDF " << m_sourcePath;
            m_pdfDoc.reset();

            return;
        }

        const int32_t docPages = m_pdfDoc->pageCount();
        if (docPages <= 0)
        {
            m_pdfDoc.reset();

            return;
        }

        //! Recognised page count (map sheets), leading pages -- correct for an "all pages" import; fall back to every page
        //! when the map has no page list (mirrors the following band).
        pageCount = map.pages.empty() ? docPages : static_cast<int32_t>(map.pages.size());
        pageCount = std::min(pageCount, docPages);
    }
    else
    {
        QImageReader reader(m_sourcePath);

        //! imageCount() FIRST: it primes qtiff's directory count so jumpToNextImage() walks a multi-frame TIFF (see the
        //! multi-frame TIFF idiom in reference_musescore_build_gotchas). For a plain single-frame image it returns 1.
        int32_t frameCount = reader.imageCount();
        if (frameCount <= 0)
        {
            frameCount = 1;
        }

        pageCount = map.pages.empty() ? frameCount : static_cast<int32_t>(map.pages.size());
        pageCount = std::min(pageCount, frameCount);
    }

    //! One placeholder per page: the Audiveris sheet-image dims are known from the map now; the rendered PNG url + pixel
    //! size are filled lazily by ensurePageRendered the first time the magnifier actually displays that page.
    for (int32_t i = 0; i < pageCount; ++i)
    {
        const int32_t sheetW = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageWidth : 0;
        const int32_t sheetH = (i < static_cast<int32_t>(map.pages.size())) ? map.pages[i].imageHeight : 0;

        QVariantMap entry;
        entry["url"] = QString();
        entry["width"] = 0;
        entry["height"] = 0;
        entry["sheetWidth"] = sheetW;
        entry["sheetHeight"] = sheetH;
        m_pages.append(entry);
    }

    m_pageCount = pageCount;

    return;
}

bool ProofingMagnifierModel::ensurePageRendered(int32_t index)
{
    if (index < 0 || index >= m_pageCount || index >= static_cast<int32_t>(m_pages.size()))
    {
        return false;
    }

    QVariantMap entry = m_pages.at(index).toMap();
    if (!entry.value("url").toString().isEmpty())
    {
        return true;   //! already rendered (cached)
    }

    const int32_t sheetW = entry.value("sheetWidth").toInt();
    const int32_t sheetH = entry.value("sheetHeight").toInt();

    const QImage img = renderPageImage(index, sheetW, sheetH);
    if (img.isNull())
    {
        return false;
    }

    const QVariantMap filled = savePageEntry(img, index, sheetW, sheetH);
    if (filled.isEmpty())
    {
        return false;
    }

    m_pages[index] = filled;

    if (!m_inRebuild)
    {
        emit changed();
    }

    return true;
}

QImage ProofingMagnifierModel::renderPageImage(int32_t index, int32_t sheetW, int32_t sheetH) const
{
    if (index < 0 || index >= m_pageCount)
    {
        return QImage();
    }

    if (m_sourceIsPdf)
    {
        if (!m_pdfDoc)
        {
            return QImage();
        }

        const QSizeF pts = m_pdfDoc->pagePointSize(index);
        if (pts.width() <= 0.0 || pts.height() <= 0.0)
        {
            return QImage();
        }

        //! #6 (0.16.15): cap the render at the NATIVE scan resolution. The proofing-scan page carries a raster at the scan's
        //! own pixel size (the Audiveris sheet-image dims), so rendering to the full 4000px budget just UPSCALES a ~3399px
        //! scan -- more work + memory for zero added detail (the scan is the detail ceiling). Clamp the longest rendered side
        //! to min(MAGNIFIER_PAGE_MAX_SIDE, native longest side). No native size -> the full budget (keeps it displayable).
        const int32_t nativeLongest = std::max(sheetW, sheetH);
        const int32_t sideCap = (nativeLongest > 0) ? std::min(MAGNIFIER_PAGE_MAX_SIDE, nativeLongest) : MAGNIFIER_PAGE_MAX_SIDE;

        //! Fit the page into a sideCap box, preserving aspect, and render straight to that pixel size.
        const double aspect = pts.width() / pts.height();
        int32_t h = sideCap;
        int32_t w = static_cast<int32_t>(std::lround(static_cast<double>(h) * aspect));
        if (w > sideCap)
        {
            w = sideCap;
            h = static_cast<int32_t>(std::lround(static_cast<double>(w) / aspect));
        }

        QPdfDocumentRenderOptions options;
        const QImage rendered = m_pdfDoc->render(index, QSize(std::max<int32_t>(1, w), std::max<int32_t>(1, h)), options);
        if (rendered.isNull())
        {
            LOGW() << "Reprise: proofing magnifier: PDF page " << index << " rendered empty";
        }

        return rendered;
    }

    //! Image source: re-open the reader and walk to frame `index` (single-frame files return frame 0). imageCount() first
    //! primes qtiff's directory count so jumpToNextImage() walks a multi-frame TIFF (the multi-frame TIFF idiom).
    QImageReader reader(m_sourcePath);
    (void)reader.imageCount();
    for (int32_t i = 0; i < index; ++i)
    {
        if (!reader.jumpToNextImage())
        {
            return QImage();
        }
    }

    const QImage frame = reader.read();
    if (frame.isNull())
    {
        LOGW() << "Reprise: proofing magnifier: image frame " << index << " could not be read from " << m_sourcePath;
    }

    return frame;
}

QVariantMap ProofingMagnifierModel::savePageEntry(const QImage& image, int32_t index, int32_t sheetW, int32_t sheetH) const
{
    QImage out = image;
    const int32_t longest = std::max(out.width(), out.height());
    if (longest > MAGNIFIER_PAGE_MAX_SIDE)
    {
        out = (out.width() >= out.height())
              ? out.scaledToWidth(MAGNIFIER_PAGE_MAX_SIDE, Qt::SmoothTransformation)
              : out.scaledToHeight(MAGNIFIER_PAGE_MAX_SIDE, Qt::SmoothTransformation);
    }

    const QString path = m_renderDir + QString("/sheet%1_%2.png").arg(index).arg(m_generation);
    if (!out.save(path, "PNG"))
    {
        LOGW() << "Reprise: proofing magnifier: failed to write page png " << path;

        return QVariantMap();
    }

    //! Fall back to the rendered dims when the map carries no sheet-image size for this page: the QML then scales the
    //! (absent) boxes against the render itself, which keeps the page displayable even if follow has nothing to line up.
    const int32_t sw = sheetW > 0 ? sheetW : out.width();
    const int32_t sh = sheetH > 0 ? sheetH : out.height();

    QVariantMap entry;
    entry["url"] = QUrl::fromLocalFile(path).toString();
    entry["width"] = out.width();
    entry["height"] = out.height();
    entry["sheetWidth"] = sw;
    entry["sheetHeight"] = sh;

    return entry;
}

void ProofingMagnifierModel::resubscribeSelection()
{
    //! Destroy + recreate the receiver so the previous notation's selectionChanged + hover subscriptions are dropped before
    //! we bind the new ones (prevents a stale score's selection/hover from driving this magnifier after a switch).
    m_selectionReceiver = std::make_unique<muse::async::Asyncable>();

    //! Any pending hover apply belonged to the previous notation -- drop it so a switch never follows a stale note/measure.
    m_pendingHoverNote = mu::notation::HoveredNoteInfo();
    m_pendingHoverMeasure = -1;
    if (m_hoverDebounce)
    {
        m_hoverDebounce->stop();
    }

    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;
    if (notation && notation->interaction())
    {
        notation->interaction()->selectionChanged().onNotify(m_selectionReceiver.get(), [this]()
        {
            updateFollow();
        });

        //! Hover-follow ALONGSIDE selection: the ungated hover path on the input controller emits the note AND the measure
        //! under the mouse; we debounce + apply (note-level preferred, measure-level fallback). Click/selection unchanged.
        notation->interaction()->hoveredNoteChanged().onReceive(m_selectionReceiver.get(), [this](const mu::notation::HoveredNoteInfo& note)
        {
            scheduleHoverNote(note);
        });

        notation->interaction()->hoveredMeasureChanged().onReceive(m_selectionReceiver.get(), [this](int measure)
        {
            scheduleHoverMeasure(static_cast<int32_t>(measure));
        });
    }

    updateFollow();

    return;
}

void ProofingMagnifierModel::updateFollow()
{
    const mu::notation::INotationPtr notation = projectSession() ? projectSession()->currentNotation() : nullptr;

    //! Resolve the selection to the note to magnify (preferred) and/or the enclosing measure (the fallback target).
    mu::engraving::Note* note = nullptr;
    mu::engraving::MeasureBase* measureBase = nullptr;

    if (notation && notation->interaction())
    {
        const mu::notation::INotationSelectionPtr sel = notation->interaction()->selection();
        if (sel && !sel->isNone())
        {
            mu::engraving::EngravingItem* el = sel->element();
            if (el && el->isNote())
            {
                note = mu::engraving::toNote(el);
            }
            else
            {
                //! A chord/stem/range hit: take the first selected note so the magnifier still follows a real head.
                const std::vector<mu::engraving::Note*> notes = sel->notes();
                if (!notes.empty())
                {
                    note = notes.front();
                }
            }

            //! The enclosing measure drives the fallback marker (grace/untrusted note, or a pure measure selection).
            measureBase = sel->startMeasureBase();
        }
    }

    const bool hasMap = proofingMapService() && proofingMapService()->hasMap();

    if (hasMap && note && note->chord() && note->chord()->measure())
    {
        //! Derive the Phase B1 runtime key straight from the live Note (NO translation). The service matches on
        //! (gm, staffIdx, tickInMeasure, step, octave); voice is advisory and excluded.
        const mu::engraving::Measure* measure = note->chord()->measure();
        const int32_t gm = measure->measureIndex();
        const int32_t staffIdx = static_cast<int32_t>(note->staffIdx());
        const int32_t tickInMeasure = (note->tick() - measure->tick()).ticks();
        const int32_t step = mu::engraving::tpc2step(note->tpc());
        const int32_t octave = note->octave();

        //! Header label shows the note's REAL pitch INCLUDING accidental (e.g. "E♭4"), taken straight from the live
        //! Note's tpc+pitch -- tpcUserName renders Unicode ♭/♯ with the sounding octave. The box MATCH stays diatonic
        //! (step+octave are what noteBox() keys on); only the displayed label carries the accidental, so a flat/sharp
        //! head reads correctly in the header instead of collapsing to its diatonic letter.
        const QString noteName = mu::engraving::tpcUserName(note->tpc(), note->pitch()).toQString();

        followNoteKey(gm, staffIdx, tickInMeasure, step, octave, noteName);

        return;
    }

    //! A measure/range selection with no single note (or a grace/untrusted note that has no chord-measure) -> dashed
    //! measure-level fallback. Nothing selected / no map -> clear hasCurrent (keeps the current page).
    if (hasMap && measureBase)
    {
        followMeasureOnly(measureBase->measureIndex());

        return;
    }

    applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

    return;
}

void ProofingMagnifierModel::applyFollow(bool found, bool fallback, int32_t page, int32_t gm,
                                         int32_t bx, int32_t by, int32_t bw, int32_t bh, const QString& noteName)
{
    const int32_t newPage = found ? page
                          : (m_currentPageIndex >= 0 ? m_currentPageIndex
                                                     : (m_pages.isEmpty() ? -1 : 0));

    const int32_t newMeasure = found ? gm : -1;
    const QString newName = fallback ? QString() : noteName;

    const bool unchanged = (found == m_hasCurrent) && (fallback == m_isFallback) && (newPage == m_currentPageIndex)
                           && (newMeasure == m_currentMeasure) && (newName == m_currentNoteName)
                           && (!found || (bx == m_boxX && by == m_boxY && bw == m_boxW && bh == m_boxH));
    if (unchanged)
    {
        return;
    }

    m_hasCurrent = found;
    m_isFallback = fallback;
    m_currentPageIndex = newPage;
    m_currentMeasure = newMeasure;
    m_currentNoteName = newName;
    if (found)
    {
        m_boxX = bx;
        m_boxY = by;
        m_boxW = bw;
        m_boxH = bh;
    }

    //! LAZY: render the page we are about to magnify if it has not been rendered yet, so the bound QML Image has a url the
    //! instant followChanged fires (ensurePageRendered emits changed() when it fills, so pages[newPage] is ready first).
    if (newPage >= 0)
    {
        ensurePageRendered(newPage);
    }

    emit followChanged();

    return;
}

void ProofingMagnifierModel::followNoteKey(int32_t gm, int32_t staffIdx, int32_t tickInMeasure, int32_t step, int32_t octave,
                                           const QString& noteName)
{
    if (!proofingMapService() || !proofingMapService()->hasMap())
    {
        applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

        return;
    }

    const ProofingNoteBox* box = proofingMapService()->noteBox(gm, staffIdx, tickInMeasure, step, octave);
    if (box)
    {
        applyFollow(true, false, box->pageIndex, gm, box->x, box->y, box->w, box->h, noteName);

        return;
    }

    //! SELF-CHECK (Phase B1 worker flagged): a lookup miss where we expected a hit is the one thing to watch. Log the key +
    //! the engraving tick so a miss is diagnosable against the stored boxes (reprise.get-proofing-map), then fall back to
    //! the enclosing measure rect -- never a false note box.
    LOGD() << "Reprise: magnifier note box MISS -> measure fallback: gm=" << gm << " staff=" << staffIdx
           << " tickInMeasure=" << tickInMeasure << " step=" << step << " octave=" << octave;

    followMeasureOnly(gm);

    return;
}

void ProofingMagnifierModel::followMeasureOnly(int32_t gm)
{
    if (gm < 0 || !proofingMapService() || !proofingMapService()->hasMap())
    {
        applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

        return;
    }

    const ProofingMap& map = proofingMapService()->map();
    for (const ProofingMeasureRect& r : map.measures)
    {
        if (r.globalMeasureIndex == gm)
        {
            applyFollow(true, true, r.pageIndex, gm, r.x, r.y, r.w, r.h, QString());

            return;
        }
    }

    //! Map held but no rect for this measure index -> clear hasCurrent (keeps the current page).
    applyFollow(false, false, m_currentPageIndex, -1, 0, 0, 0, 0, QString());

    return;
}

void ProofingMagnifierModel::scheduleHoverNote(const mu::notation::HoveredNoteInfo& note)
{
    //! Ignore all hover while no map is held (nothing to magnify). Otherwise latch the latest hovered note, then drive the
    //! LEADING-edge + fixed-window throttle. NOTE on ordering: NotationViewInputController::updateHoveredMeasure (and the
    //! hover-leave path) ALWAYS emit setHoveredMeasure() BEFORE setHoveredNote() for a given mouse event, so by the time this
    //! note channel fires both m_pendingHoverMeasure and m_pendingHoverNote are latched for THIS move -- which is why the
    //! leading apply lives here (the last channel of the pair), so applyHover() sees the complete state and picks note-vs-
    //! measure correctly with no measure-before-note flash.
    if (!m_hasMap)
    {
        return;
    }

    m_pendingHoverNote = note;

    //! LEADING EDGE: the first move of a burst (no window open) applies immediately so the magnifier feels instant, then opens
    //! the throttle window. Moves within the window are latched and applied by the trailing timeout -- a steady update rate
    //! during a continuous sweep instead of apply-only-after-the-mouse-stops.
    if (m_hoverDebounce && !m_hoverDebounce->isActive())
    {
        applyHover();
        m_hoverDebounce->start();
    }

    return;
}

void ProofingMagnifierModel::scheduleHoverMeasure(int32_t measure)
{
    //! Latch the latest hovered measure (INCLUDING -1 on leave, so applyHover() can tell a leave from a rest-hover). This
    //! channel fires FIRST of the measure/note pair (see scheduleHoverNote), so it only latches -- the note channel that
    //! follows drives the leading-edge apply + throttle window once both values are in hand. Ignored while no map is held.
    if (!m_hasMap)
    {
        return;
    }

    m_pendingHoverMeasure = measure;

    return;
}

void ProofingMagnifierModel::applyHover()
{
    //! Note-level wins when the cursor is over a head; else the hovered measure drives the dashed measure-level fallback
    //! (rest / between notes / empty). A leave (no note AND measure < 0) holds the last view rather than blanking it --
    //! matching the following band, so moving the mouse off the score to read the magnifier itself doesn't clear it.
    if (m_pendingHoverNote.isValid())
    {
        const QString noteName = mu::engraving::tpcUserName(m_pendingHoverNote.tpc, m_pendingHoverNote.pitch).toQString();
        followNoteKey(m_pendingHoverNote.globalMeasureIndex, m_pendingHoverNote.staffIdx, m_pendingHoverNote.tickInMeasure,
                      m_pendingHoverNote.step, m_pendingHoverNote.octave, noteName);

        return;
    }

    if (m_pendingHoverMeasure >= 0)
    {
        followMeasureOnly(m_pendingHoverMeasure);

        return;
    }

    return;
}

void ProofingMagnifierModel::setUserZoom(double zoom)
{
    const double clamped = std::clamp(zoom, 0.5, 4.0);
    if (qFuzzyCompare(clamped, m_userZoom))
    {
        return;
    }

    m_userZoom = clamped;
    emit userZoomChanged();

    return;
}

void ProofingMagnifierModel::clearRendered()
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

QString ProofingMagnifierModel::renderDir() const
{
    //! Per-instance subdir so the magnifier never tramples the following band's (or another view's) rendered pages, and
    //! the dtor's sweep only removes this magnifier's own files.
    return QDir::tempPath() + "/reprise-proofing-magnifier/" + QString::number(reinterpret_cast<quintptr>(this));
}
