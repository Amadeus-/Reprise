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

#include "reprfile.h"

#include "global/serialization/json.h"
#include "global/serialization/zipreader.h"
#include "global/serialization/zipwriter.h"
#include "global/io/buffer.h"
#include "global/log.h"

using namespace muse;
using namespace muse::io;
using namespace mu::reprise;

namespace mu::project {
namespace {
//! Build the manifest "import" JsonObject back from a compact JSON string (empty / malformed -> an invalid, dropped obj).
JsonObject importObjectFromJson(const std::string& importJson)
{
    if (importJson.empty())
    {
        return JsonObject();
    }

    std::string err;
    JsonDocument doc = JsonDocument::fromJson(ByteArray(importJson.c_str(), importJson.size()), &err);
    if (!doc.isObject())
    {
        return JsonObject();
    }

    return doc.rootObject();
}

std::string byteArrayToStdString(const ByteArray& ba)
{
    if (ba.empty())
    {
        return std::string();
    }

    return std::string(ba.constChar(), ba.size());
}
}

ByteArray serializeProofingMap(const ProofingMap& map)
{
    JsonObject root;
    root.set("measureCount", map.measureCount);

    JsonArray pages;
    for (const ProofingPageInfo& p : map.pages)
    {
        JsonObject o;
        o.set("index", p.index);
        o.set("imageWidth", p.imageWidth);
        o.set("imageHeight", p.imageHeight);
        o.set("dpi", p.dpi);
        pages << o;
    }
    root.set("pages", pages);

    JsonArray measures;
    for (const ProofingMeasureRect& m : map.measures)
    {
        JsonObject o;
        o.set("gm", m.globalMeasureIndex);
        o.set("page", m.pageIndex);
        o.set("x", m.x);
        o.set("y", m.y);
        o.set("w", m.w);
        o.set("h", m.h);
        measures << o;
    }
    root.set("measures", measures);

    JsonArray notes;
    for (const ProofingNoteBox& b : map.notes)
    {
        JsonObject o;
        o.set("gm", b.key.globalMeasureIndex);
        o.set("staff", b.key.staffIdx);
        o.set("tick", b.key.tickInMeasure);
        o.set("step", b.key.step);
        o.set("octave", b.key.octave);
        o.set("voice", b.key.voice);
        o.set("page", b.pageIndex);
        o.set("x", b.x);
        o.set("y", b.y);
        o.set("w", b.w);
        o.set("h", b.h);
        notes << o;
    }
    root.set("notes", notes);

    return JsonDocument(root).toJson(JsonDocument::Format::Compact);
}

bool parseProofingMap(const ByteArray& bytes, ProofingMap& out)
{
    std::string err;
    JsonDocument doc = JsonDocument::fromJson(bytes, &err);
    if (!doc.isObject())
    {
        return false;
    }

    JsonObject root = doc.rootObject();

    out = ProofingMap();
    out.measureCount = root.value("measureCount").toInt();

    const JsonArray pages = root.value("pages").toArray();
    for (size_t i = 0; i < pages.size(); ++i)
    {
        const JsonObject o = pages.at(i).toObject();
        ProofingPageInfo p;
        p.index = o.value("index").toInt();
        p.imageWidth = o.value("imageWidth").toInt();
        p.imageHeight = o.value("imageHeight").toInt();
        p.dpi = o.value("dpi").toInt();
        out.pages.push_back(p);
    }

    const JsonArray measures = root.value("measures").toArray();
    for (size_t i = 0; i < measures.size(); ++i)
    {
        const JsonObject o = measures.at(i).toObject();
        ProofingMeasureRect m;
        m.globalMeasureIndex = o.value("gm").toInt();
        m.pageIndex = o.value("page").toInt();
        m.x = o.value("x").toInt();
        m.y = o.value("y").toInt();
        m.w = o.value("w").toInt();
        m.h = o.value("h").toInt();
        out.measures.push_back(m);
    }

    const JsonArray notes = root.value("notes").toArray();
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const JsonObject o = notes.at(i).toObject();
        ProofingNoteBox b;
        b.key.globalMeasureIndex = o.value("gm").toInt();
        b.key.staffIdx = o.value("staff").toInt();
        b.key.tickInMeasure = o.value("tick").toInt();
        b.key.step = o.value("step").toInt();
        b.key.octave = o.value("octave").toInt();
        b.key.voice = o.value("voice").toInt();
        b.pageIndex = o.value("page").toInt();
        b.x = o.value("x").toInt();
        b.y = o.value("y").toInt();
        b.w = o.value("w").toInt();
        b.h = o.value("h").toInt();
        out.notes.push_back(b);
    }

    //! measureCount is advisory; trust the actual measures vector so a hand-edited / partial map stays self-consistent.
    out.measureCount = static_cast<int32_t>(out.measures.size());
    out.valid = true;

    return true;
}

ByteArray serializeNavigatorState(const NavigatorStateMap& states)
{
    JsonObject root;

    JsonArray records;
    for (const auto& pair : states)
    {
        //! DEFAULT records (0.18.0: collapsed + active) carry no information -> skip (the model also prunes these, but guard
        //! here too). isDefaultNavigatorState centralizes the collapsed-default polarity.
        if (isDefaultNavigatorState(pair.second))
        {
            continue;
        }

        JsonObject o;
        o.set("key", pair.first);
        o.set("collapsed", pair.second.collapsed);
        o.set("ignored", pair.second.ignored);
        records << o;
    }
    root.set("records", records);

    return JsonDocument(root).toJson(JsonDocument::Format::Compact);
}

bool parseNavigatorState(const ByteArray& bytes, NavigatorStateMap& out)
{
    std::string err;
    JsonDocument doc = JsonDocument::fromJson(bytes, &err);
    if (!doc.isObject())
    {
        return false;
    }

    out.clear();

    //! Guard the optional "records" key: calling .toArray() on the null-type JsonValue that a MISSING muse-JSON key returns
    //! is a RELEASE-mode (NDEBUG) null-deref -- JsonArray::size() then reads [null+8] -> SIGSEGV. isArray() only reads the
    //! picojson type tag, so it is safe on a null value. A well-formed-but-recordless object is valid -> empty state, not an error.
    const JsonValue recordsVal = doc.rootObject().value("records");
    if (!recordsVal.isArray())
    {
        return true;
    }

    const JsonArray records = recordsVal.toArray();
    for (size_t i = 0; i < records.size(); ++i)
    {
        const JsonObject o = records.at(i).toObject();
        const std::string key = o.value("key").toStdString();
        if (key.empty())
        {
            continue;
        }

        reprise::NavigatorRecordState st;
        st.collapsed = o.value("collapsed").toBool();
        st.ignored = o.value("ignored").toBool();

        //! Keep the map minimal: only NON-default records are stored (0.18.0 default == collapsed + active). An older/foreign
        //! entry that happens to equal the default is dropped here. isDefaultNavigatorState centralizes the polarity.
        if (!isDefaultNavigatorState(st))
        {
            out[key] = st;
        }
    }

    return true;
}

ByteArray serializeViewState(const reprise::ProofingViewState& state)
{
    JsonObject root;
    root.set("followedMeasure", state.followedMeasure);
    root.set("bandZoom", state.bandZoom);
    root.set("magnifierZoom", state.magnifierZoom);

    return JsonDocument(root).toJson(JsonDocument::Format::Compact);
}

bool parseViewState(const ByteArray& bytes, reprise::ProofingViewState& out)
{
    std::string err;
    JsonDocument doc = JsonDocument::fromJson(bytes, &err);
    if (!doc.isObject())
    {
        return false;
    }

    const JsonObject root = doc.rootObject();

    //! Guard each optional key: reading a MISSING muse-JSON key then using the value is a RELEASE-mode hazard, so only
    //! overwrite the default when the key is actually present (a partial/older object keeps the defaults for absent fields).
    const JsonValue measureVal = root.value("followedMeasure");
    if (measureVal.isNumber())
    {
        out.followedMeasure = static_cast<int32_t>(measureVal.toInt());
    }

    const JsonValue bandVal = root.value("bandZoom");
    if (bandVal.isNumber())
    {
        out.bandZoom = bandVal.toDouble();
    }

    const JsonValue magVal = root.value("magnifierZoom");
    if (magVal.isNumber())
    {
        out.magnifierZoom = magVal.toDouble();
    }

    return true;
}

ByteArray assembleRepr(const ByteArray& scoreMscz, const ByteArray& original,
                       const std::string& originalFileName, const std::string& originalSuffix,
                       const std::string& importJson, const ProofingMap& map,
                       const ByteArray& proofingScan, const std::string& proofingScanSuffix,
                       const std::vector<double>& skewAnglesDeg,
                       const NavigatorStateMap& navigatorState,
                       const reprise::ProofingViewState& viewState)
{
    ByteArray out;
    Buffer buf(&out);
    buf.open(IODevice::WriteOnly);

    ZipWriter zw(&buf);

    //! manifest.json -- the version gate + import provenance, read first on open.
    JsonObject manifest;
    manifest.set("formatVersion", REPR_FORMAT_VERSION);
    manifest.set("app", std::string("Reprise"));

    JsonObject origObj;
    origObj.set("fileName", originalFileName);
    origObj.set("suffix", originalSuffix);
    manifest.set("original", origObj);

    const JsonObject importObj = importObjectFromJson(importJson);
    if (importObj.isValid() && !importObj.empty())
    {
        manifest.set("import", importObj);
    }

    //! proofingScan -- the deskewed render, recorded ONLY when a straightened copy was produced (proofingScan non-empty).
    //! Old builds ignore this unknown manifest object; when absent, the bundle is byte-identical to a pre-0.16.5 save.
    const std::string proofingScanSfx = proofingScanSuffix.empty() ? std::string("pdf") : proofingScanSuffix;
    if (!proofingScan.empty())
    {
        JsonObject psObj;
        psObj.set("suffix", proofingScanSfx);
        psObj.set("deskewed", true);
        if (!skewAnglesDeg.empty())
        {
            JsonArray angles;
            for (double a : skewAnglesDeg)
            {
                angles << a;
            }
            psObj.set("skewAnglesDeg", angles);
        }
        manifest.set("proofingScan", psObj);
    }

    zw.addFile(REPR_ENTRY_MANIFEST, JsonDocument(manifest).toJson(JsonDocument::Format::Indented));

    //! score.mscz -- a real standalone .mscz stored verbatim (ZIP-in-ZIP; already compressed, so deflate costs ~0).
    zw.addFile(REPR_ENTRY_SCORE, scoreMscz);

    //! original.<ext> -- the scan, byte-for-byte, extension preserved from the import source.
    const std::string origEntry = "original." + (originalSuffix.empty() ? std::string("dat") : originalSuffix);
    zw.addFile(origEntry, original);

    //! proofing-scan.<ext> -- the deskewed render, written ONLY when present (keeps a straight import byte-identical).
    if (!proofingScan.empty())
    {
        zw.addFile(REPR_ENTRY_PROOFINGSCAN + "." + proofingScanSfx, proofingScan);
    }

    //! proofingmap.json -- the scan<->score coordinate map (sourcePath omitted; re-derived on open).
    zw.addFile(REPR_ENTRY_PROOFINGMAP, serializeProofingMap(map));

    //! navigatorstate.json (0.18.0+) -- the bad-timing navigator per-record collapse/ignore state. Written ONLY when a
    //! non-default record exists, so a bundle the user never collapsed/ignored stays byte-identical to a pre-0.18.0 save;
    //! old builds ignore this unknown entry. serializeNavigatorState already drops default records, so count them here.
    bool anyNavState = false;
    for (const auto& pair : navigatorState)
    {
        if (!isDefaultNavigatorState(pair.second))
        {
            anyNavState = true;
            break;
        }
    }
    if (anyNavState)
    {
        zw.addFile(REPR_ENTRY_NAVIGATORSTATE, serializeNavigatorState(navigatorState));
    }

    //! viewstate.json (0.18.2+) -- the followed measure + band + magnifier per-document zoom. Written always so the view state
    //! round-trips; old builds ignore this unknown entry. A bundle never navigated/zoomed simply carries the defaults.
    zw.addFile(REPR_ENTRY_VIEWSTATE, serializeViewState(viewState));

    zw.close();
    buf.close();

    return out;
}

Ret readRepr(const path_t& path, ReprBundle& out)
{
    ZipReader zip(path);
    if (!zip.exists() || zip.hasError())
    {
        return Ret(static_cast<int>(Ret::Code::UnknownError), "Reprise: cannot open .repr bundle: " + path.toStdString());
    }

    const ByteArray manifestBytes = zip.fileData(REPR_ENTRY_MANIFEST);
    if (manifestBytes.empty())
    {
        return Ret(static_cast<int>(Ret::Code::UnknownError), "Reprise: .repr is missing manifest.json");
    }

    std::string err;
    const JsonDocument mdoc = JsonDocument::fromJson(manifestBytes, &err);
    const JsonObject manifest = mdoc.rootObject();

    out.formatVersion = manifest.value("formatVersion").toInt();
    if (out.formatVersion > REPR_FORMAT_VERSION)
    {
        return Ret(static_cast<int>(Ret::Code::UnknownError),
                   "This Reprise project was created by a newer version of Reprise and cannot be opened by this build.");
    }

    const JsonObject origObj = manifest.value("original").toObject();
    out.originalFileName = origObj.value("fileName").toStdString();
    out.originalSuffix = origObj.value("suffix").toStdString();

    if (manifest.contains("import"))
    {
        out.importJson = byteArrayToStdString(JsonDocument(manifest.value("import").toObject())
                                              .toJson(JsonDocument::Format::Compact));
    }

    out.scoreMscz = zip.fileData(REPR_ENTRY_SCORE);
    if (out.scoreMscz.empty())
    {
        return Ret(static_cast<int>(Ret::Code::UnknownError), "Reprise: .repr is missing or has an empty score.mscz");
    }

    //! Prefer the manifest-recorded original.<ext>; fall back to scanning for any "original.*" entry (so a bundle whose
    //! manifest lost the suffix still restores the scan), backfilling the suffix from the found entry name.
    if (!out.originalSuffix.empty())
    {
        out.original = zip.fileData("original." + out.originalSuffix);
    }
    if (out.original.empty())
    {
        for (const ZipReader::FileInfo& fi : zip.fileInfoList())
        {
            if (!fi.isFile)
            {
                continue;
            }

            const std::string name = fi.filePath.toStdString();
            if (name.rfind("original.", 0) == 0)
            {
                out.original = zip.fileData(name);
                out.originalSuffix = name.substr(std::string("original.").size());
                if (out.originalFileName.empty())
                {
                    out.originalFileName = name;
                }
                break;
            }
        }
    }

    //! Optional (0.16.5+) proofing-scan.<ext> -- the deskewed render. Read the manifest proofingScan object first (its
    //! suffix + the per-page skew angles), then extract the entry. Mirror the original's manifest-preferred-then-glob
    //! fallback so a bundle whose manifest lost the suffix still restores the render. Absence is tolerated (legacy bundles).
    if (manifest.contains("proofingScan"))
    {
        const JsonObject psObj = manifest.value("proofingScan").toObject();
        out.proofingScanSuffix = psObj.value("suffix").toStdString();

        //! skewAnglesDeg is OPTIONAL: assembleRepr emits it only when the angle vector is non-empty, so a deskewed bundle
        //! saved with no recorded angles has no such key. JsonObject::value() then returns a null JsonValue, and calling
        //! .toArray().size() on a non-array value dereferences a null picojson array pointer -> SIGSEGV. Guard with isArray()
        //! so a missing/non-array key is tolerated (leaves skewAnglesDeg empty), matching the "absence tolerated" contract.
        const JsonValue anglesVal = psObj.value("skewAnglesDeg");
        if (anglesVal.isArray())
        {
            const JsonArray angles = anglesVal.toArray();
            for (size_t i = 0; i < angles.size(); ++i)
            {
                out.skewAnglesDeg.push_back(angles.at(i).toDouble());
            }
        }
    }

    if (!out.proofingScanSuffix.empty())
    {
        out.proofingScan = zip.fileData(REPR_ENTRY_PROOFINGSCAN + "." + out.proofingScanSuffix);
    }
    if (out.proofingScan.empty())
    {
        const std::string prefix = REPR_ENTRY_PROOFINGSCAN + ".";
        for (const ZipReader::FileInfo& fi : zip.fileInfoList())
        {
            if (!fi.isFile)
            {
                continue;
            }

            const std::string name = fi.filePath.toStdString();
            if (name.rfind(prefix, 0) == 0)
            {
                out.proofingScan = zip.fileData(name);
                out.proofingScanSuffix = name.substr(prefix.size());
                break;
            }
        }
    }
    out.hasProofingScan = !out.proofingScan.empty();

    const ByteArray mapBytes = zip.fileData(REPR_ENTRY_PROOFINGMAP);
    if (!mapBytes.empty() && parseProofingMap(mapBytes, out.map))
    {
        out.hasMap = true;
    }

    //! Optional (0.18.0+) navigatorstate.json -- the bad-timing navigator collapse/ignore state. Absence tolerated (an
    //! older bundle, or one the user never collapsed/ignored) -> out.navigatorState stays empty, every record default.
    const ByteArray navBytes = zip.fileData(REPR_ENTRY_NAVIGATORSTATE);
    if (!navBytes.empty() && parseNavigatorState(navBytes, out.navigatorState))
    {
        out.hasNavigatorState = true;
    }

    //! Optional (0.18.2+) viewstate.json -- the followed measure + band/magnifier zoom. Absence tolerated (an older bundle, or
    //! one never navigated/zoomed) -> out.viewState stays at its defaults (followedMeasure -1 / band 0.5 / magnifier 1.0).
    const ByteArray viewBytes = zip.fileData(REPR_ENTRY_VIEWSTATE);
    if (!viewBytes.empty() && parseViewState(viewBytes, out.viewState))
    {
        out.hasViewState = true;
    }

    return muse::make_ok();
}

Ret readReprOriginal(const path_t& path, ByteArray& out, std::string& outSuffix, std::string& outFileName)
{
    out = ByteArray();
    outSuffix.clear();
    outFileName.clear();

    ZipReader zip(path);
    if (!zip.exists() || zip.hasError())
    {
        return Ret(static_cast<int>(Ret::Code::UnknownError),
                   "Reprise: cannot open .repr bundle for original extraction: " + path.toStdString());
    }

    //! Manifest first for the recorded original suffix + file name (the same source of truth readRepr uses).
    const ByteArray manifestBytes = zip.fileData(REPR_ENTRY_MANIFEST);
    if (!manifestBytes.empty())
    {
        std::string err;
        const JsonDocument mdoc = JsonDocument::fromJson(manifestBytes, &err);
        const JsonObject origObj = mdoc.rootObject().value("original").toObject();
        outFileName = origObj.value("fileName").toStdString();
        outSuffix = origObj.value("suffix").toStdString();
    }

    //! Prefer the manifest-recorded original.<ext>; fall back to scanning for any "original.*" entry (mirrors readRepr) so a
    //! bundle whose manifest lost the suffix still yields the scan, backfilling the suffix/name from the found entry.
    if (!outSuffix.empty())
    {
        out = zip.fileData("original." + outSuffix);
    }
    if (out.empty())
    {
        for (const ZipReader::FileInfo& fi : zip.fileInfoList())
        {
            if (!fi.isFile)
            {
                continue;
            }

            const std::string name = fi.filePath.toStdString();
            if (name.rfind("original.", 0) == 0)
            {
                out = zip.fileData(name);
                outSuffix = name.substr(std::string("original.").size());
                if (outFileName.empty())
                {
                    outFileName = name;
                }
                break;
            }
        }
    }

    return muse::make_ok();
}
}
