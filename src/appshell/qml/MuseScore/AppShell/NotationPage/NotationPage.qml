/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2024 MuseScore Limited and others
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
import Muse.Dock
import Muse.Extensions
import MuseScore.AppShell

import MuseScore.NotationScene
import MuseScore.Palette
import MuseScore.PropertiesPanel
import MuseScore.InstrumentsScene
import MuseScore.Playback
import MuseScore.Reprise

DockPage {
    id: root

    objectName: "Notation"
    uri: "musescore://notation"

    required property NavigationSection topToolbarKeyNavSec

    property NotationPageModel pageModel: NotationPageModel {}

    property NavigationSection noteInputKeyNavSec: NavigationSection {
        name: "NoteInputSection"
        order: 2
    }

    property NavigationSection keynavTopPanelSec: NavigationSection {
        name: "NavigationTopPanel"
        enabled: root.visible
        order: 3
    }

    property NavigationSection keynavLeftPanelSec: NavigationSection {
        name: "NavigationLeftPanel"
        enabled: root.visible
        order: 4
    }

    property NavigationSection keynavRightPanelSec: NavigationSection {
        name: "NavigationRightPanel"
        enabled: root.visible
        order: 6
    }

    property NavigationSection keynavBottomPanelSec: NavigationSection {
        name: "NavigationBottomPanel"
        enabled: root.visible
        order: 7
    }

    function navigationPanelSec(location) {
        switch(location) {
        case Location.Top: return keynavTopPanelSec
        case Location.Left: return keynavLeftPanelSec
        case Location.Right: return keynavRightPanelSec
        case Location.Bottom: return keynavBottomPanelSec
        }

        return null
    }

    onInited: {
        Qt.callLater(pageModel.init)
    }

    readonly property int verticalPanelDefaultWidth: 300

    readonly property int horizontalPanelMinHeight: 100
    readonly property int horizontalPanelMaxHeight: 520

    readonly property int panelMinDimension: 10
    readonly property int panelMaxDimension: 7500 //! NOTE: Value found experimentally - see issue #27770

    readonly property string verticalPanelsGroup: "VERTICAL_PANELS"
    readonly property string horizontalPanelsGroup: "HORIZONTAL_PANELS"

    readonly property var verticalPanelDropDestinations: [
        { "dock": root.centralDock, "dropLocation": Location.Left, "dropDistance": root.verticalPanelDefaultWidth },
        { "dock": root.centralDock, "dropLocation": Location.Right, "dropDistance": root.verticalPanelDefaultWidth }
    ]

    readonly property var horizontalPanelDropDestinations: [
        root.panelTopDropDestination,
        root.panelBottomDropDestination
    ]

    property var notationView: null

    mainToolBars: [
        DockToolBar {
            id: notationToolBar

            objectName: "notationToolBar"
            title: qsTrc("appshell", "Notation toolbar")

            floatable: false
            closable: false
            resizable: false
            separatorsVisible: false

            alignment: DockToolBarAlignment.Center
            contentBottomPadding: 2

            compactPriorityOrder: 1

            navigationSection: root.topToolbarKeyNavSec

            NotationToolBar {
                isCompactMode: notationToolBar.isCompact

                navigationPanel.section: notationToolBar.navigationSection
                navigationPanel.order: 2
            }
        },

        DockToolBar {
            id: playbackToolBar

            objectName: root.pageModel.playbackToolBarName()
            title: qsTrc("appshell", "Playback controls")

            separatorsVisible: false
            alignment: DockToolBarAlignment.Right
            resizable: !floating

            contentBottomPadding: floating ? 8 : 2
            contentTopPadding: floating ? 8 : 0

            dropDestinations: [
                { "dock": notationToolBar, "dropLocation": Location.Right }
            ]

            navigationSection: root.topToolbarKeyNavSec

            PlaybackToolBar {
                navigationPanelSection: playbackToolBar.navigationSection
                navigationPanelOrder: 3

                floating: playbackToolBar.floating
            }
        },

        DockToolBar {
            id: extDockToolBar

            objectName: root.pageModel.extensionsToolBarName()
            title: qsTrc("appshell", "Extensions toolbar")

            separatorsVisible: false
            orientation: Qt.Horizontal
            alignment: DockToolBarAlignment.Right

            contentBottomPadding: floating ? 8 : 2
            contentTopPadding: floating ? 8 : 0

            //! NOTE: Opened by page model when there are extensions with toolbar actions
            visible: false

            dropDestinations: [
                { "dock": notationToolBar, "dropLocation": Location.Right },
                { "dock": playbackToolBar, "dropLocation": Location.Right }
            ]

            navigationSection: root.topToolbarKeyNavSec

            ExtensionsToolBar {
                id: extToolBar

                navigationPanel.section: extDockToolBar.navigationSection
                navigationPanel.order: 4
            }
        },

        DockToolBar {
            id: undoRedoToolBar

            objectName: root.pageModel.undoRedoToolBarName()
            title: qsTrc("appshell", "Undo/redo")

            floatable: false
            closable: false
            resizable: false
            separatorsVisible: false

            alignment: DockToolBarAlignment.Right
            contentBottomPadding: 2

            navigationSection: root.topToolbarKeyNavSec

            UndoRedoToolBar {
                navigationPanel.section: undoRedoToolBar.navigationSection
                navigationPanel.order: 5
            }
        }
    ]

    toolBars: [
        DockToolBar {
            id: noteInputBar

            objectName: root.pageModel.noteInputBarName()
            title: qsTrc("appshell", "Note input")

            dropDestinations: [
                root.toolBarTopDropDestination,
                root.toolBarBottomDropDestination,
                root.toolBarLeftDropDestination,
                root.toolBarRightDropDestination
            ]

            thickness: orientation === Qt.Horizontal ? 40 : 76
            resizable: !floating

            navigationSection: root.noteInputKeyNavSec

            NoteInputBar {
                orientation: noteInputBar.orientation
                floating: noteInputBar.floating

                maximumWidth: noteInputBar.width
                maximumHeight: noteInputBar.height

                navigationPanel.section: noteInputBar.navigationSection
                navigationPanel.order: 1
            }
        }
    ]

    panels: [
        DockPanel {
            id: palettesPanel

            objectName: root.pageModel.palettesPanelName()
            title: qsTrc("appshell", "Palettes")

            navigationSection: root.navigationPanelSec(palettesPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            dropDestinations: root.verticalPanelDropDestinations

            PalettesPanel {
                navigationSection: palettesPanel.navigationSection
                navigationOrderStart: palettesPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    palettesPanel.contextMenuModel = contextMenuModel
                }
            }
        },

        DockPanel {
            id: layoutPanel

            objectName: root.pageModel.layoutPanelName()
            title: qsTrc("appshell", "Layout")

            navigationSection: root.navigationPanelSec(layoutPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            dropDestinations: root.verticalPanelDropDestinations

            LayoutPanel {
                navigationSection: layoutPanel.navigationSection
                navigationOrderStart: layoutPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    layoutPanel.contextMenuModel = contextMenuModel
                }
            }
        },

        DockPanel {
            id: propertiesPanel

            objectName: root.pageModel.propertiesPanelName()
            title: qsTrc("appshell", "Properties")

            navigationSection: root.navigationPanelSec(propertiesPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            dropDestinations: root.verticalPanelDropDestinations

            PropertiesPanel {
                navigationSection: propertiesPanel.navigationSection
                navigationOrderStart: propertiesPanel.contentNavigationPanelOrderStart
                notationView: root.notationView
            }
        },

        DockPanel {
            id: selectionFilterPanel

            objectName: root.pageModel.selectionFiltersPanelName()
            title: qsTrc("appshell", "Selection filter")

            navigationSection: root.navigationPanelSec(selectionFilterPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            dropDestinations: root.verticalPanelDropDestinations

            SelectionFilterPanel {
                navigationSection: selectionFilterPanel.navigationSection
                navigationOrderStart: selectionFilterPanel.contentNavigationPanelOrderStart
            }
        },

        DockPanel {
            id: undoHistoryPanel

            objectName: root.pageModel.undoHistoryPanelName()
            title: qsTrc("notation", "History")

            navigationSection: root.navigationPanelSec(undoHistoryPanel.location)

            width: root.verticalPanelDefaultWidth
            minimumWidth: root.verticalPanelDefaultWidth
            maximumWidth: root.verticalPanelDefaultWidth

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup
            location: Location.Right

            //! NOTE: hidden by default
            visible: false

            dropDestinations: root.verticalPanelDropDestinations

            UndoHistoryPanel {
                navigationSection: undoHistoryPanel.navigationSection
                navigationOrderStart: undoHistoryPanel.contentNavigationPanelOrderStart
            }
        },

        //! NOTE: Reprise Two-Pane Proofing FULL DETAIL VIEW (Phase B2). The floating/dockable magnifier: a view-only,
        //! note-level zoom of the ORIGINAL scan centred on the current selection, with the selected note highlighted
        //! (the PhotoScore detail view that makes proofing fast). A normal floatable DockPanel (floatable defaults true),
        //! so it floats out to its own window AND docks. Its content (ProofingMagnifier) renders only while a proofing
        //! map is held -- right after an OMR import -- and otherwise shows a short placeholder; it is resizable (unlike
        //! the fixed-width vertical panels) so the user can give the zoom more room.
        DockPanel {
            id: proofingMagnifierPanel

            objectName: "proofingMagnifierPanel"
            title: qsTrc("reprise", "Full detail view")

            navigationSection: root.navigationPanelSec(proofingMagnifierPanel.location)

            //! Default set to the width the user manually dragged to and confirmed as ideal for reading the bad-timing
            //! navigator rows (recovered as 257 logical px from the persisted KDDock state); the worklist rows are now the
            //! binding constraint on this dock, and this width shows them in full.
            width: 257
            minimumWidth: 240
            maximumWidth: 900

            minimumHeight: root.panelMinDimension
            maximumHeight: root.panelMaxDimension

            groupName: root.verticalPanelsGroup
            location: Location.Right

            dropDestinations: root.verticalPanelDropDestinations

            ProofingMagnifier {
                navigationSection: proofingMagnifierPanel.navigationSection
            }
        },

        // =============================================
        // Horizontal Panels
        // =============================================

        DockPanel {
            id: mixerPanel

            objectName: root.pageModel.mixerPanelName()
            title: qsTrc("appshell", "Mixer")

            height: 368
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(mixerPanel.location)

            MixerPanel {
                id: mixerPanelComponent

                navigationSection: mixerPanel.navigationSection
                contentNavigationPanelOrderStart: mixerPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    mixerPanel.contextMenuModel = contextMenuModel
                    mixerPanel.toolbarComponent = toolbarComponent
                }

                Component.onDestruction: {
                    mixerPanel.contextMenuModel = null
                    mixerPanel.toolbarComponent = null
                }

                onResizeRequested: function(newWidth, newHeight) {
                    mixerPanel.resize(newWidth, newHeight)
                }

                Connections {
                    target: mixerPanel
                    function onPanelShown() {
                        mixerPanelComponent.resizePanelToContentHeight()
                    }
                }
            }
        },

        DockPanel {
            id: pianoKeyboardPanel

            objectName: root.pageModel.pianoKeyboardPanelName()
            title: qsTrc("appshell", "Piano keyboard")

            height: 200
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(pianoKeyboardPanel.location)

            PianoKeyboardPanel {
                navigationSection: pianoKeyboardPanel.navigationSection
                contentNavigationPanelOrderStart: pianoKeyboardPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    pianoKeyboardPanel.contextMenuModel = contextMenuModel
                }
            }
        },

        DockPanel {
            id: timelinePanel

            objectName: root.pageModel.timelinePanelName()
            title: qsTrc("appshell", "Timeline")

            height: 200
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(timelinePanel.location)

            Timeline {
                navigationSection: timelinePanel.navigationSection
                contentNavigationPanelOrderStart: timelinePanel.contentNavigationPanelOrderStart
            }
        },

        DockPanel {
            id: percussionPanel

            objectName: root.pageModel.percussionPanelName()
            title: qsTrc("appshell", "Percussion")

            height: 200
            minimumHeight: root.horizontalPanelMinHeight
            maximumHeight: root.horizontalPanelMaxHeight

            minimumWidth: root.panelMinDimension
            maximumWidth: root.panelMaxDimension

            groupName: root.horizontalPanelsGroup

            //! NOTE: hidden by default
            visible: false

            location: Location.Bottom

            dropDestinations: root.horizontalPanelDropDestinations

            navigationSection: root.navigationPanelSec(percussionPanel.location)

            PercussionPanel {
                id: percussionComponent

                navigationSection: percussionPanel.navigationSection
                contentNavigationPanelOrderStart: percussionPanel.contentNavigationPanelOrderStart

                Component.onCompleted: {
                    percussionPanel.toolbarComponent = toolbarComponent
                }

                Component.onDestruction: {
                    percussionPanel.toolbarComponent = null
                }

                onResizeRequested: function(newWidth, newHeight) {
                    percussionPanel.resize(newWidth, newHeight)
                }

                Connections {
                    target: percussionPanel
                    function onPanelShown() {
                        percussionComponent.resizePanelToContentHeight()
                    }
                }
            }
        }
    ]

    //! NOTE: Reprise Two-Pane Proofing (Phase A2). The central area is wrapped so the view-only ORIGINAL-SCAN strip can
    //! sit full-width ABOVE the notation editor. The strip (and its resize divider) appear only while a proofing map is
    //! held (right after an OMR import); otherwise the NotationView fills the whole central area exactly as before. The
    //! wrapper re-exposes the NotationView's navigationSection so DockCentralView's keyboard navigation is unchanged.
    central: Item {
        id: centralWrapper

        readonly property alias navigationSection: notationView.navigationSection

        //! Expanded height of the original-scan band, dragged via the divider (clamped in the handler). Defaults tall
        //! enough that a readable system of the original is visible (Phase A3 following band).
        property int stripHeight: 320

        readonly property int stripCurrentHeight: !proofingStrip.hasContent ? 0
                                                 : (proofingStrip.expanded ? centralWrapper.stripHeight
                                                                           : proofingStrip.headerHeight)

        ProofingStrip {
            id: proofingStrip

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: centralWrapper.stripCurrentHeight

            visible: hasContent
        }

        Rectangle {
            id: stripDivider

            anchors.top: proofingStrip.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 4

            visible: proofingStrip.hasContent && proofingStrip.expanded
            color: dividerMouse.containsMouse || dividerMouse.pressed ? ui.theme.accentColor : ui.theme.strokeColor

            MouseArea {
                id: dividerMouse

                anchors.fill: parent
                anchors.topMargin: -2
                anchors.bottomMargin: -2

                hoverEnabled: true
                cursorShape: Qt.SizeVerCursor

                onPositionChanged: function(mouse) {
                    let pt = mapToItem(centralWrapper, mouse.x, mouse.y)
                    let minH = proofingStrip.headerHeight + 40
                    let maxH = centralWrapper.height - 160
                    if (maxH < minH) {
                        maxH = minH
                    }

                    centralWrapper.stripHeight = Math.max(minH, Math.min(maxH, pt.y))
                }
            }
        }

        NotationView {
            id: notationView
            name: "MainNotationView"

            anchors.top: (proofingStrip.hasContent && proofingStrip.expanded) ? stripDivider.bottom : proofingStrip.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            isNavigatorVisible: root.pageModel.isNavigatorVisible
            isBraillePanelVisible: root.pageModel.isBraillePanelVisible
            isMainView: true

            Component.onCompleted: {
                root.notationView = notationView.paintView

                root.setDefaultNavigationControl(notationView.defaultNavigationControl)
            }

            Component.onDestruction: {
                root.setDefaultNavigationControl(null)
            }
        }
    }

    statusBar: DockStatusBar {
        objectName: root.pageModel.statusBarName()

        navigationSection: content.navigationSection

        NotationStatusBar {
            id: content
        }
    }

    tours: [
        {
            "eventCode": "online_sounds_added",
            "tour": {
                "id": "online-sounds-first-use",
                "steps": [
                    {
                        "title": qsTrc("playback", "This sound processes online"),
                        "description": qsTrc("playback", "Audio is processed in the background while you work. To trigger processing yourself, turn off automatic processing in Preferences > Audio & MIDI > Online sounds."),
                        "controlUri": "control://NotationStatusBar/NotationStatusBar/OnlineSoundsStatusView",
                        "previewImageOrGifUrl": "qrc:/resources/OnlineSoundsPreview.gif",
                        "videoExplanationUrl": "https://youtu.be/hQ4YqmHM3BE?utm_source=mss-app-yt-4.6-cantai&utm_medium=mss-app-yt-4.6-cantai&utm_campaign=mss-app-yt-4.6-cantai"
                    }
                ]
            }
        },
        {
            "eventCode": "online_sounds_manual_processing_allowed",
            "tour": {
                "id": "online-sounds-manual-process",
                "steps": [
                    {
                        "title": qsTrc("playback", "Online sounds"),
                        "description": qsTrc("playback", "Click to manually process online sounds."),
                        "controlUri": "control://NotationStatusBar/NotationStatusBar/OnlineSoundsStatusView",
                    }
                ]
            }
        },
    ]
}
