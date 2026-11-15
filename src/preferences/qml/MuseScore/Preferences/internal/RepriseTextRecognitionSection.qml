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

//! NOTE: Reprise's OCR language-pack manager, shown on the Reprise Preferences page below the Audiveris recognition
//! settings. It replaces the old standalone-Tesseract UI (the Tesseract OCR engine ships bundled inside Audiveris;
//! Reprise ships + manages its training data). Lets the user pick the default OCR language, see which language packs
//! are installed in Reprise's own tessdata folder, update or remove them, and add new ones -- each add/update is an
//! explicit, on-demand download (no background fetch). Everything delegates to the ReprisePreferencesModel, which in
//! turn drives the shared ILanguagePackService (the same code path the MCP helpers use).
BaseSection {
    id: root

    title: qsTrc("preferences", "Text recognition")

    navigation.direction: NavigationPanel.Both

    //! The ReprisePreferencesModel instance, supplied by the page.
    property var model: null

    //! Parsed [{code, name}] arrays, (re)built by reload() from the model's JSON. installed drives the default-language
    //! dropdown + the installed list; available drives the "Add language..." picker.
    property var installedPacks: []
    property var availablePacks: []

    //! The code currently selected in the "Add language..." picker (empty until one is chosen / when none are left).
    property string pendingAddCode: ""

    //! A transient status line under the controls (e.g. "Downloading French...", or an error). busy disables the
    //! action buttons + shows the in-progress note while a (blocking) download runs.
    property string statusText: ""
    property bool statusIsError: false
    property bool busy: false

    //! {text, value} model for the default-language dropdown: only INSTALLED packs can be the default. Never empty --
    //! falls back to English so the control always has a selection.
    readonly property var defaultLanguageModel: {
        var out = []
        for (var i = 0; i < root.installedPacks.length; ++i) {
            out.push({ "text": root.installedPacks[i].name, "value": root.installedPacks[i].code })
        }
        if (out.length === 0) {
            out.push({ "text": qsTrc("preferences", "English"), "value": "eng" })
        }
        return out
    }

    //! {text, value} model for the "Add language..." picker (available, not-yet-installed packs).
    readonly property var availableLanguageModel: {
        var out = []
        for (var i = 0; i < root.availablePacks.length; ++i) {
            out.push({ "text": root.availablePacks[i].name, "value": root.availablePacks[i].code })
        }
        return out
    }

    //! (Re)read the installed + available packs from the model and refresh the derived lists + the add-picker default.
    function reload() {
        if (!root.model) {
            return
        }

        try {
            root.installedPacks = JSON.parse(root.model.installedPacksJson())
        } catch (e) {
            root.installedPacks = []
        }

        try {
            root.availablePacks = JSON.parse(root.model.availablePacksJson())
        } catch (e2) {
            root.availablePacks = []
        }

        root.pendingAddCode = root.availablePacks.length > 0 ? root.availablePacks[0].code : ""
    }

    function setStatus(text, isError) {
        root.statusText = text
        root.statusIsError = isError
    }

    //! Shared add/update entry point: run the blocking download, then refresh. An empty return means success.
    function runAddOrUpdate(code, friendlyName) {
        if (!root.model || !Boolean(code)) {
            return
        }

        root.busy = true
        root.setStatus(qsTrc("preferences", "Downloading %1...").arg(friendlyName), false)

        var err = root.model.addOrUpdateLanguage(code)

        root.busy = false
        if (Boolean(err)) {
            root.setStatus(err, true)
        } else {
            root.setStatus(qsTrc("preferences", "%1 is ready.").arg(friendlyName), false)
        }

        root.reload()
    }

    function runRemove(code, friendlyName) {
        if (!root.model || !Boolean(code)) {
            return
        }

        var err = root.model.removeLanguage(code)
        if (Boolean(err)) {
            root.setStatus(err, true)
        } else {
            root.setStatus(qsTrc("preferences", "%1 removed.").arg(friendlyName), false)
        }

        root.reload()
    }

    //! ===== Default language =====
    Column {
        width: parent.width
        spacing: 6

        StyledTextLabel {
            text: qsTrc("preferences", "Default language")
            horizontalAlignment: Text.AlignLeft
        }

        Row {
            width: parent.width
            spacing: root.columnSpacing

            StyledDropdown {
                id: defaultLanguageDropdown

                width: root.columnWidth
                anchors.verticalCenter: parent.verticalCenter

                enabled: !root.busy

                model: root.defaultLanguageModel
                currentIndex: {
                    var idx = indexOfValue(root.model ? root.model.ocrLanguage : "eng")
                    return idx >= 0 ? idx : 0
                }

                navigation.name: "DefaultLanguageDropdown"
                navigation.panel: root.navigation
                navigation.row: 0
                navigation.column: 0

                onActivated: function(index, value) {
                    if (root.model) {
                        root.model.ocrLanguage = value
                    }
                }
            }

            StyledTextLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTrc("preferences", "Used by default when you import a scan")
                opacity: 0.6
                horizontalAlignment: Text.AlignLeft
            }
        }
    }

    SeparatorLine {}

    //! ===== Installed packs =====
    Column {
        width: parent.width
        spacing: 8

        StyledTextLabel {
            text: qsTrc("preferences", "Installed languages")
            horizontalAlignment: Text.AlignLeft
        }

        Repeater {
            id: installedRepeater

            model: root.installedPacks

            delegate: Row {
                id: packRow

                required property var modelData
                required property int index

                readonly property bool isBaseline: packRow.modelData.code === "eng"
                readonly property bool isDefault: Boolean(root.model) && packRow.modelData.code === root.model.ocrLanguage

                width: parent.width
                spacing: root.columnSpacing

                StyledTextLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    width: root.columnWidth
                    text: packRow.modelData.name
                    horizontalAlignment: Text.AlignLeft
                }

                StyledTextLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    text: packRow.isDefault ? qsTrc("preferences", "Installed (default)") : qsTrc("preferences", "Installed")
                    opacity: 0.6
                }

                FlatButton {
                    anchors.verticalCenter: parent.verticalCenter

                    text: qsTrc("preferences", "Update")
                    enabled: !root.busy

                    navigation.name: "UpdateLanguage_" + packRow.modelData.code
                    navigation.panel: root.navigation
                    navigation.row: 1 + packRow.index
                    navigation.column: 1

                    onClicked: root.runAddOrUpdate(packRow.modelData.code, packRow.modelData.name)
                }

                FlatButton {
                    anchors.verticalCenter: parent.verticalCenter

                    text: qsTrc("preferences", "Remove")

                    //! English is the bundled baseline and the current default language cannot be removed.
                    visible: !packRow.isBaseline && !packRow.isDefault
                    enabled: !root.busy

                    navigation.name: "RemoveLanguage_" + packRow.modelData.code
                    navigation.panel: root.navigation
                    navigation.row: 1 + packRow.index
                    navigation.column: 2

                    onClicked: root.runRemove(packRow.modelData.code, packRow.modelData.name)
                }
            }
        }
    }

    //! ===== Add a language =====
    Column {
        width: parent.width
        spacing: 6

        StyledTextLabel {
            text: qsTrc("preferences", "Add a language")
            horizontalAlignment: Text.AlignLeft
        }

        Row {
            width: parent.width
            spacing: root.columnSpacing

            visible: root.availablePacks.length > 0

            StyledDropdown {
                id: addLanguageDropdown

                width: root.columnWidth
                anchors.verticalCenter: parent.verticalCenter

                enabled: !root.busy

                model: root.availableLanguageModel
                currentIndex: {
                    var idx = indexOfValue(root.pendingAddCode)
                    return idx >= 0 ? idx : 0
                }

                navigation.name: "AddLanguageDropdown"
                navigation.panel: root.navigation
                navigation.row: 100
                navigation.column: 0

                onActivated: function(index, value) {
                    root.pendingAddCode = value
                }
            }

            FlatButton {
                anchors.verticalCenter: parent.verticalCenter

                text: qsTrc("preferences", "Add")
                accentButton: true
                enabled: !root.busy && Boolean(root.pendingAddCode)

                navigation.name: "AddLanguageButton"
                navigation.panel: root.navigation
                navigation.row: 100
                navigation.column: 1

                onClicked: {
                    var name = addLanguageDropdown.currentText
                    root.runAddOrUpdate(root.pendingAddCode, name)
                }
            }
        }

        StyledTextLabel {
            width: parent.width
            visible: root.availablePacks.length === 0
            text: qsTrc("preferences", "All available languages are installed.")
            opacity: 0.6
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
        }
    }

    //! Transient status / progress line (downloading, ready, or an error).
    StyledTextLabel {
        width: parent.width
        visible: Boolean(root.statusText)
        text: root.statusText
        color: ui.theme.fontColor
        opacity: root.statusIsError ? 1.0 : 0.7
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.WordWrap
    }

    //! Info line: packs use the legacy OCR model Reprise needs; adding or updating needs an internet connection.
    StyledTextLabel {
        width: parent.width
        text: qsTrc("preferences", "Language packs are downloaded from the Tesseract project. Adding or updating a language needs an internet connection; English works offline out of the box.")
        opacity: 0.6
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.WordWrap
    }
}
