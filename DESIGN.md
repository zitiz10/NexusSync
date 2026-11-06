# NexusSync — design system and screens

This file is the visual source of truth. If code and this file disagree, this file wins; if this file is wrong, change it first, then the code.

The approved mockups (Overview and Compare versions) were made before this spec. Screenshots belong in `design/mockups/`. This document already includes the fixes from the Impeccable critique of those mockups (see §12).

## 1. Direction: "Still water"

Calm, spacious, precise. The feeling of a quiet, well-run machine room, not a control panel. Restraint does the work: large light type, generous space, few colors with fixed meanings, one deliberate animation per screen, and data visuals that explain the system rather than decorate it.

The design is in **Operate** mode: success means people understand state and finish tasks quickly. Expression lives in precise details, not effects.

Three rules decide most questions:
1. **Color has meaning.** Blue always means "already stored, shared". Peach always means "new, sent over the network". Nothing else uses these two colors.
2. **Every number is real** and comes from the server.
3. **One orchestrated motion moment per screen.** Everything else is a quiet transition.

## 2. Tokens

Put these in `web/src/styles/tokens.css`. Components reference tokens only.

```css
:root {
  color-scheme: light;

  /* surfaces */
  --bg:            #EEF1F0;   /* page ground, mist */
  --surface:       #F7F9F8;   /* panels, drawers, tray */
  --surface-hover: #E3E9E9;   /* row hover, subtle fills */
  --surface-press: #DFE6E6;   /* segmented control track, pressed */
  --line:          #DCE3E3;   /* hairline separators */
  --line-strong:   #B9C6C9;   /* input borders, secondary button border */

  /* text */
  --ink:    #17242B;          /* primary text, primary button */
  --ink-2:  #4A5A61;          /* secondary text */
  --ink-3:  #55646B;          /* small meta text (passes 4.5:1 on --bg) */
  --on-ink: #F4F6F5;          /* text on --ink */

  /* meaning */
  --shared:      #2C5878;     /* already stored / reused / shared */
  --shared-soft: #6C90A9;
  --new:         #E9B08B;     /* new / sent over the network */
  --ok:          #4C8A68;     /* online, verified */
  --warn:        #8A5A12;     /* warning text */
  --warn-bg:     #F3E6CF;
  --danger:      #B5473A;     /* errors, destructive */
  --danger-bg:   #F4DEDA;

  /* block-field share levels (written once -> shared by 5+) */
  --lvl-1: #C4D1D6;           /* darkened from the mockup for contrast */
  --lvl-2: #A7BECB;
  --lvl-3: #6C90A9;
  --lvl-4: var(--shared);
  --tile-empty: transparent;  /* empty state draws outline with --line-strong */
  --v1-block:   #C9D5D9;      /* compare: version 1 blocks at rest */
  --v1-dim:     #DFE5E6;

  /* type */
  --font-sans: "Geist", system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  --font-mono: "Geist Mono", ui-monospace, "SFMono-Regular", Consolas, monospace;
  --fs-display: clamp(44px, 6.4vw, 92px);
  --fs-h1:      clamp(40px, 5vw, 72px);
  --fs-h2-big:  clamp(30px, 3.4vw, 46px);
  --fs-h2:      22px;
  --fs-lead:    18px;
  --fs-body:    16px;
  --fs-ui:      15px;
  --fs-small:   14px;
  --fs-meta:    13px;

  /* space (4 px base) */
  --s-1: 4px;  --s-2: 8px;  --s-3: 12px; --s-4: 16px; --s-5: 20px; --s-6: 24px;
  --s-8: 32px; --s-10: 40px; --s-12: 48px; --s-14: 56px; --s-18: 72px; --s-24: 96px;
  --gutter: clamp(20px, 4vw, 56px);
  --max-w: 1280px;

  /* shape */
  --r-tile: 3px;   /* block tiles */
  --r-block: 4px;  /* compare blocks */
  --r-ctl: 10px;   /* buttons, inputs, segmented control (no pills) */
  --r-panel: 20px; /* large panels */
  --r-tray: 14px;  /* upload tray, drawer edge, menus */

  /* elevation: only floating things get a shadow */
  --shadow-float: 0 1px 2px rgba(23,36,43,.06), 0 12px 32px rgba(23,36,43,.10);

  /* motion */
  --ease-out:  cubic-bezier(.2,.7,.2,1);
  --ease-emph: cubic-bezier(.16,.8,.24,1);
  --t-micro: 200ms;  --t-state: 320ms;  --t-enter: 900ms;
}

@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { /* same values as [data-theme="dark"] below */ }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --bg: #10181C; --surface: #172127; --surface-hover: #1E2A31; --surface-press: #24323A;
  --line: #24323A; --line-strong: #3A4B54;
  --ink: #E6ECEC; --ink-2: #A9B7BC; --ink-3: #98A8AE; --on-ink: #10181C;
  --shared: #7FA8C6; --shared-soft: #527C99; --new: #E0A57F;
  --ok: #6FB08C; --warn: #E2B66A; --warn-bg: #2E2614; --danger: #E07E70; --danger-bg: #33201D;
  --lvl-1: #2A3B44; --lvl-2: #36546A; --lvl-3: #527C99; --lvl-4: var(--shared);
  --v1-block: #34454E; --v1-dim: #222F36;
}
```

**Tokens added in M8** (implementation in `web/src/styles/tokens.css`; values for both themes live there):
- `--line-hover` (#8FA2A8, the secondary-button hover border named in §6);
- `--line-input` / `--line-input-hover` (light #7E8D92 / #6E7D82, dark #5E6E76 / #728188). Input borders need 3:1 against both the field fill and the page (WCAG 1.4.11; the contrast floor below). `--line-strong` is 1.7:1, so it stays for hairline-strength borders only;
- `--shared-ring` (the 20% focus ring of §6);
- fixed sizes: `--ctl-h` 44, `--avatar` 32, `--dot` 8, `--spinner` 14, `--icon` 18, `--auth-w` 400, `--menu-w` 240, `--calm-w` 560;
- `--fs-wordmark` 19, `--r-round` 50% (avatar and status dot only), `--t-pulse` 1200 ms, `--t-spin` 800 ms, `--offline-dim` 0.6;
- a dark `--shadow-float` with deeper black, since the light one disappears on `--bg` dark.

**Tokens added in M9** (`web/src/styles/tokens.css`): `--tray-w` 420, `--drawer-w` 480, `--bar-h` 6 and `--r-bar` 3 (dedup bar), `--r-row` 12 (row hover), `--tile-gap` 4 (2 on phones), `--tile-outline` 1.5 and `--tile-dim` 0.22 (linked hover), the §9 timings as `--t-tile-enter` 1100 ms, `--stagger-col` 26 ms, `--stagger-row` 38 ms, `--t-tile-hover` 380 ms, `--t-tile-pulse` 600 ms, `--t-toast` 6 s, and layout widths `--field-w` 480, `--side-w` 360, `--col-size`, `--col-date`, `--col-bar`, `--col-actions`.

Light mode is the default and the projector mode. Dark mode follows the system unless the user picks a theme in the user menu (stored in `localStorage`). Copy the dark values into the `prefers-color-scheme` block when implementing.

Contrast floor: body and meta text at least 4.5:1 against the surface it sits on; graphical elements that carry meaning (tiles, bars, blocks, focus rings) at least 3:1 against their neighbors.

## 3. Typography

- **Geist** for everything; **Geist Mono** only for fingerprints, offsets, LSNs, and the engine event log. Both are OFL-licensed; self-host the woff2 files in `web/public/fonts/` with `font-display: swap`. No runtime Google Fonts (lab networks may have no internet).
- Global `font-feature-settings: "tnum" 1` so numbers align.
- Display (Overview headline): `--fs-display`, weight 300, line-height 1.0, letter-spacing -0.035em, `text-wrap: balance`.
- Page title (h1 on other screens): `--fs-h1`, weight 300, line-height 1.02, letter-spacing -0.035em.
- Big summary sentence (Compare): `--fs-h2-big`, weight 300, line-height 1.1, letter-spacing -0.025em.
- Section title (h2): `--fs-h2`, weight 400, letter-spacing -0.01em.
- Lead paragraph: `--fs-lead`, line-height 1.6, `--ink-2`, max 34em.
- Body 16, UI controls 15, small 14, meta 13 (`--ink-3`).
- No all-caps labels, no letter-spaced eyebrows, no italics for emphasis.

Alternative face, if the team wants to swap later: Hanken Grotesk (same weights). Swap only `--font-sans`.

## 4. Layout

- Content column max `--max-w`, centered, side padding `--gutter`.
- App shell: header 88 px tall area (32 px vertical padding), then the screen.
- Sections separated by a 1 px `--line` rule and 48 px of space, not by boxes. Panels (`--surface`, `--r-panel`) only where content is a single visual object (the compare strip, the sign-in form).
- No card grids. Lists are rows with hairline separators.
- Breakpoints: desktop ≥ 1200 px, compact 768–1199 px (projectors, small laptops), phone < 768 px. Layout uses `flex-wrap` and `auto-fit` grids so columns stack naturally; nothing scrolls sideways except the compare strip and wide tables inside their own `overflow-x: auto` container.

## 5. Brand mark

Four rounded squares: top-left filled ink, top-right and bottom-left outlined, bottom-right filled `--shared`. It reads as "four blocks, one of them shared". Use as the favicon (SVG) too.

```svg
<svg width="26" height="26" viewBox="0 0 26 26" aria-hidden="true">
  <rect x="1"  y="1"  width="11" height="11" rx="2.5" fill="var(--ink)"/>
  <rect x="14" y="1"  width="11" height="11" rx="2.5" fill="none" stroke="var(--ink)" stroke-width="1.5"/>
  <rect x="1"  y="14" width="11" height="11" rx="2.5" fill="none" stroke="var(--ink)" stroke-width="1.5"/>
  <rect x="14" y="14" width="11" height="11" rx="2.5" fill="var(--shared)"/>
</svg>
```

Wordmark "NexusSync" in Geist 500, 19 px, letter-spacing -0.01em, next to the mark with 12 px gap.

No photos, illustrations, stock imagery, or decorative graphics anywhere. The data visuals are the imagery.

Icons: `lucide-react`, size 18, `strokeWidth={1.5}`, color inherits. Used sparingly: upload, download, git-compare, trash-2, search, more-horizontal, check, alert-triangle, x, log-out, sun, moon.

## 6. Components

**Buttons.** Height 44 px, padding 0 20 px, `--r-ctl`, `--fs-ui` weight 500.
- Primary: `--ink` background, `--on-ink` text; hover background `--shared` over `--t-micro`.
- Secondary: transparent, 1 px `--line-strong` border, ink text; hover fills `--surface-hover` and border darkens to `#8FA2A8`.
- Danger: `--danger` text and border; hover fills `--danger-bg`.
- Disabled: 45% opacity, no hover. Loading: label stays, a 14 px spinner replaces the icon slot, width does not change.
- No arrows or chevrons on buttons. Labels are verbs: "Upload a file", "Download", "Delete", "Run cleanup".

**Nav links.** Text links, 15 px, `--ink-2`, padding 12 px 16 px. Hover: color to ink and a 1 px underline that grows left to right over 320 ms `--ease-out` (background-size technique). Current page: ink text with the full 1 px underline, `aria-current="page"`. No pill backgrounds.

**Inputs.** Height 44 px, `--r-ctl`, 1 px `--line-input` border (hover `--line-input-hover`), `--surface` fill, 15 px text. Focus: border `--shared` plus 3 px ring of `--shared` at 20% opacity. Error: border `--danger` and a 13 px message below in `--danger`. Labels above inputs, never placeholder-only.

**Segmented control** (Compare mode). Track `--surface-press`, 4 px padding, `--r-ctl`. Selected segment: `--ink` fill, `--on-ink` text. Unselected hover: `--bg` fill, ink text. `aria-pressed` on each button.

**Rows.** 20 px vertical padding, 1 px `--line` bottom border. Hover: `--surface-hover` fill, border fades, 12 px radius, the row extends 16 px into the gutter on both sides so text does not move.

**Dedup bar.** 6 px tall, 3 px radius, track is `--new` (sent), fill is `--shared` (already stored) from the left. Under it, a 13 px note such as "94% already stored, 1.1 MB sent".

**Upload tray.** Fixed bottom-right, 420 px wide (full width minus 16 px on phones), `--surface`, `--r-tray`, `--shadow-float`. Collapsible. See §8.4.

**Drawer.** Right side, 480 px, `--surface`, slides in 320 ms `--ease-emph`, closes on Escape or outside click. Used for file details.

**Toast.** Bottom-left, `--ink` background, `--on-ink` text, `--r-ctl`, 14 px. Auto-dismiss in 6 s. Used for "Deleted DBMS_notes.csv. Undo".

**Status indicator.** 8 px dot plus text. Live: `--ok` dot, "Live, updated 3 s ago". Reconnecting: `--warn` dot pulsing at 1.2 s, "Reconnecting". Offline: `--danger` dot, "Offline".

**Banner.** Full-width strip under the header for system states (offline, recovering). `--warn-bg` or `--danger-bg`, 14 px text, one action at most. On `--danger-bg` the text is `--ink`: `--danger` on `--danger-bg` is 4.15:1, below the 4.5:1 floor. The same applies to form-level error notes.

**Charts.** Hand-built SVG, no chart library. Sparklines 120×28, 1.5 px line in `--shared`, no axes. Bar histograms with 2 px gaps. Every chart has a text equivalent nearby.

## 7. App shell

```
[mark] NexusSync     Overview  Files  Compare  Engine*  Users*        ● Live, updated 2 s ago   (R) Riya ▾
                                                       (* admin only)
```

- Left: mark and wordmark (links to Overview).
- Center-left: nav. Students see Overview, Files, Compare. Admins also see Engine and Users. Engine is its own page (fixes the mockup inconsistency).
- Right: status indicator, then the user menu: initial in a 32 px circle (`--surface-press` fill) and display name; menu holds Theme (Light, Dark, System) and Sign out.
- Drag-and-drop anywhere in the app opens the upload tray with the dropped files.
- Phone (< 768 px): row one holds the brand, the status dot with its state word ("Live"), and the avatar alone; row two holds the nav, one line with tighter link padding. The "updated 3 s ago" detail and the display name are hidden.
- A "Skip to content" link is the first Tab stop.

## 8. Screens

### 8.1 Sign in and join

Centered column, max 400 px, vertically centered in the viewport.

```
[mark] NexusSync

Sign in to the lab store            (h1-size, weight 300)

Username        [__________]
Password        [__________]
[ Sign in ]  (primary, full width)

New here? Create an account with your lab's join code.   (link)
```

Join form: Display name, Username (letters, digits, dot, dash, underscore; 3–32; stored lower-case, SPEC §13), Password (minimum 10 characters, show/hide toggle), Join code. Each field's rule stays visible as a hint; an error under it says what went wrong. Errors appear inline under the relevant field, for example "That join code is not valid. Ask the lab admin for the current one." Rate-limit message: "Too many attempts. Try again in a minute."

### 8.2 Overview

Students see their own and lab-shared files in Recent uploads; admins see everyone's.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ header                                                                       │
├──────────────────────────────────────────────────────────────────────────────┤
│ 214.6 GB uploaded.                         ▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪ │
│ 61.3 GB on disk.                           ▪▪▪▪▪▪▪▪▪▪ block field 32 × 12 ▪▪ │
│                                            ▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪▪ │
│ Repeated blocks are stored once and        Each tile is about 160 MB of      │
│ shared between files. 153.3 GB of writes   stored blocks.                    │
│ skipped, 148.9 GB never sent over Wi-Fi.   □ written once □ 2 □ 3–4 ■ 5+     │
│ [Upload a file]  [Compare two versions]                                      │
├──────────────────────────────────────────────────────────────────────────────┤
│ Recent uploads                      See all files │ Engine (admin) / Your    │
│ name / who, when │ size │ dedup bar + note        │ storage (student)        │
│ ...                                               │ key-value list           │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Headline.** "{logical} uploaded. {physical} on disk." built from `/api/stats`. Lead paragraph names writes skipped (logical − physical) and network bytes saved (both are real and different).

**Headline size beside the field (M9).** At desktop widths the text column next to a 480 px field is about 630 px, and `--fs-display` (92 px at 1440) broke "9.7 MB uploaded." over two lines. In the two-column hero the display size is therefore `min(--fs-display, 10cqi)` of its column (about 63 px at 1440), so each sentence stays on one line as in the mockup. Stacked layouts (compact, phone) keep `--fs-display`. Numbers and units are joined by a no-break space everywhere ("5.7 MB" never wraps).

**Block field.** 32 × 12 = 384 square tiles, 4 px gap, `--r-tile`. Each tile is an equal slice of the physical store in storage order; its shade is the mean share count of the blocks in that slice: 1 → `--lvl-1`, 2 → `--lvl-2`, 3–4 → `--lvl-3`, 5+ → `--lvl-4`. Caption states the tile size from real data ("Each tile is about 160 MB of stored blocks"). Legend below the caption.

**Linked hover (fixed per critique).** Hovering a row in Recent uploads requests `/api/blockmap?file=ID`. Tiles containing that file's blocks keep their color and gain an inset 1.5 px `--ink` outline; every other tile drops to 22% opacity. Transition 380 ms. The caption switches to one line, for example "Tiles holding blocks of CV_dataset_train.csv. All new data, each block written once." Leaving the row restores everything. This must be clearly visible for files whose blocks are all `--lvl-1`.

**Live updates.** When `upload.progress` events report new blocks, the tiles they landed in pulse once (scale 1 → 1.15 → 1, 600 ms `--ease-emph`). Headline numbers tween to new values over 400 ms only when they change; never count up on page load.

**Recent uploads.** Up to 8 rows: name (wraps, never truncates mid-word on phones), meta "Kavya, 11 min ago", size, dedup bar with note. In-progress uploads appear at the top with the same row shape and a live bar (see §8.4).

**Right column.** Admin: Engine summary (write-ahead log, block index, containers, waiting for cleanup, integrity scan) with a link "Open engine". Student: "Your storage": your files, total size of your files, how much of it was already on the server when you uploaded.

**Empty state** (fresh server). Headline "Nothing stored yet." Lead "Upload a file to begin. Repeated blocks will be stored once and shared." Primary button. Block field shows 384 tiles drawn as 1 px `--line-strong` outlines. Recent uploads shows one quiet line: "Uploads will appear here."

### 8.3 Files

```
Files                                                   [Upload a file]
[ Search files            ]   Sort: Newest ▾   Show: Mine | Shared with lab

name  v2                      owner*   size     dedup bar + note          date      ⋯
```

- Row hover reveals actions on the right: Download, Compare with previous (only if a previous version exists), More (Share with lab toggle, Delete).
- Clicking a row opens the **file drawer**: name, versions list (each with date and size, any two selectable → opens Compare), size, uploaded by and when, blocks total, blocks unique to this file, blocks shared with other files, network bytes sent at upload, chunking method, whole-file SHA-256 in Geist Mono with a copy button (used in the demo to verify downloads), Download and Delete buttons.
- **Delete** asks inline in the drawer: "Delete DBMS_notes.csv? Blocks shared with other files are kept." Confirm sends nothing yet; a toast "Deleted DBMS_notes.csv. Undo" appears and the request is sent after 6 s unless Undo is pressed.
- Empty state: "No files yet. Drop a file anywhere on this page to upload it." Search with no results: "No files match "{query}"."
- **Show (M9):** three choices, All | Mine | Shared with lab. "All" is what an admin needs (every file) and what a student usually wants (theirs plus shared). The two in the sketch alone could not show both at once.

### 8.4 Upload flow and tray (new; fixes critique P1)

Starting an upload opens the tray. Each upload moves through visible phases:

1. **Reading** — "Fingerprinting blocks, 38%". The browser chunks and hashes in a Web Worker.
2. **Checking** — "1,482 blocks. Asking the server which it already has."
3. **Sending** — "1,479 already stored. Sending 3 new blocks, 12 KB." Dedup bar fills live; transfer rate shown when above 1 s.
4. **Verifying** — "Server is checking the whole file."
5. **Done** — "Stored. 12 KB sent instead of 5.8 MB." Row then settles into Recent uploads.

Failure states, each with a plain reason and one action:
- Lost connection: "Connection lost. Retry" (retry resumes; blocks already sent are not resent).
- Hash mismatch: "The server rejected 1 block that did not match its fingerprint. Retry".
- Disk full: "The server is out of space. Ask the lab admin."
- Session expired: the app returns to sign-in with "Your session ended. Sign in again" (§8.8). The upload keeps its file, and once the same person is signed in again, the tray row reads "Your session ended before the upload finished." with Retry (M9). If a different person signs in on that PC, the upload is dropped, so it can never go into the wrong account.
- When every upload has finished, the tray folds to its header after 4 s and the files settle into Recent uploads. It can be closed only when nothing has failed.
- Cancel is always available until Verifying.

### 8.5 Compare versions

```
Compare two versions                                  [ Fixed 4 KB blocks | Content-defined ]
Version 1 [ DBMS_notes.csv  v1 ▾ ]   Version 2 [ DBMS_notes.csv  v2 ▾ ]

┌ panel ─────────────────────────────────────────────────────────────────────┐
│ Version 1   ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬ (window of 32 blocks)                  │
│ Already on server                                                          │
│ Version 2   ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬                                       │
│ Select a block                                                             │
│ whole file  ════════════════════════════════════  (minimap, click to move) │
│ Showing blocks 120–151 of 1,482      ■ already stored  ■ new, sent          │
└────────────────────────────────────────────────────────────────────────────┘

1,479 of 1,482 blocks were already on the server.     Block 131 of 1,482
12 KB sent instead of 5.8 MB.                          Status, SHA-256, size,
Explanation paragraph for the selected mode.           stored in, shared by
```

- **Defaults (fixed per critique):** opens in **Fixed 4 KB blocks** mode so the toggle reveals the improvement. Pickers default to the latest file that has a previous version, comparing its last two versions. Users can pick any two of their own or lab-shared files.
- **Window:** 32 blocks, starting 3 blocks before the first difference. The minimap is an 8 px bar of the whole file, each block a sliver colored blue or peach, with the current window outlined; clicking moves the window.
- Block widths are proportional to block size, minimum 10 px.
- **Hover** a version 2 block: if reused, its partner in version 1 turns `--shared` and the others dim to `--v1-dim`; the version 1 label reads "Match for block 131". If new, all version 1 blocks dim and the label reads "No matching block". Clicking selects a block for the details panel.
- **Details panel** for stored blocks: status, SHA-256 (Geist Mono, first 16 and last 8 hex characters with a copy button for the full value), size in bytes, "Container 412, offset 1,236,992", "Used 6 times across stored files" (see the M10 note below). In fixed mode, blocks are computed for comparison only, so the panel says "Not stored. NexusSync stores content-defined blocks; this fixed-size view is calculated for comparison."
- **Explanation copy.** Fixed: "Every block is exactly 4 KB. The edit pushes all later data along, so every boundary after it lands somewhere new and later blocks stop matching." Content-defined: "Boundaries are chosen by a rolling hash over the content itself, not by position. After the edit the cuts fall in the same places again, so only the blocks around the edit change."
- Empty state: "Upload a file and then an edited copy to compare them here."
- **Copy as built (M10, D-043, D-045).**
  - Summary: "210 of 211 blocks were already stored in version 1." (content-defined) or "122 of 245 blocks match version 1." (fixed), then "New in version 2: 4.7 KB of 977 KB." Compare measures version 2 against version 1. The real upload cost, which other files can lower, is in the Files drawer.
  - Fixed-mode blocks are calculated, not stored, so their legend and labels read "Matches version 1" / "Different from version 1". "Already stored" / "New, sent" appear in content-defined mode only. A grey "Version 1 block" swatch completes the legend.
  - Hover label: "Matches version 1 block 104" (the version 1 number), and the hovered block gets an ink ring. At rest the line reads "Hover a version 2 block to find its match".
  - Details: "Used 6 times across stored files", since the index keeps a reference count and one file can use a block twice.
  - The fixed explanation is conditional: "When an edit inserts or removes bytes, all later data shifts along…".
  - Keyboard: one Tab stop per strip, arrows move, Escape clears the selection. The minimap is a slider (arrows, Page Up/Down, Home/End) with a 24 px hit area.
  - Phones show 20 blocks per window, so both strips fit without scrolling apart.

### 8.6 Engine (admin)

The systems showcase. Calm, dense, readable. Two columns on desktop, one on compact.

Sections, each an h2 with a key-value list, a small visual where it helps, and at most one action:
- **Server** — version, uptime, data directory, chunking method and parameters, LAN addresses with a QR code for each ("Open on another laptop"), current join code with Rotate.
- **Write-ahead log** — current LSN, records since checkpoint, WAL size, last checkpoint time and duration. Action: Checkpoint now.
- **Block index** — entries, global depth, buckets, average bucket fill, buffer pool hit rate with a 60 s sparkline, dirty pages. One-line explanation: "Extendible hashing: the directory doubles when a bucket overflows, so lookups stay one page read."
- **Containers** — count, total size, live-data histogram (how full each container is). Reclaimable space. Action: Run cleanup.
- **Integrity** — last scan time, blocks verified, failures (0 shown in `--ok`). Action: Scan now (shows progress).
- **Recovery history** — date, records replayed, transactions redone, duration, result.
- **Event log** — the last 200 engine events in Geist Mono 13 px: time, type, details. Auto-scrolls unless the user has scrolled up.

### 8.7 Users (admin)

List of users: display name, username, role, files, stored size, last seen, status. Actions: Make admin, Disable, Enable. Join code shown at the top with Copy and Rotate; rotating warns "Anyone with the old code can no longer join."

### 8.8 System states (fixes critique P2)

- **Offline:** banner "Lost connection to the lab server. Retrying in 4 s." Data on screen stays visible but dims to 60%; the status indicator turns red. Reconnect with backoff 1, 2, 4, 8, max 10 s. On reconnect, refetch stats and lists.
- **Recovering:** `/api/health` reports `recovering`; full-page calm state with the mark, "The server is recovering after an unexpected stop", and live progress from the server: "Replaying the log, 3 s so far." (The server replays inside one call and reports elapsed time, not record counts; counts arrive afterwards in the `engine.recovery` event for the Engine screen.) When done: "Recovery complete. Every committed change is back in place." and the app reloads data. (Changed in M8 from "All files checked": recovery replays the log, it does not check files; that is `--fsck`.)
- **Unreachable on first load:** before the app knows who is signed in, a calm page "Cannot reach the lab server", what to check, and the retry countdown with "Retry now". Once signed in, the offline banner is used instead.
- **Session ended:** redirect to sign-in with "Your session ended. Sign in again."
- **Sign out while offline:** refused with "Signing out needs the server. Try again when the connection is back." The session cookie is HttpOnly, so only the server can end it; a local-only sign-out would leave the next person at a shared lab PC signed in.
- **Loading:** content-shaped skeletons in `--surface-hover` with no shimmer animation. Never spinners on whole pages.
- **Errors in lists:** inline line with a Retry link, never a modal.

## 9. Motion

| Where | What | Timing |
|---|---|---|
| Overview load | Tiles settle in: from opacity 0, translateY(-8px), scale(.86) | 1100 ms `--ease-out`, delay col × 26 ms + row × 38 ms |
| Overview live | Tile pulse where new blocks land | 600 ms `--ease-emph`, once |
| Overview numbers | Tween to new value on change only | 400 ms |
| Compare load and on mode toggle | Version 1 blocks wipe in left to right (scaleX from left) | 650 ms `--ease-out`, 22 ms stagger |
| | Version 2 reused blocks rise 16 px into place | 900 ms `--ease-emph`, start 280 ms, 24 ms stagger |
| | Version 2 new blocks arrive last, after a pause | start after reused + 200 ms, 40 ms stagger |
| Compare block width change | flex-grow transition | 600 ms `--ease-out` |
| Hover: nav underline, buttons, rows, tiles, v1 partners | Color, opacity, background-size | 200–380 ms |
| Drawer, tray | Slide and fade | 320 ms `--ease-emph` |

Rules: animate only `opacity`, `transform`, `background-color`, `background-size`, `flex-grow`. Replaying the Compare entrance happens on mode change only, never on hover or selection. Under `prefers-reduced-motion: reduce`, remove entrances and pulses and keep instant state changes.

## 10. Copy rules

Sentence case everywhere. Units always ("12 MB", "4,096 bytes"), thousands separators, one decimal for GB. Relative times under 24 h ("11 min ago"), dates after. No exclamation marks, no "Oops", no emoji. Error messages say what happened and what to do. Never claim energy or carbon savings.

## 11. Bans

No gradients (including gradient text), glows, glassmorphism, drop shadows on non-floating elements, KPI card grids, pill buttons or pill nav, all-caps eyebrows, emoji, stock images, 3D, parallax, scroll-jacking, smooth-scroll libraries, custom cursors, magnetic buttons, confetti, count-up on load, badges or leaderboards, carousels, modal dialogs for routine actions.

## 12. Critique fixes already folded in

| Critique finding | Where fixed |
|---|---|
| P1 hover highlight invisible for written-once tiles | §8.2 linked hover: outline plus 22% dimming; `--lvl-1` darkened |
| P1 upload moment not designed | §8.4 upload tray with phases and live tiles §8.2 |
| P2 no empty, error, offline, recovering states | §8.2, §8.3, §8.5 empty states; §8.4 failures; §8.8 system states |
| P2 Compare tells the story backwards, no file choice | §8.5 defaults to fixed; version pickers; window and minimap |
| P3 tile unit undefined, lightest shade too faint | §8.2 caption from real data; `--lvl-1` token |
| Engine nav inconsistent | §7 Engine is a page |
| Pill shapes read as template | §2 `--r-ctl` 10 px, §6 nav without pills |
| Tiny last block hard to hover | §8.5 minimum block width 10 px |

## 13. Verification before calling a screen done

1. Matches this document at 1440×900, 1366×768, and 390×844 in light mode; check dark mode once.
2. Every number traced to an API field.
3. Empty, loading, error, and offline states rendered and checked.
4. `/impeccable critique` and `/impeccable audit` run, every P0 and P1 fixed.
5. Keyboard: Tab reaches every control with a visible focus ring; Escape closes drawer and menus.
6. Reduced motion checked.
