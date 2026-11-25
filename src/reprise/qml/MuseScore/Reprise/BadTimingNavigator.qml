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

    //! NON-CREAM, theme-aware section background. Reprise convention: CREAM == a READ-ONLY reference surface (the following
    //! band + the magnifier crop are cream because they are view-only). This Timing-problems navigator is INTERACTIVE (its
    //! rows respond to clicks), so it deliberately does NOT inherit the host magnifier panel's cream -- it uses the theme's
    //! standard NEUTRAL panel tone instead (light grey in a light theme, the normal dark panel tone in a dark theme). The
    //! result inside the "Full detail view" panel is a clear split: cream magnifier on top (read-only), neutral navigator
    //! below (interactive).
    BadTimingNavigatorModel {
        id: navModel
    }

    Component.onCompleted: {
        navModel.load()
    }

    Rectangle {
        anchors.fill: parent
        color: ui.theme.backgroundSecondaryColor
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
                color: ui.theme.backgroundSecondaryColor
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
                    required property string accentColor
                    required property string detail
                    //! true only for an UNDER-full bar ("Too few beats"): the one-click Fix (extend-to-timesig, fill with
                    //! rests) is safe. false for over-full / corrupted / exactly-nominal rows -> the Fix button is disabled.
                    required property bool canRegularize

                    height: 28
                    width: ListView.view.width

                    isSelected: ListView.isCurrentItem

                    //! Hover tooltip: the exact "<kind>  <found> of <expected>" ratio (DetailRole). ListItemBlank shows `hint`
                    //! via ui.tooltip on hover, so the precise numbers stay one hover away while the row text stays plain.
                    hint: item.detail

                    onClicked: {
                        listView.currentIndex = item.index
                        navModel.jumpTo(item.index)
                    }

                    //! Per-kind coloured accent down the left edge so the problem kinds stay visually distinct at a glance
                    //! (violet = wrong length, amber = incomplete, orange = voice too long, red = corrupted repeat). The
                    //! colour comes from the model (accentColor role) so the kind -> colour map lives in one place.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 4
                        color: item.accentColor
                    }

                    StyledTextLabel {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        //! Leave room on the right for the (narrow) Fix button so the row text never runs under it.
                        anchors.rightMargin: 60
                        horizontalAlignment: Text.AlignLeft
                        elide: Text.ElideRight
                        text: item.display
                        color: ui.theme.fontPrimaryColor
                    }

                    //! One-click "Regularize measure to time signature". ENABLED only for an under-full bar (canRegularize);
                    //! DISABLED (greyed) otherwise so it can never truncate an over-full bar. The tooltip states, in the
                    //! enabled case, that it fills with rests; in the disabled case, that the extra beats must be removed by
                    //! hand. The server-side regularize(row) guard refuses anyway, so this is purely the UI affordance.
                    FlatButton {
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter

                        height: 20
                        //! isNarrow drops muse FlatButton's default minWidth 132 -> 24 (margins 16 -> 12) so the button hugs
                        //! the short "Fix" label instead of overflowing the row; width stays content-sized (locale-safe).
                        isNarrow: true

                        text: qsTrc("reprise", "Fix")
                        enabled: item.canRegularize

                        toolTipTitle: qsTrc("reprise", "Regularize measure")
                        toolTipDescription: item.canRegularize
                                            ? qsTrc("reprise", "Extend this measure to its time signature (adds rests to fill)")
                                            : qsTrc("reprise", "Only a measure with too few beats can be auto-extended — remove the extra beats manually")

                        onClicked: {
                            listView.currentIndex = item.index
                            navModel.regularize(item.index)
                        }
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
