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
#include <string>

#include "modularity/imoduleinterface.h"
#include "async/notification.h"

namespace mu::reprise {
//! Per-document proofing VIEW STATE (Reprise 0.18.2): the followed measure plus the follow band + floating magnifier zooms,
//! persisted together in the .repr so reopening a scan lands where the user left it instead of always on measure 1 at the
//! default zoom. Defaults are the fresh-import look: followedMeasure -1 (== "no saved target" -> select the first note on open),
//! the BAND at 0.5 (half fit-to-width, zoomed out), the MAGNIFIER at 1.0 (its auto note zoom). Each zoom is re-clamped to its
//! own range on apply (band [0.25, 4.0], magnifier [0.5, 4.0]) by the model's setUserZoom.
struct ProofingViewState
{
    int32_t followedMeasure = -1;   //! global 0-based measure index to restore the selection to (-1 = none -> first-note default)
    double bandZoom = 0.5;          //! 0.18.2 band default: half fit-to-width
    double magnifierZoom = 1.0;     //! magnifier default: its auto note zoom
};

//! Equality (0.18.2): lets the write-on-close flush compare the live view state to the on-disk one and SKIP the re-pack when
//! nothing changed. Exact double compare is intended -- the zooms round-trip through JSON losslessly for the values in play, and
//! a rare false-negative only costs one needless (harmless) re-pack.
inline bool operator==(const ProofingViewState& a, const ProofingViewState& b)
{
    return a.followedMeasure == b.followedMeasure && a.bandZoom == b.bandZoom && a.magnifierZoom == b.magnifierZoom;
}

inline bool operator!=(const ProofingViewState& a, const ProofingViewState& b)
{
    return !(a == b);
}

//! NOTE: Single app-lifetime holder of the current document's proofing view state, exported globally -- the bridge between the
//! QML-instantiated ProofingStripModel (band, which owns the followed measure + band zoom) and ProofingMagnifierModel
//! (magnifier zoom), and the .repr persistence in NotationProject (which has no QML reference to those transient models).
//! Mirrors INavigatorStateService: each model seeds its slice from here on load + re-applies on viewStateChanged (re-hydrate),
//! and pushes its live value here on a user change (syncFollowedMeasure/syncBandZoom/syncMagnifierZoom, silent). NotationProject
//! serializes viewState() on .repr save and injects setViewState on .repr open; a one-shot document-ownership claim stops a
//! previous document's view state from bleeding onto the next. GUI-thread only.
//!
//! DIRTY POLICY (0.18.2): the models push here WITHOUT marking the project unsaved -- a zoom / navigate is a view preference,
//! not a document edit, so it must not raise a spurious "save changes?" prompt. The view state is captured whenever the
//! document is saved (doSaveRepr reads viewState()); the RESTORE path never dirties either.
class IProofingViewStateService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IProofingViewStateService)

public:
    virtual ~IProofingViewStateService() = default;

    //! LOAD path (.repr open): replace the held view state with the bundle's (the DEFAULTS when the bundle carried none), arm
    //! the one-shot document-ownership claim, and FIRE viewStateChanged() so the live models re-hydrate (SILENTLY). See
    //! onDocumentLoaded for the ownership lifecycle.
    virtual void setViewState(const ProofingViewState& state) = 0;

    //! RUNTIME path (user navigate / wheel-zoom): store the model's current value WITHOUT firing viewStateChanged -- the model
    //! is already the source of truth, so re-notifying would make it re-hydrate from what it just pushed. Ownership unchanged.
    //! A negative measure (no selection) is ignored so a transient null selection on open cannot clobber the restored target.
    virtual void syncFollowedMeasure(int32_t measure) = 0;
    virtual void syncBandZoom(double zoom) = 0;
    virtual void syncMagnifierZoom(double zoom) = 0;

    //! The held view state, for the .repr save path (doSaveRepr serializes this -- the latest the models pushed).
    virtual const ProofingViewState& viewState() const = 0;

    //! Reset to the defaults (fires viewStateChanged -> the live models re-hydrate to first-note / band 0.5 / magnifier 1.0).
    virtual void clear() = 0;

    //! Same document-ownership one-shot claim idiom as IProofingMapService/INavigatorStateService: setViewState() arms a claim;
    //! the FIRST onDocumentLoaded() after it binds the state to that document; a later load of any OTHER document finds no
    //! pending claim and a foreign owner, so it resets to defaults. Opening a plain score / non-OMR import (neither calls
    //! setViewState) therefore reverts to the defaults instead of carrying the previous .repr's view state.
    virtual void onDocumentLoaded(const std::string& documentPath) = 0;

    //! Fires when the held view state is replaced (setViewState) or reset (clear). The live models subscribe to re-hydrate.
    //! The sync* methods deliberately do NOT fire it (avoids a self-feedback loop on the model's own push).
    virtual muse::async::Notification viewStateChanged() const = 0;
};
}
