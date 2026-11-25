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

    //! The bad-timing NAVIGATOR (BadTimingNavigatorModel) registers itself on load() and unregisters in its dtor. Held as a
    //! bare QObject* so the dev reprise.proofing-debug-navigator handler can drive its setCollapsed/setIgnored Q_INVOKABLEs
    //! by meta-call (the same decoupled idiom set-zoom uses for userZoom) with no concrete-type coupling to the QML model.
    virtual void registerNavigator(QObject* model) = 0;
    virtual void unregisterNavigator(QObject* model) = 0;
    virtual QObject* navigatorModel() const = 0;

    //! The MAIN notation EDITOR paint view (a notationscene AbstractNotationPaintView, itself a QQuickItem) publishes
    //! ITSELF here so reprise view-models that live in OTHER docks -- the bad-timing navigator in the right column -- can
    //! reach the editor's geometry without a QML reference to it (the navigator is in a different QML scope and cannot see
    //! notationView.paintView). Held as a bare QObject* so this interface carries NO notationscene dependency: the navigator
    //! maps a measure's logical (canvas) rect to global screen coords via the view's fromLogicalRect() (a meta-call, no
    //! concrete type -- the same decoupled idiom the set-zoom handler uses to drive userZoom) plus QQuickItem::mapToGlobal(),
    //! then QCursor::setPos to warp the OS cursor onto the measure in the editor. Registered by the editor-side reprise model
    //! (ProblemMeasureHighlightModel, which already lives next to the paint view in NotationPage.qml) and QPointer-guarded.
    virtual void registerEditorView(QObject* view) = 0;
    virtual void unregisterEditorView(QObject* view) = 0;
    virtual QObject* editorView() const = 0;

    //! The editor-side CUE SINK -- the ProblemMeasureHighlightModel, which lives next to the editor overlays in
    //! NotationPage.qml. The bad-timing navigator (a different dock) uses this to request a FOCUS CUE on the editor
    //! measure at the moment its cursor-warp fires: a PULSE of the existing red problem-overlay (navigator double-click,
    //! the measure is still a problem) or a transient accent-blue FLASH box (post-Fix, the measure is no longer a problem
    //! so the red overlay is gone). Reached as a bare QObject* and driven by meta-call (requestPulse / requestFlashForSelection),
    //! the same decoupled idiom editorView() uses for the warp. Registered by ProblemMeasureHighlightModel::load() and
    //! QPointer-guarded; the model emits pulseRequested()/flashRequested() which the QML overlays animate.
    virtual void registerEditorCueSink(QObject* model) = 0;
    virtual void unregisterEditorCueSink(QObject* model) = 0;
    virtual QObject* editorCueSink() const = 0;
};
}
