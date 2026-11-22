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

#include "modularity/imoduleinterface.h"

class QObject;

namespace mu::reprise {
//! NOTE: App-lifetime bridge from the MCP/API layer to the LIVE Two-Pane Proofing view-models. The follow band
//! (ProofingStripModel) and the floating magnifier (ProofingMagnifierModel) are QML-instantiated and transient, so a
//! C++ command handler cannot reach them directly. Each model registers ITSELF here (as a bare QObject*) when it loads
//! and unregisters in its destructor; this control holds those two pointers (as QPointers, so they auto-null if a model
//! is destroyed without unregistering). The dev reprise.proofing-debug-set-zoom handler looks up the requested panel's
//! model and drives its `userZoom` Q_PROPERTY via the Qt property system -- no concrete-type coupling, mirroring how
//! IImportProgressTracker holds the transient progress popup's QQuickWindow as a QObject*. GUI-thread only (both the
//! model registration and the handler reads run on the Qt UI thread).
class IProofingViewControl : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IProofingViewControl)

public:
    virtual ~IProofingViewControl() = default;

    //! The follow band (ProofingStripModel) registers itself on load() and unregisters in its dtor. Passed as a bare
    //! QObject* so this interface carries no dependency on the concrete QML view-model type.
    virtual void registerBand(QObject* model) = 0;
    virtual void unregisterBand(QObject* model) = 0;
    virtual QObject* bandModel() const = 0;

    //! The floating magnifier (ProofingMagnifierModel) registers itself on load() and unregisters in its dtor.
    virtual void registerMagnifier(QObject* model) = 0;
    virtual void unregisterMagnifier(QObject* model) = 0;
    virtual QObject* magnifierModel() const = 0;
};
}
