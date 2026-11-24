/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

import Muse.Dock
import Muse.Interactive
import Muse.Ui
import Muse.UiComponents
import MuseScore.AppShell
import MuseScore.Reprise

import "./HomePage"
import "./NotationPage"
import "./NotationReviewPage"
import "./DevTools"

DockWindow {
    id: root

    objectName: "WindowContent"

    onPageLoaded: {
        console.log("WindowContent::onPageLoaded")
        interactiveProvider.onPageOpened()

        //! NOTE (Reprise): on the FIRST page load with no persisted window geometry (fresh profile), open the main window
        //! maximized instead of the hardcoded 1150x800 QML default. muse's own fallback for this is disabled upstream
        //! (dockwindow.cpp) and muse\ is kept pristine, so this reinstates the intent from the appshell side. The helper
        //! runs its decision at most once and only when no geometry was restored, so a normal launch that restores the
        //! user's saved size/position is never force-maximized.
        startupWindow.maximizeOnFirstLaunchIfNeeded()
    }

    StartupWindowModel {
        id: startupWindow
    }

    InteractiveProvider {
        id: interactiveProvider
        topParent: root

        onRequestedDockPage: function(uri, params) {
            root.loadPage(uri, params)
        }
    }

    NavigationSection {
        id: topToolbarKeyNavSec
        name: "TopTool"
        order: 1
    }

    toolBars: [
        DockToolBar {
            id: mainToolBar

            objectName: "mainToolBar"
            title: qsTrc("appshell", "Main toolbar")

            floatable: false
            closable: false

            navigationSection: topToolbarKeyNavSec

            PageTabBar {
                id: toolBar

                model: MainToolBarModel {}

                navigation.name: "MainToolBar"
                accessibleName: qsTrc("appshell", "Main toolbar")

                navigation.section: mainToolBar.navigationSection
                navigation.order: 1

                currentUri: root.currentPageUri

                navigation.onActiveChanged: {
                    if (navigation.active) {
                        mainToolBar.forceActiveFocus()
                    }
                }

                onSelected: function(uri) {
                    root.openPage(uri)
                }

                Component.onCompleted: {
                    toolBar.focusOnFirst()
                }
            }
        }
    ]

    pages: [
        HomePage {
            window: root.window
        },

        NotationPage {
            topToolbarKeyNavSec: topToolbarKeyNavSec
        },

        NotationReviewPage {
            topToolbarKeyNavSec: topToolbarKeyNavSec
        },

        DevToolsPage {}
    ]
}
