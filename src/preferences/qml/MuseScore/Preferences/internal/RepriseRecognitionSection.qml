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

import Muse.Ui
import Muse.UiComponents

BaseSection {
    id: root

    title: qsTrc("preferences", "Recognition")

    navigation.direction: NavigationPanel.Both

    property string audiverisPath: ""
    property string audiverisDir: ""
    property string audiverisStatus: ""

    property int scanResolution: 300
    property bool closePopupOnComplete: false

    readonly property var executableFilter: [qsTrc("preferences", "Executable files") + " (*.exe)"]

    signal audiverisPathChangeRequested(string path)
    signal scanResolutionChangeRequested(int dpi)
    signal closePopupOnCompleteChangeRequested(bool value)
    signal detectAudiverisRequested()

    //! Show a transient recognition status. A success ("Found: ...") message fades out after 2s; a
    //! "Not detected" message stays persistent. The detected path itself lives in the FilePicker, not here,
    //! so fading the status never clears the path. The row owns its own timer + animation.
    function showAudiverisStatus(text, isSuccess) {
        audiverisFadeTimer.stop()
        audiverisFadeAnimation.stop()
        root.audiverisStatus = text
        audiverisStatusLabel.opacity = Boolean(text) ? audiverisStatusLabel.baseOpacity : 0.0
        if (Boolean(text) && isSuccess) {
            audiverisFadeTimer.restart()
        }
    }

    Column {
        width: parent.width
        spacing: 6

        StyledTextLabel {
            text: qsTrc("preferences", "Audiveris program (Audiveris.exe)")
            horizontalAlignment: Text.AlignLeft
        }

        Row {
            width: parent.width
            spacing: root.columnSpacing

            FilePicker {
                id: audiverisPicker

                pathFieldWidth: root.columnWidth
                anchors.verticalCenter: parent.verticalCenter

                dialogTitle: qsTrc("preferences", "Choose the Audiveris program")
                filter: root.executableFilter
                dir: root.audiverisDir

                path: root.audiverisPath

                navigation: root.navigation
                navigationRowOrderStart: 0
                navigationColumnOrderStart: 0

                onPathEdited: function(newPath) {
                    root.audiverisPathChangeRequested(newPath)
                }
            }

            FlatButton {
                anchors.verticalCenter: parent.verticalCenter

                text: qsTrc("preferences", "Detect")

                navigation.name: "DetectAudiverisButton"
                navigation.panel: root.navigation
                navigation.row: 0
                navigation.column: 2

                onClicked: {
                    root.detectAudiverisRequested()
                }
            }
        }

        StyledTextLabel {
            id: audiverisStatusLabel

            readonly property real baseOpacity: 0.6

            width: parent.width
            text: root.audiverisStatus
            //! An empty status is hidden; a Column excludes an invisible child from layout AND spacing,
            //! so the row collapses to zero height and everything below shifts up.
            visible: root.audiverisStatus !== ""
            opacity: baseOpacity
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap

            Timer {
                id: audiverisFadeTimer
                interval: 2000
                repeat: false
                onTriggered: audiverisFadeAnimation.start()
            }

            NumberAnimation {
                id: audiverisFadeAnimation
                target: audiverisStatusLabel
                property: "opacity"
                to: 0.0
                duration: 600

                //! Fade THEN collapse: only once the opacity fade completes naturally do we clear the text,
                //! which flips visible to false and collapses the row. onFinished fires only on natural
                //! completion, not on stop(), so a re-detect that interrupts the fade never clears spuriously.
                onFinished: root.showAudiverisStatus("", false)
            }
        }
    }

    IncrementalPropertyControlWithTitle {
        title: qsTrc("preferences", "Scan resolution")

        columnWidth: root.columnWidth
        spacing: root.columnSpacing
        control.width: 112

        minValue: 200
        maxValue: 400
        currentValue: root.scanResolution
        measureUnitsSymbol: qsTrc("preferences", "dpi")

        navigation.name: "ScanResolutionControl"
        navigation.panel: root.navigation
        navigation.row: 2
        navigation.column: 0

        onValueEdited: function(newValue) {
            root.scanResolutionChangeRequested(newValue)
        }
    }

    CheckBox {
        width: parent.width

        text: qsTrc("preferences", "Close the progress window automatically after a successful import")
        checked: root.closePopupOnComplete

        navigation.name: "ClosePopupOnCompleteBox"
        navigation.panel: root.navigation
        navigation.row: 3

        onClicked: {
            root.closePopupOnCompleteChangeRequested(!checked)
        }
    }
}
