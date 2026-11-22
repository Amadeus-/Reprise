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

#include <QObject>
#include <QString>
#include <qqmlintegration.h>

namespace mu::reprise {
//! View-model behind the dedicated "Recognition failed" window (ImportErrorDialog). The dialog shows the message +
//! selectable Audiveris output handed to it as UriQuery params; this model only provides the Copy-to-clipboard helper
//! (QtQuick has no clipboard binding, so the write must go through C++ -- same QGuiApplication::clipboard() idiom the
//! progress popup previously used). Deliberately holds no IoC context, so there is no "QQmlContext is not set" ctor risk.
class ImportErrorModel : public QObject
{
    Q_OBJECT

    QML_ELEMENT

public:
    explicit ImportErrorModel(QObject* parent = nullptr);

    //! Copy the given text (the dialog builds "message\n\ndetails") to the system clipboard.
    Q_INVOKABLE void copyToClipboard(const QString& text) const;

    //! Open the on-disk Audiveris log (reprise-audiveris.log) in the OS default handler. No-op if the path is empty or gone.
    Q_INVOKABLE void openLogFile(const QString& path) const;
};
}
