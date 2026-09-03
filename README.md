# mdrite

A small DOS/FreeDOS markdown writer, inspired by [ArtfulType](https://github.com/ActionRetro/ArtfulType)
for 68k Mac.

## Build

Open Watcom:

```
wcl -0 -ml -bt=dos mdrite.c -fe=mdrite
```

(no ".exe" after `-fe=` -- `wcl` appends it itself)

## Run

```
mdrite.exe [filename]
```

## Keybindings

- **Arrows, Home, End, PgUp, PgDn** -- move cursor
- **Shift + any of the above** -- select text (works across lines);
  releasing Shift keeps the selection until you move without Shift,
  type, or press Esc
- **Enter / Backspace / Del** -- edit text
- **Ctrl+S** Save &nbsp; **Ctrl+O** Open &nbsp; **Ctrl+N** New &nbsp; **Ctrl+A** Save As
- **Ctrl+F** Find &nbsp; **F3** Find Next &nbsp; **Ctrl+G** Go To Line
- **Ctrl+Z** Undo (last edit only)
- **Ctrl+C** Copy &nbsp; **Ctrl+X** Cut &nbsp; **Ctrl+V** Paste
  - Cut/Copy need an active selection ("Nothing selected." in the
    status bar otherwise). Paste replaces an active selection if
    there is one, same as everywhere else; plain typing does NOT
    yet replace a selection the same way -- worth knowing until
    that's unified. One clipboard slot, no history.
- **F2** Toggle Writer view / raw Markdown view
- **F4** Toggle vim-lite keymapping on/off (also under View)
- **Alt+X** Quit (confirms if there are unsaved changes) -- moved
  here from Esc to match the WordStar/early-DOS-editor convention
  of Alt+X for eXit
- **Alt+F / Alt+E / Alt+S / Alt+V** open the File / Edit / Search /
  View pull-down menu on the bottom bar. Arrows move within it,
  Left/Right switch menus, Enter runs the selected item, Esc closes
  it. Every menu item just calls the same function its shortcut
  does -- the menu is a second way in, not a separate code path.
- **Vim-lite mode** (F4 to toggle, off by default). Starts in Normal
  sub-mode:
  - `h`/`j`/`k`/`l` move, `0`/`$` start/end of line, `i` insert
    (before cursor), `a` insert (after cursor), `x` delete char,
    `dd` delete line, `u` undo, `:` opens a command line
    (`:w` `:q` `:wq` `:q!`).
  - Esc in Insert sub-mode returns to Normal.
  - Ctrl-shortcuts (including Copy/Cut/Paste), Enter, and Backspace
    keep working the same in both sub-modes. This is a small,
    honestly-scoped subset -- no word motions (`w`/`b`/`e`), no
    vim-style visual-mode selection (Shift+arrows works in vim mode
    too, just not `v`/`V`), no yank/paste registers, no counts
    (`3dd`), no macros. See [Known Limitations](#known-limitations)
    below for what's still missing, vim-specific and otherwise.
- **Esc**, outside of vim mode and menus: clears an active selection
  if there is one; otherwise it's a no-op now that quitting has its
  own key (Alt+X).

## Supported Markdown (Writer view)

- `**bold**`  `*italic*`  `` `code` ``  `~~strikethrough~~`
- `#` / `##` / `###` heading (background-highlighted, one style for
  all levels for now -- per-level styling is an easy follow-up)
- `>` blockquote (background-highlighted, whole line, no nested
  inline styles inside it yet)
- `[link text](url)` -- url is hidden, only the label shows,
  underline-style color instead of a real underline
- `- list item` (hyphen bullets only -- asterisk bullets would
  collide with italic's `*` in a simple single-pass scanner)
- `---` on its own line -- full-width horizontal rule

## Known Limitations

Text-mode limits, on purpose:

- No real bold weight, italic slant, or underline glyph exists in a
  fixed 8x16 text-mode font -- everything above is color-coded
  instead. Headings/blockquotes get a background fill since that
  reads clearly without needing an actual underline attribute.
- Each screen cell has exactly one attribute byte, so if bold and
  italic are both "on" at the same spot, only one color wins
  (code > strikethrough > bold > italic > normal). Can't stack
  colors in 16-color text mode.

Not implemented yet:

- Zoom, true reflowed word-wrap, Replace, and multi-level undo are
  each their own small project.
- A selection deleted or replaced across multiple lines isn't
  undoable yet, for the same single-line-undo reason line
  splits/merges aren't.
- No word motions (`w`/`b`/`e`), vim-style visual-mode selection,
  yank/paste registers, counts (`3dd`), or macros in vim-lite mode.
