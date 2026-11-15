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

#include "audiverisrunner.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "global/translation.h"
#include "global/log.h"

using namespace muse;
using namespace mu::reprise;

//! Ordered Audiveris per-sheet steps. Used only to turn the current step into an approximate within-sheet
//! fraction for the progress bar; unknown steps simply hold the last position. Kept lenient on naming.
static const QStringList STEP_ORDER = {
    "LOAD", "BINARY", "SCALE", "STEM_SCALE", "GRID", "HEADERS", "STAVES",
    "LEDGERS", "HEADS", "STEMS", "STEM_SEEDS", "BEAMS", "FLAGS", "RESTS",
    "KEYS", "CLEFS", "TIME", "MEASURES", "CHORDS", "CURVES", "TEXTS",
    "SYMBOLS", "RHYTHMS", "LINKS", "PAGE"
};

muse::io::path_t AudiverisRunner::findAudiveris() const
{
    //! A user-set path wins (decision: a configured path overrides detection), as long as it still exists.
    const io::path_t configured = repriseConfiguration()->audiverisPath();
    if (!configured.empty() && QFileInfo::exists(configured.toQString()))
    {
        return configured;
    }

    //! Otherwise the C#-matched detection search order (PATH names, then the standard install folders).
    return repriseConfiguration()->detectAudiveris();
}

muse::io::path_t AudiverisRunner::findRepriseTessdata() const
{
    //! Resolved at RUNTIME from the running executable's directory, never a build-time path, so the deployed
    //! standalone build and the dev build each find the tessdata folder shipped beside their own Reprise.exe.
    const QDir exeDir(QCoreApplication::applicationDirPath());
    const QDir dir(exeDir.absoluteFilePath("tessdata"));
    if (dir.exists() && !dir.entryList(QStringList() << "*.traineddata", QDir::Files).isEmpty())
    {
        return io::path_t(QDir::cleanPath(dir.absolutePath()));
    }

    return io::path_t();
}

std::vector<std::pair<std::string, std::string> > AudiverisRunner::installedOcrLanguages() const
{
    //! Delegate to the single owner of language-pack enumeration (ILanguagePackService): it enumerates the
    //! *.traineddata packs in Reprise's shipped tessdata folder (osd excluded) and maps each code to a friendly
    //! display name. This keeps the intake-dialog dropdown, the Preferences pack manager, and the MCP helpers all
    //! reading the exact same installed list. Returned here as (code, name) pairs for buildLanguagesJson().
    std::vector<std::pair<std::string, std::string> > result;
    for (const LanguagePackInfo& pack : languagePackService()->installedPacks())
    {
        result.push_back({ pack.code, pack.name });
    }

    return result;
}

muse::io::path_t AudiverisRunner::locateOutput(const muse::io::path_t& dir, const std::string& base, const std::string& ext) const
{
    const QString wanted = QString::fromStdString(base) + "." + QString::fromStdString(ext);
    const QString glob = "*." + QString::fromStdString(ext);

    //! Prefer an exact <base>.<ext> match, anywhere under the output dir.
    QString firstAny;
    QDirIterator it(dir.toQString(), QStringList() << glob, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        if (QFileInfo(path).fileName().compare(wanted, Qt::CaseInsensitive) == 0)
        {
            return io::path_t(path);
        }

        if (firstAny.isEmpty())
        {
            firstAny = path;
        }
    }

    return firstAny.isEmpty() ? io::path_t() : io::path_t(firstAny);
}

std::vector<int> AudiverisRunner::expandSheets(const QString& pages) const
{
    //! "all"/empty -> every page (no -sheets argument). Otherwise expand comma lists and dashed ranges into
    //! explicit 1-based sheet numbers; Audiveris '-sheets' accepts a space-separated int[] (e.g. "1 4-5").
    std::vector<int> sheets;

    const QString trimmed = pages.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("all", Qt::CaseInsensitive) == 0)
    {
        return sheets;
    }

    const QStringList tokens = trimmed.split(',', Qt::SkipEmptyParts);
    for (const QString& rawToken : tokens)
    {
        const QString token = rawToken.trimmed();
        if (token.isEmpty())
        {
            continue;
        }

        const int dash = token.indexOf('-');
        if (dash > 0)
        {
            bool okLo = false;
            bool okHi = false;
            const int lo = token.left(dash).trimmed().toInt(&okLo);
            const int hi = token.mid(dash + 1).trimmed().toInt(&okHi);
            if (okLo && okHi && lo >= 1 && hi >= lo)
            {
                for (int n = lo; n <= hi; ++n)
                {
                    sheets.push_back(n);
                }
            }
        }
        else
        {
            bool ok = false;
            const int n = token.toInt(&ok);
            if (ok && n >= 1)
            {
                sheets.push_back(n);
            }
        }
    }

    return sheets;
}

std::vector<std::string> AudiverisRunner::buildTextConstants(const RecognitionOptions& options) const
{
    std::vector<std::string> constants;

    auto addConstant = [&constants](const std::string& assignment)
    {
        constants.push_back("-constant");
        constants.push_back(assignment);
    };

    //! Master OFF: skip the whole TEXTS/OCR step (notes-only, faster). Nothing else text-related applies when OFF.
    if (!options.recognizeText)
    {
        addConstant("org.audiveris.omr.text.tesseract.TesseractOCR.useOCR=false");
        return constants;
    }

    //! Master ON (default): pass NO constant when a sub-option is at its Audiveris default. Audiveris's own defaults
    //! are lyrics on, chords off, lyrics below the staff, language eng -- so the default path emits an empty list and
    //! the proven default OCR behavior is left exactly as-is (no redundant constants clutter the command).
    if (options.chordSymbols)
    {
        addConstant("org.audiveris.omr.sheet.ProcessingSwitches.chordNames=true");
    }

    if (options.lyricsAboveStaff)
    {
        addConstant("org.audiveris.omr.sheet.ProcessingSwitches.lyricsAboveStaff=true");
    }

    //! Language: only when the chosen code differs from Audiveris's own default (eng). Passing eng explicitly is
    //! harmless but redundant, so it is omitted. A '+'-joined multi-language spec (e.g. "eng+deu") passes through as-is.
    const QString lang = options.language.trimmed();
    if (!lang.isEmpty() && lang.compare("eng", Qt::CaseInsensitive) != 0)
    {
        addConstant("org.audiveris.omr.text.Language.defaultSpecification=" + lang.toStdString());
    }

    return constants;
}

bool AudiverisRunner::isRunning() const
{
    return m_process != nullptr;
}

bool AudiverisRunner::start(const muse::io::path_t& pdfPath, const QString& pages, int dpi, const RecognitionOptions& options)
{
    const io::path_t audiverisExe = findAudiveris();
    if (audiverisExe.empty())
    {
        return false;
    }

    m_pdfPath = pdfPath;
    m_cancelled = false;
    m_totalSheets = -1;
    m_currentSheet = -1;
    m_requestedSheetCount = 0;
    m_totalLocked = false;
    m_maxSheet = 0;
    m_maxPercent = -1;
    m_currentStep.clear();
    m_stdoutBuffer.clear();
    m_tail.clear();

    //! A persistent temp dir: outputs (.mxl, .omr) must survive the run. Durable copy is a later step.
    QTemporaryDir tempDir(QDir::tempPath() + "/reprise-omr-XXXXXX");
    tempDir.setAutoRemove(false);
    if (!tempDir.isValid())
    {
        //! Deferred so the caller can connect() to finished() before it fires.
        QMetaObject::invokeMethod(this, [this]()
        {
            emit finished(false, QString(), muse::qtrc("reprise", "Could not create a temporary output directory"), QString());
        }, Qt::QueuedConnection);
        return true;
    }

    m_outDir = tempDir.path();

    std::vector<std::string> args = {
        "-batch",
        "-save",
        "-export",
        //! Scan resolution. Runtime-confirmed effect on the rasterised image (e.g. =400 -> 3305x4677 vs 300-default 2479x3508).
        "-constant", "org.audiveris.omr.image.ImageLoading.pdfResolution=" + std::to_string(dpi),
        "-output", m_outDir.toStdString()
    };

    //! Per-import recognition options -> Audiveris '-constant' switches. Both the GUI and headless/MCP paths reach
    //! here with a RecognitionOptions, so this one call is the single source of truth for the text constants. Master
    //! ON at defaults contributes nothing (preserving Audiveris's own proven defaults); master OFF adds useOCR=false.
    const std::vector<std::string> textConstants = buildTextConstants(options);
    args.insert(args.end(), textConstants.begin(), textConstants.end());

    //! Page selection. '-sheets' takes a space-separated int[]; keep it immediately before '--' so args4j
    //! stops consuming numbers at the separator. Empty list -> omit entirely -> Audiveris reads every page.
    const std::vector<int> sheets = expandSheets(pages);
    m_requestedSheetCount = static_cast<int>(sheets.size());

    //! When a page range was requested we already know EXACTLY how many sheets Audiveris will process, so take that
    //! as the authoritative total up front and lock it -- no parsing, and immune to any stray "N sheet(s)" line.
    if (m_requestedSheetCount > 0)
    {
        m_totalSheets = m_requestedSheetCount;
        m_totalLocked = true;
    }

    if (!sheets.empty())
    {
        args.push_back("-sheets");
        for (int sheet : sheets)
        {
            args.push_back(std::to_string(sheet));
        }
    }

    args.push_back("--");
    args.push_back(pdfPath.toQString().toStdString());

    QStringList qargs;
    for (const std::string& a : args)
    {
        qargs << QString::fromStdString(a);
    }

    m_process = new QProcess(this);
    m_process->setProgram(audiverisExe.toQString());
    m_process->setArguments(qargs);

    //! Point Tesseract at its training data. Reprise SHIPS its own legacy+LSTM tessdata beside the executable and it
    //! is the SOLE authoritative source: Audiveris inits Tesseract in LEGACY mode, which the system LSTM-only "fast"
    //! eng.traineddata cannot satisfy (legacy init fails -> no OCR -> no lyrics/titles), so only the self-contained
    //! legacy data Reprise ships + manages is ever used. If the shipped folder is somehow missing we log a clear
    //! error rather than silently falling back to a wrong system model (which would produce garbage OCR).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const io::path_t tessdata = findRepriseTessdata();
    if (!tessdata.empty())
    {
        env.insert("TESSDATA_PREFIX", tessdata.toQString());
        LOGI() << "Reprise: TESSDATA_PREFIX set to " << tessdata.toQString();
    }
    else
    {
        LOGW() << "Reprise: shipped tessdata folder (<exeDir>/tessdata) is missing or empty; OCR will not work. "
                  "Reprise ships and manages this data -- reinstall or redeploy to restore it.";
    }
    m_process->setProcessEnvironment(env);

    connect(m_process, &QProcess::readyReadStandardOutput, this, &AudiverisRunner::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &AudiverisRunner::onReadyReadStandardError);
    connect(m_process, &QProcess::errorOccurred, this, &AudiverisRunner::onErrorOccurred);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) { onFinished(exitCode); });

    LOGI() << "Reprise: running Audiveris: " << audiverisExe.toQString() << " on " << pdfPath.toQString()
           << " (dpi=" << dpi << ", sheets=" << (sheets.empty() ? std::string("all") : std::to_string(sheets.size())) << ")";

    //! Make the recognition options observable on the headless path (no GUI). The assembled text '-constant' args also
    //! appear on the full command line logged below; this summary line states the resolved option values directly.
    QStringList constantArgsForLog;
    for (const std::string& c : textConstants)
    {
        constantArgsForLog << QString::fromStdString(c);
    }
    LOGI() << "Reprise: recognition options: recognizeText=" << (options.recognizeText ? "true" : "false")
           << " language=" << options.language
           << " chordSymbols=" << (options.chordSymbols ? "true" : "false")
           << " lyricsAboveStaff=" << (options.lyricsAboveStaff ? "true" : "false")
           << " -> text constants: " << (constantArgsForLog.isEmpty() ? QString("(none)") : constantArgsForLog.join(' '));

    LOGI() << "[audiveris] command: \"" << audiverisExe.toQString() << "\" " << qargs.join(' ');

    //! Kick an initial indeterminate tick so the popup shows an animated bar immediately.
    QMetaObject::invokeMethod(this, [this]()
    {
        emit progressChanged(-1, -1, -1, muse::qtrc("reprise", "Starting Audiveris..."));
    }, Qt::QueuedConnection);

    m_process->start();
    return true;
}

void AudiverisRunner::cancel()
{
    m_cancelled = true;

    if (m_process)
    {
        //! Disconnect first so the kill does not fire onFinished()/onErrorOccurred() into the model.
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
}

void AudiverisRunner::onReadyReadStandardOutput()
{
    if (!m_process)
    {
        return;
    }

    m_stdoutBuffer += QString::fromLocal8Bit(m_process->readAllStandardOutput());

    int nl = -1;
    while ((nl = m_stdoutBuffer.indexOf('\n')) >= 0)
    {
        QString line = m_stdoutBuffer.left(nl);
        m_stdoutBuffer.remove(0, nl + 1);
        line.remove('\r');
        if (!line.trimmed().isEmpty())
        {
            LOGI() << "[audiveris] " << line;
            parseLine(line);
            pushTail(line);
            emit outputLine(line);
        }
    }
}

void AudiverisRunner::onReadyReadStandardError()
{
    if (!m_process)
    {
        return;
    }

    const QString text = QString::fromLocal8Bit(m_process->readAllStandardError());
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (const QString& raw : lines)
    {
        QString line = raw;
        line.remove('\r');
        if (!line.trimmed().isEmpty())
        {
            LOGW() << "[audiveris:stderr] " << line;
            pushTail(line);
            emit outputLine(line);
        }
    }
}

void AudiverisRunner::parseLine(const QString& line)
{
    bool changed = false;

    //! Total sheets. The book prints its sheet count ONCE ("... N sheets in <pdf>"), before any per-sheet
    //! processing, so the authoritative total is the FIRST "N sheets" line that is not itself a per-sheet-tagged
    //! status line (those carry a "#<n>]" tag). We lock on that first accept, so a later stray "<n> sheet(s)"
    //! line can never overwrite it -- that clobber is what previously drove the total down to 1 ("page 3 of 1").
    //! Skipped entirely once the total is already locked (e.g. from a requested page range).
    static const QRegularExpression reSheetTag("#\\d+\\]");
    if (!m_totalLocked && !reSheetTag.match(line).hasMatch())
    {
        static const QRegularExpression reTotal("(\\d+)\\s+sheets?\\b", QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch mTotal = reTotal.match(line);
        if (mTotal.hasMatch())
        {
            const int total = mTotal.captured(1).toInt();
            if (total > 0)
            {
                m_totalSheets = total;
                m_totalLocked = true;
                changed = true;
            }
        }
    }

    //! Current sheet: the LEADING logger tag "LEVEL [<book>#<N>] ...". Anchored to that first bracket on purpose: a
    //! multi-sheet run also prints "Book reaching PAGE on sheets:[#1#2]" (empty leading tag) BEFORE any sheet starts, and an
    //! unanchored "#<n>]" search matched its trailing "#2]" -> "page 2 of 2" + a near-full bar from the first moment.
    static const QRegularExpression reSheet("^\\s*\\w+\\s+\\[[^\\]]*#(\\d+)\\]");
    const QRegularExpressionMatch mSheet = reSheet.match(line);
    if (mSheet.hasMatch())
    {
        const int sheet = mSheet.captured(1).toInt();
        if (sheet > 0 && sheet != m_currentSheet)
        {
            m_currentSheet = sheet;
            changed = true;
        }
    }

    //! Current step: "StepMonitoring ... | STEP".
    if (line.contains("StepMonitoring"))
    {
        const int bar = line.lastIndexOf('|');
        if (bar >= 0)
        {
            const QString step = line.mid(bar + 1).trimmed().section(' ', 0, 0).toUpper();
            if (!step.isEmpty() && step != m_currentStep)
            {
                m_currentStep = step;
                changed = true;
            }
        }
    }

    if (changed)
    {
        //! Clamp the current sheet to the known total so we never report current > total ("page 3 of 1").
        int sheet = m_currentSheet;
        if (m_totalSheets > 0 && sheet > m_totalSheets)
        {
            sheet = m_totalSheets;
        }

        //! Monotonic: recognition only moves forward, so never let the reported sheet / percent go backwards.
        if (sheet > m_maxSheet)
        {
            m_maxSheet = sheet;
        }
        sheet = m_maxSheet;

        int percent = computePercent(sheet);
        if (percent > m_maxPercent)
        {
            m_maxPercent = percent;
        }
        percent = m_maxPercent;

        emit progressChanged(percent, sheet, m_totalSheets, humanStage(m_currentStep));
    }
}

int AudiverisRunner::computePercent(int currentSheet) const
{
    if (m_totalSheets <= 0)
    {
        return -1;
    }

    const int sheet = currentSheet > 0 ? currentSheet : 1;

    double withinSheet = 0.0;
    const int stepIndex = STEP_ORDER.indexOf(m_currentStep);
    if (stepIndex >= 0 && !STEP_ORDER.isEmpty())
    {
        withinSheet = double(stepIndex + 1) / double(STEP_ORDER.size());
    }

    double fraction = (double(sheet - 1) + withinSheet) / double(m_totalSheets);
    if (fraction < 0.0)
    {
        fraction = 0.0;
    }
    if (fraction > 1.0)
    {
        fraction = 1.0;
    }

    return int(fraction * 100.0 + 0.5);
}

QString AudiverisRunner::humanStage(const QString& rawStep) const
{
    if (rawStep.isEmpty())
    {
        return muse::qtrc("reprise", "Recognizing...");
    }

    //! Friendly labels for the common Audiveris steps; unknown steps fall back to the raw name, lowercased.
    if (rawStep == "LOAD")       { return muse::qtrc("reprise", "loading the page..."); }
    if (rawStep == "BINARY")     { return muse::qtrc("reprise", "cleaning up the image..."); }
    if (rawStep == "SCALE" || rawStep == "STEM_SCALE") { return muse::qtrc("reprise", "measuring the staves..."); }
    if (rawStep == "GRID" || rawStep == "STAVES" || rawStep == "HEADERS") { return muse::qtrc("reprise", "detecting staves..."); }
    if (rawStep == "LEDGERS")    { return muse::qtrc("reprise", "reading ledger lines..."); }
    if (rawStep == "HEADS")      { return muse::qtrc("reprise", "reading notes..."); }
    if (rawStep == "STEMS" || rawStep == "STEM_SEEDS") { return muse::qtrc("reprise", "finding stems..."); }
    if (rawStep == "BEAMS" || rawStep == "FLAGS") { return muse::qtrc("reprise", "reading beams..."); }
    if (rawStep == "RESTS")      { return muse::qtrc("reprise", "reading rests..."); }
    if (rawStep == "KEYS")       { return muse::qtrc("reprise", "reading key signatures..."); }
    if (rawStep == "CLEFS")      { return muse::qtrc("reprise", "reading clefs..."); }
    if (rawStep == "TIME")       { return muse::qtrc("reprise", "reading time signatures..."); }
    if (rawStep == "MEASURES")   { return muse::qtrc("reprise", "finding measures..."); }
    if (rawStep == "CHORDS")     { return muse::qtrc("reprise", "reading chords..."); }
    if (rawStep == "CURVES")     { return muse::qtrc("reprise", "reading slurs and ties..."); }
    if (rawStep == "TEXTS")      { return muse::qtrc("reprise", "reading text and lyrics..."); }
    if (rawStep == "SYMBOLS")    { return muse::qtrc("reprise", "reading symbols..."); }
    if (rawStep == "RHYTHMS")    { return muse::qtrc("reprise", "working out the rhythm..."); }
    if (rawStep == "LINKS")      { return muse::qtrc("reprise", "connecting everything up..."); }
    if (rawStep == "PAGE")       { return muse::qtrc("reprise", "finishing the page..."); }

    return rawStep.toLower() + "...";
}

void AudiverisRunner::pushTail(const QString& line)
{
    //! Small ring of the most recent output lines, surfaced in the popup's error state.
    static const int MAX_TAIL = 16;
    m_tail << line;
    while (m_tail.size() > MAX_TAIL)
    {
        m_tail.removeFirst();
    }
}

void AudiverisRunner::onErrorOccurred()
{
    if (m_cancelled || !m_process)
    {
        return;
    }

    const QString err = m_process->errorString();
    const QString tail = m_tail.join('\n');

    m_process->deleteLater();
    m_process = nullptr;

    emit finished(false, QString(), muse::qtrc("reprise", "Audiveris could not be started.") + " " + err, tail);
}

void AudiverisRunner::onFinished(int exitCode)
{
    if (m_cancelled)
    {
        return;
    }

    //! Drain anything still buffered before we judge the outcome.
    onReadyReadStandardOutput();
    onReadyReadStandardError();

    LOGI() << "[audiveris] exited with code " << exitCode;

    const QString tail = m_tail.join('\n');

    if (m_process)
    {
        m_process->deleteLater();
        m_process = nullptr;
    }

    if (exitCode != 0)
    {
        LOGE() << "Reprise: Audiveris exited with code " << exitCode;
        emit finished(false, QString(),
                      muse::qtrc("reprise", "Audiveris failed to recognise the scan (exit code %1).").arg(exitCode), tail);
        return;
    }

    const std::string base = QFileInfo(m_pdfPath.toQString()).completeBaseName().toStdString();

    io::path_t output = locateOutput(io::path_t(m_outDir), base, "mxl");
    if (output.empty())
    {
        output = locateOutput(io::path_t(m_outDir), base, "xml");
    }

    if (output.empty())
    {
        LOGE() << "Reprise: Audiveris produced no .mxl/.xml under " << m_outDir;
        emit finished(false, QString(), muse::qtrc("reprise", "Audiveris produced no MusicXML output."), tail);
        return;
    }

    LOGI() << "Reprise: Audiveris output located: " << output.toQString();
    emit finished(true, output.toQString(), QString(), tail);
}
