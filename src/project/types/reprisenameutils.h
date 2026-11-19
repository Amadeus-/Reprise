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

#pragma once

#include <QFileInfo>
#include <QString>

namespace mu::project {
//! Reprise: DISPLAY-ONLY clean name shared by the Scans list NAME column and the main-window title.
//! Strips the final extension using LAST-dot logic (QFileInfo::completeBaseName keeps every internal
//! dot, unlike a first-dot split) then removes the fork's trailing ".reprise-stripped" marker. The
//! argument is a file path (or a bare file name); the underlying path/identity is never mutated.
inline QString repriseCleanDisplayName(const QString& pathOrName)
{
    QString baseName = QFileInfo(pathOrName).completeBaseName();

    static const QString repriseStrippedSuffix(".reprise-stripped");
    if (baseName.endsWith(repriseStrippedSuffix))
    {
        baseName.chop(repriseStrippedSuffix.size());
    }

    return baseName;
}
}
