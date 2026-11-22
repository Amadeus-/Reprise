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
    property string logFilePath: ""

    //! Collapsed by default: the raw Audiveris log lives behind a "Details" disclosure so the calm plain-English cause
    //! leads and the technical tail is on demand. Mirrors the intake "Options" disclosure idiom (chevron + animated reveal).
    property bool showDetails: false

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

                //! LEAD: the plain-English cause + suggestion (from AudiverisRunner::onFinished). Calm dark ink, not a red
                //! alarm -- the header title/badge already signal the error; the body should read as a friendly explanation.
                StyledTextLabel {
                    Layout.fillWidth: true
                    text: root.message
                    color: root.inkStep
                    font: ui.theme.largeBodyFont
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                }

                //! ===== "Details" disclosure: the raw Audiveris log, collapsed by default =====
                //! Mirrors the intake "Options" disclosure (ImportPdfOptionsDialog.qml): a clickable header row with a
                //! rotating chevron over an animated height/opacity reveal. Present only when there is a log tail; when
                //! expanded it shows the full scrollable log plus the Open-log + Copy actions.
                Column {
                    id: detailsSection

                    Layout.fillWidth: true
                    spacing: 8

                    visible: root.details.length > 0

                    //! Header is an Item so the full-width MouseArea can fill it without clashing with a positioner.
                    Item {
                        id: detailsHeader

                        width: parent.width
                        height: detailsHeaderRow.implicitHeight

                        Row {
                            id: detailsHeaderRow

                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6

                            StyledIconLabel {
                                anchors.verticalCenter: parent.verticalCenter

                                iconCode: IconCode.SMALL_ARROW_DOWN
                                //! Points right when collapsed, down when expanded; the rotation animates for a smooth disclosure.
                                rotation: root.showDetails ? 0 : -90
                                color: root.inkStep
                                font.pixelSize: 12

                                Behavior on rotation {
                                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                                }
                            }

                            StyledTextLabel {
                                anchors.verticalCenter: parent.verticalCenter

                                text: qsTrc("reprise", "Details")
                                color: root.inkStep
                                font.bold: true
                                horizontalAlignment: Text.AlignLeft
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.showDetails = !root.showDetails
                        }
                    }

                    //! Animated reveal: height tracks the content's implicitHeight when expanded (0 when collapsed). Because
                    //! the dialog's contentHeight binds to mainColumn.implicitHeight, animating this height resizes the whole
                    //! window smoothly rather than snapping. clip hides the content mid-animation.
                    Item {
                        id: detailsReveal

                        width: parent.width
                        clip: true

                        height: root.showDetails ? detailsContent.implicitHeight : 0
                        opacity: root.showDetails ? 1 : 0
                        visible: height > 0

                        Behavior on height {
                            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                        }
                        Behavior on opacity {
                            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                        }

                        Column {
                            id: detailsContent

                            width: parent.width
                            spacing: 12

                            //! Selectable Audiveris output: the FULL log, scrollable (StyledFlickable gives themed scrollbars)
                            //! and keyboard-selectable (Ctrl+A / Ctrl+C).
                            Rectangle {
                                width: parent.width
                                height: 180

                                radius: 4
                                color: "#FFFFFF"
                                border.width: 1
                                border.color: root.cardStroke

                                StyledFlickable {
                                    id: logFlick
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    contentWidth: width
                                    contentHeight: tailLabel.implicitHeight

                                    //! Read-only but SELECTABLE (mouse AND keyboard) so the user can drag-select or Ctrl+A /
                                    //! Ctrl+C the recognition log. Ctrl+C with no active selection copies the whole log.
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
                                        selectByKeyboard: true
                                        activeFocusOnPress: true
                                        persistentSelection: true
                                        selectionColor: root.brandGold
                                        opacity: 0.85

                                        Keys.onPressed: function(event) {
                                            if (event.modifiers & Qt.ControlModifier) {
                                                if (event.key === Qt.Key_A) {
                                                    tailLabel.selectAll()
                                                    event.accepted = true
                                                } else if (event.key === Qt.Key_C) {
                                                    if (tailLabel.selectionStart === tailLabel.selectionEnd) {
                                                        errorModel.copyToClipboard(root.details)
                                                    } else {
                                                        tailLabel.copy()
                                                    }
                                                    event.accepted = true
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            //! Log actions live WITH the log (behind Details): Open log file = NAVY, Copy(-all) = GOLD.
                            Row {
                                spacing: 12

                                //! Open log file: opens the on-disk Audiveris log in the OS default handler. Enabled only
                                //! when a log path was captured for this run.
                                FlatButton {
                                    text: qsTrc("reprise", "Open log file")

                                    enabled: root.logFilePath.length > 0

                                    normalColor: root.brandNavy
                                    hoverHitColor: root.brandNavyDeep

                                    contentItem: Component {
                                        StyledTextLabel {
                                            text: qsTrc("reprise", "Open log file")
                                            color: "#FFFFFF"
                                            font: ui.theme.bodyFont
                                        }
                                    }

                                    onClicked: errorModel.openLogFile(root.logFilePath)
                                }

                                //! Copy: copies the whole error message + Audiveris output to the clipboard.
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
                            }
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

                    //! Close = NAVY. Always visible; the log actions (Open log file / Copy) live behind Details with the log.
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
