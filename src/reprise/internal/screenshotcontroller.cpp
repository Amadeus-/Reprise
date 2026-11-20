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

#include "screenshotcontroller.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QQuickWindow>
#include <QTimer>
#include <QWindow>

#include "global/log.h"
#include "global/types/val.h"
#include "global/io/file.h"
#include "global/io/iodevice.h"

#include "notation/inotation.h"
#include "project/inotationwriter.h"

#include "reprise/reprisecommands.h"

using namespace mu;
using namespace mu::reprise;
using namespace muse;

void ScreenshotController::init()
{
    //! Explicit std::function selects the exact HasOutput onRequest overload (auto-marshals Output -> structuredContent),
    //! mirroring ImportPdfController's read-only query registration.
    commandDispatcher()->onRequest<CaptureScreenshotCommand>(
        this,
        std::function<muse::RetVal<CaptureScreenshotCommand::Output>(const CaptureScreenshotCommand&)>(
            [this](const CaptureScreenshotCommand& c) { return capture(c); }));

    return;
}

muse::io::path_t ScreenshotController::screenshotsDir() const
{
    const io::path_t dir = globalConfiguration()->userAppDataPath() + "/screenshots";
    fileSystem()->makePath(dir);

    return dir;
}

muse::RetVal<CaptureScreenshotCommand::Output> ScreenshotController::capture(const CaptureScreenshotCommand& cmd)
{
    using RV = muse::RetVal<CaptureScreenshotCommand::Output>;

    CaptureScreenshotCommand::Output out;

    //! Default target is "window" when the arg is omitted/empty.
    const std::string target = cmd.target.empty() ? std::string("window") : cmd.target;

    const io::path_t dir = screenshotsDir();

    if (target == "window")
    {
        const io::path_t absPath = cmd.path.empty() ? (dir + "/mcp-window.png") : io::path_t(cmd.path);

        QWindow* qw = mainWindow() ? mainWindow()->qWindow() : nullptr;
        QQuickWindow* w = qobject_cast<QQuickWindow*>(qw);
        if (!w)
        {
            out.error = "no main window";
            return RV::make_ok(out);
        }

        //! Bring the window forward so the grab is of visible content (best-effort; grab still works if it is exposed).
        w->raise();
        w->requestActivate();

        QImage img = w->grabWindow();
        if (img.isNull())
        {
            out.error = "grab failed (window not visible?)";
            return RV::make_ok(out);
        }

        if (!img.save(absPath.toQString(), "PNG"))
        {
            out.error = "failed to save PNG to " + absPath.toStdString();
            return RV::make_ok(out);
        }

        out.ok = true;
        out.path = absPath.toStdString();
        return RV::make_ok(out);
    }

    if (target == "score")
    {
        const io::path_t absPath = cmd.path.empty() ? (dir + "/mcp-score.png") : io::path_t(cmd.path);

        notation::INotationPtr notation = projectSession()->currentNotation();
        if (!notation)
        {
            out.error = "no score open";
            return RV::make_ok(out);
        }

        project::INotationWriterPtr writer = notationWritersRegister()->writer("png");
        if (!writer)
        {
            out.error = "no PNG writer registered";
            return RV::make_ok(out);
        }

        //! PngWriter is PER_PAGE: one page per call. PAGE_NUMBER is 0-based; the approved default is page 0.
        project::INotationWriter::Options options;
        options[project::INotationWriter::OptionKey::PAGE_NUMBER] = Val(cmd.page);

        io::File file(absPath);
        if (!file.open(io::IODevice::WriteOnly))
        {
            out.error = "could not open output file: " + absPath.toStdString();
            return RV::make_ok(out);
        }

        Ret ret = writer->write(notation, file, options);
        file.close();

        if (!ret)
        {
            out.error = "png writer failed: " + ret.toString();
            return RV::make_ok(out);
        }

        out.ok = true;
        out.path = absPath.toStdString();
        return RV::make_ok(out);
    }

    if (target == "dialog")
    {
        const io::path_t absPath = cmd.path.empty() ? (dir + "/mcp-dialog.png") : io::path_t(cmd.path);

        //! The topmost window on the interactive stack. Owned popups/dialogs/menus live in their OWN QQuickWindow and are
        //! NOT part of the main-window grab, so this is the only way to capture a dialog the agent opened separately.
        QWindow* tw = interactive() ? interactive()->topWindow() : nullptr;
        QQuickWindow* dlg = qobject_cast<QQuickWindow*>(tw);
        if (!dlg)
        {
            out.error = "no dialog or popup is open";
            return RV::make_ok(out);
        }

        //! Reject the main window itself: its top window IS the main window when nothing is layered above it.
        QWindow* mw = mainWindow() ? mainWindow()->qWindow() : nullptr;
        if (dlg == mw)
        {
            out.error = "no dialog or popup is open";
            return RV::make_ok(out);
        }

        //! Bounded nested event-loop pump so the dialog window is exposed before the grab (same idiom as
        //! ImportPdfController::captureImportDialog). We do NOT force-close the dialog afterwards -- the agent opened it
        //! separately and may still want it; this tool only grabs and returns.
        auto spin = [](int ms)
        {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(ms);
            loop.exec();
        };

        QElapsedTimer timer;
        timer.start();
        const int timeoutMs = 5000;
        bool exposed = false;
        while (timer.elapsed() < timeoutMs)
        {
            if (dlg->isVisible() && dlg->isExposed())
            {
                exposed = true;
                break;
            }

            spin(60);
        }

        if (!exposed)
        {
            out.error = "dialog window did not become visible in time";
            return RV::make_ok(out);
        }

        //! A couple more pumps so the first frame is fully rendered before the grab.
        spin(120);

        QImage img = dlg->grabWindow();
        if (img.isNull())
        {
            out.error = "grab failed (dialog window not renderable?)";
            return RV::make_ok(out);
        }

        if (!img.save(absPath.toQString(), "PNG"))
        {
            out.error = "failed to save PNG to " + absPath.toStdString();
            return RV::make_ok(out);
        }

        out.ok = true;
        out.path = absPath.toStdString();
        return RV::make_ok(out);
    }

    out.error = "unknown target '" + target + "' (expected 'window', 'score' or 'dialog')";

    return RV::make_ok(out);
}
