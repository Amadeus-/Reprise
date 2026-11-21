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
import QtQuick
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents

import MuseScore.Reprise

//! NOTE: Reprise's dedicated "Recognition failed" window. Opened by ImportPdfController::showRecognitionError after the
//! recognition progress popup is closed + settled, so the progress popup never doubles as the error view. Branded navy
//! header (Reprise mark + REPRISE wordmark + "Recognition failed" title + X) over a cream card: a red headline message,
//! a selectable Audiveris output tail, and Copy + Close. message/details arrive as UriQuery params -> root properties.
StyledDialogView {
    id: root

    //! Set from the opening UriQuery params (see ImportPdfController::showRecognitionError).
    property string message: ""
    property string details: ""

    objectName: "ImportErrorDialog"

    //! Brand palette (matches ImportProgressDialog). Fixed LIGHT card independent of the app theme, plus a red error accent.
    readonly property color brandNavy: "#23264A"
    readonly property color brandNavyDeep: "#191C38"
    readonly property color brandGold: "#C9A24B"
    readonly property color brandGoldLight: "#DCB95F"
    readonly property color brandCream: "#EFE7D6"
    readonly property color brandError: "#D64C3C"
    readonly property color cardBody: "#FBF9F3"
    readonly property color cardFooter: "#F3EFE2"
    readonly property color cardStroke: "#E4DCC8"
    readonly property color inkStep: "#2A2E3F"

    contentWidth: 468
    contentHeight: mainColumn.implicitHeight
    margins: 0

    //! Modeless like the progress popup (owned/transient-parented to the Reprise main window, floats above it only). Escape
    //! closes it -- there is no running job to orphan. A plain root.hide() close is safe here: nothing races it (unlike the
    //! success auto-close, which raced the project-open stack churn), so this dialog needs no settled-close machinery.
    modal: false
    frameless: true
    closeOnEscape: true

    ImportErrorModel {
        id: errorModel
    }

    //! Build the whole copyable blob: the headline message, then the Audiveris output beneath it.
    function copyAll() {
        var text = root.message
        if (root.details.length > 0) {
            if (text.length > 0) {
                text += "\n\n"
            }
            text += root.details
        }
        errorModel.copyToClipboard(text)
    }

    Rectangle {
        id: container

        anchors.fill: parent

        radius: 6
        clip: true
        color: root.cardBody

        ColumnLayout {
            id: mainColumn

            width: parent.width
            spacing: 0

            // ===== Branded navy header =====
            Rectangle {
                id: header

                Layout.fillWidth: true
                Layout.preferredHeight: 60

                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#2A2E54" }
                    GradientStop { position: 1.0; color: root.brandNavyDeep }
                }

                //! Title-bar drag handle (frameless window): declared first so it sits below the X in z-order.
                MouseArea {
                    id: headerDrag

                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton

                    property int pressX: 0
                    property int pressY: 0

                    onPressed: function(mouse) {
                        headerDrag.pressX = mouse.x
                        headerDrag.pressY = mouse.y
                    }

                    onPositionChanged: function(mouse) {
                        if (!root.window) {
                            return
                        }

                        root.window.x += mouse.x - headerDrag.pressX
                        root.window.y += mouse.y - headerDrag.pressY
                    }
                }

                //! Red accent band at the very bottom of the header (gold on the progress popup; red here = error).
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 2
                    color: root.brandError
                    opacity: 0.9
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 8
                    spacing: 12

                    Image {
                        Layout.preferredWidth: 52
                        Layout.preferredHeight: 52
                        Layout.alignment: Qt.AlignVCenter

                        source: "resources/titlebar-mark.svg"
                        sourceSize.width: 104
                        sourceSize.height: 104
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                    }

                    ColumnLayout {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.fillWidth: true
                        spacing: 1

                        StyledTextLabel {
                            text: "REPRISE"
                            color: root.brandGold
                            font.family: "Georgia"
                            font.pixelSize: 13
                            font.bold: true
                            font.letterSpacing: 2.5
                            horizontalAlignment: Text.AlignLeft
                        }

                        StyledTextLabel {
                            text: qsTrc("reprise", "Recognition failed")
                            color: "#FFFFFF"
                            font.pixelSize: 16
                            font.bold: true
                            horizontalAlignment: Text.AlignLeft
                        }
                    }

                    //! Red circular alert badge, left of the X.
                    Rectangle {
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter

                        radius: width / 2
                        color: root.brandError

                        StyledTextLabel {
                            anchors.centerIn: parent
                            text: "!"
                            color: "#FFFFFF"
                            font.pixelSize: 16
                            font.bold: true
                        }
                    }

                    // Header close X
                    Rectangle {
                        id: closeButton

                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        Layout.alignment: Qt.AlignVCenter

                        radius: 4
                        color: closeMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

                        StyledIconLabel {
                            anchors.centerIn: parent
                            iconCode: IconCode.CLOSE_X_ROUNDED
                            color: root.brandCream
                            font.pixelSize: 16
                        }

                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.hide()
                        }
                    }
                }
            }

            // ===== Body (cream) =====
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.topMargin: 16
                Layout.bottomMargin: 14
                spacing: 12

                //! Red headline message.
                StyledTextLabel {
                    Layout.fillWidth: true
                    text: root.message
                    color: root.brandError
                    font: ui.theme.largeBodyBoldFont
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                }

                //! Selectable Audiveris output tail (shown only when there is a details tail).
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150

                    visible: root.details.length > 0

                    radius: 4
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: root.cardStroke

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        contentWidth: width
                        contentHeight: tailLabel.implicitHeight

                        //! Read-only but SELECTABLE so the user can drag-select the recognition error/log text (the Copy
                        //! button beside Close copies the whole error in one click).
                        TextEdit {
                            id: tailLabel
                            width: parent.width
                            text: root.details
                            color: root.inkStep
                            font: ui.theme.bodyFont
                            horizontalAlignment: Text.AlignLeft
                            wrapMode: TextEdit.Wrap
                            readOnly: true
                            selectByMouse: true
                            selectionColor: root.brandGold
                            opacity: 0.85
                        }
                    }
                }
            }

            // ===== Footer (cream) =====
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: footerRow.implicitHeight + 28

                color: root.cardFooter

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: root.cardStroke
                }

                RowLayout {
                    id: footerRow

                    anchors.fill: parent
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    anchors.topMargin: 14
                    anchors.bottomMargin: 14
                    spacing: 12

                    Item { Layout.fillWidth: true }

                    //! Copy = GOLD: copies the whole error message + Audiveris output to the clipboard.
                    FlatButton {
                        text: qsTrc("global", "Copy")

                        normalColor: root.brandGold
                        hoverHitColor: root.brandGoldLight

                        contentItem: Component {
                            StyledTextLabel {
                                text: qsTrc("global", "Copy")
                                color: root.brandNavyDeep
                                font: ui.theme.bodyFont
                            }
                        }

                        onClicked: root.copyAll()
                    }

                    //! Close = NAVY.
                    FlatButton {
                        text: qsTrc("global", "Close")

                        normalColor: root.brandNavy
                        hoverHitColor: root.brandNavyDeep

                        contentItem: Component {
                            StyledTextLabel {
                                text: qsTrc("global", "Close")
                                color: "#FFFFFF"
                                font: ui.theme.bodyFont
                            }
                        }

                        onClicked: root.hide()
                    }
                }
            }
        }
    }
}
