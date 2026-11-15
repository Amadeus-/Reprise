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

import Muse.UiComponents
import MuseScore.Preferences

import "internal"

PreferencesPage {
    id: root

    Component.onCompleted: {
        reprisePreferencesModel.load()

        //! Never leave the Audiveris path blank: auto-detect it when it was never set. A found path is persisted at
        //! once (the model's setter writes the shared setting immediately), so it is still there on reopen.
        if (!Boolean(reprisePreferencesModel.audiverisPath)) {
            var audiveris = reprisePreferencesModel.detectAudiveris()
            if (Boolean(audiveris)) {
                reprisePreferencesModel.audiverisPath = audiveris
                recognitionSection.showAudiverisStatus(qsTrc("preferences", "Found: %1").arg(audiveris), true)
            } else {
                recognitionSection.showAudiverisStatus(qsTrc("preferences", "Not detected. Set the path manually."), false)
            }
        }

        textRecognitionSection.reload()
    }

    ReprisePreferencesModel {
        id: reprisePreferencesModel
    }

    Column {
        width: parent.width
        spacing: root.sectionsSpacing

        RepriseRecognitionSection {
            id: recognitionSection

            audiverisPath: reprisePreferencesModel.audiverisPath
            audiverisDir: reprisePreferencesModel.fileDirectory(reprisePreferencesModel.audiverisPath)

            scanResolution: reprisePreferencesModel.scanResolution
            closePopupOnComplete: reprisePreferencesModel.closePopupOnComplete

            navigation.section: root.navigationSection
            navigation.order: root.navigationOrderStart + 1

            onAudiverisPathChangeRequested: function(path) {
                reprisePreferencesModel.audiverisPath = path
                recognitionSection.showAudiverisStatus("", false)
            }

            onScanResolutionChangeRequested: function(dpi) {
                reprisePreferencesModel.scanResolution = dpi
            }

            onClosePopupOnCompleteChangeRequested: function(value) {
                reprisePreferencesModel.closePopupOnComplete = value
            }

            onDetectAudiverisRequested: {
                var found = reprisePreferencesModel.detectAudiveris()
                if (Boolean(found)) {
                    reprisePreferencesModel.audiverisPath = found
                    recognitionSection.showAudiverisStatus(qsTrc("preferences", "Found: %1").arg(found), true)
                } else {
                    recognitionSection.showAudiverisStatus(qsTrc("preferences", "Audiveris was not found. Set the path manually."), false)
                }
            }

            onFocusChanged: {
                if (activeFocus) {
                    root.ensureContentVisibleRequested(Qt.rect(x, y, width, height))
                }
            }
        }

        RepriseTextRecognitionSection {
            id: textRecognitionSection

            model: reprisePreferencesModel

            navigation.section: root.navigationSection
            navigation.order: root.navigationOrderStart + 2

            onFocusChanged: {
                if (activeFocus) {
                    root.ensureContentVisibleRequested(Qt.rect(x, y, width, height))
                }
            }
        }
    }
}
