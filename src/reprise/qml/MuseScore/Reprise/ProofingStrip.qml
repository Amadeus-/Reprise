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
                color: ui.theme.fontSecondaryColor
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

        //! Scale from sheet-image pixels to on-screen band pixels: the page is drawn at the full band width, so one
        //! scale factor maps both axes (the rendered image is stretched into a box of the exact sheet aspect).
        readonly property real imgScale: sheetW > 0 ? width / sheetW : 1
        readonly property real pageDispH: sheetH * imgScale

        //! Centre the followed measure's system in the viewport, clamped to the page bounds.
        function scrollToCurrent() {
            if (!bandModel.hasCurrent || imgScale <= 0) {
                return
            }

            let target = (bandModel.rectY + bandModel.rectH / 2) * imgScale - flick.height / 2
            let maxY = Math.max(0, flick.contentHeight - flick.height)
            flick.contentY = Math.max(0, Math.min(maxY, target))
        }

        Rectangle {
            anchors.fill: parent
            color: "white"
            border.color: ui.theme.strokeColor
            border.width: 1
        }

        Flickable {
            id: flick

            anchors.fill: parent
            anchors.margins: 1

            //! View-only: auto-follow drives contentY; the user does not pan the original (matches PhotoScore).
            interactive: false
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            contentWidth: width
            contentHeight: bandArea.pageDispH

            Behavior on contentY {
                NumberAnimation {
                    duration: 260
                    easing.type: Easing.OutCubic
                }
            }

            Image {
                id: pageImage

                width: flick.width
                height: bandArea.pageDispH

                //! The box is already the exact sheet aspect, so Stretch fills it with no distortion and keeps the
                //! top-left origin at (0,0) -- which the highlight's sheet-pixel -> band-pixel scaling relies on.
                fillMode: Image.Stretch
                source: bandArea.pageEntry ? bandArea.pageEntry.url : ""
                asynchronous: true
                cache: true
                //! Decode at the rendered PNG's own width so the page stays crisp at readable band widths.
                sourceSize.width: bandArea.pageEntry ? bandArea.pageEntry.width : 0
            }

            Rectangle {
                id: highlight

                visible: bandModel.hasCurrent && bandArea.imgScale > 0

                x: bandModel.rectX * bandArea.imgScale
                y: bandModel.rectY * bandArea.imgScale
                width: bandModel.rectW * bandArea.imgScale
                height: bandModel.rectH * bandArea.imgScale

                color: Qt.rgba(ui.theme.accentColor.r, ui.theme.accentColor.g, ui.theme.accentColor.b, 0.22)
                border.color: ui.theme.accentColor
                border.width: 2
                radius: 2
            }
        }

        Connections {
            target: bandModel
            function onFollowChanged() {
                bandArea.scrollToCurrent()
            }
        }

        onWidthChanged: scrollToCurrent()
        onPageDispHChanged: scrollToCurrent()

        StyledTextLabel {
            //! Shown before any page has rendered / while the follow has nothing to line up.
            anchors.centerIn: parent
            text: qsTrc("reprise", "Select a note to follow the original scan")
            color: ui.theme.fontSecondaryColor
            visible: !bandArea.pageEntry
        }
    }
}
