# Changelog

All notable changes to Reprise are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and Reprise aims to follow [Semantic Versioning](https://semver.org/). Everything
below `1.0.0` is pre-release.

## [Unreleased]

## [0.12.14] — 2026-11-19

### Changed
- **The last-used "New Scan" import folder is now permanently protected from being reset by future app-identity changes.**
  This is hardening, not a fix for an active bug: at the current version the remembered import folder already persists
  across launches *and* across version upgrades, because Reprise stores it in the normal user-settings file whose location
  is keyed by the application name — and that name is now the stable, version-independent constant `RepriseDevelopment`.
  Earlier in the fork's life the application identity changed several times (MuseScore Studio 5 → Reprise 5 → Reprise), and
  each change moved the settings file and so appeared to "reset" the remembered folder on a new build. That churn has
  settled, so the folder already sticks today; this release simply *pins* the settings identity to that constant (mirroring
  upstream's own fix for the same class of problem) so that a future rebrand, version bump, or other identity change can
  never again orphan a user's settings. The pinned name equals the name in use today, so no existing settings are moved or
  lost. The remembered folder continues to be written with the same cross-instance-synced setter as before.
- **The "New Scan" dialog's first-run / empty fallback folder is documented as the platform's top-level Documents folder.**
  When no import folder has been remembered yet (or the remembered one no longer exists), the dialog opens at the operating
  system's Documents folder — Windows `C:\Users\<you>\Documents`, macOS `~/Documents`, Linux `~/Documents` — resolved
  per-OS, never the program's working directory. No behavior change; the cross-platform intent is now stated in the code.

## [0.12.13] — 2026-11-18

### Changed
- **The "Recognizing sheet music" progress popup no longer blocks the rest of Reprise.** Recognition can take minutes, so
  the popup is now modeless instead of modal: while it is up you can click, move, and minimize the main Reprise window and
  keep working. The popup floats above the Reprise main window (and only the main window — it is not globally on top of
  other applications): it stays above the score so it is never lost behind it, follows normal window order (it goes behind
  when you switch to another app), and minimizes and restores together with Reprise. After "Done" the score behind it
  stays clickable. The dark header strip (the Reprise mark and the "Recognizing sheet music" title) is now a drag handle
  you can use to move the popup; the body controls (Details, the recognition log, the "Close this window after importing"
  checkbox, Cancel, and Close) stay fully clickable and are never a drag surface. The popup re-centers over the main
  window every time it opens and never remembers a dragged position. Everything else is unchanged: same look, same
  controls, the same auto-close on success, and Cancel.

## [0.12.12] — 2026-11-18

### Fixed
- **A slow import that eventually opens successfully is no longer wrongly reported as failed.** 0.12.3 added a graceful
  "could not produce a usable score" error for scans whose recognised MusicXML is too corrupt or incomplete to open, but
  it detected that failure with a fixed 8-second wall-clock timer — and a large multi-sheet score can take much longer
  than that to open (one Pastoral scan takes ~18 s, partly because the #711 pad-and-retry runs Audiveris twice). A fully
  successful but slow import could therefore trip the timer and be shown as a failure even as the score opened. Reprise
  now takes the real open result directly from MuseScore's open path instead of racing a clock: a declined open reports
  its error the instant it resolves, and a slow-but-valid open is simply one still in progress — it is never mis-flagged,
  however long it takes. A genuinely corrupt or unopenable scan still shows the same graceful error (never a silent
  "Done" and never a crash), and the 0.12.2 empty-recognition refusal is unchanged.

## [0.12.11] — 2026-11-18

### Fixed
- **TIFF scans are now padded with a white margin before recognition, just like every other image.** 0.12.10 added
  the Audiveris #711 edge-crash white-margin pad for PNG/JPG/BMP/GIF (and a retry pad for PDFs) but had to leave TIFF
  unpadded because the Qt kit shipped no TIFF plugin. With that plugin now present, TIFF gets the same protection:
  - **Single-page TIFF:** padded exactly like any other image (centered, unscaled, on a 100-pixel white border).
  - **Multi-page TIFF:** every page is decoded, each padded with the same border, and the pages are re-assembled into a
    single multi-page file that Audiveris reads — so all pages are recognized, in order, with the edge-crash fix applied
    to each. (The padded pages are carried in a PDF, because the Qt TIFF plugin cannot write a multi-page TIFF; the page
    content is unchanged, only a white border is added.)
  The oversized-scan guard now measures the padded TIFF size too, skipping the margin for any page already near the
  20-megapixel limit. And if anything about the TIFF pad is uncertain — a page cannot be decoded, the page count cannot
  be confirmed — Reprise falls back to feeding Audiveris the original TIFF, which it reads natively, so a multi-page TIFF
  import can never drop, reorder, or corrupt a page.

### Fixed
- **Scans whose top or bottom staff sits right at the page edge no longer crash recognition.** Audiveris has a known
  bug (#711) where a staff touching the image border trips an internal edge probe
  (`AreaUtil.verticalCore` → `ArrayIndexOutOfBoundsException`) and aborts the whole scan. The maintainer's workaround is
  to pad the image with a white margin, which Reprise now does automatically:
  - **Image scans (PNG/JPG/BMP/GIF):** before recognition, the image is composited, centered and unscaled, onto a
    slightly larger white canvas (a 100-pixel border on every side) and that padded copy is what Audiveris reads. The
    music content is untouched — only a white border is added — so recognition quality is unchanged.
  - **PDF scans:** the original PDF is tried first. Only if it crashes with the #711 signature does Reprise re-render
    the PDF to grayscale page images (matching Audiveris's own rasterization), add the same white margin, re-assemble a
    padded PDF, and re-run recognition once. Normal PDFs are completely unaffected.
  The fix keeps full scan resolution (the margin never lowers the DPI), and the oversized-scan guard now measures the
  padded size, automatically skipping the margin on a scan that is already near the 20-megapixel limit rather than
  rejecting it. (Multi-page TIFF padding is a separate follow-up.)

## [0.12.9] — 2026-11-18

### Fixed
- **Saving to a path with Windows backslash separators produced a `.mscz` that could not be reopened.** When a
  save target used `\` separators (as an MCP `save-at` argument could), the written archive's internal `.mscx`
  entry name and the `container.xml` rootfile became the whole drive-stripped path instead of a plain basename,
  and the two disagreed on separators — so the reader could not find the root `.mscx` and the file silently
  failed to reopen (also leaving a `*.mscz_saving` orphan). Reprise now normalizes separators at the save
  chokepoint (covering Save, Save As, Save at, and autosave) and at the MCP save-at/open entry points, so
  backslash paths round-trip correctly. (Note: the underlying cause is upstream — muse's
  `FileInfo::completeBaseName` splits on `/` only; flagged for an upstream fix.)

## [0.12.8] — 2026-11-18

### Removed
- **The "How would you like to save?" cloud/computer chooser is gone.** Reprise is a free, offline app with
  no cloud account, so the save-location chooser (and its "To the cloud (free)" / "On your computer" cards)
  no longer served a purpose. **Save**, **Save As**, and the first save of an imported score now go straight
  to a local file — the normal system **Save** dialog opens directly, with no extra screen in between.
- **The leftover "Save to cloud" command.** Its menu entry was already removed earlier; the inert command
  behind it (and its plumbing) is now deleted too.

### Changed
- **Save As on an imported scan now suggests the original scan's name in your Scores folder.** An imported
  score is opened from a temporary working file, so Save As previously defaulted to that temp folder and
  offered an awkward internal name (ending in `.reprise-stripped`). It now suggests **`<original scan
  name>.mscz`** in your **Scores** folder instead. (This only changes the suggested path in the Save dialog;
  nothing about the score's saved/unsaved state or autosave changes.)

### Added
- **The recognition error log is now selectable and has a "Copy" button.** When Audiveris cannot recognize a
  scan, the error popup's log text can be selected with the mouse, and a new **Copy** button copies the whole
  error message plus the recognition log to the clipboard in one click — ready to paste when asking for help.

## [0.12.7] — 2026-11-18

### Fixed
- **The import progress popup could crash (SIGSEGV) when it auto-closed after a successful import.** With
  "Close this window after importing" enabled, the popup closed itself (`root.hide()`) ~1.4 s after the
  recognized score opened — but at that moment the project-open was often still rebuilding the notation
  view and its docks, churning the interactive window stack. The popup's self-close then raced that churn
  and, intermittently, the popup's close was delivered twice to the framework, tripping an internal
  assertion (`Interactive::onClose`, `objectId.isValid()`) and crashing the app with a SIGSEGV. The
  auto-close is now performed by the application-level import controller, which outlives the popup: it
  closes the popup's own window only while it is still on the interactive stack (so the close is never
  double-delivered) and then waits for the window to be fully torn down, draining any racing teardown —
  the same settle-on-close technique used for the intake-dialog fix. Both settings still behave as before
  (auto-close on, or the user closing the window manually), and the score opens correctly either way.

## [0.12.6] — 2026-11-18

### Changed
- **Imported scans are now re-engraved with MuseScore's own layout.** Audiveris bakes the *scan's*
  geometry into its MusicXML — per-measure widths, system and page breaks, absolute pixel positions,
  and page/system/staff layout blocks — which MuseScore faithfully reproduced, so a recognized score
  mirrored the paper original (stretched, sparse bars such as a lone over-wide measure on its own
  line, and uneven scan-mirrored spacing). Reprise now post-processes the recognized MusicXML before
  it opens, stripping that geometry (plus slur bezier control points and per-measure widths) so
  MuseScore engraves a clean, evenly spaced layout of its own. Applied to every import; all notes,
  rests, lyrics, dynamics, slurs, text, and metadata are preserved exactly.

### Fixed
- **The title block no longer doubles or overlaps on imported scans.** Audiveris emits the title,
  subtitle, composer, and arranger as *scan-positioned* credits, several of which landed in the same
  title-frame slot and overprinted each other (e.g. "Organ Transcription" over "from the Nutcracker
  Suite", and the arranger over the composer). Reprise now de-duplicates the scan credits into one
  clean text per slot, promotes the real title into the score's Title property, and drops stray scan
  artifacts (page numbers, stray marks, organ-manual registration labels), leaving a single tidy
  title block.

## [0.12.5] — 2026-11-17

### Fixed
- **The intake dialog could still show "multiple" instead of the real page count for edited, re-saved
  PDFs**: the page counter was a no-dependency byte-scan of the page-tree root's `/Count`, which cannot
  read PDF-1.5+ files whose cross-reference table and objects are stored in **compressed streams** — the
  common shape of a PDF that has been opened, edited, and saved again (e.g. a score synced through a
  cloud drive). For those files the scan bailed out and the dialog fell back to showing the page range,
  so a ten-page score read as **"multiple"** rather than **10**. Reprise now reads the page count with
  **Qt PDF (`QPdfDocument`)**, which is authoritative for **every** PDF, including the compressed
  variety; the byte-scan is kept as a fallback for the rare case Qt PDF cannot open a file. The safe
  contract is unchanged (a confident single page hides the range; any remaining uncertainty shows the
  range so pages are never dropped), and the TIFF multi-frame path is untouched (Qt PDF handles PDFs
  only; images still use the image readers).

## [0.12.4] — 2026-11-17

### Fixed
- **The intake dialog could show the wrong page count for some PDFs**: the page counter scanned the
  file for `/Type /Page` objects and counted every one it found. A PDF saved with incremental updates
  (e.g. an Acrobat-revised file synced through OneDrive) can leave stale, duplicate page objects
  behind, so that scan over-counted — one such four-page score reported **8** pages, which then
  mislabelled the page-range field. Reprise now reads the authoritative page count from the page-tree
  root's `/Count`, resolved through the document's **current** revision (its last trailer → `/Root` →
  `/Pages`), so incremental updates no longer inflate the number. The safe fallback is unchanged: if
  the count cannot be determined with certainty, the page range is shown so pages are never dropped,
  and the TIFF multi-frame path is untouched.

## [0.12.3] — 2026-11-17

### Fixed
- **An import whose recognised score is too corrupt or incomplete to open now shows a clear error
  instead of a silent "Done"**: some scans recognise a valid part structure yet produce data-corrupt
  MusicXML (measures with wrong total durations), which MuseScore's reader then declines to open.
  Post-0.12.2 that no longer crashed, but the progress popup still flipped to **Done** while no score
  appeared and nothing told the user why. Reprise now treats the score as finished only once it
  actually opens: it waits for the open to resolve and, if no score ever becomes current, surfaces a
  graceful error — "Reprise could not produce a usable score from this scan" — in the progress popup
  and via `reprise.get-import-errors`. This complements the 0.12.2 zero-part guard (empty recognition)
  by catching **any** reason the recognised score fails to open, and never false-flags a valid open.

## [0.12.2] — 2026-11-17

### Fixed
- **Importing a scan that recognises no visible parts no longer crashes**: some scans (e.g. a
  multi-stave choral score where whole staves rest for long stretches) made Audiveris emit
  per-measure "hidden empty staff" markers, which switch MuseScore's **hide empty staves** style on
  during import. On layout a system could then end up with **zero visible parts**, and the engraving
  instrument-name positioner indexed an empty list — a hard crash (SIGSEGV) in a release build.
  Reprise now guards that layout path (an empty group/part is skipped instead of dereferenced), and
  additionally refuses a genuinely empty recognition (zero recognised parts) gracefully with a clear
  message — surfaced in the progress popup and via `reprise.get-import-errors` — instead of opening a
  broken score.

## [0.12.1] — 2026-11-15

### Changed
- **Language lists now put English first**: the Preferences **Installed languages** list and
  **Default language** dropdown, and the intake dialog's **Text language** dropdown, now list
  **English** first (the built-in baseline) with every other language alphabetized by its friendly
  display name below it; the **Add a language** picker is likewise alphabetized by display name.
  Previously the lists sorted purely alphabetically, so English fell between German and French.

### Removed
- **The "?" help icons on the Import Scan Options panel**: the faint question-mark glyphs (and their
  hover tooltips) next to **Recognize text & lyrics** and **Lyrics appear above the staff
  (uncommon)** are gone — the labels stand on their own.

## [0.12.0] — 2026-11-15

### Added
- **Language-pack management in Preferences**: the Reprise Preferences page gains a **Text
  recognition** section that manages the OCR language packs Reprise ships and reads. Pick the
  **Default language** used when importing a scan; see the **Installed languages** (each with an
  **Update** button, and a **Remove** button except for the built-in English baseline and the
  current default); and **Add a language** from a curated set (German, French, Italian, Spanish,
  Latin, Portuguese, Dutch, Russian, Polish). Each add/update downloads the pack on demand from the
  Tesseract project's main `tessdata` repository (the legacy-capable combined model Reprise's OCR
  needs), installing it to Reprise's own `tessdata` folder where Audiveris reads it. Downloads are
  explicit and user-initiated only — English works fully offline out of the box, with no
  background fetch or phone-home. The intake dialog's language dropdown now defaults to the
  configured default language.
- **Agent-facing MCP helpers** `reprise.list-languages`, `reprise.add-language`, and
  `reprise.remove-language` (sharing the exact install/remove/enumerate code path as the
  Preferences buttons), plus `reprise.capture-preferences` to grab the Preferences page headlessly.

### Removed
- **The standalone Tesseract path setting** (and its Detect button and auto-detection) from
  Preferences. The Tesseract OCR engine is bundled inside Audiveris, and Reprise ships and manages
  the training data itself — so there was never a separate Tesseract program to point at. The
  shipped `tessdata` folder beside the executable is now the sole OCR data source.

## [0.11.0] — 2026-11-14

### Added
- **Intake "Options" panel to configure recognition**: the Import Scan dialog gains a collapsed
  **▸ Options** disclosure (default collapsed, present for both a single image and a PDF) that
  reveals a master **Recognize text & lyrics** toggle with indented sub-options — a **Text
  language** dropdown (populated from the Tesseract language packs installed in Reprise's own
  `tessdata` folder, shown as friendly names; `osd` is excluded as it is not a text language), an
  **Also detect chord symbols** toggle, and a **Lyrics appear above the staff (uncommon)** toggle.
  The sub-options grey out when the master is off. These are wired to Audiveris per-import: turning
  the master off skips the OCR step entirely for a faster notes-only import; the others pass the
  matching Audiveris recognition switches. Defaults mirror Audiveris's own (text on, English, chords
  off, lyrics below), so the default import is unchanged and still recovers the full lyric set.
- **Recognition options on the headless `reprise.import-pdf` tool**: new optional `recognizeText`
  (bool, default true), `language` (string, default `eng`), `chordSymbols` (bool, default false),
  and `lyricsAboveStaff` (bool, default false) arguments feed the same per-import Audiveris
  recognition switches as the GUI panel, so an import can be driven and verified headlessly.

## [0.10.0] — 2026-11-14

### Added
- **Automatic text & lyric OCR — titles, lyrics, and tempo markings are now recognized**: Reprise
  ships its own Tesseract **legacy+LSTM** training data (`eng` + `osd`) beside the executable and
  points Audiveris at it (`TESSDATA_PREFIX`, resolved at runtime from the running exe's own
  `tessdata` folder). Audiveris performs its text recognition (its TEXTS step) by initializing
  Tesseract in **legacy** mode, which the system "fast" LSTM-only `eng.traineddata` cannot satisfy —
  so without this, every OCR run failed silently (`No OCR'd lines`) and no lyrics, titles, or tempo
  text ever reached the score. With the combined legacy data shipped in-tree, OCR works out of the
  box and self-contained, independent of any system Tesseract install. On a Foster vocal score this
  recovers ~195 lyric syllables plus the title and composer.

## [0.9.9] — 2026-11-14

### Fixed
- **Import Scan dialog could crash (SIGSEGV) when closed and reopened before the close fully
  settled**: closing the intake dialog and opening it again while the first close was still in
  flight tripped `Interactive::onClose`'s `objectId.isValid()` assert, because the reopen parented
  onto the still-closing dialog window. The dev `reprise.capture-import-dialog` tool triggered it on
  repeated calls in one app session (the second call crashed). The dialog now sequences the reopen
  after the close completes — it pumps the event loop until the prior dialog is fully off the
  interactive stack (its objectId invalidated) before opening again — so back-to-back captures, and
  a fast user cancel → reopen of the real Import Scan dialog, no longer race.

## [0.9.8] — 2026-11-14

### Changed
- **Import Scan intake dialog trimmed of redundant captions**: the single-image state no longer
  shows the separator rule or the *"Quality tiers apply to PDF imports; images are recognized at
  their native resolution."* caption — a single image now shows only File / Pages / Resolution
  above the buttons. The PDF state drops its redundant bottom *"Matched to your scan — this PDF is
  about N dpi."* "why" caption; the blue **Matched to your scan** badge beside the Standard tier
  and the header separator are kept.

### Removed
- The single-image quality-tier explanation caption and its separator rule, and the PDF
  matched-resolution "why" caption, from the Import Scan dialog.

## [0.9.7] — 2026-11-14

### Fixed
- **Import Scan images now show their true embedded DPI, or `~300 dpi, assumed`, never Qt's
  back-filled 96**: the Resolution line's DPI is now decided by parsing the scan file's own bytes
  for real resolution metadata (PNG **pHYs** chunk, JPEG **JFIF/EXIF** density, TIFF **IFD**
  resolution tags, BMP **biXPelsPerMeter**; GIF has none) instead of trusting
  `QImage::dotsPerMeterX()`, which silently reports 96 dpi for a PNG that carries no `pHYs` chunk.
  When real metadata is present the line shows the file's true value (**(~N dpi)**, not snapped to
  a tier); when it is truly absent the line now reads **(~300 dpi, assumed)** for every image type
  — so a no-`pHYs` PNG that previously read a bogus **(~96 dpi)** now correctly reads
  **(~300 dpi, assumed)**. The PDF rasterization-tier wording (**(…, default)**) is unchanged — it
  means a real applied setting, not an informational guess.

## [0.9.6] — 2026-11-13

### Fixed
- **TIFF scans now show a DPI on the Import Scan Resolution line**: a PNG/JPG showed its
  resolution (e.g. **(~300 dpi)**) but a TIFF showed only pixel dimensions, because this build's
  Qt ships no TIFF plugin and the in-house TIFF reader did not parse the resolution tags. The
  reader now reads the TIFF IFD resolution tags (XResolution/YResolution/ResolutionUnit, honoring
  the file's byte order and converting cm → inch) from the reported (largest) frame.

### Changed
- **The Resolution line always shows a DPI, for every image type**: when a scan carries no
  resolution metadata (no PNG/JPG density, or a TIFF with ResolutionUnit = none / absent tags) the
  line now shows the default, clearly labelled — **(~300 dpi, default)** — instead of omitting it,
  so PNG / JPG / TIFF / BMP / GIF all read consistently (DPI is informational for images, which
  Audiveris recognizes at their native pixel resolution).
- **Escape now cancels the Import Scan dialog**: pressing Escape does exactly what clicking Cancel
  does (dismiss with no import), including when focus is in the Pages field (where the text input
  previously swallowed the key).

## [0.9.5] — 2026-11-13

### Fixed
- **Import Scan dialog "why" caption is now legible**: the PDF matched-resolution caption
  (and its "No scan resolution detected" sibling) was the last text in the dialog still drawn
  in a secondary font color that rendered nearly invisible in this theme. It now uses the
  primary font color, so every line in the Import Scan dialog reads clearly in both light and
  dark themes.

## [0.9.4] — 2026-11-13

### Fixed
- **Import Scan dialog header is now legible**: the read-only header (file name, page count,
  and — for images — native resolution) and the single-image note were drawn in a secondary
  font color that rendered nearly invisible in this theme. They now use the primary font color,
  so they read clearly in both light and dark themes; the read-only signal still comes from the
  layout (no input control + the separator).

### Added
- **Dev capture of the import dialog**: a new `reprise.capture-import-dialog` MCP/API tool opens
  the Import-Scan intake dialog for a given scan (PDF or image) and grabs it to a PNG, so the
  modal intake dialog — otherwise unreachable headlessly — can be verified without a live picker.

## [0.9.3] — 2026-11-13

### Changed
- **Import Scan dialog redesigned with a read-only header**: every import now shows a muted
  header with the file name, the page count, and — for images — the native resolution
  (e.g. **2581 x 3495 px (native)**, with a **(~300 dpi)** suffix only when the image file
  actually carries resolution metadata). The settable controls sit below it: a PDF keeps the
  recognition-quality tier (with a caption naming the detected dpi that drove the match) and
  the page range; a single-page image shows no settable control, just a one-line note; a
  multi-page TIFF keeps the page range.

### Fixed
- **Resolution line restored on multi-page TIFF**: the native resolution now appears for all
  images in the header, including multi-frame TIFFs (previously only single images showed it).

## [0.9.2] — 2026-11-13

### Fixed
- **Oversized-image pre-flight now covers TIFF files**: a too-large TIFF was silently
  bypassing the 20-megapixel guard because this build's Qt ships no TIFF image plugin, so
  `QImageReader` reported it as 0×0 and the check passed. The pre-flight now reads TIFF
  dimensions with the in-house TIFF reader (the largest frame, since Audiveris caps
  resolution per sheet) and blocks an oversized TIFF up front, matching the other images.

## [0.9.1] — 2026-11-13

### Fixed
- **Multi-page TIFF page selection**: the intake dialog now shows the page range for a
  multi-page (multi-frame) TIFF. The page count is read by an in-house walk of the TIFF
  frame directories, because this build's Qt ships no TIFF image plugin — so `QImageReader`
  could not read a TIFF at all and the field was wrongly hidden.

### Added
- **Single-image intake shows the scan's native resolution**: for a single image, the
  quality tier (which does not apply to an image) is replaced by a read-only line reading
  e.g. **Scan: 2581 x 3495 px (native resolution)**.

### Changed
- **Page range hidden for single-page PDFs too** (consistent with single images), with a
  safe-fallback to **shown** whenever the page count is uncertain (a compressed
  object-stream PDF, or any ambiguity) so pages are never silently dropped.

## [0.9.0] — 2026-11-13

### Added
- **Import image files (PNG / JPG / JPEG / TIFF / BMP / GIF) alongside PDF** via Open, New,
  and the MCP `reprise.import-pdf` tool — the same Audiveris recognition path now accepts a
  plain scan image, not just a PDF.
- **Multi-page TIFF page selection**: a multi-frame TIFF is treated as multiple sheets, and the
  page-range field appears for it just as it does for a PDF (single-page images skip it).
- **Oversized-image pre-flight**: an image whose pixel count exceeds the recognizer's limit
  (20 megapixels) is now blocked up front with a clear message, in both the dialog and the MCP
  tool, instead of failing mid-recognition with a raw Audiveris exit code.

### Changed
- **"Import PDF" renamed to "Import Scan"** throughout the intake flow.
- **The recognition-quality tier and the page-range field are hidden for single-image inputs**
  (an image has no PDF resolution to pick and is a single sheet).

## [0.8.0] — 2026-11-13

### Added
- Dev MCP tool `reprise.capture-screenshot` (target=window | score) that captures the GUI/rendered score to a PNG and returns its path.

### Changed
- **Recognition-quality tier labels now show the dpi** — **Faster (200 dpi)** / **Standard (300 dpi)** / **Best (400 dpi)**; the descriptions after each label and all behavior are unchanged.
- **Online/cloud module compiled out — Reprise is now an offline-only binary**: the umbrella
  `MUSE_MODULE_CLOUD` is OFF (the muse framework's cloud module is replaced by a stub), Qt
  NetworkAuth is no longer linked, and the account / cloud-scores / save-to-cloud surfaces are
  gone or inert.
- **Synced with upstream MuseScore `main`** (2026-11-13 merge).

### Fixed
- **Agent/headless-launched instances now self-clean their session** on exit, so the user no
  longer gets a spurious "restore previous session?" prompt on the next normal launch.

## [0.7.0] — 2026-11-12

### Added
- **PDF import "Recognition quality" selector** (**Faster** / **Standard** / **Best**)
  that auto-detects the PDF's native resolution and pre-selects the matching tier, showing
  a **"Matched to your PDF"** badge when a real embedded-image resolution is detected (and
  falling back to **Standard** otherwise). The MCP `reprise.import-pdf` shares the same
  auto-detect when no dpi is given.

### Changed
- **Import-PDF intake dialog recognition control reworked**: the free-form DPI input box
  (and its 200–400 range validation) is replaced by the Recognition-quality selector above,
  and all DPI jargon is removed from that UI.

## [0.6.0] — 2026-11-12

### Changed
- **Top menu bar stripped down for Reprise**, continuing the 0.4.0 GUI-strip theme:
  the composition/engraving entries irrelevant to the scan-and-correct workflow are
  gone. **File** loses **Save selection** and **Open parts**, and the **"Save other"**
  submenu is flattened to a direct **Save a copy**. **Add ▸ Text** drops the
  **chord symbol**, **Roman numeral**, **Nashville number**, **figured bass**, and
  **instrument change** items. The **Help** web and update items are kept visible but
  grayed out (disabled).
- **Scans page default view is now List instead of Grid.**

### Removed
- **View menu**: removed the **Instruments** / **Braille** / **Percussion** panel
  toggles and the **Workspaces** entry.
- **Add menu**: removed the **Chords & fretboard diagrams** submenu.
- **Tools menu**: removed **Explode**, **Implode**, **Realize chord symbols**,
  **Fill with slashes**, and **Toggle rhythmic slash notation**.

### Added
- **Build-type chooser** (Release default / Debug / RelWithDebInfo) in the helper
  build scripts.

### Fixed
- **Notation MCP query structs decoupled from upstream's renamed field-declaration
  macro**, fixing a build break after an upstream sync.
- **+Update now builds before pushing** `reprise-main` (+Build propagates the failure
  exit code), so a broken tree is no longer pushed.

## [0.5.0] — 2026-11-12

### Added
- **Agent-testable MCP control surface**: Reprise's built-in MCP server now exposes a
  focused, agent-facing toolset so the Import-PDF pipeline can be driven and verified
  headlessly (no GUI clicks).
  - **Six read-only state queries**: `project.get-state` (whether a scan is open, its
    name/path, unsaved state), `score.get-counts` (measure / part / instrument / staff /
    page counts), `score.get-health` (corrupt and irregular measures), `notation.get-editstate`
    (whether editing is available), and `reprise.get-import-progress` / `reprise.get-import-errors`
    (live Import-PDF / OMR progress and errors).
  - **Headlessly-drivable Import PDF**: `reprise.import-pdf` accepts optional `path` / `pages`
    / `dpi` arguments, so an import can be started directly, skipping the file picker and
    options dialog.
- **Separate Release PDB** carried forward from 0.4.0 lets these be developed and crash-debugged
  with full symbols.

### Changed
- **Curated MCP toolset**: the MCP tool list is trimmed to the agent-facing set (the six
  queries above + `reprise.import-pdf` + the core `project.open` / `close` / `save` /
  `save-as` / `export`). Every other command has only its **MCP** availability bit cleared —
  the plugin API, menus, keyboard shortcuts, and all in-app behaviour are untouched.

## [0.4.0] — 2026-11-11

### Changed
- **Home/shell stripped down for Reprise**: the sidebar is trimmed (removed
  **MuseSounds** and **My accounts**), **"Scores"** is renamed to **"Scans"**, and
  **"New"** / **"New Scan"** are rewired to the **Import PDF** flow. Removed the
  welcome promos, and applied a targeted user-facing **"Score" → "Scan"** rename
  across the shell. The **Learn** page is now a **"Coming soon"** stub.
- **Top ribbon "Score" tab renamed to "Edit"**, and disabled/grayed until a scan is
  open — Reprise is edit-only.
- **Separate PDB for Release builds** (`/Z7`): Release crashes can now be symbolized
  without bloating the shipped binary.

### Removed
- **Publish tab and all cloud/online UI**: the Publish tab and the remaining
  cloud/online surfaces (and their promos) are gone — Reprise is fully offline.

## [0.3.0] — 2026-11-11

### Added
- **Auto-detect OMR paths on first Preferences open**: if the Audiveris or Tesseract path has
  never been set, the Reprise page detects it on open and saves it so it stays; if the program
  is not installed the field reads "Not detected" instead of being blank.
- **Reprise Preferences page**: a new **Reprise** section in Edit > Preferences (placed
  after Import) collecting the recognition settings in one place — the **Audiveris** and
  **Tesseract** program paths (each with a **Detect** button that finds the installed
  program and fills the field), the **scan resolution** (200–400 dpi), and the **"close
  the progress window after a successful import"** toggle.
- **Audiveris/Tesseract auto-detection** matching the original Reprise search order: the
  PATH first, then the standard install folders under Program Files / Program Files (x86) /
  ProgramW6432 / LocalAppData for Audiveris, and `…\Tesseract-OCR\tesseract.exe` for
  Tesseract. A path you set by hand always overrides detection.
- **Tesseract path drives OCR**: when a Tesseract program path is set, `TESSDATA_PREFIX`
  for Audiveris' OCR is derived from it (its sibling `tessdata` folder); otherwise the
  previous automatic detection next to Audiveris is used.

### Changed
- **Detect status is now transient**: the green **"Found: &lt;path&gt;"** confirmation fades
  out after 2 seconds and its status row then auto-collapses, so no empty gap is left behind.
  The detected path itself stays in the field — only the dynamic status line fades.
- **"Not detected" status stays persistent**: when a program is not found the "Not detected"
  status remains visible and does not fade, so the row is never ambiguously blank.

## [0.2.0] — 2026-11-11

### Added
- **Recognition progress popup**: Import PDF now runs Audiveris asynchronously and
  shows a branded progress window — a navy header (Reprise mark + wordmark + title,
  with a gold check badge once finished) over a cream card. It shows the file, page
  range and dpi; a live current-stage line ("Analyzing page X of N — detecting
  staves…"); a gold progress bar on a tan track with a "Page X of N" / percent
  caption; a **Details** expander that reveals the live Audiveris recognition log;
  and a **Cancel** button (gold) that stops recognition and aborts the import. On
  success the step line becomes "Done — recognized M measures across N pages.", the
  score opens, and a navy **Close** button appears; on failure the window stays up
  with the Audiveris error and a tail of its output instead of vanishing.
- **"Close this window after importing"** option in the popup (remembered
  between runs, **off by default**): when checked, the window closes itself
  automatically once the score has opened.
- **Audiveris output in the log file** (internal diagnostics): the recognition
  command line, every stdout/stderr line and the exit code are now also written to
  the Reprise log file, tagged `[audiveris]`, so failed imports can be diagnosed later.

### Fixed
- **Progress popup started at "page 2 of 2" with a full bar on a multi-page PDF with a
  page range**: Audiveris prints `Book reaching PAGE on sheets:[#1#2]` before any sheet
  starts, and the sheet parser read its trailing `#2]` as the current sheet. The current
  sheet is now read only from the leading `[<book>#N]` log tag. The popup also resets all
  per-import state when a run begins.
- **Recognition popup header mark**: the header now shows the full cream-R +
  gold-blade + two-notes mark (the transparent `titlebar-mark` glyph) at the 52px
  size of the approved mockup, instead of the small app badge (a navy rounded square
  whose inner glyph read tiny against the navy header).
- **Progress total / backwards bar on some PDFs**: the sheet total is now fixed up
  front from a reliable source — the requested page count when a range was given,
  otherwise the book's one-time "N sheets" line, locked so a later stray "N sheet(s)"
  line can no longer clobber it down to 1. The current sheet is clamped to the total
  (no more "page 3 of 1") and both the bar and the "page X" caption are monotonic, so
  progress never regresses.
- **Import PDF crash on launch**: the recognition popup's view-model
  (`ImportProgressModel`) resolved its IoC context inside its constructor, but a
  QML-instantiated object has no `QQmlContext` yet during construction — the
  injected `IProjectSession` came back null and the app crashed (SIGSEGV) the moment
  an import began. The context-dependent setup now runs from `load()` (driven by the
  popup's `Component.onCompleted`), matching the other MuseScore QML dialog models.

## [0.1.0] — 2026-11-10

Initial pre-release. Reprise is established as a fork of
[MuseScore Studio](https://github.com/musescore/MuseScore) — rebranded and slimmed —
with the first slice of the PDF scan-and-correct workflow.

### Added
- **Import PDF** (*File ▸ Import PDF*): runs [Audiveris](https://github.com/Audiveris/audiveris)
  optical music recognition on a PDF and opens the recognized score via MuseScore's
  native MusicXML import.
- **Intake dialog**: choose a page range (`all`, `1-5`, `1,3,8`) and a scan
  resolution (200–400 dpi, default 300) before recognition.
- **Last-folder memory**: the Import-PDF picker reopens at the folder you last
  imported from, independent of MuseScore's Open dialog.
- Reprise branding throughout (name, app icon, splash, About) and version `0.1.0`.

### Changed
- Forked from MuseScore Studio; **disabled the auto-update check and all telemetry /
  phone-home endpoints**.
- Replaced the cloud "Convert file…" menu item with the local Audiveris Import-PDF
  flow (Reprise has a single, offline OMR path).
- Slimmed the build: excluded debug DLLs and non-English locales and disabled
  unused modules (import/export formats, cloud/learn/tours/etc.) while keeping the
  editor, playback, and MusicXML core.

### Temporary
- Every imported part currently defaults to **Grand Piano**, overriding the
  instruments OMR guesses — a deliberate stopgap until instrument selection is
  added to the import flow.

[Unreleased]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.12]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.9]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.8]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.6]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.5]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.4]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.3]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.2]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.1]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.12.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.11.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.10.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.9]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.8]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.7]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.6]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.5]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.4]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.3]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.2]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.1]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.9.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.8.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.7.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.6.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.5.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.4.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.3.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.2.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
[0.1.0]: https://github.com/Amadeus-/Reprise/tree/reprise-main
