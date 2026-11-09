# Drawing Archive Tool: Specification

Status: **draft, agreed rules from planning chat**. Items marked **[HOME]** are
filled in on the PC (they need a real drawing, AutoCAD or the Excel file open).

---

## 1. Purpose

Work through a copy of the drawing archive (`Stamtegninger`) and rebuild it as a
new, clean structure:

- every file sorted into the correct site / building / discipline / file-type folder
- duplicates removed, older versions moved to `00 Udgået`
- DWG files repaired (xrefs reloaded, units checked) and saved in the new structure
- every good DWG printed to **DWFx at 1:100**
- drawings that only exist as PDF/TIFF moved to `Scannede Filer`
- the drawing list Excel in `00 Tegningsliste` updated
- a report of everything that was done, flagged or discarded

The tool is run by Claude Code on the PC (via Remote Control) when told to
"start". The rules in this document are what "fix it" means.

---

## 2. Source and output

| | Path | Treatment |
|---|---|---|
| **Source** | the archive copy (`Stamtegninger`) [HOME: exact path] | **Read-only.** Never changed, nothing deleted. |
| **Output** | new `Stamtegninger` structure [HOME: exact path] | Everything is written here. |

"Deleted" or "discarded" in this spec means **not carried into the output** and
**listed in the report**. The source copy always keeps the original.

---

## 3. Folder structure (output)

```
Stamtegninger\
  <Site>\                         e.g. GLO-Glostrup
    <Building>\                   e.g. 74 Bygning (psyk)
      00 Tegningsliste\           drawing list Excel (updated, see §11)
      00 Udgået\                  superseded versions (see §6)
      Arkitekt\
      Brand\
      EL\
      Konstruktion\
      VVS\
        DWFx\                     prints made by the tool
        DWG\                      drawings with a title block
          Xref\                   files that only exist as xrefs (see §8)
        PDF\                      reserved, see §9
        Revit <year>\             ignored by the tool
        Scannede Filer\           old drawings: PDF/TIFF with no DWG
```

- Missing subfolders are created.
- `Revit <year>` folders are **ignored completely**: not scanned, moved or created.
- Site and building names are taken from the existing archive folder names.

---

## 4. Pipeline (order of work)

1. **Scan**: index every file in the source (path, size, date, checksum).
2. **Inspect**: read every DWG, PDF and TIFF (see §5).
3. **Map connected buildings** (see §8.2).
4. **Group** files into drawings and resolve duplicates (§6).
5. **Check placement**: title block vs. folder (§7).
6. **Process DWGs**: copy, check units, resolve xrefs, check title block, save (§8).
7. **Print** good DWGs to DWFx (§10).
8. **Reconcile** DWG vs PDF/TIFF of the same drawing (§9).
9. **Update** the drawing list Excel (§11).
10. **Report** (§12).

### Run modes
- **Plan (default):** runs steps 1–5 and produces a report of what *would*
  happen. Nothing is written to the output.
- **Apply:** carries out the plan. Moves of misplaced files (§7) only happen if
  they were confirmed after the plan run.

---

## 5. Inspection: what is read

| File | Read |
|---|---|
| DWG | title block attributes (drawing number, revision, date, title, …) [HOME: block name + attribute tags], xref list with status and paths, units (`INSUNITS`), layouts and their paper sizes, all text (for room numbers / building references) |
| PDF | text layer (title block text: drawing number, revision, date), page count, page size; whether it has text at all |
| TIFF | size, resolution, page count |

DWG reading is done through AutoCAD's batch engine `accoreconsole.exe`
(AutoCAD 2018+) [HOME: install path].

---

## 6. Duplicates and versions

### 6.1 Same drawing
Two files are the **same drawing** if **either**:
- the drawing number (from the title block) matches, **or**
- the file name matches, ignoring the revision part of the name.

Files of different types (DWG vs PDF) of the same drawing are handled in §9,
not here.

### 6.2 Exact copies
Byte-identical files: **one is kept, the rest are deleted.**

### 6.3 Newer version wins
Among versions of the same drawing (same file type):
1. newest **title block date** wins
2. if dates are equal or missing: highest **revision** wins
3. revision is read from the title block, otherwise the file name.
   **The title block always wins** over the file name.

The winner continues through the pipeline. Older versions go to the building's
**`00 Udgået`**.

If no date or revision can be determined → keep all, **flag**.

---

## 7. Misplaced files

The title block (building, discipline) is compared with the folder the file
sits in. Mismatches are **reported, not moved automatically**. After
confirmation they are placed in the folder the title block points to.

[HOME: which title block fields hold building and discipline]

---

## 8. DWG processing

### 8.1 Units
- Expected: **millimetres**.
- If set to inches (or anything else): change the setting to millimetres
  **without rescaling** and **flag** the file.

### 8.2 Xrefs
Only xrefs listed in the drawing's xref list are handled (detached xrefs are not
visible and are ignored).

**Missing or unloaded xrefs** are searched for by file name in this order:
1. same building
2. connected buildings
3. rest of the same site

If found in more than one place: prefer **same discipline → same building →
newest file**. Still ambiguous → **flag**, don't guess.

Paths are rewritten so xrefs keep working in the new structure (relative paths).

**Connected buildings** are found from drawing text: a drawing in building 84
that contains room numbers or references belonging to building 85 means 84 and
85 are connected. The resulting connection list is included in the report and
can be corrected by hand; corrections are kept for later runs.
[HOME: room number format, e.g. `85.01.012`]

### 8.3 Xref-only files
A DWG that **is referenced by other drawings** and **has no title block** is an
xref-only file → saved in `<Discipline>\DWG\Xref\`. It is not discarded and
not printed.

### 8.4 Title block
- No title block (and not xref-only) → **discarded**: no print, not carried
  into the output, listed in the report.

### 8.5 "Good" drawing
A drawing is **good** when:
- all xrefs are found and loaded, and
- it has a title block.

Good drawings are saved in `<Discipline>\DWG\` and printed (§10).
Drawings with unresolved xrefs are saved but **not printed**, and flagged.

---

## 9. DWG vs PDF/TIFF of the same drawing

| Situation | Result |
|---|---|
| DWG good, its date/revision same as or newer than the PDF | DWFx made from DWG, **PDF deleted** |
| PDF has a **newer** date/revision than the DWG | PDF is more reliable: PDF kept in `Scannede Filer`, DWG to `00 Udgået`, no DWFx |
| DWG fails (no title block / can't be assembled) | PDF kept in `Scannede Filer` |
| PDF can't be read (image only, no text) | keep both, **flag** |
| Drawing exists **only** as PDF and/or TIFF | `Scannede Filer` |

TIFF files always go to `Scannede Filer`.
The PDF is only deleted **after** the DWFx was successfully created.

---

## 10. Printing to DWFx

- Printer: AutoCAD's **DWFx ePlot (XPS Compatible)**.
- Scale: **1:100** (drawing units = mm).
- Output: `<Discipline>\DWFx\<same name as DWG>.dwfx`.
- Paper choice:
  1. use the drawing's existing paper layout if it fits at 1:100
  2. otherwise the first of **A3 → A2 → A1 → A0** where the drawing fits at 1:100
  3. doesn't fit A0 → not printed, **flag**
- DWFx only (no PDF output).

---

## 11. Drawing list (`00 Tegningsliste`)

The existing Excel file holds all drawing numbers and their metadata.
- A **backup** is made before any change.
- Rows are updated / added from the title block data of the drawings in the
  output.
[HOME: file name pattern, sheet and column layout]

---

## 12. Report

One report per run (Excel + readable summary), listing per file:
source path → output path, action (kept / moved / superseded / deleted /
discarded / printed), reason, and flags.

Separate sections for:
- flagged items needing a human decision
- misplaced files awaiting confirmation
- discarded and deleted files
- connected buildings list
- files whose units were changed

---

## 13. To fill in at home

- [ ] Source archive path and output path
- [ ] `accoreconsole.exe` path / AutoCAD version
- [ ] Title block block name(s) and attribute tags (drawing no., revision, date, building, discipline)
- [ ] Room number format
- [ ] `00 Tegningsliste` Excel layout
- [ ] Revision naming in file names (e.g. `_rev C`, `-B`)
- [ ] First test run on one building
