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

#include "scangeometrystripper.h"

#include <memory>
#include <vector>
#include <utility>

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "global/log.h"
#include "global/io/path.h"
#include "global/types/bytearray.h"
#include "global/serialization/zipreader.h"

using namespace mu::reprise;

namespace {
//! Lightweight in-memory MusicXML node. A pull-parser (QXmlStreamReader) cannot look ahead -- but this transform must
//! inject <work> at the TOP of the document after seeing <credit>s near the BOTTOM, de-duplicate the title credits, and
//! drop a <print>/<defaults> only AFTER discovering it is empty once its geometry children are removed. So we parse into
//! this tiny tree, mutate it, then re-serialize with QXmlStreamWriter. Text nodes are kept VERBATIM (whitespace
//! included) so every musical and textual value survives byte-for-byte and the output keeps the source's pretty-printing.
struct XmlNode
{
    enum Kind { Element, Text };

    Kind kind = Element;
    QString name;                                       // element name (Kind == Element)
    QString text;                                       // character data (Kind == Text)
    std::vector<std::pair<QString, QString> > attrs;    // ordered attribute (name, value) pairs
    std::vector<std::unique_ptr<XmlNode> > kids;
};

//! Position attributes stripped from EVERY element EXCEPT credits: absolute/relative scan coordinates. (Credits keep
//! their position so MuseScore's title-frame placement/alignment logic -- which buckets credits into the page header by
//! default-y and aligns them by default-x -- still works; see the credit handling in stripBytes.)
static bool isPositionAttr(const QString& a)
{
    return a == "default-x" || a == "default-y" || a == "relative-x" || a == "relative-y";
}

//! Slur-curve control points: scan-fitted bezier handles. MuseScore re-shapes slurs from the notes, so these only
//! pin the curve to the scan's geometry.
static bool isBezierAttr(const QString& a)
{
    return a == "bezier-x" || a == "bezier-y" || a == "bezier-x2" || a == "bezier-y2";
}

//! Pure-geometry layout blocks removed wherever they appear (under <defaults> and under <print>): the scan's page,
//! system and staff geometry plus the global scaling / appearance tuning. Their tag names never name musical content,
//! so removing them by name anywhere is safe.
static bool isGeometryBlock(const QString& name)
{
    return name == "page-layout" || name == "system-layout" || name == "staff-layout"
           || name == "scaling" || name == "appearance";
}

//! Break attributes on <print> that force the scan's system/page breaks (and the scan page number).
static bool isPrintBreakAttr(const QString& a)
{
    return a == "new-system" || a == "new-page" || a == "blank-page" || a == "page-number";
}

static bool hasElementChild(const XmlNode* n)
{
    for (const std::unique_ptr<XmlNode>& k : n->kids)
    {
        if (k->kind == XmlNode::Element)
        {
            return true;
        }
    }

    return false;
}

//! Strip geometry recursively: remove position + bezier attributes on every element, the <measure width> attribute,
//! the <print> break attributes, and the whole geometry-block subtrees. A <print> left with no attributes AND no
//! element children (e.g. one that carried only system-layout + staff-layout) is dropped; a <print> still carrying a
//! non-geometry child (e.g. <measure-numbering>) is kept, reduced to that. A <defaults> left with no element children
//! (its scaling + page-layout removed) is dropped; one still carrying fonts (e.g. <lyric-font>) is kept. CREDIT
//! subtrees are left UNTOUCHED -- they are rebuilt separately (with their position intact) by the credit handling.
static void stripGeometry(XmlNode* n)
{
    std::vector<std::pair<QString, QString> > keptAttrs;
    for (std::pair<QString, QString>& a : n->attrs)
    {
        if (isPositionAttr(a.first) || isBezierAttr(a.first))
        {
            continue;
        }

        if (n->name == "measure" && a.first == "width")
        {
            continue;
        }

        if (n->name == "print" && isPrintBreakAttr(a.first))
        {
            continue;
        }

        keptAttrs.push_back(std::move(a));
    }
    n->attrs = std::move(keptAttrs);

    std::vector<std::unique_ptr<XmlNode> > keptKids;
    for (std::unique_ptr<XmlNode>& k : n->kids)
    {
        if (k->kind == XmlNode::Element)
        {
            if (isGeometryBlock(k->name))
            {
                continue;
            }

            //! Leave credit subtrees exactly as the credit handling rebuilt them (position preserved).
            if (k->name == "credit")
            {
                keptKids.push_back(std::move(k));
                continue;
            }

            stripGeometry(k.get());

            if (k->name == "print" && k->attrs.empty() && !hasElementChild(k.get()))
            {
                continue;
            }

            if (k->name == "defaults" && !hasElementChild(k.get()))
            {
                continue;
            }
        }

        keptKids.push_back(std::move(k));
    }
    n->kids = std::move(keptKids);
}

//! Concatenate every descendant text node into one string -- the visible text of an element.
static QString innerText(const XmlNode* n)
{
    QString out;
    for (const std::unique_ptr<XmlNode>& k : n->kids)
    {
        if (k->kind == XmlNode::Text)
        {
            out += k->text;
        }
        else
        {
            out += innerText(k.get());
        }
    }

    return out;
}

static bool hasLetter(const QString& s)
{
    for (const QChar& c : s)
    {
        if (c.isLetter())
        {
            return true;
        }
    }

    return false;
}

//! One scan <credit>, reduced to what the title-frame decision needs: its page, its combined credit-words text, the
//! largest credit-words font size, its first credit-words position (raw string + numeric), and any explicit type.
struct CreditData
{
    QString page;
    QString text;
    double fontSize = 0.0;
    double defaultX = 0.0;
    QString rawX;
    QString rawY;
    QString explicitType;
};

//! One merged title-frame slot: all same-slot scan credits folded into a single multi-line text, with the first
//! credit's page + position kept so MuseScore places and aligns it the way it placed the originals.
struct CreditSlot
{
    QString name;
    QString page;
    QString rawX;
    QString rawY;
    QString mergedText;
};

//! Gather one credit's text, largest font size, first position, and explicit credit-type.
static void collectCredit(const XmlNode* n, CreditData& out, bool& haveX)
{
    for (const std::unique_ptr<XmlNode>& k : n->kids)
    {
        if (k->kind != XmlNode::Element)
        {
            continue;
        }

        if (k->name == "credit-type")
        {
            if (out.explicitType.isEmpty())
            {
                out.explicitType = innerText(k.get()).trimmed();
            }
        }
        else if (k->name == "credit-words")
        {
            const QString t = innerText(k.get()).trimmed();
            if (!t.isEmpty())
            {
                if (!out.text.isEmpty())
                {
                    out.text += " ";
                }

                out.text += t;
            }

            for (const std::pair<QString, QString>& a : k->attrs)
            {
                if (a.first == "font-size")
                {
                    bool ok = false;
                    const double fs = a.second.toDouble(&ok);
                    if (ok && fs > out.fontSize)
                    {
                        out.fontSize = fs;
                    }
                }
                else if (a.first == "default-x" && !haveX)
                {
                    out.rawX = a.second;
                    out.defaultX = a.second.toDouble();
                    haveX = true;
                }
                else if (a.first == "default-y" && out.rawY.isEmpty())
                {
                    out.rawY = a.second;
                }
            }
        }
        else
        {
            collectCredit(k.get(), out, haveX);
        }
    }

    return;
}

//! Classify a credit into a title-frame slot, mirroring MuseScore's own creditWordTypeGuess (importmusicxmlpass1):
//! an explicit credit-type wins; otherwise the right column is the composer, the left column the lyricist, and the
//! middle column is the title (largest font) or a subtitle (anything smaller in the middle). This lets the frame be
//! built deterministically from credit-type, independent of exact position.
static QString classifyCredit(const CreditData& c, const std::vector<CreditData>& all, double pageWidth)
{
    if (c.explicitType == "title" || c.explicitType == "subtitle"
        || c.explicitType == "composer" || c.explicitType == "lyricist")
    {
        return c.explicitType;
    }

    const double pw1 = pageWidth / 3.0;
    const double pw2 = pageWidth * 2.0 / 3.0;
    if (c.defaultX > pw2)
    {
        return QStringLiteral("composer");
    }

    if (c.defaultX < pw1)
    {
        return QStringLiteral("lyricist");
    }

    for (const CreditData& o : all)
    {
        if (&o == &c)
        {
            continue;
        }

        if (o.defaultX < pw1 || o.defaultX > pw2)
        {
            continue;
        }

        if (c.fontSize < o.fontSize)
        {
            return QStringLiteral("subtitle");
        }
    }

    return QStringLiteral("title");
}

static std::unique_ptr<XmlNode> makeElement(const QString& name)
{
    auto n = std::make_unique<XmlNode>();
    n->kind = XmlNode::Element;
    n->name = name;

    return n;
}

static std::unique_ptr<XmlNode> makeText(const QString& text)
{
    auto n = std::make_unique<XmlNode>();
    n->kind = XmlNode::Text;
    n->text = text;

    return n;
}

//! Build one clean, de-duplicated title-frame credit: <credit page="P"><credit-type>SLOT</credit-type>
//! <credit-words default-x="X" default-y="Y">merged text</credit-words></credit>. The merged text may carry embedded
//! newlines (several same-slot scan credits folded into one multi-line text), so each style slot holds exactly one
//! text and MuseScore never stacks two texts at the same frame position (the cause of the doubled/overlapping title).
static std::unique_ptr<XmlNode> makeCredit(const QString& page, const QString& slot,
                                           const QString& rawX, const QString& rawY, const QString& mergedText)
{
    std::unique_ptr<XmlNode> credit = makeElement("credit");
    if (!page.isEmpty())
    {
        credit->attrs.emplace_back(QStringLiteral("page"), page);
    }

    std::unique_ptr<XmlNode> type = makeElement("credit-type");
    type->kids.push_back(makeText(slot));

    std::unique_ptr<XmlNode> words = makeElement("credit-words");
    if (!rawX.isEmpty())
    {
        words->attrs.emplace_back(QStringLiteral("default-x"), rawX);
    }
    if (!rawY.isEmpty())
    {
        words->attrs.emplace_back(QStringLiteral("default-y"), rawY);
    }
    words->kids.push_back(makeText(mergedText));

    credit->kids.push_back(makeText("\n    "));
    credit->kids.push_back(std::move(type));
    credit->kids.push_back(makeText("\n    "));
    credit->kids.push_back(std::move(words));
    credit->kids.push_back(makeText("\n  "));

    return credit;
}

//! Build <work><work-title>title</work-title></work> for the file's Title property (the frame itself is built from the
//! title credit; this keeps the score metadata correct too). Placed as the first child of score-partwise.
static std::unique_ptr<XmlNode> makeWork(const QString& title)
{
    std::unique_ptr<XmlNode> work = makeElement("work");
    std::unique_ptr<XmlNode> workTitle = makeElement("work-title");
    workTitle->kids.push_back(makeText(title));

    work->kids.push_back(makeText("\n    "));
    work->kids.push_back(std::move(workTitle));
    work->kids.push_back(makeText("\n  "));

    return work;
}

//! Serialize a node (and its subtree) through QXmlStreamWriter. Text is written with writeCharacters (which re-escapes
//! &, <, >), attributes with writeAttribute (which escapes too), so the output is always well-formed.
static void writeNode(QXmlStreamWriter& w, const XmlNode* n)
{
    if (n->kind == XmlNode::Text)
    {
        w.writeCharacters(n->text);
        return;
    }

    w.writeStartElement(n->name);
    for (const std::pair<QString, QString>& a : n->attrs)
    {
        w.writeAttribute(a.first, a.second);
    }

    for (const std::unique_ptr<XmlNode>& k : n->kids)
    {
        writeNode(w, k.get());
    }

    w.writeEndElement();

    return;
}
}

QByteArray ScanGeometryStripper::stripBytes(const QByteArray& xml)
{
    if (xml.isEmpty())
    {
        return QByteArray();
    }

    //! Parse the whole document into the node tree, preserving text verbatim and capturing the DOCTYPE so it can be
    //! written back out unchanged.
    auto document = std::make_unique<XmlNode>();
    document->kind = XmlNode::Element;
    document->name = "#document";

    QString doctype;
    std::vector<XmlNode*> stack;
    stack.push_back(document.get());

    QXmlStreamReader reader(xml);
    while (!reader.atEnd())
    {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::StartElement)
        {
            auto node = std::make_unique<XmlNode>();
            node->kind = XmlNode::Element;
            node->name = reader.name().toString();
            for (const QXmlStreamAttribute& a : reader.attributes())
            {
                node->attrs.emplace_back(a.qualifiedName().toString(), a.value().toString());
            }

            XmlNode* raw = node.get();
            stack.back()->kids.push_back(std::move(node));
            stack.push_back(raw);
        }
        else if (token == QXmlStreamReader::EndElement)
        {
            if (stack.size() > 1)
            {
                stack.pop_back();
            }
        }
        else if (token == QXmlStreamReader::Characters)
        {
            stack.back()->kids.push_back(makeText(reader.text().toString()));
        }
        else if (token == QXmlStreamReader::DTD)
        {
            doctype = reader.text().toString();
        }
    }

    if (reader.hasError())
    {
        LOGW() << "Reprise scan-geometry strip: XML parse error: " << reader.errorString();
        return QByteArray();
    }

    //! Locate the score root (partwise; timewise handled the same way).
    XmlNode* scoreRoot = nullptr;
    for (const std::unique_ptr<XmlNode>& k : document->kids)
    {
        if (k->kind == XmlNode::Element && (k->name == "score-partwise" || k->name == "score-timewise"))
        {
            scoreRoot = k.get();
            break;
        }
    }

    if (!scoreRoot)
    {
        LOGW() << "Reprise scan-geometry strip: no <score-partwise> root found";
        return QByteArray();
    }

    //! The page width the scan declared (defaults/page-layout/page-width), used only to classify credits by column
    //! (title/subtitle in the middle, composer at right, lyricist at left) the same way MuseScore does. Read BEFORE the
    //! geometry strip removes page-layout. A sensible default covers a file that declares no page width.
    double pageWidth = 1536.0;
    for (const std::unique_ptr<XmlNode>& k : scoreRoot->kids)
    {
        if (k->kind != XmlNode::Element || k->name != "defaults")
        {
            continue;
        }

        for (const std::unique_ptr<XmlNode>& dk : k->kids)
        {
            if (dk->kind == XmlNode::Element && dk->name == "page-layout")
            {
                for (const std::unique_ptr<XmlNode>& pk : dk->kids)
                {
                    if (pk->kind == XmlNode::Element && pk->name == "page-width")
                    {
                        bool ok = false;
                        const double pw = innerText(pk.get()).trimmed().toDouble(&ok);
                        if (ok && pw > 0.0)
                        {
                            pageWidth = pw;
                        }
                    }
                }
            }
        }
    }

    //! Does the file already carry a work-title? If not, we add one from the promoted title (for the score's Title
    //! property). MuseScore builds the on-page title FRAME from the credits below, so this never double-prints.
    bool hasWorkTitle = false;
    for (const std::unique_ptr<XmlNode>& k : scoreRoot->kids)
    {
        if (k->kind == XmlNode::Element && k->name == "work")
        {
            for (const std::unique_ptr<XmlNode>& wk : k->kids)
            {
                if (wk->kind == XmlNode::Element && wk->name == "work-title"
                    && !innerText(wk.get()).trimmed().isEmpty())
                {
                    hasWorkTitle = true;
                }
            }
        }
    }

    //! Collect every scan credit (in document order), dropping scan artifacts (a lone quote, "*", a page number -- any
    //! text with no letter) and the organ "Great"-manual "Gt" registration marks (behind the toggle).
    std::vector<CreditData> kept;
    bool anyCredit = false;
    for (const std::unique_ptr<XmlNode>& k : scoreRoot->kids)
    {
        if (k->kind != XmlNode::Element || k->name != "credit")
        {
            continue;
        }

        anyCredit = true;

        CreditData data;
        for (const std::pair<QString, QString>& a : k->attrs)
        {
            if (a.first == "page")
            {
                data.page = a.second;
            }
        }

        bool haveX = false;
        collectCredit(k.get(), data, haveX);

        const QString t = data.text.trimmed();
        if (t.isEmpty() || !hasLetter(t))
        {
            continue;                                   // scan artifact
        }

        if (t == "Gt" && kStripOrganManualCredits)
        {
            continue;                                   // organ "Great"-manual registration mark
        }

        data.text = t;
        kept.push_back(std::move(data));
    }

    //! Classify each kept credit into a title-frame slot, then merge all credits in the same slot into one multi-line
    //! credit (document order). This yields at most one TITLE / SUBTITLE / COMPOSER / LYRICIST text, so MuseScore never
    //! overprints two texts in the same slot -- the fix for the doubled/overlapping title -- while every real title,
    //! subtitle, composer and arranger line is preserved.
    static const char* const kSlotOrder[] = { "title", "subtitle", "composer", "lyricist" };
    std::vector<CreditSlot> creditSlots;
    for (const char* const slotName : kSlotOrder)
    {
        CreditSlot slot;
        slot.name = QString::fromLatin1(slotName);
        bool found = false;
        for (const CreditData& c : kept)
        {
            if (classifyCredit(c, kept, pageWidth) != slot.name)
            {
                continue;
            }

            if (!found)
            {
                slot.page = c.page;
                slot.rawX = c.rawX;
                slot.rawY = c.rawY;
                slot.mergedText = c.text;
                found = true;
            }
            else if (slot.mergedText != c.text && !slot.mergedText.contains(c.text))
            {
                slot.mergedText += "\n";
                slot.mergedText += c.text;              // same slot, different line -> stack within one text
            }
        }

        if (found)
        {
            creditSlots.push_back(std::move(slot));
        }
    }

    //! The promoted title text (for the optional work-title). Empty if no title slot was found.
    QString promotedTitle;
    for (const CreditSlot& s : creditSlots)
    {
        if (s.name == "title")
        {
            promotedTitle = s.mergedText;
        }
    }

    //! Rebuild the score-root children: drop every old credit, insert the merged credits where the first credit was,
    //! and inject <work> at the very front when the file had no work-title.
    if (anyCredit || (!hasWorkTitle && !promotedTitle.isEmpty()))
    {
        std::vector<std::unique_ptr<XmlNode> > rebuilt;

        if (!hasWorkTitle && !promotedTitle.isEmpty())
        {
            rebuilt.push_back(makeText("\n  "));
            rebuilt.push_back(makeWork(promotedTitle));
        }

        bool insertedCredits = false;
        for (std::unique_ptr<XmlNode>& k : scoreRoot->kids)
        {
            if (k->kind == XmlNode::Element && k->name == "credit")
            {
                if (!insertedCredits)
                {
                    insertedCredits = true;
                    for (const CreditSlot& s : creditSlots)
                    {
                        rebuilt.push_back(makeText("\n  "));
                        rebuilt.push_back(makeCredit(s.page, s.name, s.rawX, s.rawY, s.mergedText));
                    }
                }

                continue;                               // drop the original credit
            }

            rebuilt.push_back(std::move(k));
        }

        scoreRoot->kids = std::move(rebuilt);
    }

    //! Now strip the scan geometry (position/bezier attrs, measure width, print breaks, layout blocks, empty
    //! print/defaults). Credit subtrees are left untouched -- they were rebuilt above with their position intact.
    stripGeometry(scoreRoot);

    //! Serialize. The DOCTYPE (if any) is written back verbatim; the body keeps the source whitespace, so the result
    //! reads just like the original minus the stripped geometry.
    QByteArray out;
    QXmlStreamWriter writer(&out);
    writer.setAutoFormatting(false);
    writer.writeStartDocument("1.0");
    if (!doctype.isEmpty())
    {
        writer.writeDTD(doctype);
    }

    writeNode(writer, scoreRoot);
    writer.writeEndDocument();

    if (writer.hasError() || out.isEmpty())
    {
        LOGW() << "Reprise scan-geometry strip: XML serialize failed";
        return QByteArray();
    }

    return out;
}

QString ScanGeometryStripper::stripToSibling(const QString& mxlOrXmlPath)
{
    //! Read the raw MusicXML: a .mxl is a ZIP whose real MusicXML is the rootfile named in META-INF/container.xml
    //! (the same extraction MuseScore's own importer uses); a plain .xml/.musicxml is read directly.
    QByteArray xml;

    const muse::io::path_t path(mxlOrXmlPath);
    const std::string suffix = muse::io::suffix(path);
    if (suffix == "mxl")
    {
        muse::ZipReader zip(path);
        if (!zip.exists() || zip.hasError())
        {
            LOGW() << "Reprise scan-geometry strip: cannot open .mxl: " << mxlOrXmlPath;
            return QString();
        }

        const muse::ByteArray container = zip.fileData("META-INF/container.xml");
        std::string rootfile;
        QXmlStreamReader e(container.toQByteArrayNoCopy());
        while (e.readNextStartElement())
        {
            if (e.name() != QLatin1String("container"))
            {
                break;
            }

            while (e.readNextStartElement())
            {
                if (e.name() != QLatin1String("rootfiles"))
                {
                    break;
                }

                while (e.readNextStartElement())
                {
                    if (e.name() == QLatin1String("rootfile"))
                    {
                        rootfile = e.attributes().value("full-path").toString().toStdString();
                        break;
                    }
                    else
                    {
                        e.skipCurrentElement();
                    }
                }
            }
        }

        if (rootfile.empty())
        {
            LOGW() << "Reprise scan-geometry strip: no rootfile in .mxl container: " << mxlOrXmlPath;
            return QString();
        }

        xml = zip.fileData(rootfile).toQByteArray();
    }
    else
    {
        QFile file(mxlOrXmlPath);
        if (!file.open(QIODevice::ReadOnly))
        {
            LOGW() << "Reprise scan-geometry strip: cannot read: " << mxlOrXmlPath;
            return QString();
        }

        xml = file.readAll();
        file.close();
    }

    const QByteArray stripped = stripBytes(xml);
    if (stripped.isEmpty())
    {
        return QString();
    }

    //! Write the cleaned MusicXML to a sibling plain .musicxml next to the input (the Audiveris output lives in a temp
    //! working folder, so this stays in temp and never touches the repo). Opening a plain .musicxml is fully supported.
    const QFileInfo fileInfo(mxlOrXmlPath);
    const QString outPath = fileInfo.absolutePath() + "/" + fileInfo.completeBaseName() + ".reprise-stripped.musicxml";

    QFile outFile(outPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        LOGW() << "Reprise scan-geometry strip: cannot write sibling: " << outPath;
        return QString();
    }

    const qint64 written = outFile.write(stripped);
    outFile.close();
    if (written != static_cast<qint64>(stripped.size()))
    {
        LOGW() << "Reprise scan-geometry strip: short write to sibling: " << outPath;
        return QString();
    }

    return outPath;
}
