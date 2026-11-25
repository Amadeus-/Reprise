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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

import Muse.Ui
import Muse.UiComponents

import MuseScore.Reprise

//! NOTE: Reprise Two-Pane Proofing FULL DETAIL VIEW -- the floating "Full detail view" MAGNIFIER (Phase B2). A view-only,
//! note-level zoom of the ORIGINAL scan: a whole page shrunk to screen size is too small to read individual notes, so
//! this shows just the spot being proofed, magnified, with the selected note highlighted (the PhotoScore detail view).
//! It AUTO-FOLLOWS the score selection: as the user arrows through notes, the model derives the note's Phase B1 key,
//! looks up its pixel head box in the proofing map, and the viewport centres + magnifies that spot with a SOLID accent
//! box over the note. A grace/untrusted note (no head box) -- or a plain measure selection -- falls back to the enclosing
//! MEASURE rect, drawn as a DASHED measure-level marker so it reads as a fallback, not a false note-point. One-way:
//! nothing here moves the score cursor. The host puts this content in a floatable + dockable DockPanel.
Item {
    id: root

    //! Supplied by the host DockPanel (the dock content pattern). The magnifier is view-only with no focusable controls,
    //! so it simply holds the section; it is declared so the panel can bind it without a missing-property error.
    property NavigationSection navigationSection: null

    //! True once the model rendered at least one original page from the held proofing map (right after an OMR import).
    readonly property bool hasContent: magModel.hasMap

    //! THEME-AWARE read-only "original scan" surface tint -- the SAME binding ProofingStrip.qml uses, kept in sync by
    //! hand (both panels are view-only scan surfaces and should match). Light theme -> soft warm ivory; dark theme ->
    //! warm muted charcoal (a bright cream would glare in the dark). Dark vs light comes from the theme's own primary
    //! background luminance, so the tint tracks whatever theme the user picks.
    readonly property color readOnlyCream: {
        let bg = ui.theme.backgroundPrimaryColor
        let lum = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b
        return lum < 0.5 ? Qt.rgba(0.16, 0.145, 0.12, 1.0) : Qt.rgba(0.969, 0.945, 0.882, 1.0)
    }

    ProofingMagnifierModel {
        id: magModel
    }

    Component.onCompleted: {
        magModel.load()
    }

    //! Whole-panel cream: the header ("Full detail view · Measure N · <note>") and all panel padding sit on the SAME
    //! readOnlyCream, so the entire panel reads as a cream read-only reference with the scan detail matted in the middle.
    Rectangle {
        anchors.fill: parent
        color: root.readOnlyCream
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            StyledIconLabel {
                iconCode: IconCode.ZOOM_IN
                Layout.alignment: Qt.AlignVCenter
            }

            StyledTextLabel {
                //! "Measure N · C4" for a matched note; "Measure N (measure view)" on a measure-level fallback.
                text: {
                    if (!magModel.hasCurrent) {
                        return qsTrc("reprise", "Full detail view")
                    }

                    let base = qsTrc("reprise", "Measure %1").arg(magModel.currentMeasure + 1)
                    if (magModel.isFallback) {
                        return base + qsTrc("reprise", " (measure view)")
                    }

                    if (magModel.currentNoteName.length > 0) {
                        base += " · " + magModel.currentNoteName
                    }

                    return base
                }
                font: ui.theme.bodyBoldFont
                color: ui.theme.fontPrimaryColor
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
            }
        }

        Item {
            id: view

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            //! The page entry currently followed (url + rendered px + sheet-image px). Null until a page has rendered.
            readonly property var pageEntry: (magModel.currentPageIndex >= 0 && magModel.currentPageIndex < magModel.pages.length)
                                             ? magModel.pages[magModel.currentPageIndex] : null

            readonly property real sheetW: pageEntry ? pageEntry.sheetWidth : 0
            readonly property real sheetH: pageEntry ? pageEntry.sheetHeight : 0

            //! Centre point of the box to magnify, in sheet-image pixels.
            readonly property real boxCx: magModel.boxX + magModel.boxW / 2
            readonly property real boxCy: magModel.boxY + magModel.boxH / 2

            //! Target on-screen height (px) for a note head. The magnifier's defining job is a READABLE close-up, so a
            //! matched note aims for a fixed, generous head size rather than fitting a wide context -- a narrow docked
            //! panel would otherwise shrink a multi-note context until the "magnifier" was no larger than the A3 band.
            readonly property real targetHeadPx: 52

            //! Note zoom (screen px per sheet px): aim for targetHeadPx-tall heads, but never so tight that fewer than
            //! ~3.5 heads fit across or ~4 tall (so at least a little neighbouring context stays visible for comparison).
            //! A wider/taller floated window lifts those caps, so floating the panel shows more context at the same size.
            readonly property real noteBaseZoom: magModel.boxH > 0 ? targetHeadPx / magModel.boxH : 2
            readonly property real noteCapW: magModel.boxW > 0 ? detailArea.width / (magModel.boxW * 3.5) : noteBaseZoom
            readonly property real noteCapH: magModel.boxH > 0 ? detailArea.height / (magModel.boxH * 4) : noteBaseZoom
            readonly property real noteZoom: Math.max(0.4, Math.min(noteBaseZoom, noteCapW, noteCapH, 6))

            //! Fallback (measure) zoom: frame the WHOLE measure rect with a small pad, so the dashed marker reads as a
            //! measure-level fallback rather than a tight note close-up.
            readonly property real fitZoom: (magModel.boxW > 0 && magModel.boxH > 0 && detailArea.width > 0 && detailArea.height > 0)
                                            ? Math.max(0.1, Math.min(detailArea.width / (magModel.boxW * 1.1), detailArea.height / (magModel.boxH * 1.1), 6))
                                            : 1

            //! Effective magnification = the auto-chosen note/measure zoom times the user's mouse-wheel zoom (model.userZoom,
            //! clamped [0.5, 4.0]). The crop (clipW/clipH) and the overlays all derive from `zoom`, so folding userZoom in
            //! here scales the render AND the highlight box together and keeps the followed spot centered -- all automatic.
            readonly property real zoom: (magModel.isFallback ? fitZoom : noteZoom) * magModel.userZoom

            //! Rendered PNG pixel dims of the followed page, and the rendered px per sheet px (the PNG is capped at
            //! MAGNIFIER_PAGE_MAX_SIDE, so it can be slightly smaller than the sheet-image space the boxes live in).
            readonly property real renderedW: pageEntry ? pageEntry.width : 0
            readonly property real renderedH: pageEntry ? pageEntry.height : 0
            readonly property real sheetToRendered: sheetW > 0 ? renderedW / sheetW : 1

            //! Sub-rectangle of the rendered PNG to display, in rendered px, centred on the followed box. Its size is the
            //! context the viewport frames at the chosen zoom (view.width/zoom sheet px across), so decoding JUST this
            //! crop into a viewport-sized Image gives the magnification without a giant or off-screen image item (Qt
            //! declined to paint both of those). The crop aspect == the viewport aspect, so Stretch fills it undistorted,
            //! and scaling crop->viewport works out to exactly `zoom` screen px per sheet px -- matching the overlays.
            readonly property real clipW: zoom > 0 ? (detailArea.width / zoom) * sheetToRendered : renderedW
            readonly property real clipH: zoom > 0 ? (detailArea.height / zoom) * sheetToRendered : renderedH

            //! MANUAL pan nudge (on-screen px) ADDED ON TOP of the auto-centered crop: a left-drag sets these so the user can
            //! fine-tune which part of the magnified spot is centered AFTER follow auto-centers on the note. Reset to 0 on
            //! every new follow (onFollowChanged) so follow lands clean. Kept across a zoom (the clip shift divides by zoom, so
            //! the same screen-px nudge tracks the spot as magnification changes).
            property real userPanX: 0
            property real userPanY: 0

            //! The crop is centred on the box, then shifted by the pan: moving the content RIGHT by panX screen px means
            //! revealing content further LEFT, i.e. the clip window slides left by panX worth of rendered px (panX *
            //! sheetToRendered / zoom). Subtracting it here makes the overlays' (+userPan) move and the crop shift agree.
            readonly property real clipX: boxCx * sheetToRendered - clipW / 2 - (zoom > 0 ? userPanX * sheetToRendered / zoom : 0)
            readonly property real clipY: boxCy * sheetToRendered - clipH / 2 - (zoom > 0 ? userPanY * sheetToRendered / zoom : 0)

            //! Keep the followed box's centre inside the viewport: clamp each pan axis to half the detail area minus a small
            //! margin, so a drag can never carry the magnified note entirely out of view.
            function clampPanX(pan) {
                let limit = Math.max(0, detailArea.width / 2 - 20)

                return Math.max(-limit, Math.min(limit, pan))
            }

            function clampPanY(pan) {
                let limit = Math.max(0, detailArea.height / 2 - 20)

                return Math.max(-limit, Math.min(limit, pan))
            }

            Rectangle {
                anchors.fill: parent
                color: root.readOnlyCream
                border.color: ui.theme.strokeColor
                border.width: 1
            }

            //! Inset "matte" container: holds the magnified crop AND its highlight/fallback overlays, inset ~14px so the
            //! readOnlyCream background shows as a frame around the detail. Because the crop and its overlays share THIS
            //! container, the inset shifts them together and the highlight stays aligned with the note. The zoom/clip math
            //! above reads detailArea.width/height, so the crop fills exactly this inset box (scan colors untouched -- no
            //! tint over the scan; the cream only frames it).
            Item {
                id: detailArea

                anchors.fill: parent
                anchors.margins: 14
                clip: true

                //! The magnified scan crop. NOT drawn directly (visible: false) -- it is only the MULTIPLY blend's foreground
                //! source below, so the scan PAPER reads cream while the notes stay dark (same mechanism as the following band).
                Image {
                    id: pageImage

                    anchors.fill: parent
                    visible: false

                    //! The crop shares the viewport aspect, so Stretch magnifies it to fill with no distortion.
                    fillMode: Image.Stretch
                    //! Nearest-neighbour: at high user-zoom the crop upscales past the rendered PNG's own resolution, where
                    //! smooth (bilinear) filtering would blur the scan -- crisp pixels read better for proofing than a smear.
                    smooth: false
                    source: view.pageEntry ? view.pageEntry.url : ""
                    //! Synchronous decode: an async load left the crop blank for >1s after each selection (the clip re-extract
                    //! was still Loading). cache keeps the decoded page, so only the first note on a page costs a decode; the
                    //! rest re-crop instantly. A proofing close-up must appear the instant the note is selected.
                    asynchronous: false
                    cache: true
                    //! Decode only the crop around the followed note/measure (rendered px); re-crops on selection with no
                    //! page re-render.
                    sourceClipRect: Qt.rect(view.clipX, view.clipY, view.clipW, view.clipH)

                    //! Reprise: same page-switch Blend re-sample as the band -- when the followed note is on a different
                    //! page, force blendedCrop to re-grab the new page's texture (the Blend does not re-sample on a
                    //! source-url change alone). Fires only on a PAGE change, not a within-page re-crop (sourceClipRect).
                    onSourceChanged: {
                        blendedCrop.foregroundSource = null
                        blendedCrop.foregroundSource = pageImage
                    }
                }

                //! Solid cream fill matching the crop: the MULTIPLY blend's BASE source (view-only, not drawn direct). A
                //! uniform cream, so white paper x cream = cream and black notes x cream = black regardless of crop position.
                Rectangle {
                    id: magCream

                    anchors.fill: parent
                    color: root.readOnlyCream
                    visible: false
                }

                //! TRUE multiply blend of the magnified crop over the cream (Qt5Compat.GraphicalEffects -- same mechanism as
                //! the following band, no new shader/build infra). The note-head highlight + dashed fallback marker below still
                //! render on top. Shown on the same condition the raw crop used to be (a decodable crop for the followed page).
                Blend {
                    id: blendedCrop

                    anchors.fill: parent
                    visible: view.pageEntry !== null && view.clipW > 0 && view.clipH > 0

                    source: magCream
                    foregroundSource: pageImage
                    mode: "multiply"
                }

                //! Matched-note highlight: a SOLID accent box over the exact recognised head (PhotoScore purple-ish via the
                //! theme accent). The note is always centred, so this is a fixed centred overlay, drawn OUTSIDE the scaled
                //! image so its border stays crisp and 2px at any zoom. Centred in detailArea (parent), so it tracks the
                //! inset crop exactly.
                Rectangle {
                    id: noteHighlight

                    visible: magModel.hasCurrent && !magModel.isFallback && view.pageEntry !== null

                    width: Math.max(3, magModel.boxW * view.zoom)
                    height: Math.max(3, magModel.boxH * view.zoom)
                    x: (parent.width - width) / 2 + view.userPanX
                    y: (parent.height - height) / 2 + view.userPanY

                    //! RED when the followed measure is a bad-timing PROBLEM measure (same walk as the navigator), else accent.
                    color: magModel.currentMeasureIsProblem
                           ? Qt.rgba(0.82, 0.23, 0.18, 0.22)
                           : Qt.rgba(ui.theme.accentColor.r, ui.theme.accentColor.g, ui.theme.accentColor.b, 0.22)
                    border.color: magModel.currentMeasureIsProblem ? "#d23b2e" : ui.theme.accentColor
                    border.width: 2
                    radius: 2
                }

                //! Measure-level FALLBACK marker: a DASHED accent outline over the whole measure rect (grace/untrusted/
                //! unmatched note, or a measure selection). Visibly a fallback -- never a solid note-point pretending
                //! precision. QtQuick.Shapes is unused in this codebase, so draw the dashes with a plain Canvas. Centred
                //! overlay like the note highlight (the measure box is likewise centred in the viewport).
                Canvas {
                    id: fallbackMarker

                    visible: magModel.hasCurrent && magModel.isFallback && view.pageEntry !== null

                    width: Math.max(3, magModel.boxW * view.zoom)
                    height: Math.max(3, magModel.boxH * view.zoom)
                    x: (parent.width - width) / 2 + view.userPanX
                    y: (parent.height - height) / 2 + view.userPanY

                    onPaint: {
                        let ctx = getContext("2d")
                        ctx.reset()
                        ctx.clearRect(0, 0, width, height)
                        //! RED on a bad-timing PROBLEM measure (same walk as the navigator), else the normal accent -- the
                        //! dashed measure-level fallback marker picks up the problem colour just like the solid note box.
                        ctx.strokeStyle = magModel.currentMeasureIsProblem ? "#d23b2e" : ui.theme.accentColor
                        ctx.lineWidth = 2
                        ctx.setLineDash([10, 6])
                        ctx.strokeRect(1, 1, Math.max(0, width - 2), Math.max(0, height - 2))
                    }

                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onVisibleChanged: {
                        if (visible) {
                            requestPaint()
                        }
                    }
                }
            }

            StyledTextLabel {
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: !magModel.hasMap ? qsTrc("reprise", "Import a scan to use the detail view")
                                       : qsTrc("reprise", "Select a note to magnify the original scan")
                color: ui.theme.fontSecondaryColor
                visible: !view.pageEntry || !magModel.hasCurrent
            }

            //! Repaint the dashed fallback marker when the followed box moves (position changes do not retrigger
            //! width/height handlers, so the dash geometry would otherwise go stale).
            Connections {
                target: magModel
                function onFollowChanged() {
                    //! A new follow re-centers the magnifier on the note/measure -> drop any manual pan nudge so the followed
                    //! spot lands clean (the user then drags to fine-tune from the centered position).
                    view.userPanX = 0
                    view.userPanY = 0
                    if (fallbackMarker.visible) {
                        fallbackMarker.requestPaint()
                    }
                }
            }

            //! Mouse-wheel zoom: each notch (~120) steps the magnifier zoom by ~8% (1 + d/1500), clamped in the model to
            //! [0.5, 4.0]. SHIFT held = FINE zoom at ~1.3%/notch (divisor /9000) for precise adjustment. Windows gotcha:
            //! holding Shift while scrolling makes Qt deliver the wheel on angleDelta.X instead of .Y, so take whichever axis
            //! is non-zero as the effective delta. The crop recenters on the followed box automatically, so no scroll
            //! bookkeeping is needed. Consumes the event so nothing behind the panel scrolls.
            WheelHandler {
                onWheel: function(wheel) {
                    let d = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x
                    let fine = (wheel.modifiers & Qt.ShiftModifier) !== 0
                    let factor = 1 + d / (fine ? 12000 : 2400)
                    magModel.userZoom = Math.max(0.5, Math.min(4.0, magModel.userZoom * factor))
                    wheel.accepted = true
                }
            }

            //! Left-DRAG pan: an ADDITIVE fine-tune on top of the auto-centered crop. target: null means the handler never
            //! moves an item -- it only reports the drag, which we fold into view.userPanX/userPanY (clamped). It coexists with
            //! the WheelHandler (wheel vs drag) and never selects score items (this panel is view-only). centroid (position -
            //! pressPosition) is the drag delta since press -- the same idiom LayoutPanelTree uses.
            DragHandler {
                id: panHandler

                target: null
                acceptedButtons: Qt.LeftButton

                //! Pan value when THIS drag began, so the delta accumulates across successive drags instead of snapping to 0.
                property real startPanX: 0
                property real startPanY: 0

                onActiveChanged: {
                    if (active) {
                        startPanX = view.userPanX
                        startPanY = view.userPanY
                    }
                }

                onCentroidChanged: {
                    if (!active) {
                        return
                    }

                    view.userPanX = view.clampPanX(startPanX + centroid.position.x - centroid.pressPosition.x)
                    view.userPanY = view.clampPanY(startPanY + centroid.position.y - centroid.pressPosition.y)
                }
            }

            ProofingZoomReadout {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 6
                z: 10
                zoomPercent: Math.round(magModel.userZoom * 100)
            }
        }

        //! Reprise TIMING-PROBLEMS SECTION (0.17.1, Option C). The bad-timing worklist is hosted HERE, inside the
        //! ALWAYS-OPEN "Full detail view" panel, as a collapsing section beneath the magnifier -- NOT as a separate
        //! hideable KDDock dock. A dock that is fully closed reshows EMPTY (KDDock destroys the sole-widget group's QML
        //! guest, and muse's pristine DockPanelView never recreates it), so across a score switch the worklist came back
        //! blank. This panel is never closed -> its BadTimingNavigator (and the BadTimingNavigatorModel that navigator
        //! owns) is never destroyed -> the model stays subscribed to currentNotationChanged + the undo stack's
        //! stackChanged and keeps re-walking, so the list is always populated with the LIVE score. The model's lifetime
        //! therefore equals this panel's -- airtight across every cross-switch.
        //!
        //! COLLAPSE when clean: problemCount == 0 -> visible:false, which excludes the section from the ColumnLayout so it
        //! takes zero height and the magnifier fills the whole panel. count > 0 -> the section appears with a bottom slice
        //! (its own list scroll) that grows with the row count, capped at ~45% of the panel so the magnifier keeps the top.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: ui.theme.strokeColor
            visible: timingSection.problemCount > 0
        }

        BadTimingNavigator {
            id: timingSection

            Layout.fillWidth: true
            Layout.fillHeight: false
            //! 0.18.0: STATE-AWARE section height. Records default collapsed (line 1 only, ~28px) and expand to two lines on
            //! demand, so a flat per-row estimate either over-reserves (empty space under an all-collapsed list) or clips
            //! expanded rows. timingSection.contentHeight sums each row's ACTUAL height (line 1 always + line 2 only when
            //! expanded), so the default collapsed list hugs tight while an expanded row still gets its full height. Still
            //! capped at ~45% of the panel (the list scrolls beyond that); contentHeight covers active + dimmed-ignored rows.
            Layout.preferredHeight: visible
                                    ? Math.round(Math.min(root.height * 0.45, Math.max(120, 44 + timingSection.contentHeight)))
                                    : 0

            //! Collapse (zero height, excluded from the layout) when the live score is clean, so the magnifier fills the panel.
            visible: problemCount > 0

            navigationSection: root.navigationSection
        }
    }
}
