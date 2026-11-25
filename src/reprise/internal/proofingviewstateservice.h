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

#include "iproofingviewstateservice.h"

namespace mu::reprise {
//! NOTE: Trivial in-memory holder for the current document's proofing view state (followed measure + band + magnifier zoom).
//! All access is on the Qt UI thread (the models write it as the user navigates / wheel-zooms; NotationProject reads it on
//! .repr save and writes it on .repr open), so no locking is needed. Mirrors NavigatorStateService's single-owner app-lifetime
//! + document-claim pattern.
class ProofingViewStateService : public IProofingViewStateService
{
public:
    ProofingViewStateService() = default;

    void setViewState(const ProofingViewState& state) override
    {
        m_state = state;

        //! Arm the one-shot ownership claim: the next onDocumentLoaded() binds this state to the document now opening.
        m_claimPending = true;
        m_ownerPath.clear();

        m_viewStateChanged.notify();
    }

    void syncFollowedMeasure(int32_t measure) override
    {
        //! Ignore a negative (no selection): a transient null selection while a document loads must not clobber the restored
        //! followed measure before maybeSelectFirstOnOpen gets to apply it.
        if (measure < 0)
        {
            return;
        }

        m_state.followedMeasure = measure;
    }

    void syncBandZoom(double zoom) override
    {
        m_state.bandZoom = zoom;
    }

    void syncMagnifierZoom(double zoom) override
    {
        m_state.magnifierZoom = zoom;
    }

    const ProofingViewState& viewState() const override
    {
        return m_state;
    }

    void clear() override
    {
        m_state = ProofingViewState();
        m_claimPending = false;
        m_ownerPath.clear();
        m_viewStateChanged.notify();
    }

    void onDocumentLoaded(const std::string& documentPath) override
    {
        //! A state set just before this load (.repr open) claims ownership of the document now loading.
        if (m_claimPending)
        {
            m_ownerPath = documentPath;
            m_claimPending = false;

            return;
        }

        //! No fresh state for this load: if state is still held from a DIFFERENT document, it is stale -> reset to defaults so
        //! the band/magnifier revert to first-note / band 0.5 / magnifier 1.0 rather than carrying the previous document's view.
        if (m_ownerPath != documentPath)
        {
            clear();
        }

        return;
    }

    muse::async::Notification viewStateChanged() const override
    {
        return m_viewStateChanged;
    }

private:
    ProofingViewState m_state;
    muse::async::Notification m_viewStateChanged;

    //! Document-ownership for the held state (see onDocumentLoaded): m_claimPending is armed by setViewState() and consumed by
    //! the first following load, which records that document's path in m_ownerPath. A later load of any other document resets it.
    bool m_claimPending = false;
    std::string m_ownerPath;
};
}
