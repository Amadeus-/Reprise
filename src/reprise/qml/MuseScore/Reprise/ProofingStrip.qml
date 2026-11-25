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
import QtQuick.Window
import Qt5Compat.GraphicalEffects

import Muse.Ui
import Muse.UiComponents

import MuseScore.Reprise

//! NOTE: Reprise Two-Pane Proofing READABLE FOLLOWING BAND (Phase A3). A full-width, view-only band docked above the
//! notation editor that renders the ORIGINAL scanned page(s) the current score was recognised from -- at a READABLE size
//! so the user can compare the original against the import (the PhotoScore proofing surface). It AUTO-FOLLOWS the score:
//! as the selection moves, the model derives the current GLOBAL MEASURE INDEX, looks up its page + pixel rect in the
//! proofing map, switches the band to that page, scrolls the matching system into view, and the band draws a highlight box
//! over the current measure. One-way: clicking the band does NOT move the score cursor. The note-level magnifier is Phase
//! B. The band is visible only while a proofing map is held (right after an OMR import); otherwise `hasContent` is false
//! and the host collapses it away. A header row gives a label, the current page/measure, and a chevron that collapses the
//! band to just the header; the host owns the resizable height via a divider beneath the band.
Rectangle {
    id: root

    //! True once the model rendered at least one original page -> the host shows the band (and its divider).
    readonly property bool hasContent: bandModel.hasMap

    //! Expanded = the readable band is visible; collapsed = header only. Read by the host to size the band.
    property bool expanded: true

    //! Fixed height of the always-present header row (the host uses it as the collapsed height).
    readonly property int headerHeight: 32

    //! THEME-AWARE read-only "original scan" surface tint. The two proofing panels paint this behind the scan so they
    //! read as distinct view-only surfaces, not pure white editor space. In a LIGHT theme it's a soft warm ivory
    //! (clearly off-white, low saturation, not yellow); in a DARK theme a bright cream would glare, so we drop to a
    //! warm muted charcoal instead. Dark vs light is judged from the theme's own primary background luminance, so the
    //! tint tracks any theme the user picks. ProofingMagnifier.qml carries the SAME binding for a matched look.
    readonly property color readOnlyCream: {
        let bg = ui.theme.backgroundPrimaryColor
        let lum = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b
        return lum < 0.5 ? Qt.rgba(0.16, 0.145, 0.12, 1.0) : Qt.rgba(0.969, 0.945, 0.882, 1.0)
    }

    color: ui.theme.backgroundPrimaryColor
    clip: true

    ProofingStripModel {
        id: bandModel
    }

    Component.onCompleted: {
        bandModel.load()
    }

    Rectangle {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.headerHeight

        color: ui.theme.backgroundSecondaryColor

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 6
            spacing: 8

            StyledIconLabel {
                iconCode: IconCode.IMAGE_MOUNTAINS
                Layout.alignment: Qt.AlignVCenter
            }

            StyledTextLabel {
                text: qsTrc("reprise", "Original scan")
                font: ui.theme.bodyBoldFont
                Layout.alignment: Qt.AlignVCenter
            }

            StyledTextLabel {
                //! "Page X of N" while a page is shown, plus the followed measure once a measure is selected.
                text: {
                    let n = bandModel.pages.length
                    if (n <= 0) {
                        return ""
                    }

                    let pageNo = (bandModel.currentPageIndex >= 0 ? bandModel.currentPageIndex : 0) + 1
                    let base = qsTrc("reprise", "Page %1 of %2").arg(pageNo).arg(n)
                    if (bandModel.hasCurrent) {
                        base += qsTrc("reprise", " · measure %1").arg(bandModel.currentMeasure + 1)
                    }

                    return base
                }
                //! Primary (not secondary) colour: fontSecondaryColor was too light to read on the cream band header in the
                //! light theme. Primary is theme-aware (dark-on-cream in light, light in dark) and the hierarchy is kept by
                //! weight -- the "Original scan" title is bodyBoldFont, this subtitle is regular.
                color: ui.theme.fontPrimaryColor
                visible: bandModel.pages.length > 0
                Layout.alignment: Qt.AlignVCenter
            }

            Item {
                Layout.fillWidth: true
            }

            FlatButton {
                icon: root.expanded ? IconCode.SMALL_ARROW_UP : IconCode.SMALL_ARROW_DOWN
                transparent: true
                toolTipTitle: root.expanded ? qsTrc("reprise", "Collapse original scan")
                                            : qsTrc("reprise", "Expand original scan")
                Layout.alignment: Qt.AlignVCenter
                onClicked: {
                    root.expanded = !root.expanded
                }
            }
        }
    }

    Item {
        id: bandArea

        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8

        visible: root.expanded
        clip: true

        //! The page entry currently followed (url + rendered px + sheet-image px). Null until a page has rendered.
        readonly property var pageEntry: (bandModel.currentPageIndex >= 0 && bandModel.currentPageIndex < bandModel.pages.length)
                                         ? bandModel.pages[bandModel.currentPageIndex] : null

        //! Sheet-image pixel space the map's measure rects live in (shares the page aspect with the rendered PNG).
        readonly property real sheetW: pageEntry ? pageEntry.sheetWidth : 0
        readonly property real sheetH: pageEntry ? pageEntry.sheetHeight : 0

        //! Scale from sheet-image pixels to on-screen band pixels. At userZoom = 1 the page is drawn at the full band
        //! width (fit-to-width); userZoom multiplies that so one scale factor still maps both axes (the rendered image is
        //! stretched into a box of the exact sheet aspect). model.userZoom is the mouse-wheel zoom, clamped [0.25, 4.0].
        readonly property real imgScale: (sheetW > 0 ? width / sheetW : 1) * bandModel.userZoom
        readonly property real pageDispH: sheetH * imgScale

        //! On-screen width of the page at the current zoom (sheet space * imgScale == band width * userZoom). At userZoom
        //! > 1 this exceeds the viewport, so the band scrolls HORIZONTALLY too; the highlight shares this space (both scale
        //! by imgScale and shift by scrollOffsetX), so the page image is sized to it to stay aligned when scrolled.
        readonly property real pageDispW: sheetW > 0 ? sheetW * imgScale : viewport.width

        //! Vertical scroll position (px, >= 0) of the page within the viewport: the page's top is drawn at
        //! y = -scrollOffset, so a larger offset reveals lower systems. Assigned by scrollToCurrent; the Behavior
        //! below animates the move SMOOTHLY while the window is visible, or snaps it instantly while minimized.
        property real scrollOffset: 0

        //! Horizontal scroll position (px, >= 0), mirroring scrollOffset: the page's left is drawn at x = -scrollOffsetX,
        //! so a larger offset reveals columns to the right. Only meaningful once userZoom > 1 (the page is wider than the
        //! viewport); scrollToCurrent centers the followed measure on X exactly as it does on Y.
        property real scrollOffsetX: 0

        //! MANUAL pan nudge (on-screen px) ADDED ON TOP of the follow-centered scrollOffset/scrollOffsetX: a left-drag sets
        //! these so the user can fine-tune the scan's placement AFTER follow auto-centers. Reset to 0 on every new follow
        //! (onFollowChanged) so follow always lands clean, then re-clamped (clampPan) on a zoom/resize so a pan valid at one
        //! size can't fling the page out of view after the page grows/shrinks.
        property real userPanX: 0
        property real userPanY: 0

        //! True while the containing window is minimized. A minimized / non-exposed window FREEZES Qt's QML
        //! animation driver, so an animated scrollOffset would stay stuck at its start value -- which is exactly
        //! what broke the earlier animated band under our minimized-window capture harness (the "band shows the
        //! title block for a mid-page measure" symptom). Only real/visible users ever see a non-minimized window.
        readonly property bool windowMinimized: Window.window ? (Window.window.visibility === Window.Minimized) : false

        //! GATED smooth scroll: when the window is visible, animate scrollOffset to the target (the page Image and
        //! the highlight overlay both bind to scrollOffset, so they glide in lockstep and the measure box stays
        //! aligned during the move). When minimized, the Behavior is disabled so the assignment lands DIRECTLY
        //! (instant) -- a minimized window (our capture harness) can't freeze mid-animation and always settles on
        //! the correct system. Only HOW we reach the offset is gated; the target math in scrollToCurrent is unchanged.
        Behavior on scrollOffset {
            enabled: !bandArea.windowMinimized

            NumberAnimation {
                duration: 220
                easing.type: Easing.OutCubic
            }
        }

        //! Same minimized-window gate as scrollOffset above: animate the horizontal move when the window is visible, snap
        //! it instantly while minimized (the capture harness) so it can never freeze mid-animation at the wrong column.
        Behavior on scrollOffsetX {
            enabled: !bandArea.windowMinimized

            NumberAnimation {
                duration: 220
                easing.type: Easing.OutCubic
            }
        }

        //! Centre the followed measure's system in the viewport, clamped to the page bounds. Assigns scrollOffset to
        //! the target; the Behavior above animates (visible window) or snaps instantly (minimized) based on window
        //! state -- what we scroll TO is window-state-independent, only HOW (animate vs direct-set) varies.
        function scrollToCurrent() {
            if (!bandModel.hasCurrent || imgScale <= 0) {
                return
            }

            let target = (bandModel.rectY + bandModel.rectH / 2) * imgScale - viewport.height / 2
            let maxY = Math.max(0, pageDispH - viewport.height)
            scrollOffset = Math.max(0, Math.min(maxY, target))

            //! Center the followed measure horizontally too (only bites once userZoom > 1 makes the page wider than the
            //! viewport; at fit-to-width maxX is 0, so this clamps to 0 and the page stays left-aligned as before).
            let targetX = (bandModel.rectX + bandModel.rectW / 2) * imgScale - viewport.width / 2
            let maxX = Math.max(0, pageDispW - viewport.width)
            scrollOffsetX = Math.max(0, Math.min(maxX, targetX))
        }

        //! Clamp a candidate horizontal pan so at least PAN_KEEP px of the page stay inside the viewport. The page's on-screen
        //! left edge is (-scrollOffsetX + pan); keeping that in [keep - pageDispW, viewport.width - keep] (both shifted by
        //! scrollOffsetX) guarantees at least `keep` px of overlap on whichever edge the user drags toward -> the scan can
        //! never be dragged entirely out of view.
        function clampPanX(pan) {
            let keep = 40
            let lo = keep - pageDispW + scrollOffsetX
            let hi = viewport.width - keep + scrollOffsetX
            if (lo > hi) {
                return 0
            }

            return Math.max(lo, Math.min(hi, pan))
        }

        //! Vertical twin of clampPanX, against pageDispH / scrollOffset / viewport.height.
        function clampPanY(pan) {
            let keep = 40
            let lo = keep - pageDispH + scrollOffset
            let hi = viewport.height - keep + scrollOffset
            if (lo > hi) {
                return 0
            }

            return Math.max(lo, Math.min(hi, pan))
        }

        //! Re-clamp the existing pan against current geometry (called after a zoom/resize changes pageDispW/H) so a pan that
        //! was valid at the old size can't leave the page off-view at the new one.
        function clampPan() {
            userPanX = clampPanX(userPanX)
            userPanY = clampPanY(userPanY)

            return
        }

        Rectangle {
            anchors.fill: parent
            color: root.readOnlyCream
            border.color: ui.theme.strokeColor
            border.width: 1
        }

        Item {
            id: viewport

            anchors.fill: parent
            anchors.margins: 1
            clip: true

            //! The raw scanned page. NOT drawn directly (visible: false) -- it is only the MULTIPLY blend's foreground source,
            //! so the white paper reads cream while the black notes stay black. CRITICAL: it is sized at the rendered PNG's
            //! NATURAL pixels (not the zoomed on-screen size), because a GraphicalEffects source item is captured into an
            //! offscreen texture at its OWN size -- sizing it to the zoomed page would allocate a huge FBO at high zoom. The
            //! Blend item below carries the zoom/scroll geometry and simply samples this bounded texture, so crispness and
            //! cost match the old direct-draw at every zoom level.
            Image {
                id: pageImage

                width: bandArea.pageEntry ? bandArea.pageEntry.width : 0
                height: bandArea.pageEntry ? bandArea.pageEntry.height : 0

                //! 1:1 decode at the rendered PNG's own pixels -> a crisp, bounded source texture; the Blend upscales it.
                fillMode: Image.Stretch
                source: bandArea.pageEntry ? bandArea.pageEntry.url : ""
                //! SYNCHRONOUS decode (matches the magnifier). The Blend below samples THIS item as its foreground texture, but
                //! a Qt5Compat GraphicalEffects Blend captures its foregroundSource at first composite and does NOT re-capture
                //! when an ASYNC image's texture arrives a frame later -- so on score OPEN the band painted only the cream BASE
                //! and stayed blank until any re-layout (a zoom nudge changing pageDispW/H) dirtied the Blend geometry and forced
                //! a re-composite. Decoding synchronously makes the foreground texture present BEFORE the first composite, so
                //! measure 1's scan paints on open with no hover/nudge. The page PNG is bounded (BAND_PAGE_MAX_SIDE) and cached,
                //! so only the first follow onto a given page costs a decode; later follows on that page reuse it.
                asynchronous: false
                cache: true
                sourceSize.width: bandArea.pageEntry ? bandArea.pageEntry.width : 0
                visible: false

                //! Reprise: on a multi-page follow (page 2+), force the multiply Blend to RE-SAMPLE this foreground. A
                //! Qt5Compat GraphicalEffects Blend snapshots its foregroundSource and does NOT re-sample on a source-content
                //! change alone (page geometry -- pageDispW/H -- is identical across pages), so a page switch would otherwise
                //! keep showing the previous page's scan. Re-assigning foregroundSource drops + re-grabs the (synchronously
                //! decoded) new-page texture. Fires only on a PAGE change (source url), not within-page scroll/zoom.
                onSourceChanged: {
                    blendedPage.foregroundSource = null
                    blendedPage.foregroundSource = pageImage
                }
            }

            //! Solid cream fill: the MULTIPLY blend's BASE source (view-only, not drawn direct). white_paper x cream = cream;
            //! black_note x cream = black -- so the scan PAPER reads cream and the notes stay dark. A uniform colour, so a
            //! small texture upscales identically; matched to the page texture size purely for clarity.
            Rectangle {
                id: pageCream

                width: pageImage.width
                height: pageImage.height
                color: root.readOnlyCream
                visible: false
            }

            //! TRUE multiply blend of the scan over the cream (Qt5Compat.GraphicalEffects, already used across the app -- no
            //! new shader/build infra). THIS item carries the zoom + scroll geometry (the old pageImage geometry): it samples
            //! the bounded source textures above and paints the cream-tinted page at the display size, so the top-left origin
            //! at (0,0)-minus-scroll still lines the highlight up. Highlight overlay below renders on top. Shown only once the
            //! async page image is Ready so an unresolved (empty) source never flashes before the scan appears.
            Blend {
                id: blendedPage

                x: -bandArea.scrollOffsetX + bandArea.userPanX
                y: -bandArea.scrollOffset + bandArea.userPanY
                width: bandArea.pageDispW
                height: bandArea.pageDispH

                source: pageCream
                foregroundSource: pageImage
                mode: "multiply"
                visible: pageImage.status === Image.Ready
            }

            Rectangle {
                id: highlight

                visible: bandModel.hasCurrent && bandArea.imgScale > 0

                x: bandModel.rectX * bandArea.imgScale - bandArea.scrollOffsetX + bandArea.userPanX
                y: bandModel.rectY * bandArea.imgScale - bandArea.scrollOffset + bandArea.userPanY
                width: bandModel.rectW * bandArea.imgScale
                height: bandModel.rectH * bandArea.imgScale

                //! RED when the followed measure is a bad-timing PROBLEM measure (currentMeasureIsProblem, driven by the same
                //! read-only walk as the navigator), normal accent blue otherwise -- so a problem bar is obvious while proofing.
                color: bandModel.currentMeasureIsProblem
                       ? Qt.rgba(0.82, 0.23, 0.18, 0.22)
                       : Qt.rgba(ui.theme.accentColor.r, ui.theme.accentColor.g, ui.theme.accentColor.b, 0.22)
                border.color: bandModel.currentMeasureIsProblem ? "#d23b2e" : ui.theme.accentColor
                border.width: 2
                radius: 2
            }
        }

        Connections {
            target: bandModel
            function onFollowChanged() {
                //! A new follow re-centers the band -> drop any manual pan nudge so the followed measure lands clean (the
                //! user then drags to fine-tune from the centered position).
                bandArea.userPanX = 0
                bandArea.userPanY = 0
                bandArea.scrollToCurrent()
            }
            //! After a wheel-zoom, re-center the followed measure so it stays put while the page grows/shrinks around it;
            //! because imgScale folds in userZoom, the zoom persists across auto-follow for free. The manual pan is KEPT
            //! across a zoom (so fine-zoom while panned stays put), only re-clamped to the new page size.
            function onUserZoomChanged() {
                bandArea.scrollToCurrent()
                bandArea.clampPan()
            }
        }

        onWidthChanged: {
            scrollToCurrent()
            clampPan()
        }
        onPageDispHChanged: {
            scrollToCurrent()
            clampPan()
        }

        //! Mouse-wheel zoom: each notch (~120) steps the band zoom by ~8% (1 + d/1500), clamped in the model to [0.25, 4.0].
        //! SHIFT held = FINE zoom at ~1.3%/notch (divisor /9000) for precise adjustment. Windows gotcha: holding Shift while
        //! scrolling makes Qt deliver the wheel on angleDelta.X instead of .Y, so take whichever axis is non-zero as the
        //! effective delta. Consumes the event so the notation editor behind never also scrolls.
        WheelHandler {
            onWheel: function(wheel) {
                let d = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x
                let fine = (wheel.modifiers & Qt.ShiftModifier) !== 0
                let factor = 1 + d / (fine ? 12000 : 2400)
                bandModel.userZoom = Math.max(0.25, Math.min(4.0, bandModel.userZoom * factor))
                wheel.accepted = true
            }
        }

        //! Left-DRAG pan: an ADDITIVE fine-tune on top of follow-centering. target: null means the handler never moves an
        //! item -- it only reports the drag, which we fold into userPanX/userPanY (clamped). It coexists with the WheelHandler
        //! (wheel vs drag are different event types) and never selects score items (these panels are view-only). centroid
        //! (position - pressPosition) is the drag delta since press -- the same idiom LayoutPanelTree uses.
        DragHandler {
            id: panHandler

            target: null
            acceptedButtons: Qt.LeftButton

            //! Pan value when THIS drag began, so the delta accumulates across successive drags instead of snapping to 0.
            property real startPanX: 0
            property real startPanY: 0

            onActiveChanged: {
                if (active) {
                    startPanX = bandArea.userPanX
                    startPanY = bandArea.userPanY
                }
            }

            onCentroidChanged: {
                if (!active) {
                    return
                }

                bandArea.userPanX = bandArea.clampPanX(startPanX + centroid.position.x - centroid.pressPosition.x)
                bandArea.userPanY = bandArea.clampPanY(startPanY + centroid.position.y - centroid.pressPosition.y)
            }
        }

        ProofingZoomReadout {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 8
            z: 10
            zoomPercent: Math.round(bandModel.userZoom * 100)
        }

        StyledTextLabel {
            //! Shown before any page has rendered / while the follow has nothing to line up.
            anchors.centerIn: parent
            text: qsTrc("reprise", "Select a note to follow the original scan")
            color: ui.theme.fontSecondaryColor
            visible: !bandArea.pageEntry
        }
    }
}
