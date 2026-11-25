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

#include <QObject>
#include <QPointer>

#include "iproofingviewcontrol.h"

namespace mu::reprise {
//! NOTE: Trivial in-memory holder of the two live Two-Pane Proofing view-models (follow band + magnifier). All access
//! is on the Qt UI thread (both the model self-registration and the command-handler reads run there), so no locking is
//! needed. The pointers are QPointers so a model destroyed without unregistering auto-nulls -- unregister only clears
//! the slot when it still points at the departing model, so a newer model that registered after is never dropped.
class ProofingViewControl : public IProofingViewControl
{
public:
    ProofingViewControl() = default;

    void registerBand(QObject* model) override
    {
        m_band = model;
    }

    void unregisterBand(QObject* model) override
    {
        if (m_band.data() == model)
        {
            m_band = nullptr;
        }
    }

    QObject* bandModel() const override
    {
        return m_band.data();
    }

    void registerMagnifier(QObject* model) override
    {
        m_magnifier = model;
    }

    void unregisterMagnifier(QObject* model) override
    {
        if (m_magnifier.data() == model)
        {
            m_magnifier = nullptr;
        }
    }

    QObject* magnifierModel() const override
    {
        return m_magnifier.data();
    }

    void registerEditorView(QObject* view) override
    {
        m_editorView = view;
    }

    void unregisterEditorView(QObject* view) override
    {
        if (m_editorView.data() == view)
        {
            m_editorView = nullptr;
        }
    }

    QObject* editorView() const override
    {
        return m_editorView.data();
    }

private:
    //! The live follow band + magnifier view-models (held as QPointers so they auto-null if a model is destroyed by some
    //! other path first). Driven by the dev reprise.proofing-debug-set-zoom handler through the Qt property system.
    QPointer<QObject> m_band;
    QPointer<QObject> m_magnifier;

    //! The main notation editor paint view (notationscene AbstractNotationPaintView), published by the editor-side model so
    //! the bad-timing navigator -- in a different dock -- can map a measure to global screen coords and warp the cursor onto
    //! it. QPointer so it auto-nulls if the editor view is torn down (page reload) before the model unregisters.
    QPointer<QObject> m_editorView;
};
}
