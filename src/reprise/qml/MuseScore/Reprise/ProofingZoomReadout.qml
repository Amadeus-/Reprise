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

import Muse.Ui
import Muse.UiComponents

//! NOTE: Reprise Two-Pane Proofing shared ZOOM READOUT pill. A small rounded chip ("NNN%") the band + magnifier each
//! anchor top-right of their view. The host feeds it the current user-zoom percent; whenever that value changes the pill
//! flashes to full opacity and a ~2s one-shot timer (restarted on every change) then fades it back out via a Behavior on
//! opacity -- so a burst of wheel steps keeps it lit, and it quietly disappears once the user stops zooming. Theme-aware
//! (ui.theme tokens), styled to match the accent-bordered proofing overlays. View-only; no pointer interaction.
Rectangle {
    id: pill

    //! The zoom percentage to show (e.g. 100 at 1.0x). The instancing panel binds this to its current user zoom; a change
    //! re-flashes the pill. Rounded by the caller so the pill just renders the integer it is handed.
    property int zoomPercent: 100

    //! Suppress the initial flash: the binding fires onZoomPercentChanged once at load (100%) before any wheel input, which
    //! would briefly pop the pill for no reason. Arm only after construction so the first real zoom is the first flash.
    property bool armed: false

    implicitWidth: label.implicitWidth + 20
    implicitHeight: label.implicitHeight + 10
    width: implicitWidth
    height: implicitHeight

    radius: height / 2

    //! Translucent theme background + accent border, matching the proofing highlight/marker overlays. Reads in both themes.
    color: Qt.rgba(ui.theme.backgroundPrimaryColor.r, ui.theme.backgroundPrimaryColor.g, ui.theme.backgroundPrimaryColor.b, 0.9)
    border.color: ui.theme.accentColor
    border.width: 1

    //! Hidden until the first real zoom, then shown INSTANTLY on a change (no fade-in) and eased away after the hold. Instant
    //! appear -- not an animated fade-in -- so the pill is visible the moment the value updates AND so an offscreen/headless
    //! capture (where Qt pauses animations for a non-exposed window) still shows it; a fade-in would stay stuck at 0 there.
    opacity: 0

    NumberAnimation {
        id: fadeOut

        target: pill
        property: "opacity"
        to: 0
        duration: 300
        easing.type: Easing.OutCubic
    }

    StyledTextLabel {
        id: label

        anchors.centerIn: parent
        text: pill.zoomPercent + "%"
        font: ui.theme.bodyBoldFont
        color: ui.theme.fontPrimaryColor
    }

    Timer {
        id: hideTimer

        interval: 2000
        repeat: false
        onTriggered: {
            fadeOut.start()
        }
    }

    onZoomPercentChanged: {
        if (!pill.armed) {
            return
        }

        fadeOut.stop()
        pill.opacity = 1
        hideTimer.restart()
    }

    Component.onCompleted: {
        pill.armed = true
    }
}
