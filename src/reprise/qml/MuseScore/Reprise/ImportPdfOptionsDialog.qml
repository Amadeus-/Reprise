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

//! NOTE: Reprise's Import-Scan intake dialog. A read-only header (file name, page count, and -- for images -- native
//! resolution), on the primary font color for legibility, appears on every import, above the settable controls. The
//! read-only signal comes from the layout (no input control + a separator). The settable controls below it depend on
//! the input type: a PDF gets the 3-tier "Recognition quality" selector (Faster/Standard/Best -> Audiveris
//! pdfResolution 200/300/400) with a matched badge + a "why" caption naming the detected dpi; a multi-page input (PDF
//! or multi-frame TIFF) also gets the page-range field; a single-page image has NO settable control, just a one-line
//! note. Shown after the file is picked and before Audiveris runs. Returns { accepted, pages, dpi } via openSync().
StyledDialogView {
    id: root

    //! Set from the opening UriQuery params. "tier" is the controller's pre-selected quality (200/300/400, already
    //! snapped); "matched" is true when it came from a successful auto-detect (drives the badge + the "why" caption).
    property string pages: "all"
    property int tier: 300
    property bool matched: false

    //! True when the picked file is an image (not a PDF): the controller derives this from the file extension. When set,
    //! the recognition-quality tier is always hidden (an image has no PDF resolution to pick); submit() keeps the
    //! seeded default tier 300. The tier is shown for ALL PDFs (it drives PDF rasterization).
    property bool isImage: false

    //! Whether the page-range controls (the instruction label + the "Pages:" row) are shown. The controller decides this
    //! (single source of truth, also logged): shown for a multi-page PDF or a multi-frame TIFF; HIDDEN for a single image
    //! and for a confidently-single-page PDF. When hidden, submit() defaults pages to "all".
    property bool showPageRange: true

    //! Read-only header fields, all passed by the controller. fileName is the picked file's basename (with extension).
    //! pageCount is the frame count for an image, or the byte-scan page count for a PDF (0 == unknown). pdfConfident is
    //! true only when the PDF page count was determined with certainty -- when false the header shows "multiple" rather
    //! than a guessed number, so a wrong count is never displayed.
    property string fileName: ""
    property int pageCount: 0
    property bool pdfConfident: false

    //! Native pixel dimensions of an image input (0 for a PDF or when the dimensions could not be read), plus the
    //! embedded DPI when the image file actually carries resolution metadata (0 == none present). embeddedDpi > 0 is a
    //! genuine value read from the file's bytes (PNG pHYs / JPEG JFIF-EXIF / TIFF IFD / BMP DIB); when it is 0 the
    //! resolution line still shows a value using the labelled "assumed" default (never a silently-invented measurement).
    //! The resolution line is shown for ANY image (single or multi-frame), never a PDF.
    property int imageWidth: 0
    property int imageHeight: 0
    property int embeddedDpi: 0

    //! DPI shown (labelled "assumed") when an image carries no real resolution metadata, for EVERY image type. Mirrors the
    //! C++ DEFAULT_IMAGE_DPI; the user prefers a shown value over a blank, and for images DPI is informational (Audiveris
    //! uses native pixels). "assumed" is deliberately distinct from the PDF path's "default" (a real applied tier setting).
    readonly property int defaultDpi: 300

    //! The PDF's detected native resolution (raw, un-snapped) that drove the tier match; shown in the "why" caption
    //! when matched. 0/absent for an image or when nothing was detected.
    property int detectedDpi: 0

    //! "Options" panel data + values (see the Options disclosure below). languagesJson is a JSON array of {code, name}
    //! the controller builds in C++ from the installed tessdata packs (QML cannot enumerate files; 'osd' is excluded).
    //! The master + sub-option values are seeded from the controller defaults and returned by submit(), then wired to
    //! Audiveris. Defaults mirror Audiveris's own (text on, eng, chords off, lyrics below), so the default run is
    //! unchanged. Text/OCR applies to every scan, so the panel appears in BOTH the single-image and the PDF states.
    property string languagesJson: "[]"
    property bool recognizeText: true
    property string language: "eng"
    property bool chordSymbols: false
    property bool lyricsAboveStaff: false

    //! The Options disclosure is DEFAULT COLLAPSED so the dialog is no taller by default.
    property bool optionsExpanded: false

    //! {text, value} model for the language dropdown (friendly display name -> Tesseract code), parsed from
    //! languagesJson. Never empty -- falls back to English so the dropdown always has at least one entry.
    readonly property var languageModel: {
        var out = []
        try {
            var arr = JSON.parse(root.languagesJson)
            for (var i = 0; i < arr.length; ++i) {
                out.push({ "text": arr[i].name, "value": arr[i].code })
            }
        } catch (e) {
        }
        if (out.length === 0) {
            out.push({ "text": qsTrc("reprise", "English"), "value": "eng" })
        }
        return out
    }

    objectName: "ImportPdfOptionsDialog"

    title: qsTrc("reprise", "Import Scan")

    contentWidth: 440
    contentHeight: content.implicitHeight
    margins: 16

    modal: true

    //! The quality tier the control is currently on. Seeded by snapping the incoming "tier" (defensive: a stored
    //! free-form scanResolution like 350 must map to the nearest tier, never be assumed to already be 200/300/400).
    property int selectedTier: root.snapTier(root.tier)

    //! The tier the auto-detect matched to; the badge is pinned here regardless of what the user later picks.
    readonly property int detectedTier: root.snapTier(root.tier)

    //! The resolution line is shown for any image whose native pixel dimensions were read; never for a PDF.
    readonly property bool showResolution: root.isImage && root.imageWidth > 0 && root.imageHeight > 0

    //! The page-count value for the header. Never a guessed number: an image with an unknown frame count, or a PDF whose
    //! count is not confident, reads "multiple".
    readonly property string pagesText: {
        if (root.isImage) {
            return root.pageCount >= 1 ? root.pageCount.toString() : qsTrc("reprise", "multiple")
        }
        return (root.pdfConfident && root.pageCount >= 1) ? root.pageCount.toString() : qsTrc("reprise", "multiple")
    }

    //! The resolution line text: native pixel dimensions, always followed by a dpi value for consistency across every
    //! file type. A measured dpi (embeddedDpi > 0) shows plainly as the file's true embedded value (NOT snapped to a
    //! 200/300/400 tier -- images are not tiered); when none is present, the labelled "assumed" default makes clear it is
    //! a guess, not a measurement. Identical treatment for PNG/JPG/TIFF/BMP/GIF.
    readonly property string resolutionText: {
        var base = qsTrc("reprise", "%1 x %2 px (native)").arg(root.imageWidth).arg(root.imageHeight)
        if (root.embeddedDpi > 0) {
            base += " " + qsTrc("reprise", "(~%1 dpi)").arg(root.embeddedDpi)
        } else {
            base += " " + qsTrc("reprise", "(~%1 dpi, assumed)").arg(root.defaultDpi)
        }
        return base
    }

    readonly property var tierOptions: [
        { "value": 200, "title": qsTrc("reprise", "Faster (200 dpi)"), "subtitle": qsTrc("reprise", "Quicker, less detail") },
        { "value": 300, "title": qsTrc("reprise", "Standard (300 dpi)"), "subtitle": qsTrc("reprise", "Recommended for most scores") },
        { "value": 400, "title": qsTrc("reprise", "Best (400 dpi)"), "subtitle": qsTrc("reprise", "Slower, most detail") }
    ]

    //! Nearest of {200, 300, 400} to an arbitrary value (mirrors the C++ snapToTier()).
    function snapTier(value) {
        var tiers = [200, 300, 400]
        var best = tiers[0]
        var bestDist = Math.abs(value - best)
        for (var i = 1; i < tiers.length; ++i) {
            var dist = Math.abs(value - tiers[i])
            if (dist < bestDist) {
                bestDist = dist
                best = tiers[i]
            }
        }
        return best
    }

    function submit() {
        var typedPages = pagesField.currentText.trim()
        root.ret = {
            errcode: 0,
            value: {
                "accepted": true,
                "pages": typedPages.length > 0 ? typedPages : "all",
                "dpi": root.selectedTier,
                "recognizeText": root.recognizeText,
                "language": root.language,
                "chordSymbols": root.chordSymbols,
                "lyricsAboveStaff": root.lyricsAboveStaff
            }
        }
        root.hide()
    }

    function dismiss() {
        root.ret = { errcode: 0, value: { "accepted": false } }
        root.hide()
    }

    onNavigationActivateRequested: {
        pagesField.navigation.requestActive()
    }

    Column {
        id: content

        width: parent.width
        spacing: 12

        //! Read-only header, shown on every import (file name, page count, and -- for images -- native resolution).
        //! The whole line uses ui.theme.fontColor (the theme's PRIMARY font-color token) so it is clearly legible in
        //! BOTH light and dark themes; an earlier build muted it with fontSecondaryColor, which rendered nearly
        //! invisible in this theme. The read-only signal still comes from the layout (no input control + the separator
        //! below), so legibility of these values wins over a subtle muted treatment.
        Column {
            id: header

            width: parent.width
            spacing: 2

            StyledTextLabel {
                width: parent.width

                text: qsTrc("reprise", "File: %1").arg(root.fileName)
                color: ui.theme.fontColor
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.Wrap
            }

            StyledTextLabel {
                width: parent.width

                text: qsTrc("reprise", "Pages: %1").arg(root.pagesText)
                color: ui.theme.fontColor
                horizontalAlignment: Text.AlignLeft
            }

            StyledTextLabel {
                width: parent.width

                visible: root.showResolution

                text: qsTrc("reprise", "Resolution: %1").arg(root.resolutionText)
                color: ui.theme.fontColor
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.Wrap
            }
        }

        SeparatorLine {
            //! Hidden in the single-image state (isImage with the page range hidden): there is no settable control
            //! below, so no header/controls divider is wanted. Shown for a PDF (the tiers + page range sit below it)
            //! and for a multi-page TIFF (the page range sits below it).
            visible: !(root.isImage && !root.showPageRange)
        }

        StyledTextLabel {
            width: parent.width

            //! Page-range instruction: shown for a PDF or a multi-frame TIFF; hidden for a single-page image.
            visible: root.showPageRange

            text: qsTrc("reprise", "Please enter the range of pages to open from this scan.")
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
        }

        Row {
            width: parent.width
            spacing: 8

            //! Page-range picker: shown for a PDF or a multi-frame TIFF; hidden for a single-page image (treated as one
            //! page; submit() defaults pages to "all").
            visible: root.showPageRange

            StyledTextLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTrc("reprise", "Pages:")
            }

            TextInputField {
                id: pagesField

                width: 160

                currentText: root.pages
                hint: qsTrc("reprise", "all, 1-5, 1,3,8")

                navigation.panel: navPanel
                navigation.order: 1
                navigation.accessible.name: qsTrc("reprise", "Pages") + " " + pagesField.currentText

                onTextChanged: function(newTextValue) {
                    pagesField.currentText = newTextValue
                }

                //! Escape while the Pages field has focus must cancel the whole dialog, exactly like clicking Cancel.
                //! TextInputField consumes Escape itself (defocuses + emits escaped()) and never lets it reach the
                //! dialog's NavigationSection, so the StyledDialogView closeOnEscape path alone would be swallowed here --
                //! hence this explicit hook. For any OTHER focus (tier radios, buttons, nothing) Escape still reaches the
                //! section and closeOnEscape (default true) cancels it; this covers the field-focused case. Same idiom as
                //! SelectMeasuresCountDialog's onEscaped.
                onEscaped: {
                    root.dismiss()
                }
            }

            StyledTextLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTrc("reprise", "(e.g. all, 1-5, 1,3,8)")
                opacity: 0.6
            }
        }

        StyledTextLabel {
            width: parent.width

            //! Recognition-quality label: PDF-only (an image has no PDF resolution to pick). Hidden for an image.
            visible: !root.isImage

            text: qsTrc("reprise", "Recognition quality")
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
        }

        RadioButtonGroup {
            id: tierGroup

            //! Recognition-quality tier picker: PDF-only; hidden for any image (submit() keeps the default 300).
            visible: !root.isImage

            width: parent.width
            spacing: 10
            orientation: ListView.Vertical

            model: root.tierOptions

            delegate: Item {
                id: tierItem

                required property var modelData
                required property int index

                width: tierGroup.width
                height: tierRow.implicitHeight

                readonly property bool isMatchedTier: root.matched && tierItem.modelData.value === root.detectedTier

                Row {
                    id: tierRow

                    width: parent.width
                    spacing: 8

                    RoundedRadioButton {
                        id: tierRadio

                        anchors.verticalCenter: parent.verticalCenter

                        checked: root.selectedTier === tierItem.modelData.value
                        text: tierItem.modelData.title

                        navigation.panel: navPanel
                        navigation.order: 2 + tierItem.index
                        navigation.accessible.name: tierItem.modelData.title + ". " + tierItem.modelData.subtitle
                            + (tierItem.isMatchedTier ? ". " + qsTrc("reprise", "Matched to your scan") : "")

                        onToggled: root.selectedTier = tierItem.modelData.value
                    }

                    StyledTextLabel {
                        anchors.verticalCenter: parent.verticalCenter

                        text: tierItem.modelData.subtitle
                        opacity: 0.6
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter

                        visible: tierItem.isMatchedTier
                        radius: 3
                        color: ui.theme.accentColor
                        height: badgeLabel.implicitHeight + 4
                        width: badgeLabel.implicitWidth + 12

                        StyledTextLabel {
                            id: badgeLabel

                            anchors.centerIn: parent
                            text: qsTrc("reprise", "Matched to your scan")
                            color: "#FFFFFF"
                        }
                    }
                }
            }
        }

        //! ===== "Options" disclosure: recognition controls, present in BOTH the single-image and PDF states =====
        //! Collapsed by default (so the dialog is no taller by default); a smooth height/opacity animation reveals a
        //! master toggle ("Recognize text & lyrics") with indented sub-options (language, chord symbols, lyrics-above)
        //! that grey out when the master is off. Wired to Audiveris per-import via submit() -> the controller ->
        //! AudiverisRunner. The chevron rotates; clicking anywhere on the header row toggles the panel.
        Column {
            id: optionsSection

            width: parent.width
            spacing: 8

            //! Header is an Item (not a Row) so the full-width MouseArea can fill it without clashing with a positioner.
            Item {
                id: optionsHeader

                width: parent.width
                height: optionsHeaderRow.implicitHeight

                Row {
                    id: optionsHeaderRow

                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    StyledIconLabel {
                        id: discIcon

                        anchors.verticalCenter: parent.verticalCenter

                        iconCode: IconCode.SMALL_ARROW_DOWN
                        //! Points right when collapsed, down when expanded; the rotation animates for a smooth disclosure.
                        rotation: root.optionsExpanded ? 0 : -90
                        font.pixelSize: 12

                        Behavior on rotation {
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                    }

                    StyledTextLabel {
                        anchors.verticalCenter: parent.verticalCenter

                        text: qsTrc("reprise", "Options")
                        font.bold: true
                        horizontalAlignment: Text.AlignLeft
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.optionsExpanded = !root.optionsExpanded
                }
            }

            //! Animated reveal: the container's height tracks its content's implicitHeight when expanded (0 when
            //! collapsed). Because the dialog's contentHeight binds to content.implicitHeight, animating this height
            //! makes the whole dialog window resize smoothly rather than snapping. clip hides the content mid-animation.
            Item {
                id: optionsReveal

                width: parent.width
                clip: true

                height: root.optionsExpanded ? optionsContent.implicitHeight : 0
                opacity: root.optionsExpanded ? 1 : 0
                visible: height > 0

                Behavior on height {
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }
                Behavior on opacity {
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }

                Column {
                    id: optionsContent

                    width: parent.width
                    spacing: 10

                    //! Master toggle (default ON). When OFF the sub-options below grey out (enabled bound to it) and
                    //! the import runs notes-only (Audiveris useOCR=false).
                    Row {
                        width: parent.width
                        spacing: 6

                        CheckBox {
                            id: masterCheck

                            anchors.verticalCenter: parent.verticalCenter

                            text: qsTrc("reprise", "Recognize text & lyrics")
                            checked: root.recognizeText

                            navigation.panel: navPanel
                            navigation.order: 10
                            navigation.accessible.name: qsTrc("reprise", "Recognize text and lyrics")

                            onClicked: root.recognizeText = !root.recognizeText
                        }

                        StyledIconLabel {
                            anchors.verticalCenter: parent.verticalCenter

                            iconCode: IconCode.QUESTION_MARK
                            opacity: 0.5
                            font.pixelSize: 12

                            MouseArea {
                                id: masterHelp
                                anchors.fill: parent
                                hoverEnabled: true

                                onContainsMouseChanged: {
                                    if (masterHelp.containsMouse) {
                                        ui.tooltip.show(parent, qsTrc("reprise", "Read titles, lyrics, and tempo markings from the scan using OCR. Turn off for a faster notes-only import."))
                                    } else {
                                        ui.tooltip.hide(parent)
                                    }
                                }
                            }
                        }
                    }

                    //! Indented sub-options (greyed when the master is off).
                    Column {
                        id: subOptions

                        x: 26
                        width: parent.width - 26
                        spacing: 10

                        Row {
                            width: parent.width
                            spacing: 8

                            StyledTextLabel {
                                anchors.verticalCenter: parent.verticalCenter

                                text: qsTrc("reprise", "Text language")
                                opacity: root.recognizeText ? 1.0 : ui.theme.itemOpacityDisabled
                            }

                            StyledDropdown {
                                id: languageDropdown

                                anchors.verticalCenter: parent.verticalCenter

                                width: 180
                                enabled: root.recognizeText

                                model: root.languageModel
                                currentIndex: {
                                    var idx = indexOfValue(root.language)
                                    return idx >= 0 ? idx : 0
                                }

                                navigation.panel: navPanel
                                navigation.order: 11

                                onActivated: function(index, value) {
                                    root.language = value
                                }
                            }
                        }

                        CheckBox {
                            id: chordCheck

                            width: parent.width
                            enabled: root.recognizeText

                            text: qsTrc("reprise", "Also detect chord symbols")
                            checked: root.chordSymbols

                            navigation.panel: navPanel
                            navigation.order: 12

                            onClicked: root.chordSymbols = !root.chordSymbols
                        }

                        Row {
                            width: parent.width
                            spacing: 6

                            CheckBox {
                                id: lyricsAboveCheck

                                anchors.verticalCenter: parent.verticalCenter

                                enabled: root.recognizeText

                                text: qsTrc("reprise", "Lyrics appear above the staff (uncommon)")
                                checked: root.lyricsAboveStaff

                                navigation.panel: navPanel
                                navigation.order: 13

                                onClicked: root.lyricsAboveStaff = !root.lyricsAboveStaff
                            }

                            StyledIconLabel {
                                anchors.verticalCenter: parent.verticalCenter

                                iconCode: IconCode.QUESTION_MARK
                                opacity: root.recognizeText ? 0.5 : (ui.theme.itemOpacityDisabled * 0.5)
                                font.pixelSize: 12

                                MouseArea {
                                    id: lyricsAboveHelp
                                    anchors.fill: parent
                                    hoverEnabled: true

                                    onContainsMouseChanged: {
                                        if (lyricsAboveHelp.containsMouse) {
                                            ui.tooltip.show(parent, qsTrc("reprise", "Enable only if the lyrics in this scan are printed above the notes rather than below them."))
                                        } else {
                                            ui.tooltip.hide(parent)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        ButtonBox {
            id: buttonBox

            width: parent.width

            navigationPanel.section: root.navigationSection
            navigationPanel.order: 3

            FlatButton {
                text: qsTrc("global", "Cancel")

                buttonRole: ButtonBoxModel.RejectRole
                buttonId: ButtonBoxModel.CustomButton + 1

                onClicked: root.dismiss()
            }

            FlatButton {
                text: qsTrc("global", "OK")
                accentButton: true

                buttonRole: ButtonBoxModel.AcceptRole
                buttonId: ButtonBoxModel.CustomButton + 2

                onClicked: root.submit()
            }
        }
    }

    NavigationPanel {
        id: navPanel

        name: "ImportPdfOptions"
        section: root.navigationSection
        order: 1
        direction: NavigationPanel.Vertical
    }
}
