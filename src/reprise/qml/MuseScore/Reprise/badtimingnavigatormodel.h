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

namespace mu::engraving {
class Measure;
}

namespace mu::reprise {
//! View-model behind the Reprise Two-Pane Proofing BAD-TIMING NAVIGATOR (Phase 1) -- a PhotoScore-style worklist of the
//! measures whose durations do not add up, docked beneath the magnifier in the right column. OMR regularly emits a measure
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

public:
    explicit BadTimingNavigatorModel(QObject* parent = nullptr);
    ~BadTimingNavigatorModel() override;

    //! The kind of timing problem a row reports. Mirrors the branches of Score::sanityCheckLocal that this walk replicates.
    enum class Kind : int32_t
    {
        Incomplete = 0,     //! voice 1 (voices[0]) does not fill the measure length
        VoiceTooLong,       //! a voice > 1 holds more than the measure length
        CorruptedRepeat     //! a measure-repeat group whose repeat element is missing
    };
    Q_ENUM(Kind)

    //! One problem measure x staff, as surfaced to the worklist. measureIndex0 is the POSITIONAL 0-based index (pickup = 0),
    //! matching the proofing map's globalMeasureIndex and the magnifier header, so a click lines the whole view up. found /
    //! expected are rendered as fraction strings (e.g. "3/4", "4/4").
    struct BadTimingEntry
    {
        int32_t measureIndex0 = 0;
        int32_t measureNumber1 = 0;
        int32_t staffIdx = 0;
        Kind kind = Kind::Incomplete;
        int32_t voice = 0;
        QString found;
        QString expected;
    };

    enum Roles
    {
        DisplayRole = Qt::UserRole + 1,
        MeasureNumberRole,
        StaffRole,
        KindRole,
        FoundRole,
        ExpectedRole
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

signals:
    void countChanged();

private:
    //! Re-walk the current notation's score read-only and reset the model rows. Safe with no score (clears to empty).
    void rebuild();

    //! Append the problem rows for one measure x staff (replicates the sanityCheckLocal branches, NO mutation).
    void scanMeasure(const mu::engraving::Measure* measure, int32_t measureIndex0);

    //! Drop + rebind the undo-stack + selection subscriptions for the CURRENT notation (at load and on every switch), then
    //! re-walk. Mirrors the ProofingStripModel/magnifier resubscribe idiom so a previous score's edits never drive us.
    void resubscribe();

    //! Human row text: "m. {measureNumber1}  staff {staffIdx+1}  {kind}  {found}/{expected}".
    QString rowText(const BadTimingEntry& e) const;

    //! Localised short label for a kind ("Incomplete", "Voice too long", "Corrupted repeat").
    QString kindLabel(Kind kind) const;

    bool m_inited = false;
    bool m_hasScore = false;
    std::vector<BadTimingEntry> m_entries;

    //! Receiver for the CURRENT notation's undo-stack + selection channels; destroyed+recreated on every notation switch so
    //! a previous notation's notifications never drive this model (the NotationSwitchListModel idiom the magnifier uses).
    std::unique_ptr<muse::async::Asyncable> m_notationReceiver;
};
}
