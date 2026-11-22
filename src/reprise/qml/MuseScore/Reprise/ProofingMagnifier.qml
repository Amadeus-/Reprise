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

    ProofingMagnifierModel {
        id: magModel
    }

    Component.onCompleted: {
        magModel.load()
    }

    Rectangle {
        anchors.fill: parent
        color: ui.theme.backgroundPrimaryColor
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
            readonly property real noteCapW: magModel.boxW > 0 ? width / (magModel.boxW * 3.5) : noteBaseZoom
            readonly property real noteCapH: magModel.boxH > 0 ? height / (magModel.boxH * 4) : noteBaseZoom
            readonly property real noteZoom: Math.max(0.4, Math.min(noteBaseZoom, noteCapW, noteCapH, 6))

            //! Fallback (measure) zoom: frame the WHOLE measure rect with a small pad, so the dashed marker reads as a
            //! measure-level fallback rather than a tight note close-up.
            readonly property real fitZoom: (magModel.boxW > 0 && magModel.boxH > 0 && width > 0 && height > 0)
                                            ? Math.max(0.1, Math.min(width / (magModel.boxW * 1.1), height / (magModel.boxH * 1.1), 6))
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
            readonly property real clipW: zoom > 0 ? (width / zoom) * sheetToRendered : renderedW
            readonly property real clipH: zoom > 0 ? (height / zoom) * sheetToRendered : renderedH
            readonly property real clipX: boxCx * sheetToRendered - clipW / 2
            readonly property real clipY: boxCy * sheetToRendered - clipH / 2

            Rectangle {
                anchors.fill: parent
                color: "white"
                border.color: ui.theme.strokeColor
                border.width: 1
            }

            Image {
                id: pageImage

                anchors.fill: parent
                visible: view.pageEntry !== null && view.clipW > 0 && view.clipH > 0

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
            }

            //! Matched-note highlight: a SOLID accent box over the exact recognised head (PhotoScore purple-ish via the
            //! theme accent). The note is always centred, so this is a fixed centred overlay, drawn OUTSIDE the scaled
            //! image so its border stays crisp and 2px at any zoom.
            Rectangle {
                id: noteHighlight

                visible: magModel.hasCurrent && !magModel.isFallback && view.pageEntry !== null

                width: Math.max(3, magModel.boxW * view.zoom)
                height: Math.max(3, magModel.boxH * view.zoom)
                x: (view.width - width) / 2
                y: (view.height - height) / 2

                color: Qt.rgba(ui.theme.accentColor.r, ui.theme.accentColor.g, ui.theme.accentColor.b, 0.22)
                border.color: ui.theme.accentColor
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
                x: (view.width - width) / 2
                y: (view.height - height) / 2

                onPaint: {
                    let ctx = getContext("2d")
                    ctx.reset()
                    ctx.clearRect(0, 0, width, height)
                    ctx.strokeStyle = ui.theme.accentColor
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
                    if (fallbackMarker.visible) {
                        fallbackMarker.requestPaint()
                    }
                }
            }

            //! Mouse-wheel zoom: each notch (~120) steps the magnifier zoom by ~10% (1 + angleDelta.y/1200), clamped in the
            //! model to [0.5, 4.0]. The crop recenters on the followed box automatically, so no scroll bookkeeping is needed.
            //! Consumes the event so nothing behind the panel scrolls. No drag-pan.
            WheelHandler {
                onWheel: function(wheel) {
                    let factor = 1 + wheel.angleDelta.y / 1200
                    magModel.userZoom = Math.max(0.5, Math.min(4.0, magModel.userZoom * factor))
                    wheel.accepted = true
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
    }
}
