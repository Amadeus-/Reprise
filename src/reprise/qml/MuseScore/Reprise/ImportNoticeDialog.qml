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

//! NOTE: Reprise's "Import finished" partial-import notice window (0.16.3). Opened by ImportPdfController::showImportNotice
//! AFTER the valid pages imported, when some pages were skipped. A branded navy header (Reprise mark + REPRISE wordmark +
//! "Import finished" title + X) with a GOLD accent band + GOLD success badge (not the red error dialog's), over a cream card:
//! a calm lead line, an optional interior-pages explanation when survivors were split into movements, and the per-page list
//! of what was skipped shown DIRECTLY (no Details disclosure), with Copy + Close. skippedReport/multiMovement arrive as
//! UriQuery params -> root properties. Clone of ImportErrorDialog.qml; only the accent colour, badge, title, body and footer differ.
StyledDialogView {
    id: root

    //! Set from the opening UriQuery params (see ImportPdfController::showImportNotice). skippedReport is the newline-joined
    //! per-page list ("Page N: ... -- skipped"); multiMovement is true when the survivor pages were non-contiguous.
    property string skippedReport: ""
    property bool multiMovement: false
    //! PART 2 smart-deskew ladder: true when OCR was dropped by the notes-only retry (the text-role NPE recovery), so the
    //! notice explains that only the notes were imported. May be set with OR without any skipped pages.
    property bool textDropped: false

    objectName: "ImportNoticeDialog"

    //! Brand palette (matches ImportProgressDialog). Fixed LIGHT card independent of the app theme; the accent is GOLD here
    //! (a calm INFO notice), not the red error accent.
    readonly property color brandNavy: "#23264A"
    readonly property color brandNavyDeep: "#191C38"
    readonly property color brandGold: "#C9A24B"
    readonly property color brandGoldLight: "#DCB95F"
    readonly property color brandCream: "#EFE7D6"
    readonly property color cardBody: "#FBF9F3"
    readonly property color cardFooter: "#F3EFE2"
    readonly property color cardStroke: "#E4DCC8"
    readonly property color inkStep: "#2A2E3F"

    contentWidth: 468
    contentHeight: mainColumn.implicitHeight
    margins: 0

    //! Modeless like the progress popup + error dialog (owned/transient-parented to the Reprise main window, floats above it
    //! only). Escape closes it -- there is no running job to orphan. A plain root.hide() close is safe here: nothing races it,
    //! so this dialog needs no settled-close machinery.
    modal: false
    frameless: true
    closeOnEscape: true

    //! Reused only for its Copy-to-clipboard helper (QtQuick has no clipboard binding), exactly as ImportErrorDialog does.
    ImportErrorModel {
        id: noticeModel
    }

    //! The per-page list, derived from the newline-joined report, for the Repeater. An empty report yields an empty model.
    readonly property var skippedLines: root.skippedReport.length > 0 ? root.skippedReport.split("\n") : []

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

                //! Gold accent band at the very bottom of the header (gold here = an informational notice; the error dialog uses red).
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 2
                    color: root.brandGold
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
                            text: qsTrc("reprise", "Import finished")
                            color: "#FFFFFF"
                            font.pixelSize: 16
                            font.bold: true
                            horizontalAlignment: Text.AlignLeft
                        }
                    }

                    //! Gold circular success badge (a check), left of the X -- replaces the error dialog's red "!" alert badge.
                    Rectangle {
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter

                        radius: width / 2
                        color: root.brandGold

                        StyledIconLabel {
                            anchors.centerIn: parent
                            iconCode: IconCode.TICK_RIGHT_ANGLE
                            color: root.brandNavyDeep
                            font.pixelSize: 14
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

                //! LEAD: the calm reassurance that the rest of the music imported. Dark ink, large body font (matches the
                //! error dialog's lead treatment).
                StyledTextLabel {
                    Layout.fillWidth: true
                    //! Lead adapts to what happened: a skipped-pages import keeps the original reassurance; an import where only
                    //! the OCR was dropped (no skipped pages) uses a neutral lead, with the text-and-lyrics note just below.
                    text: root.skippedLines.length > 0
                          ? qsTrc("reprise", "Some pages were skipped, but the rest of your music was imported.")
                          : qsTrc("reprise", "Your music was imported.")
                    color: root.inkStep
                    font: ui.theme.largeBodyFont
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                }

                //! Text-and-lyrics explanation: shown when the notes-only retry dropped OCR (the text-role NPE recovery), so the
                //! user learns the notes imported on their own. A distinct paragraph, like the interior-pages explanation above.
                StyledTextLabel {
                    Layout.fillWidth: true
                    visible: root.textDropped
                    text: qsTrc("reprise", "The text and lyrics couldn't be read on this scan, so only the notes were imported.")
                    color: root.inkStep
                    font: ui.theme.bodyFont
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                }

                //! Interior-pages explanation: shown ONLY when the survivors were non-contiguous and Audiveris split them into
                //! movements, so only part of the music could be combined into one score. A distinct paragraph, not a list item.
                StyledTextLabel {
                    Layout.fillWidth: true
                    visible: root.multiMovement
                    text: qsTrc("reprise", "Some pages in the middle could not be combined into one score yet, so only part of the music was imported. Importing each section separately is coming in a later version.")
                    color: root.inkStep
                    font: ui.theme.bodyFont
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                }

                //! The per-page skipped list, shown DIRECTLY (no Details disclosure). Each entry is one "Page N: ... -- skipped"
                //! line from skippedReport. A subtle card groups them so they read as a distinct block under the lead.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: skippedColumn.implicitHeight + 20

                    visible: root.skippedLines.length > 0

                    radius: 4
                    color: "#FFFFFF"
                    border.width: 1
                    border.color: root.cardStroke

                    Column {
                        id: skippedColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.topMargin: 10
                        spacing: 6

                        Repeater {
                            model: root.skippedLines

                            StyledTextLabel {
                                width: skippedColumn.width
                                text: modelData
                                color: root.inkStep
                                font: ui.theme.bodyFont
                                horizontalAlignment: Text.AlignLeft
                                wrapMode: Text.Wrap
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

                    //! Copy = GOLD. Copies the per-page skipped report to the clipboard (via the reused ImportErrorModel helper).
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

                        onClicked: noticeModel.copyToClipboard(root.skippedReport)
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
