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

#include <cstdint>
#include <memory>
#include <vector>

#include <QAbstractListModel>
#include <QString>
#include <qqmlintegration.h>

#include "modularity/ioc.h"
#include "async/asyncable.h"

#include "project/iprojectsession.h"

#include "internal/badtimingcheck.h"
#include "internal/iproofingviewcontrol.h"

class QTimer;

namespace mu::engraving {
class Measure;
}

namespace mu::reprise {
//! View-model behind the Reprise Two-Pane Proofing BAD-TIMING NAVIGATOR (Phase 1) -- a PhotoScore-style worklist of the
//! measures whose durations do not add up, hosted as a collapsing section beneath the magnifier inside the always-open
//! "Full detail view" right-column panel (so the model is never destroyed and stays live across score switches). OMR
//! regularly emits a measure
//! that is short or over-stuffed (a missed/extra beam, a dropped rest); MuseScore's own sanity check flags these but then
//! pops a single modal "this file is corrupted" dialog and moves on. This panel instead SURFACES each problem measure as a
//! clickable row so the user can jump to it and fix it, faithful-to-Audiveris: it never auto-corrects, only reports.
//!
//! It performs a READ-ONLY walk of the current notation's score that replicates the ~30-line voices[0]-vs-measure-length
//! comparison in Score::sanityCheckLocal (src/engraving/dom/check.cpp) but MUTATES NOTHING (the real sanity check rewrites
//! full-measure-rest ticks and per-measure corrupted flags, so it must never be called for this). It re-walks on every
//! undo-stack change so a row disappears the instant its measure is fixed, and re-points its subscriptions when the user
//! switches score/part. Clicking a row selects that measure's first chord/rest; the following band, the magnifier and the
//! editor all already follow selectionChanged, so the whole proofing view jumps with no extra wiring.
class BadTimingNavigatorModel : public QAbstractListModel, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    //! Number of problem rows currently listed (0 == clean score -> the QML shows the "No timing problems found" state).
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    //! true once a score is open to walk (whether or not it has problems) -- distinguishes "clean" from "nothing loaded".
    Q_PROPERTY(bool hasScore READ hasScore NOTIFY countChanged)

    QML_ELEMENT

    muse::ContextInject<mu::project::IProjectSession> projectSession = { this };
    muse::GlobalInject<mu::reprise::IProofingViewControl> proofingViewControl;

public:
    explicit BadTimingNavigatorModel(QObject* parent = nullptr);
    ~BadTimingNavigatorModel() override;

    //! The problem row type and the problem-kind enum are defined once in internal/badtimingcheck.h and shared with the
    //! dock controller, so this worklist's rows and the panel's visibility are driven by ONE read-only walk and can never
    //! disagree (see scanScoreForBadTiming).

    enum Roles
    {
        DisplayRole = Qt::UserRole + 1,
        MeasureNumberRole,
        StaffRole,
        KindRole,
        FoundRole,
        ExpectedRole,
        AccentRole,         //! per-kind accent colour (hex string) so the QML keeps the kinds visually distinguishable
        DetailRole,         //! hover-tooltip detail: the exact "<kind>  <found> of <expected>" ratio kept off the row text
        CanRegularizeRole   //! true when the row's measure is UNDER-full (ticks < timesig) -> the one-click Fix is safe
    };

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_entries.size()); }
    bool hasScore() const { return m_hasScore; }

    //! Called from the panel's Component.onCompleted: subscribe (once) to notation/undo-stack changes and do the first walk.
    Q_INVOKABLE void load();

    //! Select the problem measure for row's first chord/rest (staff-aware), so the band + magnifier + editor all follow.
    Q_INVOKABLE void jumpTo(int row);

    //! One-click fix for an UNDER-full ("Too few beats") row: extend that measure to its nominal time signature, filling
    //! the freed beats with RESTS (Measure::adjustToLen's default -- the user then replaces them; we never guess the
    //! missing note). SAFE-ONLY: a no-op unless the row's live measure is under-full (ticks < timesig), so it can NEVER
    //! truncate an over-full bar ("Too many beats") -- that would delete the overflow notes. One undoable step (wrapped in
    //! the undo stack). The stackChanged subscription then re-walks and clears this row + its red highlight automatically.
    Q_INVOKABLE void regularize(int row);

    //! WARP the OS mouse cursor onto row's measure in the EDITOR (lower score pane), centred on the measure, AFTER the
    //! editor has scrolled it into view. Landing the real cursor over the bar triggers the editor's existing hover-follow,
    //! so the following band + magnifier re-sync to the measure (the whole point of the warp). Deferred via a one-shot
    //! settle timer and the geometry RECOMPUTED at fire time, so it warps onto the SETTLED scroll position, not the pre-scroll
    //! rect. A no-op if the editor view is not registered / not visible or the measure rect cannot be resolved (never warps
    //! to a garbage point). This is the SINGLE reusable warp entry point: the navigator double-click calls it, the one-click
    //! Fix (regularize) routes through the same core, and the planned 0.18.0 navigator redesign can re-wire new triggers to it.
    Q_INVOKABLE void warpCursorToMeasure(int row);

    //! Same warp, addressed by GLOBAL measure index (0-based, pickup = 0) instead of a navigator row -- for a caller that
    //! holds the measure index rather than a row (e.g. the planned 0.18.0 navigator redesign). Shares the core with the above.
    Q_INVOKABLE void warpCursorToMeasureIndex(int globalMeasureIndex);

signals:
    void countChanged();

private:
    //! Re-walk the current notation's score read-only (via scanScoreForBadTiming) and reset the model rows. Safe with no
    //! score (clears to empty).
    void rebuild();

    //! Drop + rebind the undo-stack + selection subscriptions for the CURRENT notation (at load and on every switch), then
    //! re-walk. Mirrors the ProofingStripModel/magnifier resubscribe idiom so a previous score's edits never drive us.
    void resubscribe();

    //! PLAIN-LANGUAGE row text the user reads at a glance: "m. {measureNumber1} [staff {staffIdx+1}] \xE2\x80\x94 {plainKind}",
    //! where plainKind is "Too many beats" / "Too few beats" / "Repeat problem" -- deliberately NOT the raw time-signature
    //! ratio, because on a fresh OMR scan the nominal (Audiveris's read of the time signature) is often itself WRONG, so
    //! showing "11/4 of 4/4" on the row misleads. The exact ratio is preserved as the hover tooltip (detailText) instead.
    QString rowText(const BadTimingProblem& e) const;

    //! Hover-tooltip detail (DetailRole): the exact technical "{kind}  {found} of {expected}" ratio moved OFF the row so the
    //! visible row stays plain, but the precise numbers a power user wants are one hover away.
    QString detailText(const BadTimingProblem& e) const;

    //! Localised short label for a kind ("Incomplete", "Voice too long", "Corrupted repeat", "Wrong length"). Used by the
    //! tooltip detail (the technical name); the row itself uses the plain over/under-full phrasing (plainKindLabel).
    QString kindLabel(BadTimingKind kind) const;

    //! PLAIN over/under-full phrasing for the row: OVERFULL (found > expected: WrongLength-over, VoiceTooLong) -> "Too many
    //! beats"; UNDERFULL (found < expected: WrongLength-under, Incomplete) -> "Too few beats"; CorruptedRepeat -> "Repeat
    //! problem". This is what the user reads; it never cites the (possibly-wrong) nominal.
    QString plainKindLabel(const BadTimingProblem& e) const;

    //! Fixed accent colour (hex string) for a kind, kept here so the kind -> colour map lives in one place and the QML
    //! delegate just binds it. Hues are chosen to read on both the light-ivory and dark-charcoal proofing surfaces.
    QString kindAccent(BadTimingKind kind) const;

    //! Shared core of the cursor warp: remember the TARGET MEASURE (positional index, pickup = 0) and (re)start the one-shot
    //! settle timer. Capturing the INDEX -- not the navigator ROW -- is deliberate: a regularize commit synchronously re-walks
    //! m_entries (stackChanged -> rebuild), so a row would be stale by the time the timer fires, but the positional measure
    //! index is stable across a duration-only edit. Both Q_INVOKABLE entry points and regularize() funnel through here.
    void scheduleWarp(int32_t measureIndex0);

    //! Fired by the settle timer: resolve the pending target measure's on-screen CENTRE in the editor and warp the OS cursor
    //! onto it. Maps measure->canvasBoundingRect() (LOGICAL/canvas coords) to view px via the registered editor paint view's
    //! fromLogicalRect() (a meta-call, no notationscene type), takes the rect centre, CLAMPS it into the editor viewport, maps
    //! to GLOBAL screen coords with QQuickItem::mapToGlobal (multi-monitor safe) and calls QCursor::setPos. No-op on any miss.
    void performWarp();

    bool m_inited = false;
    bool m_hasScore = false;
    std::vector<BadTimingProblem> m_entries;

    //! Per-row "safe to regularize" flag, parallel to m_entries (same indices), recomputed on every walk. true only when
    //! the row's live measure is UNDER-full (ticks < timesig), where adjustToLen(timesig) EXTENDS with rests (safe). An
    //! over-full bar (VoiceTooLong / WrongLength-over), an exactly-nominal bar (Incomplete in a full bar) or a corrupted
    //! repeat is false -> the Fix button is disabled, because there adjustToLen would truncate notes or do nothing.
    std::vector<bool> m_canRegularize;

    //! Receiver for the CURRENT notation's undo-stack + selection channels; destroyed+recreated on every notation switch so
    //! a previous notation's notifications never drive this model (the NotationSwitchListModel idiom the magnifier uses).
    std::unique_ptr<muse::async::Asyncable> m_notationReceiver;

    //! One-shot "settle" timer for the cursor warp: scheduleWarp (re)starts it; on timeout performWarp recomputes the
    //! measure geometry on the SETTLED scroll and warps. Lazily created, parented to this (auto-destroyed with the model).
    QTimer* m_warpTimer = nullptr;
    //! The measure the pending warp targets (positional, pickup = 0; -1 = none). Captured at schedule time so it survives the
    //! regularize re-walk, and resolved to a live Measure* in performWarp.
    int32_t m_pendingWarpMeasureIndex0 = -1;
};
}
