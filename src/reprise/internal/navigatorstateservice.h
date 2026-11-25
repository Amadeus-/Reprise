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

#include "inavigatorstateservice.h"

namespace mu::reprise {
//! NOTE: Trivial in-memory holder for the current document's bad-timing navigator per-record state. All access is on the
//! Qt UI thread (the navigator model writes it as the user toggles; NotationProject reads it on .repr save and writes it
//! on .repr open), so no locking is needed. Mirrors ProofingMapService's single-owner app-lifetime + document-claim pattern.
class NavigatorStateService : public INavigatorStateService
{
public:
    NavigatorStateService() = default;

    void setStates(const NavigatorStateMap& states) override
    {
        m_states = states;

        //! Arm the one-shot ownership claim: the next onDocumentLoaded() binds this state to the document now opening.
        m_claimPending = true;
        m_ownerPath.clear();

        m_statesChanged.notify();
    }

    void syncFromModel(const NavigatorStateMap& states) override
    {
        //! Silent store from the live model's own edit -- no notify (the model is the source of truth; re-notifying would
        //! make it re-hydrate from what it just pushed). Ownership is unchanged: the current document keeps owning this.
        m_states = states;
    }

    const NavigatorStateMap& states() const override
    {
        return m_states;
    }

    void clear() override
    {
        m_states.clear();
        m_claimPending = false;
        m_ownerPath.clear();
        m_statesChanged.notify();
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

        //! No fresh state for this load: if state is still held from a DIFFERENT document, it is stale -> drop it so the
        //! navigator resets every record to the default look rather than applying the previous document's collapse/ignore.
        if (!m_states.empty() && m_ownerPath != documentPath)
        {
            clear();
        }

        return;
    }

    muse::async::Notification statesChanged() const override
    {
        return m_statesChanged;
    }

private:
    NavigatorStateMap m_states;
    muse::async::Notification m_statesChanged;

    //! Document-ownership for the held state (see onDocumentLoaded): m_claimPending is armed by setStates() and consumed by
    //! the first following load, which records that document's path in m_ownerPath. A later load of any other document drops it.
    bool m_claimPending = false;
    std::string m_ownerPath;
};
}
