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
  &nbsp; **Ctrl+R** Replace
  - Replace asks for a search string, then a replacement, then walks
    matches one at a time: **Y** replaces this one, **N** skips it,
    **A** stops asking and replaces every remaining match, **Esc**
    (or anything else) stops. Works the same in vim mode. Like
    Paste, a replace is a delete+insert pair, so Undo only reverts
    the last character typed by the last accepted replace, not the
    whole operation.
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
  - Motions: `h` `j` `k` `l`, `w`/`b`/`e` and `W`/`B`/`E` word
    motions, `ge`/`gE` back to word end, `0`/`^`/`$` line start
    (first non-blank / column 0) and end, `gg`/`G` top/bottom (both
    take a count line number), `H`/`M`/`L` top/middle/bottom of
    screen, `{`/`}` paragraph back/forward, `%` matching bracket,
    `f`/`F`/`t`/`T` find/till char and `;`/`,` repeat, `g_` last
    non-blank. Any motion (and `dd`/`yy`) takes a count prefix, e.g.
    `3dd`, `5j`, `2w`.
  - Editing: `i`/`a`/`I`/`A` insert variants, `o`/`O` open line
    below/above, `x` delete char, `r` replace one char, `R` Replace
    sub-mode (overwrite instead of insert), `s`/`S` substitute
    char/line, `C`/`D` change/delete to end of line, `J`/`gJ` join
    lines (with/without a space), `u`/`U` undo (single-level, so
    both just undo the last edit), Ctrl+r redo (also single-level).
  - Operators: `d`/`y`/`c` combine with a motion or text object --
    `dd`/`yy`/`cc`, `dw`/`yw`, `d$`, `cw`/`ce` (real vim's cw-acts-
    like-ce quirk included), and `diw`/`daw`/`yiw`/`yaw`/`ciw` word
    objects. `p`/`P` paste after/before (linewise if the last
    yank/delete was a whole line).
  - Visual mode: `v` charwise, `V` linewise, Esc cancels. Inside
    Visual: `d`/`x` delete, `y` yank, `~`/`u`/`U` case toggle/lower/
    upper, `o` swap selection ends, and `iw`/`aw`/`ib`/`ab`/`iB`/`aB`
    text objects (word, `(...)`, `{...}`). Shift+arrows selection
    still works too, just not through `v`/`V`.
  - Scrolling: Ctrl+d/u half-page, Ctrl+e/y one line, `zz`/`zt`/`zb`
    center/top/bottom the viewport without moving the cursor.
  - `:` opens a command line (`:w` `:q` `:wq` `:q!`).
  - Esc in Insert/Replace sub-mode returns to Normal, or cancels
    Visual mode.
  - Ctrl-shortcuts (including Copy/Cut/Paste), Enter, and Backspace
    keep working the same across sub-modes -- Ctrl+r is the one
    exception, rebound to redo in Normal/Visual sub-mode only,
    since that's vim's real redo key; it's still Replace everywhere
    else (Insert sub-mode, or vim mode off).
  - Still missing: multi-level undo/redo (one step only, matching
    the editor's regular Undo), yank/paste registers beyond the one
    clipboard slot, and macros. See [Known
    Limitations](#known-limitations) below for what's still
    missing, vim-specific and otherwise.
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
- fenced code blocks (` ``` ` on its own line, optionally followed by
  a language name) -- rendered in a distinct code color, verbatim,
  with no inline bold/italic/link/list/heading parsing inside the
  fence so code isn't mangled by markdown rules meant for prose

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

- Zoom, true reflowed word-wrap, and multi-level undo are each their
  own small project.
- A selection deleted or replaced across multiple lines isn't
  undoable yet, for the same single-line-undo reason line
  splits/merges aren't.
- Vim-lite: single clipboard slot (no numbered/named registers),
  single-level undo/redo, no macros, no `gj`/`gk` (wrap-row-aware
  movement).
- Code blocks have no syntax highlighting (one flat color for the
  whole fence) and still word-wrap long lines like prose rather than
  horizontal-scrolling, so very long code lines will reflow. The
  opening fence must start at column 0 -- an indented ` ``` ` (e.g.
  inside a list item) isn't recognized, matching CommonMark's rule
  for fences but not yet supporting indented ones.

## License

BSD 3-Clause (see [LICENSE](LICENSE)), with one exception:

- `tests/02story.md` -- "The Light We Make" (c) 2026 by Joash
  Liwanag, licensed separately under
  [CC BY-ND 4.0](http://creativecommons.org/licenses/by-nd/4.0/).
  See the license notice inside that file for details. This work is
  NOT covered by the BSD 3-Clause License above.
