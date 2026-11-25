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

    //! 0.18.0 row-layout constants, shared by the delegate AND the state-aware section-height estimate below so the two can
    //! never drift. A record is a Column of LINE 1 (always, rowLine1Height) + LINE 2 (only when expanded, rowLine2Height, the
    //! zero-padded action buttons of rowButtonHeight + a hair). rowButtonHeight is the tight (label-hugging) button height.
    readonly property int rowLine1Height: 28
    readonly property int rowButtonHeight: 20
    readonly property int rowLine2Height: rowButtonHeight + 4

    //! STATE-AWARE content height the host ProofingMagnifier reserves for the section (plus its own header slice, capped at
    //! ~45%): every record contributes line 1; only EXPANDED (non-collapsed) records also contribute line 2. So the default
    //! all-collapsed list hugs tight (no empty space) while expanded rows still get their full height. navModel.expandedCount
    //! is live (updates on a collapse toggle + re-walk), so this re-evaluates whenever a row expands/collapses.
    readonly property int contentHeight: navModel.count * rowLine1Height + navModel.expandedCount * rowLine2Height

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

                    //! The badge counts ACTIVE (non-ignored) records only; ignored records stay listed, dimmed, but do
                    //! not count toward N. The section itself stays visible while ANY record exists (see problemCount).
                    return qsTrc("reprise", "Timing problems (%1)").arg(navModel.activeCount)
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

                //! 0.18.0 TWO-LINE RECORD. Each problem is a record container holding LINE 1 (disclosure chevron + per-kind
                //! accent stripe + problem text, with the exact-ratio hover tooltip) and LINE 2 (indented action buttons).
                //! The container is a Column so FUTURE extra lines can sit below line 2 unchanged. The chevron toggles line 2
                //! (collapse); an ignored record dims the WHOLE container in place (it stays in measure order, still navigates
                //! on click/double-click). Built WITHOUT ListItemBlank so the chevron, the nav zone and the buttons each get
                //! their own hit region (ListItemBlank's single whole-row MouseArea cannot distinguish them).
                delegate: Item {
                    id: record

                    required property int index
                    required property string display
                    required property string accentColor
                    required property string detail
                    //! true only for an UNDER-full bar ("Too few beats"): the one-click Fix (extend-to-timesig, fill with
                    //! rests) is safe. false for over-full / corrupted / exactly-nominal rows -> no Fix button is shown.
                    required property bool canRegularize
                    required property bool collapsed
                    required property bool ignored

                    width: ListView.view.width
                    implicitHeight: recordColumn.implicitHeight
                    height: implicitHeight

                    readonly property bool isCurrent: ListView.isCurrentItem

                    //! IGNORED look: dim the ENTIRE record (both lines) to an obviously-inactive state while leaving it fully
                    //! visible and in place. Animated so Ignore/Restore reads as a smooth state change.
                    opacity: record.ignored ? 0.45 : 1.0
                    Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

                    Column {
                        id: recordColumn

                        width: parent.width

                        //! LINE 1 -- chevron + accent stripe + problem text. The nav MouseArea fills the line (single-click =
                        //! select + band follow, double-click = warp + red-box pulse); the chevron MouseArea sits on top of it
                        //! and consumes its own clicks, so toggling collapse never also navigates.
                        Item {
                            id: line1

                            width: parent.width
                            height: root.rowLine1Height

                            //! Hover / selected tint behind line 1 (a hand-rolled stand-in for ListItemBlank's states).
                            Rectangle {
                                anchors.fill: parent
                                color: record.isCurrent ? ui.theme.accentColor : ui.theme.buttonColor
                                opacity: record.isCurrent
                                         ? (navArea.containsMouse ? ui.theme.accentOpacityHover : ui.theme.accentOpacityNormal)
                                         : (navArea.containsMouse ? ui.theme.buttonOpacityHover : 0)
                            }

                            //! NAV ZONE: the whole line. Declared before the chevron/text so those sit above it; the text has
                            //! no MouseArea so clicks over it fall through to here, while the chevron (declared last) intercepts.
                            MouseArea {
                                id: navArea

                                anchors.fill: parent
                                hoverEnabled: true

                                //! SINGLE click: select + band/magnifier follow (no warp). jumpTo clears+reselects so a
                                //! re-click of the already-selected measure still re-centres the band (0.17.x behaviour kept).
                                //! 0.18.0: a row-click ALSO EXPANDS this record (reveals line 2) -- added to, not replacing, the
                                //! navigate. setCollapsed(false) is a no-op when already expanded, so a click only ever expands;
                                //! the chevron stays the ONLY way to collapse a row back.
                                onClicked: {
                                    listView.currentIndex = record.index
                                    navModel.jumpTo(record.index)
                                    navModel.setCollapsed(record.index, false)
                                }

                                //! DOUBLE click: jumpTo PLUS warp the OS cursor onto the measure in the editor (fires its
                                //! hover-follow) with a red-box pulse. 0.18.0: also EXPANDS the record (like single-click).
                                onDoubleClicked: {
                                    listView.currentIndex = record.index
                                    navModel.jumpTo(record.index)
                                    navModel.warpCursorToMeasure(record.index)
                                    navModel.setCollapsed(record.index, false)
                                }

                                //! Hover tooltip: the exact "<kind>  <found> of <expected>" ratio (DetailRole), kept off the
                                //! visible row text (the nominal can be a wrong OMR read) and surfaced one hover away.
                                onContainsMouseChanged: {
                                    if (containsMouse) {
                                        ui.tooltip.show(record, record.detail)
                                    } else {
                                        ui.tooltip.hide(record)
                                    }
                                }
                            }

                            //! Per-kind coloured accent down the left edge (violet = wrong length, amber = incomplete, orange =
                            //! voice too long, red = corrupted repeat). Kept on LINE 1. Colour comes from the model (one map).
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 4
                                color: record.accentColor
                            }

                            //! Disclosure CHEVRON: toggles this record's line 2 (and any future lines). Its own MouseArea is
                            //! declared last in line1, so it is above navArea and a chevron click toggles collapse WITHOUT
                            //! navigating. Default = expanded (arrow down); collapsed = arrow right.
                            Item {
                                id: chevron

                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 18
                                height: 18

                                StyledIconLabel {
                                    anchors.centerIn: parent
                                    iconCode: record.collapsed ? IconCode.SMALL_ARROW_RIGHT : IconCode.SMALL_ARROW_DOWN
                                    color: ui.theme.fontPrimaryColor
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    //! A little extra hit padding around the small glyph so the toggle is easy to click.
                                    anchors.margins: -4
                                    onClicked: navModel.setCollapsed(record.index, !record.collapsed)
                                }
                            }

                            //! FADED-GREEN CHECK, shown ONLY when the record is IGNORED (appears on Ignore, disappears on Restore),
                            //! sitting between the chevron and the problem text. It inherits the record's dimmed opacity (the whole
                            //! container is 0.45 when ignored), so the green reads as a soft "acknowledged / handled by ignoring"
                            //! mark, not a loud status. Width collapses to 0 when hidden so a non-ignored row's text keeps its exact
                            //! position. SAFE: a genuinely FIXED problem leaves the list on the next walk, so a dimmed row can only
                            //! mean "handled by ignoring" -- the check never implies the timing was actually corrected.
                            Item {
                                id: ignoredCheck

                                anchors.left: chevron.right
                                anchors.verticalCenter: parent.verticalCenter
                                visible: record.ignored
                                width: visible ? 16 : 0
                                height: 16

                                StyledIconLabel {
                                    anchors.centerIn: parent
                                    iconCode: IconCode.TICK_RIGHT_ANGLE
                                    color: "#4caf50"
                                }
                            }

                            StyledTextLabel {
                                anchors.left: ignoredCheck.right
                                anchors.leftMargin: 8
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                horizontalAlignment: Text.AlignLeft
                                elide: Text.ElideRight
                                text: record.display
                                color: ui.theme.fontPrimaryColor
                            }
                        }

                        //! LINE 2 -- the action buttons, INDENTED so the record reads as one unit (not a new row). Collapsed
                        //! away (zero height) when the chevron is closed; the height animates so collapse/expand is smooth.
                        Item {
                            id: line2

                            //! 0.18.0: the expanded action line hugs its zero-padded buttons. buttonHeight is the tight button
                            //! height (just the label, no vertical padding); the line height (rowLine2Height) is that plus a hair
                            //! of breathing room. Both come from the shared root constants so the delegate and the host's
                            //! state-aware section-height estimate (root.contentHeight) always agree.
                            readonly property int buttonHeight: root.rowButtonHeight

                            width: parent.width
                            height: record.collapsed ? 0 : root.rowLine2Height
                            visible: height > 0
                            clip: true

                            Behavior on height { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

                            RowLayout {
                                anchors.left: parent.left
                                //! Indent under the line-1 text (past the accent stripe + chevron) so line 2 belongs to the record.
                                anchors.leftMargin: 34
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                //! FIX -- shown ONLY for a fixable (under-full, canRegularize) record. Regularize extends the
                                //! bar to its time signature (fills with rests); it self-schedules its own warp + accent-blue
                                //! flash inside regularize(), so QML does NOT warp here. Disabled while the record is ignored.
                                FlatButton {
                                    visible: record.canRegularize
                                    //! COMPACT button: short height (Layout.preferredHeight below the default button size), but
                                    //! with small LEFT/RIGHT padding (margins) kept so the label does not touch the edges, plus
                                    //! FlatButton's default 1px theme border + fill -- so it still reads clearly as a button, just
                                    //! a tight one. minWidth 0 lets the width hug the padded label instead of stretching to 132.
                                    isNarrow: true
                                    margins: 8
                                    minWidth: 0
                                    Layout.preferredHeight: line2.buttonHeight

                                    text: qsTrc("reprise", "Fix")
                                    enabled: record.canRegularize && !record.ignored

                                    toolTipTitle: qsTrc("reprise", "Regularize measure")
                                    toolTipDescription: qsTrc("reprise", "Extend this measure to its time signature (adds rests to fill)")

                                    onClicked: {
                                        listView.currentIndex = record.index
                                        navModel.regularize(record.index)
                                    }

                                    //! 0.18.0: a clearly-visible, THEME-AWARE border so the compact button obviously reads as a
                                    //! button -- the default 1px theme strokeColor border is too subtle at this size. A
                                    //! transparent-fill overlay sits on top of FlatButton's own background + hover/press states
                                    //! (which stay intact underneath); a plain Rectangle has no input handler, so clicks pass
                                    //! through to the button's MouseArea. Soft gray = fontPrimaryColor @ 30% alpha, so it clearly
                                    //! outlines the button in BOTH light and dark themes (soft, not heavy) without a hardcoded hex.
                                    //! radius matches the button's 3px.
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 3
                                        color: "transparent"
                                        border.width: 1
                                        border.color: Qt.rgba(ui.theme.fontPrimaryColor.r, ui.theme.fontPrimaryColor.g,
                                                              ui.theme.fontPrimaryColor.b, 0.3)
                                    }
                                }

                                //! IGNORE / RESTORE -- dims the whole record (out of the active count, Fix disabled) and
                                //! toggles its own label to "Restore" for a one-click un-ignore. Shown for EVERY record kind
                                //! (the only action offered for a non-fixable "Too many beats" / VoiceTooLong / CorruptedRepeat).
                                FlatButton {
                                    //! COMPACT button (see Fix above): short height + small left/right padding + the theme's 1px
                                    //! border so Ignore/Restore reads as a real button, just tight. minWidth 0 hugs the padded label.
                                    isNarrow: true
                                    margins: 8
                                    minWidth: 0
                                    Layout.preferredHeight: line2.buttonHeight

                                    text: record.ignored ? qsTrc("reprise", "Restore") : qsTrc("reprise", "Ignore")

                                    toolTipTitle: record.ignored ? qsTrc("reprise", "Restore record")
                                                                 : qsTrc("reprise", "Ignore record")
                                    toolTipDescription: record.ignored
                                                        ? qsTrc("reprise", "Count this measure as a timing problem again")
                                                        : qsTrc("reprise", "Dim this measure and drop it from the problem count (stays in the list)")

                                    onClicked: navModel.toggleIgnore(record.index)

                                    //! Clearly-visible theme-aware border (see Fix above): transparent-fill overlay, fontPrimaryColor
                                    //! @ 30% alpha (soft but still a clear outline), keeps the button's hover/press underneath and passes clicks through.
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 3
                                        color: "transparent"
                                        border.width: 1
                                        border.color: Qt.rgba(ui.theme.fontPrimaryColor.r, ui.theme.fontPrimaryColor.g,
                                                              ui.theme.fontPrimaryColor.b, 0.3)
                                    }
                                }
                            }
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
