# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2026 MuseScore Limited and others
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

muse_create_module(reprise)

target_sources(reprise PRIVATE
    reprisemodule.cpp
    reprisemodule.h
    reprisecommands.h

    internal/reprisecommandsregister.cpp
    internal/reprisecommandsregister.h
    internal/reprisecommandsstate.cpp
    internal/reprisecommandsstate.h
    internal/audiverisrunner.cpp
    internal/audiverisrunner.h
    internal/importpdfcontroller.cpp
    internal/importpdfcontroller.h
    internal/screenshotcontroller.cpp
    internal/screenshotcontroller.h
    internal/iimportprogresstracker.h
    internal/importprogresstracker.h
    internal/mcpcuration.h
    internal/pdfresolutiondetector.cpp
    internal/pdfresolutiondetector.h
    internal/scangeometrystripper.cpp
    internal/scangeometrystripper.h
    internal/irepriseconfiguration.h
    internal/repriseconfiguration.cpp
    internal/repriseconfiguration.h
    internal/ilanguagepackservice.h
    internal/languagepackservice.cpp
    internal/languagepackservice.h
    internal/iproofingmapservice.h
    internal/proofingmapservice.h
    internal/iproofingviewcontrol.h
    internal/proofingviewcontrol.h
    internal/proofingmapbuilder.cpp
    internal/proofingmapbuilder.h
    internal/badtimingcheck.cpp
    internal/badtimingcheck.h
)

#! NOTE: Reprise's Import-PDF flow reaches into project (IProjectSession, PROJECT_OPEN_COMMAND) and into
#! notation/engraving (IMasterNotation + the engraving Score/MeasureBase API) for the proofing coordinate map,
#! the scan-geometry strip and the Done-line measure count.
target_link_libraries(reprise PRIVATE project notation engraving)

#! NOTE: reprise.capture-screenshot grabs the main QQuickWindow (QQuickWindow::grabWindow). muse_global links
#! Qt::Quick only PRIVATEly, so the main reprise target must link it explicitly (the qml sub-target links it too).
target_link_libraries(reprise PRIVATE Qt::Quick)

#! NOTE: the OCR language-pack manager (LanguagePackService) downloads traineddata packs on demand with
#! QNetworkAccessManager. QtNetwork core is a separate module from the cloud/NetworkAuth stack that 0.8.0 compiled
#! out, so the fork links it explicitly here (user-initiated downloads only -- no background fetch / telemetry).
target_link_libraries(reprise PRIVATE Qt::Network)

#! NOTE: Reprise counts a PDF's pages authoritatively with QPdfDocument (Qt::Pdf), which reads compressed-xref-stream
#! / object-stream PDFs that the no-dependency byte-scan fallback cannot. The muse framework's SetupQt6.cmake component
#! list (in muse/, kept pristine) does not include Pdf, so the fork locates and links it here. A find_package with a
#! single component is additive/idempotent -- it just resolves Qt6::Pdf without disturbing the top-level find_package.
#! GLOBAL promotes the imported Qt::Pdf target to global scope: the app executable (MuseScoreStudio/Reprise.exe) is
#! finalized in the sibling src/app directory, and without the promotion Qt's dependency finalizer there warns
#! "non-existent target 'Qt::Pdf'" (the imported target would otherwise be visible only in this src/reprise scope),
#! even though Qt6Pdf.lib is already on the exe link line. find_package GLOBAL needs CMake >= 3.24 (kit ships 3.31).
find_package(Qt6 REQUIRED COMPONENTS Pdf GLOBAL)
target_link_libraries(reprise PRIVATE Qt::Pdf)

if (MUE_BUILD_PROJECT_QML)
    add_subdirectory(qml/MuseScore/Reprise)
endif()
