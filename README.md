<p align="center"><img src="share/icons/reprise_readme_banner.png" alt="Reprise" width="760"></p>

**From scan to score.** Open a PDF or image of sheet music, recognize it (OMR), correct it, and export.

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0.en.html)

Reprise is a focused music-scanning and correction tool built on
[MuseScore Studio](https://github.com/musescore/MuseScore). It takes a scanned PDF
or image of sheet music, runs optical music recognition (OMR) with
[Audiveris](https://github.com/Audiveris/audiveris), and gives you a streamlined
workflow to correct the result and export clean **MusicXML** — or hand the score
onward to MuseScore to finish.

> **Pre-release (0.18.2) — work in progress.** Reprise is early software under
> active development. Expect rough edges and changing behavior.

See [`CHANGELOG.md`](CHANGELOG.md) for the version history.

## Relationship to MuseScore

Reprise is a **fork of MuseScore Studio** and is built directly on MuseScore's
notation, engraving, and playback engine — the editing you do in Reprise is
MuseScore-native. All of MuseScore's copyright notices, author credits, and
license terms are preserved in this repository (see
[`LICENSE.txt`](LICENSE.txt) and the per-file source headers). Nothing in
MuseScore's attribution has been removed.

- Upstream project: https://github.com/musescore/MuseScore

Reprise is an **independent fork** and is **not affiliated with, sponsored by, or
endorsed by** MuseScore or Muse Group. *"MuseScore"* is a trademark of its
respective owners; Reprise uses the name only to describe its origin and
compatibility.

## Features

**Working now**
- **Import a PDF or image** — *File ▸ Import Scan* (or the **New** tile) runs Audiveris
  OMR on a PDF or a scan image (PNG/JPG/JPEG/TIFF/BMP/GIF) and opens the recognized score.
  Pick a page range (`all`, `1-5`, `1,3,8`) and a **Recognition quality** — *Faster /
  Standard / Best*. Reprise reads the PDF's own resolution and pre-selects the matching
  quality for you. Each recognized score is **re-engraved with MuseScore's own layout** —
  the scanner's baked-in page geometry is stripped so measures are evenly spaced (not
  stretched to mirror the paper) and the title block is clean and un-duplicated.
- **Unreadable pages don't sink the whole import** — if some pages of a multi-page scan can't be recognized
  (a cover page with no music, or a page whose resolution is too low), Reprise imports the rest of your music and
  shows an **Import finished** summary listing exactly which pages were skipped and why.
- **Tilted scans are straightened automatically** — Reprise measures each page's rotation and straightens a skewed
  page before recognition, which sharply improves results on crooked scans; already-straight pages are left
  untouched. Your original scan is preserved alongside the straightened one inside your Reprise project (`.repr`).
- **Text & lyric recognition** — titles, lyrics, and tempo markings are read automatically
  during import. The **Tesseract OCR engine is bundled inside Audiveris** — not a separate
  program — and Reprise ships the training data it needs and points it there for you, so
  recognition works out of the box with no extra setup.
- **Recognition options** — an **Options** panel on the Import Scan dialog lets you tune the
  recognition per import: toggle text/lyric OCR on or off, pick the OCR **language**, and opt in
  to **chord-symbol** detection or **lyrics-above-the-staff** scores. Sensible defaults mean you
  can ignore it entirely and still get the full text and lyric recognition.
- **Live recognition progress** — a movable progress window shows each page as Audiveris
  works, with a recognition log and a cancel option, and you can keep using the rest of
  the app while recognition runs.
- **Streamlined, correction-focused UI** — the interface is pared down around
  scanning and correcting: a **Scans** home, a dedicated **Edit** workspace, and
  menus trimmed to what the workflow needs.
- **Manage your recent scans** — the Scans home lists your recent imports labeled with their title, composer, and
  the pages you scanned; clear the whole list or remove individual entries (your files are never deleted —
  only the shortcuts to them are forgotten).
- **See the original alongside your score** — after importing, the original scanned page
  appears in a band above the editable score and smoothly follows along as you move through
  it, highlighting the measure you are on. A floating **detail view** panel shows a magnified
  close-up of the exact note you're on, so you can check each recognized note against the
  original at a readable size. You can zoom either panel in or out with the mouse wheel. Your work saves as a **Reprise project (`.repr`)** that reopens
  with the original scan and this whole proofing view intact.
- **Spot timing problems at a glance** — when the recognized rhythm doesn't add up — a measure that's the wrong
  length for its time signature (a common recognition mis-read), a voice too long for its bar, or an incomplete /
  corrupted measure — a **Timing problems** list appears in the detail-view panel, labelling each problem measure in
  plain language ("Too many beats" / "Too few beats", with the exact counts on hover); click a row to jump to and
  select that measure, which is outlined in red in the editor. Each problem is a collapsible record with one-click **Fix** (extend a short measure to its time signature) and **Ignore** actions, and your collapse/ignore choices are saved with the project. On a clean score the list simply doesn't appear. Because scanned music routinely has irregular measures, Reprise no longer interrupts a scan
  import or project reopen with a generic "this score is corrupted" dialog — those issues show up here instead.
- **Easy setup** — Preferences auto-detects your Audiveris install on first run, with
  manual override. There is no separate Tesseract to install or configure — its OCR engine
  ships **inside Audiveris**.
- **Manage recognition languages** — a **Text recognition** section in Preferences lets you set
  the default OCR language and add, update, or remove language packs. Reprise ships and manages the
  Tesseract training data itself (the OCR engine is bundled inside Audiveris); English works
  offline out of the box, and extra languages download on demand when you ask for them.
- **Fully offline** — no account, sign-in, or network connection required; the
  online/cloud module is compiled out entirely, and every save goes straight to a
  local file (no cloud/computer save-location chooser).
- **Recognized instruments are kept** — the instruments Audiveris identifies during recognition, and their
  transposition, are preserved rather than forced to piano, and you can reassign any part in the **Layout** panel.
  If the recognition looks like a guess, the import summary points you to the Layout panel to set them.
- **Built on MuseScore's editor** — full score editing, **MusicXML / MIDI import and
  export**, and **playback**.

**Planned**
- Registering the `.repr` file type with Windows at install time — double-click a `.repr`
  to open it in Reprise, with its own icon in Explorer.
- A correction-focused keypad for fast fixes, and richer navigation of flagged measures.
- Richer import/OMR options.

## Known limitations

- **Multi-measure rests show the wrong count.** On scanned parts, the recognition reads the multi-measure-rest
  symbol but not the number printed above it, so each one collapses to a single bar — which makes the total bar
  count (and every bar number after it) shift, so an imported part may not line up with a conductor score or
  rehearsal numbers. This is a limitation of the recognition engine (Audiveris); a fix is already in its
  development branch and will be picked up in a future Reprise update.
- **The time signature may not be recognized on dense or multi-staff scores.** On complex layouts — an organ score
  with several staves per system is a known case — the recognition engine (Audiveris) can fail to read the printed
  time signature, and the score then falls back to a default meter. When that happens, many measures can be flagged
  in the **Timing problems** panel even though the music itself is fine; setting the correct time signature in the
  score resolves both the meter and those flags. This is a limitation of the recognition engine.

## License

Reprise is licensed under **GPL-3.0-only** — the same license as MuseScore Studio.
See [`LICENSE.txt`](LICENSE.txt). MuseScore's original license and attribution are
retained in full.

## Requirements

- **[Audiveris](https://github.com/Audiveris/audiveris)** (installed separately) —
  performs the PDF optical music recognition. Reprise auto-detects it at its
  default install location.
- **Tesseract OCR** — **bundled inside Audiveris**, *not* a separate install. The OCR
  engine ships as part of Audiveris, and Reprise provides the training data it needs, so
  text and lyric recognition works out of the box.

## Building & running

> **Status: WIP / TBD.** Full, platform-general build instructions are still being
> written. Reprise uses the standard MuseScore build system (Qt 6, a C++ toolchain,
> and the `muse` + `muse_deps` git submodules).

The repository includes fork-local Windows helper scripts for a one-click flow:

- `+Build (release)+.bat` — configure and build
- `+Deploy (standalone)+.bat` — make the built executable double-clickable
- `+Run (dev)+.bat` — launch the development build
- `+Update (from upstream)+.bat` — sync from upstream MuseScore

For the underlying build system and dependencies, see MuseScore's
[developer setup guide](https://github.com/musescore/MuseScore/wiki/Set-up-developer-environment).

## Credits

- **[MuseScore Studio](https://github.com/musescore/MuseScore)** — the notation,
  engraving, and playback engine Reprise is built on.
- **[Audiveris](https://github.com/Audiveris/audiveris)** — optical music
  recognition.
- **[Tesseract](https://github.com/tesseract-ocr/tesseract)** — OCR for text and
  lyrics (bundled within Audiveris).

---

*Reprise is built on MuseScore Studio (GPLv3). "MuseScore" © its authors — see
[`LICENSE.txt`](LICENSE.txt).*
