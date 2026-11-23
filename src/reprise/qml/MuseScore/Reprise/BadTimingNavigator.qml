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

//! NOTE: Reprise Two-Pane Proofing BAD-TIMING NAVIGATOR (Phase 1) -- a PhotoScore-style worklist of the problem measures
//! whose durations do not add up, meant to sit BENEATH the magnifier in the right column. Each row is one measure x staff
//! whose voice 1 is short/long (or a corrupted repeat): clicking it selects that measure's first chord/rest, and the
//! following band, the magnifier and the editor all follow the selection, so the whole proofing view jumps to the problem.
//! FAITHFUL-TO-AUDIVERIS: it only SURFACES problems, it never auto-corrects. The list live-refreshes on every edit, so a
//! fixed measure's row disappears as the user works through them. A clean score shows a short "No timing problems found".
Item {
    id: root

    //! Supplied by the host magnifier panel (ProofingMagnifier, the dock content pattern). The navigator's rows are reached
    //! by mouse, not keyboard navigation, so it simply holds the section; declared so the host can bind it without a
    //! missing-property error.
    property NavigationSection navigationSection: null

    //! Live problem count, re-exposed to the host ProofingMagnifier panel so it can COLLAPSE the whole timing section (zero
    //! height) when the score is clean and reveal it the instant a timing problem exists. navModel.count is refreshed on
    //! every undo-stack change + score switch, so this stays current and the section shows/hides as edits clear or introduce
    //! problems. Because this navigator (and navModel) live inside the always-open magnifier panel, the model is never
    //! destroyed and the count is always live across score switches.
    readonly property int problemCount: navModel.count

    //! THEME-AWARE read-only "original scan" surface tint, kept in sync by hand with ProofingStrip/ProofingMagnifier so the
    //! whole right proofing column reads as one cream reference surface. Light -> soft warm ivory; dark -> warm charcoal.
    readonly property color readOnlyCream: {
        let bg = ui.theme.backgroundPrimaryColor
        let lum = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b
        return lum < 0.5 ? Qt.rgba(0.16, 0.145, 0.12, 1.0) : Qt.rgba(0.969, 0.945, 0.882, 1.0)
    }

    BadTimingNavigatorModel {
        id: navModel
    }

    Component.onCompleted: {
        navModel.load()
    }

    Rectangle {
        anchors.fill: parent
        color: root.readOnlyCream
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            StyledIconLabel {
                iconCode: IconCode.WARNING
                Layout.alignment: Qt.AlignVCenter
            }

            StyledTextLabel {
                //! "Timing problems" + a count badge once a score is open ("Timing problems (3)"), so the user sees at a
                //! glance how many measures still need attention.
                text: {
                    if (!navModel.hasScore) {
                        return qsTrc("reprise", "Timing problems")
                    }

                    return qsTrc("reprise", "Timing problems (%1)").arg(navModel.count)
                }
                font: ui.theme.bodyBoldFont
                color: ui.theme.fontPrimaryColor
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Rectangle {
                anchors.fill: parent
                color: root.readOnlyCream
                border.color: ui.theme.strokeColor
                border.width: 1
            }

            //! The worklist. One clickable row per problem measure x staff; selecting a row jumps the whole proofing view.
            StyledListView {
                id: listView

                anchors.fill: parent
                anchors.margins: 1

                model: navModel
                visible: navModel.count > 0

                delegate: ListItemBlank {
                    id: item

                    required property int index
                    required property string display

                    height: 28
                    width: ListView.view.width

                    isSelected: ListView.isCurrentItem

                    onClicked: {
                        listView.currentIndex = item.index
                        navModel.jumpTo(item.index)
                    }

                    StyledTextLabel {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        horizontalAlignment: Text.AlignLeft
                        text: item.display
                        color: ui.theme.fontPrimaryColor
                    }
                }
            }

            //! Empty state: a clean score (or nothing loaded) keeps the panel visible with a short message rather than a
            //! blank box, so the panel reads as "checked, nothing wrong" instead of "not working".
            StyledTextLabel {
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: !navModel.hasScore ? qsTrc("reprise", "Open a score to check its timing")
                                         : qsTrc("reprise", "No timing problems found")
                color: ui.theme.fontSecondaryColor
                visible: navModel.count === 0
            }
        }
    }
}
