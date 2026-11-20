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

//! NOTE: Reprise Two-Pane Proofing TOP STRIP (Phase A2). A full-width, view-only band docked above the notation editor
//! that renders the ORIGINAL scanned page(s) the current score was recognised from. It is visible only while a proofing
//! map is held (right after an OMR import); otherwise `hasContent` is false and the host collapses it away. A2 is
//! display only -- clicking a page does nothing to the score (follow + note highlight are Phase A3). A header row gives a
//! label, the page count, and a chevron that collapses the strip down to just the header; the host owns the resizable
//! height via a divider beneath the strip.
Rectangle {
    id: root

    //! True once the model rendered at least one original page -> the host shows the strip (and its divider).
    readonly property bool hasContent: stripModel.hasMap

    //! Expanded = page thumbnails visible; collapsed = header only. Read by the host to size the strip.
    property bool expanded: true

    //! Fixed height of the always-present header row (the host uses it as the collapsed height).
    readonly property int headerHeight: 32

    color: ui.theme.backgroundPrimaryColor
    clip: true

    QtObject {
        id: prv
        readonly property int pagePadding: 8
    }

    ProofingStripModel {
        id: stripModel
    }

    Component.onCompleted: {
        stripModel.load()
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
                text: {
                    let n = stripModel.pages.length
                    return n === 1 ? qsTrc("reprise", "1 page")
                                   : qsTrc("reprise", "%1 pages").arg(n)
                }
                color: ui.theme.fontSecondaryColor
                visible: stripModel.pages.length > 0
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

    StyledListView {
        id: pagesView

        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        visible: root.expanded
        orientation: ListView.Horizontal

        spacing: 10
        leftMargin: 10
        rightMargin: 10

        model: stripModel.pages

        delegate: Item {
            id: pageItem

            required property var modelData

            height: pagesView.height
            width: modelData.height > 0 ? (height - 2 * prv.pagePadding) * (modelData.width / modelData.height) : 0

            Rectangle {
                anchors.fill: parent
                anchors.topMargin: prv.pagePadding
                anchors.bottomMargin: prv.pagePadding

                color: "white"
                border.color: ui.theme.strokeColor
                border.width: 1

                Image {
                    anchors.fill: parent
                    anchors.margins: 1

                    fillMode: Image.PreserveAspectFit
                    source: pageItem.modelData.url
                    asynchronous: true
                    cache: false
                    //! Render a touch sharper than the on-screen box so the thumbnail stays crisp when the strip is
                    //! dragged taller, without decoding the full page.
                    sourceSize.height: Math.round(pageItem.height * 1.5)
                }
            }
        }
    }
}
