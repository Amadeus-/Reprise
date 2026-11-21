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

#include "reprisecommandsstate.h"

#include "reprise/reprisecommands.h"

using namespace muse;
using namespace muse::rcommand;
using namespace mu::reprise;

std::string RepriseCommandsState::moduleName() const
{
    return "reprise";
}

void RepriseCommandsState::init()
{
    //! NOTE: All reprise commands are always enabled. Seed the cache so the UI reflects the enabled state immediately.
    IModuleCommandsRegisterPtr moduleRegister = commandsRegister()->moduleRegister(moduleName());
    IF_ASSERT_FAILED(moduleRegister) {
        return;
    }

    for (const Command& command : moduleRegister->commandList())
    {
        m_commandStateChanged.send(command, commandState(command));
    }

    //! When a recognition starts / finishes / is cancelled the active flag flips; re-send the New-Scan command's state
    //! so its menu item + tile grey out while one import runs and re-enable the instant it is over. All on the UI thread.
    importProgressTracker()->activeChanged().onNotify(this, [this]()
    {
        m_commandStateChanged.send(REPRISE_IMPORT_PDF_COMMAND, commandState(REPRISE_IMPORT_PDF_COMMAND));
    });

    return;
}

void RepriseCommandsState::deinit()
{
    return;
}

CommandState RepriseCommandsState::commandState(const Command& command) const
{
    //! NOTE: The New-Scan / Import command is DISABLED while a recognition is already active -- this greys the New-Scan
    //! tile + File->New entry so a second import cannot be launched concurrently (the single-import guard). It re-enables
    //! the moment the active run ends (activeChanged re-sends this state). Every OTHER reprise command is always available
    //! -- "Import" creates a score, so with no active run it must be clickable even when no score/notation is open
    //! (equivalent to the project module's PCondition::Any).
    if (command == REPRISE_IMPORT_PDF_COMMAND)
    {
        const bool active = importProgressTracker()->snapshot().active;
        return CommandState(!active, false);
    }

    return CommandState(true, false);
}

async::Channel<Command, CommandState> RepriseCommandsState::commandStateChanged() const
{
    return m_commandStateChanged;
}
