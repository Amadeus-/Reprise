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

#include <string>
#include <unordered_map>

#include "modularity/imoduleinterface.h"
#include "async/notification.h"

namespace mu::reprise {
//! Per-record UI state for ONE bad-timing navigator record (Reprise 0.18.0 two-line navigator). A record is one problem
//! measure x staff; its state is keyed by a stable per-problem string "<globalMeasureIndex>:<kind>:<staffIdx>" so that a
//! measure carrying several problem rows disambiguates. A record defaults to COLLAPSED + active (0.18.0: the panel opens as
//! an all-chevron list; a row expands only once the user clicks/expands it), and only NON-default records are persisted.
struct NavigatorRecordState
{
    bool collapsed = true;      //! default COLLAPSED (0.18.0: chevron right -> line 1 only; expanded only once the user expands it)
    bool ignored = false;       //! default ACTIVE (counts toward the header N, its Fix stays enabled)
};

//! Equality (0.18.2): lets the write-on-close flush compare the live NavigatorStateMap to the on-disk one and SKIP the re-pack
//! when nothing changed. unordered_map<>::operator== uses this per-value, order-independent.
inline bool operator==(const NavigatorRecordState& a, const NavigatorRecordState& b)
{
    return a.collapsed == b.collapsed && a.ignored == b.ignored;
}

inline bool operator!=(const NavigatorRecordState& a, const NavigatorRecordState& b)
{
    return !(a == b);
}

//! The DEFAULT record state (0.18.0: collapsed + active). The state map stores only NON-default records (absent == default),
//! so this ONE predicate defines "is default" for every prune / skip / keep site (model storeState, reprfile serialize /
//! parse / assembleRepr) -- keeping the collapsed-default polarity in a single place rather than scattered inline checks.
inline bool isDefaultNavigatorState(const NavigatorRecordState& s)
{
    return s.collapsed && !s.ignored;
}

//! The whole document's navigator state: a map of record-key -> state. Only NON-default records are stored (a record at
//! {collapsed:true, ignored:false} is simply absent), so the map and its .repr serialization stay minimal.
using NavigatorStateMap = std::unordered_map<std::string, NavigatorRecordState>;

//! NOTE: Single app-lifetime holder of the current document's bad-timing navigator per-record collapse/ignore state,
//! exported globally -- the bridge between the QML-instantiated BadTimingNavigatorModel (the live owner/editor of the
//! state) and the .repr persistence in NotationProject (which has no QML reference to the transient model). Mirrors
//! IProofingMapService: the model pushes its live state here on every toggle (syncFromModel), NotationProject serializes
//! it on .repr save (states()) and injects the loaded state on .repr open (setStates), and a one-shot document-ownership
//! claim stops a previous document's state from bleeding onto the next one. GUI-thread only.
class INavigatorStateService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(INavigatorStateService)

public:
    virtual ~INavigatorStateService() = default;

    //! LOAD path (.repr open): replace the held state with the bundle's (an empty map when the bundle carried none), arm
    //! the one-shot document-ownership claim, and FIRE statesChanged() so the live navigator model re-hydrates. See
    //! onDocumentLoaded for the ownership lifecycle.
    virtual void setStates(const NavigatorStateMap& states) = 0;

    //! RUNTIME path (user toggles collapse/ignore in the navigator): store the model's current state WITHOUT firing
    //! statesChanged -- the model is already the source of truth, so re-notifying would make it re-hydrate from what it
    //! just pushed. Does not touch document ownership (the current document keeps owning the state).
    virtual void syncFromModel(const NavigatorStateMap& states) = 0;

    //! The held state, for the .repr save path (doSaveRepr serializes this).
    virtual const NavigatorStateMap& states() const = 0;

    //! Drop all state (fires statesChanged -> the live navigator resets every record to the default look).
    virtual void clear() = 0;

    //! Same document-ownership one-shot claim idiom as IProofingMapService::onDocumentLoaded: setStates() arms a claim;
    //! the FIRST onDocumentLoaded() after it binds the state to that document; a later load of any OTHER document finds
    //! no pending claim and a foreign owner, so it drops the state. Opening a plain score or a non-OMR import (neither of
    //! which calls setStates) therefore clears the previous .repr's collapse/ignore instead of leaving it applied.
    virtual void onDocumentLoaded(const std::string& documentPath) = 0;

    //! Fires when the held state is replaced (setStates) or dropped (clear). The live BadTimingNavigatorModel subscribes
    //! to re-hydrate. syncFromModel deliberately does NOT fire it (avoids a self-feedback loop on the model's own push).
    virtual muse::async::Notification statesChanged() const = 0;
};
}
