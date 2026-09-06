/*
 * mdrite.c - a small DOS/FreeDOS markdown writer, inspired by
 * ArtfulType (github.com/ActionRetro/ArtfulType) for 68k Mac.
 *
 * See README.md for build/run instructions, the full keybinding
 * list, supported Markdown syntax, and known limitations.
 */

#include <dos.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <bios.h>

/* ---------- program identity ---------- */
/* Bump this by hand before cutting a release build. */
#define MDRITE_VERSION     "v0.1"
#define MDRITE_NAME        "mdrite"
#define MDRITE_DESC        "A small DOS/FreeDOS markdown writer."
#define MDRITE_COPYRIGHT   "Copyright (c) 2026 Joash Liwanag"
#define MDRITE_LICENSE     "Licensed under BSD 3-Clause"
#define MDRITE_REPO_URL    "https://github.com/AJigsawnHalo/mdrite"

/* ---------- screen / buffer constants ---------- */
#define SCREEN_COLS   80
#define SCREEN_ROWS   25
/* Status and menu bars share a single bottom row: normally it shows
 * the status line, and while a menu is open (Alt held) it swaps to
 * showing File/Edit/Search/View instead -- see redraw_screen. That
 * reclaims the row the two used to split, so the editor gets 24
 * lines of text instead of 23. */
#define STATUS_ROW    (SCREEN_ROWS - 1)
#define CMDBAR_ROW    (SCREEN_ROWS - 1)
#define TEXT_ROWS     (SCREEN_ROWS - 1)
#define MAX_LINE_LEN  1024
#define MAX_LINES     2000

/* Max word-wrapped visual rows a single buffer line can occupy in
 * Writer view. Moved up here (was previously defined just above
 * compute_wrap_starts) so it's available to the Line struct below,
 * which now carries a small per-line wrap cache -- see the
 * "wrap_starts/wrap_nstarts/wrap_dirty" comment on Line. */
#define MAX_WRAP_ROWS 16

/* ---------- color attributes ---------- */
#define ATTR_NORMAL      0x07   /* light grey / black */
#define ATTR_BOLD        0x0F   /* bright white / black */
#define ATTR_ITALIC      0x0B   /* bright cyan / black */
#define ATTR_CODE        0x0D   /* bright magenta / black */
#define ATTR_STRIKE      0x0C   /* bright red / black -- color only, no literal line */
#define ATTR_HEAD        0x1E   /* bright yellow / blue background -- H2, kept as
                                    the legacy name/value so anything still
                                    referring to ATTR_HEAD keeps working */
/* Per-level heading colors: same blue background throughout (keeps headings
 * reading as one family), foreground brightness/hue steps down from H1 to H6
 * so level is visible at a glance without needing to count '#'s. */
#define ATTR_HEAD1       0x1F   /* bright white  / blue */
#define ATTR_HEAD2       0x1E   /* bright yellow / blue */
#define ATTR_HEAD3       0x1B   /* bright cyan   / blue */
#define ATTR_HEAD4       0x1A   /* bright green  / blue */
#define ATTR_HEAD5       0x1D   /* bright magenta/ blue */
#define ATTR_HEAD6       0x17   /* light grey    / blue */
#define ATTR_QUOTE       0x5F   /* bright white / magenta background */
#define ATTR_LINK        0x09   /* bright blue / black */
#define ATTR_LISTMARK    0x0A   /* bright green / black, bullet glyph only */
#define ATTR_STATUS      0x70   /* black / light grey */
#define ATTR_STATUS_FLASH 0x4E  /* bright yellow / red -- reserved for an actual error
                                    flash. Routine flashes (Saved., Vim keys ON., etc.)
                                    use the plain ATTR_STATUS background instead, so a
                                    normal confirmation doesn't read as "something broke" */
#define ATTR_CMDBAR      0x30   /* black / cyan */
#define ATTR_CMDBAR_HOT  0x3E   /* bright yellow / cyan -- menu hotkey letter */
#define ATTR_CMDBAR_SEL  0x4F   /* bright white / red -- open/selected menu (kept
                                    away from 0x70 so it doesn't fuse with the
                                    status bar directly above it) */
#define ATTR_POPUP       0x1F   /* bright white / blue -- dropdown body */
#define ATTR_POPUP_HOT   0x1E   /* bright yellow / blue -- dropdown item's mnemonic
                                    letter, same trick as ATTR_CMDBAR_HOT above but
                                    for the popup body's background */
#define ATTR_MODE_NORMAL 0x2F   /* bright white / green  -- vim Normal indicator */
#define ATTR_MODE_INSERT 0x6F   /* bright white / brown(orange) -- vim Insert indicator */

unsigned char far *video = (unsigned char far *) 0xB8000000L;

/* Forward declarations: do_move() (defined in the cursor-movement
 * section) needs scroll_to_cursor() before its own definition
 * further down that same section, and redraw_line_only() (rendering
 * section) falls back to redraw_screen() defined right after it. */
void scroll_to_cursor(void);
void redraw_screen(void);

typedef struct {
    char text[MAX_LINE_LEN + 1];
    int  len;
    /* Cached Writer-view word-wrap row-start offsets for this line,
     * as compute_wrap_starts() would return them. Wrap only depends
     * on a line's own text, never on other lines, scroll position,
     * or the cursor -- so it's safe to compute once and reuse across
     * every redraw until the line is actually edited. wrap_dirty is
     * set by line_mark_dirty() everywhere a line's text[] changes,
     * and cleared by get_line_wraps() the next time it recomputes.
     * Because the cache lives on the Line itself (not indexed by
     * line number), it automatically travels along for free whenever
     * lines are shifted around in doc[] (insert/delete line, undo,
     * etc.) -- nothing else needs to know the cache exists. */
    int  wrap_starts[MAX_WRAP_ROWS];
    int  wrap_nstarts;
    int  wrap_dirty;
} Line;

Line *doc[MAX_LINES];
int  doc_count = 1;

int  cur_line = 0, cur_col = 0;
int  top_line = 0;
int  left_col = 0;
int  modified = 0;
char filename[80] = "";
int  view_mode = 0;     /* 0 = Rich (rendered), 1 = raw Markdown.
                            Future home of a third Graphics mode --
                            keep this as a small int, not a bool,
                            once that lands. */
char status_msg[80] = "Ready.";
char last_search[80] = "";
int  want_quit = 0;

/* Redraw request state for the current keystroke, consumed once at
 * the top of main()'s loop:
 *   0        - nothing changed, skip the redraw entirely (a key that
 *              turned out to be a no-op: an arrow at the edge of the
 *              document, an unrecognized vim command that gets
 *              swallowed, etc.)
 *   1 (full) - repaint the whole viewport + status/menu bar, via
 *              redraw_screen(). Used for anything that can move text
 *              on more than one line: scrolling, inserting/removing
 *              a whole line, merging lines, view/vim mode toggles,
 *              menu actions, load/new file, undo, a selection being
 *              highlighted/cleared (its highlight can span several
 *              lines), etc.
 *   2 (line) - only buffer line dirty_line_no changed, and nothing
 *              else on screen needs to move (its wrapped row count
 *              didn't change and no scrolling was needed) -- redraw
 *              just that line's on-screen row(s) plus the status bar
 *              via redraw_line_only(). This is what makes ordinary
 *              typing/backspacing/deleting a character cheap: it's
 *              by far the most frequent edit, and normally only
 *              touches 1-2 screen rows out of TEXT_ROWS (24).
 * Always request through request_full_redraw()/request_line_redraw()
 * below rather than setting screen_dirty directly, so a "line" request
 * never silently downgrades an already-pending "full" one. */
int screen_dirty = 1;
int dirty_line_no = -1;

void request_full_redraw(void)
{
    screen_dirty = 1;
}

/* Requests a cheap single-line redraw for `line_no`. If a full
 * redraw is already pending this keystroke, or a DIFFERENT line was
 * already requested, this upgrades to a full redraw instead of
 * guessing which lines to combine -- callers that know an edit is
 * confined to exactly one line (see redraw_after_char_edit below)
 * are the only ones that should call this. */
void request_line_redraw(int line_no)
{
    if (screen_dirty == 1) return;
    if (screen_dirty == 2 && dirty_line_no != line_no) { screen_dirty = 1; return; }
    screen_dirty = 2;
    dirty_line_no = line_no;
}

/* optional vim-lite keymapping (off by default, toggle with F4
 * or View > Vim Keys). See the file header for exactly what subset
 * of vim this covers -- it's intentionally not a full emulation. */
int vim_mode = 0;
int vim_insert = 0;    /* 0 = Normal sub-mode, 1 = Insert sub-mode */
int vim_pending = 0;   /* holds the first key of a two-key command, e.g. 'd' of dd */

/* single-level undo: remembers ONE line's previous contents.
 * See the file header for what it does and doesn't cover. */
Line undo_line;
int  undo_line_no = -1;
int  undo_col = 0;

/* ---------- Alt-driven pull-down menu ---------- */
#define MENU_COUNT      5
#define MAX_MENU_ITEMS  5

typedef struct {
    char label[12];
    unsigned char altkey_scan;   /* BIOS scan code for Alt+<letter> */
    char items[MAX_MENU_ITEMS][20];
    int  mnemonic_idx[MAX_MENU_ITEMS]; /* index into items[i] of the
                                           letter highlighted/matched as
                                           that item's mnemonic -- not
                                           always item[i][0] (e.g. "Save
                                           As" highlights the 'A', "Cut"
                                           highlights the 't') */
    int  item_count;
} MenuCategory;

MenuCategory menus[MENU_COUNT] = {
    { "File",   0x21, { "New        ^N", "Open       ^O", "Save       ^S",
                         "Save As    ^A", "Exit       Alt+X" },
                      { 0, 0, 0, 5, 1 }, 5 },
    { "Edit",   0x12, { "Undo       ^Z", "Cut        ^X", "Copy       ^C",
                         "Paste      ^V" },
                      { 0, 2, 0, 0 }, 4 },
    { "Search", 0x1F, { "Find       ^F", "Find Next  F3", "Go To Line ^G",
                         "Replace    ^R" },
                      { 0, 5, 0, 0 }, 4 },
    { "View",   0x2F, { "Toggle View F2", "Vim Keys   F4" },
                      { 0, 0 }, 2 },
    { "Help",   0x23, { "About      F1" },
                      { 0 }, 1 }
};

int menu_open = -1;
int menu_sel  = 0;
int menu_col[MENU_COUNT];

/* Set while Alt is held down with no letter pressed yet -- shows the
 * menu bar as a preview (no category highlighted) without entering
 * menu navigation mode. Kept separate from menu_open, which still
 * means "a specific category is open for arrow-key navigation".
 * Updated by the polling loop in main(); see alt_down() below for
 * why a bare Alt press needs polling instead of the usual blocking
 * key read. */
int alt_held = 0;

/* Status line messages come in two kinds, both funneled through
 * status_msg -- see set_status()/flash_status() below:
 *   sticky: stays put until the next status update (errors, things
 *           the user needs to actually see and act on)
 *   flash:  shown on the plain status background for a few seconds,
 *           then reverts to plain "Ready." on its
 *           own -- or immediately, the moment any real key comes in,
 *           whichever happens first (routine confirmations: Saved.,
 *           Copied., the Vim keys ON/OFF toggle, the startup hint)
 * flash_expire is a BIOS clock tick count (see 0040:006C, ~18.2
 * ticks/sec), armed fresh by each flash_status()/flash_status_for()
 * call. */
int  flash_active = 0;
long flash_expire = 0;

/* Sticky error message that also gets a brief attention-flash: the
 * message itself behaves like set_status() (stays until the next
 * status update), but for the first FLASH_SECS(5) the bar is drawn
 * on ATTR_STATUS_FLASH (bright yellow/red) instead of the plain
 * background, then settles back to plain while status_msg is left
 * untouched -- see flash_error() below. */
int  error_flash_active = 0;
long error_flash_expire = 0;

/* ---------- clipboard ---------- */
/* Flat buffer holding a copy of the selected text, lines joined with
 * '\n'. Filled by cmd_copy, consumed by cmd_paste -- one clipboard
 * slot, no history, like every DOS-era editor's clipboard. */
#define CLIP_MAX 4000
char clipboard[CLIP_MAX] = "";
int  clip_len = 0;

/* ================= selection (raw document coordinates) ================= */

/* A selection is an anchor position plus the live cursor position
 * (cur_line/cur_col) -- both are raw Markdown buffer coordinates
 * (a line index and a column index into doc[line]->text), never
 * screen coordinates. Because of that, the same sel_active/anchor
 * pair is valid and renders correctly in both Raw and Writer view
 * with no translation when the view is toggled with F2. */
int sel_active   = 0;
int anchor_line  = 0, anchor_col = 0;

void sel_clear(void) { sel_active = 0; }

/* Starts a selection anchored at the current cursor position, if one
 * isn't already active. Safe to call on every Shift-held movement --
 * only the first call in an unbroken run of Shift-moves actually
 * sets the anchor; later calls in the same run are no-ops. */
void sel_begin(void)
{
    if (!sel_active) {
        anchor_line = cur_line;
        anchor_col  = cur_col;
        sel_active  = 1;
    }
}

/* Normalizes the anchor/cursor pair into an ordered (start <= end)
 * pair, comparing by line first and then column. */
void sel_bounds(int *sl, int *sc, int *el, int *ec)
{
    if (anchor_line < cur_line || (anchor_line == cur_line && anchor_col <= cur_col)) {
        *sl = anchor_line; *sc = anchor_col;
        *el = cur_line;    *ec = cur_col;
    } else {
        *sl = cur_line;    *sc = cur_col;
        *el = anchor_line; *ec = anchor_col;
    }
}

/* Raw [start,end) column range selected on line_no, clipped to
 * [0, line_len]. Sets *out_start to -1 when line_no isn't touched by
 * the selection at all -- including when the selection is currently
 * empty (Shift has been held but the cursor hasn't actually moved
 * off the anchor yet). Callers treat out_start == -1 as "nothing
 * selected on this row". */
void sel_line_range(int line_no, int line_len, int *out_start, int *out_end)
{
    int sl, sc, el, ec;

    *out_start = -1;
    *out_end   = -1;
    if (!sel_active) return;

    sel_bounds(&sl, &sc, &el, &ec);
    if (sl == el && sc == ec) return;           /* empty selection */
    if (line_no < sl || line_no > el) return;    /* line not touched */

    *out_start = (line_no == sl) ? sc : 0;
    *out_end   = (line_no == el) ? ec : line_len;
    if (*out_start > line_len) *out_start = line_len;
    if (*out_end   > line_len) *out_end   = line_len;
}

/* True if Shift is currently held, checked via the BIOS keyboard
 * flag byte (bit 0 = right shift, bit 1 = left shift) rather than
 * from the key code coming back from _KEYBRD_READ. The dedicated
 * arrow/Home/End/PgUp/PgDn keys report the same scan code whether or
 * not Shift is down, so the flag byte is the only way to tell a
 * plain move from a Shift-extend. */
int shift_down(void)
{
    return (_bios_keybrd(_KEYBRD_SHIFTSTATUS) & 0x03) != 0;
}

/* True if either Alt key is currently held, via the same BIOS flag
 * byte (bit 3). Unlike Shift, a bare Alt press/release generates no
 * entry in the keyboard type-ahead buffer, so _bios_keybrd(_KEYBRD_READ)
 * would just block through it -- the main loop polls this instead of
 * waiting on a keystroke so the menu-bar preview can track Alt being
 * held and released on its own. */
int alt_down(void)
{
    return (_bios_keybrd(_KEYBRD_SHIFTSTATUS) & 0x08) != 0;
}

/* BIOS keyboard ticks (0040:006C) run at ~18.2/sec; this rounds to
 * 18 so callers can just say FLASH_SECS(2) instead of a raw count. */
#define FLASH_SECS(n) ((long) (n) * 18L)

/* Sticky status message: replaces status_msg and stays put until the
 * next set_status()/flash_status() call, drawn on the plain status
 * background. Use for anything the user needs to actually notice and
 * may need to act on (errors, "could not ..." messages). */
void set_status(const char *msg)
{
    strcpy(status_msg, msg);
    flash_active = 0;
    error_flash_active = 0;
    request_full_redraw();
}

/* Flash status message: shown on the plain status background for
 * `ticks` BIOS clock ticks (see FLASH_SECS above), then reverts to
 * plain "Ready." on its own -- or immediately, the
 * moment any real key comes in, whichever happens first. The actual
 * reverting happens in main()'s poll loop and keypress handling. */
void flash_status_for(const char *msg, long ticks)
{
    strcpy(status_msg, msg);
    flash_active = 1;
    error_flash_active = 0;
    _bios_timeofday(_TIME_GETCLOCK, &flash_expire);
    flash_expire += ticks;
    request_full_redraw();
}

/* flash_status_for() with the everyday ~5-second duration. */
void flash_status(const char *msg)
{
    flash_status_for(msg, FLASH_SECS(5));
}

/* Sticky error message with an attention flash: status_msg is set
 * and stays put like set_status() -- it does NOT revert to "Ready."
 * on its own or on the next keystroke -- but the bar's background
 * flashes ATTR_STATUS_FLASH for FLASH_SECS(5) first, to make sure an
 * error actually gets noticed, then settles back to the plain
 * ATTR_STATUS background while the message itself stays put. */
void flash_error(const char *msg)
{
    strcpy(status_msg, msg);
    flash_active = 0;
    error_flash_active = 1;
    _bios_timeofday(_TIME_GETCLOCK, &error_flash_expire);
    error_flash_expire += FLASH_SECS(5);
    request_full_redraw();
}

/* ================= video primitives ================= */

/* Cell value for a glyph+attribute pair, as it sits in video memory:
 * character in the low byte, attribute in the high byte. Writing
 * this as one 16-bit far store is one far-pointer access instead of
 * two -- far writes aren't free on real-mode 8086/286 (segment
 * handling), so halving the count of them helps on every hot path
 * below. */
#define CELL(ch, attr) ((unsigned int) (unsigned char) (ch) | ((unsigned int) (attr) << 8))

/* Row base address, as an unsigned-int (cell-sized) far pointer --
 * indexing into this with [col] lands on the right byte pair without
 * a general multiply (col*2 as a pointer offset compiles to a cheap
 * shift, not a multiply routine, unlike the row*SCREEN_COLS multiply
 * this factors out). Callers that touch many cells in one row should
 * call this ONCE and then index/increment through it, rather than
 * recomputing a row's address (or worse, calling put_char, which
 * redoes the multiply AND a bounds check) for every glyph -- that
 * per-glyph multiply was the actual cost on an 8086/286, where an
 * integer multiply is dozens of cycles, run for every character of
 * every row on every redraw. */
unsigned int far *row_ptr(int row)
{
    return (unsigned int far *) (video + (long) row * SCREEN_COLS * 2);
}

/* Safe, bounds-checked single-glyph write for the scattered,
 * non-hot-loop call sites (menu bar/popup, status bar, prompt line --
 * places drawing a handful of characters at a time, not a whole
 * 80-column row). Hot per-row loops (render_line, render_writer_line,
 * fill_rect) bypass this and write straight through row_ptr()
 * instead -- see their own comments. */
void put_char(int col, int row, char ch, unsigned char attr)
{
    if (col < 0 || col >= SCREEN_COLS || row < 0 || row >= SCREEN_ROWS) return;
    row_ptr(row)[col] = CELL(ch, attr);
}

void put_string(int col, int row, const char *s, unsigned char attr)
{
    int i;
    for (i = 0; s[i]; i++) put_char(col + i, row, s[i], attr);
}

/* Blanks [col, col+w) of `row` with `attr`. Computes the row's base
 * pointer once, then writes through it with a plain incrementing
 * pointer -- no put_char, so no per-cell multiply or bounds check;
 * the clamping below establishes the safe range once, up front,
 * instead of on every cell. */
void fill_rect(int col, int row, int w, unsigned char attr)
{
    unsigned int far *rp;
    unsigned int cell;
    int start = col, end = col + w;
    if (row < 0 || row >= SCREEN_ROWS) return;
    if (start < 0) start = 0;
    if (end > SCREEN_COLS) end = SCREEN_COLS;
    if (start >= end) return;
    rp = row_ptr(row) + start;
    cell = CELL(' ', attr);
    while (start < end) { *rp++ = cell; start++; }
}

void clear_row(int row, unsigned char attr) { fill_rect(0, row, SCREEN_COLS, attr); }

void clear_screen(unsigned char attr)
{
    int r;
    for (r = 0; r < SCREEN_ROWS; r++) clear_row(r, attr);
}

void set_cursor(int col, int row)
{
    union REGS regs;
    regs.h.ah = 0x02;
    regs.h.bh = 0x00;
    regs.h.dh = (unsigned char) row;
    regs.h.dl = (unsigned char) col;
    int86(0x10, &regs, &regs);
}

/* ================= line management ================= */

Line *new_line(void)
{
    Line *l = (Line *) malloc(sizeof(Line));
    l->text[0] = '\0';
    l->len = 0;
    l->wrap_nstarts = 0;
    l->wrap_dirty = 1;   /* nothing cached yet */
    return l;
}

/* Call this on any Line* whose text[] just changed -- invalidates
 * its cached word-wrap so the next get_line_wraps() call for it
 * recomputes instead of serving a stale cache. See the wrap_dirty
 * comment on Line for why this is safe to do liberally: wrap only
 * depends on a line's own text. */
void line_mark_dirty(Line *l)
{
    if (l) l->wrap_dirty = 1;
}

void doc_reset(void)
{
    int i;
    for (i = 0; i < doc_count; i++) free(doc[i]);
    doc[0] = new_line();
    doc_count = 1;
    cur_line = cur_col = top_line = left_col = 0;
    modified = 0;
    undo_line_no = -1;
    sel_clear();
}

/* ================= undo ================= */

void save_undo(int line_no)
{
    if (line_no < 0 || line_no >= doc_count) return;
    strcpy(undo_line.text, doc[line_no]->text);
    undo_line.len = doc[line_no]->len;
    undo_line_no = line_no;
    undo_col = cur_col;
}

void do_undo(void)
{
    if (undo_line_no < 0 || undo_line_no >= doc_count) {
        flash_status_for("Nothing to undo.", FLASH_SECS(2));
        return;
    }
    sel_clear();
    strcpy(doc[undo_line_no]->text, undo_line.text);
    doc[undo_line_no]->len = undo_line.len;
    line_mark_dirty(doc[undo_line_no]);
    cur_line = undo_line_no;
    cur_col = undo_col;
    undo_line_no = -1;
    flash_status("Undid last edit.");
    modified = 1;
}

/* ================= editing ops ================= */

/* Removes the selected text from the document, single-line or
 * spanning several. Leaves the cursor at the (now-collapsed)
 * selection start and clears the selection. Like split_line/
 * delete_current_line, a multi-line delete is a structural change
 * the single-line undo can't represent, so it's invalidated rather
 * than misrepresented. Used directly by cmd_cut, and by cmd_paste
 * so pasting over an active selection replaces it instead of
 * inserting into the middle of it.
 * Returns 1 if it actually deleted something, 0 if it was a no-op
 * (nothing selected, an active-but-empty selection) or it bailed
 * out (cross-line selection too long to merge) -- in the 0 cases a
 * status_msg explaining why has already been set (or left alone,
 * for the plain "nothing selected" no-op), and callers should not
 * clobber it with a blanket success message. */
int sel_delete(void)
{
    int sl, sc, el, ec, i;
    if (!sel_active) return 0;
    sel_bounds(&sl, &sc, &el, &ec);
    if (sl == el && sc == ec) { sel_clear(); return 0; }  /* nothing actually selected */

    if (sl == el) {
        Line *l = doc[sl];
        int n = ec - sc;
        save_undo(sl);
        /* shifts text[sc+n..len] (terminator included) left by n --
         * memmove instead of a per-byte loop, see insert_char's
         * comment for why */
        memmove(l->text + sc, l->text + sc + n, (size_t) (l->len - n - sc + 1));
        l->len -= n;
        line_mark_dirty(l);
    } else {
        Line *startl = doc[sl];
        Line *endl = doc[el];
        int suffix_len = endl->len - ec;
        int shift;
        if (startl->len + suffix_len > MAX_LINE_LEN) {
            flash_error("Selection too long to delete across lines.");
            sel_clear();
            return 0;
        }
        startl->text[sc] = '\0';
        startl->len = sc;
        strcat(startl->text, endl->text + ec);
        startl->len += suffix_len;
        line_mark_dirty(startl);
        for (i = sl + 1; i <= el; i++) free(doc[i]);
        shift = el - sl;
        memmove(&doc[sl + 1], &doc[el + 1], (size_t) (doc_count - el - 1) * sizeof(Line *));
        doc_count -= shift;
        undo_line_no = -1;  /* spans lines: not representable by single-line undo */
    }
    cur_line = sl;
    cur_col = sc;
    sel_clear();
    modified = 1;
    request_full_redraw();
    return 1;
}

/* Returns 1 on success, 0 if the line was already full (status_msg
 * is set to explain why in that case). Ordinary typing ignores the
 * return value; cmd_paste uses it to notice a truncated paste.
 *
 * Does NOT request a redraw itself -- callers decide the scope (see
 * do_insert_char() below, which is what every real key-dispatch site
 * should call instead of this directly; cmd_paste calls this raw and
 * relies on its own trailing flash_status() to trigger one redraw
 * for the whole paste instead of one per character). */
int insert_char(int ch)
{
    Line *l = doc[cur_line];
    if (l->len >= MAX_LINE_LEN) { flash_error("Line full."); return 0; }
    sel_clear();
    save_undo(cur_line);
    /* Shift text[cur_col..len-1] (and the terminator) one byte right
     * to open a gap at cur_col. A hand-written byte-at-a-time loop
     * here used to cost one iteration's worth of loop overhead per
     * character shifted; memmove compiles to a block-copy routine
     * (rep movsb/movsw on most 16-bit DOS compilers) instead. */
    memmove(l->text + cur_col + 1, l->text + cur_col, (size_t) (l->len - cur_col + 1));
    l->text[cur_col] = (char) ch;
    l->len++;
    line_mark_dirty(l);
    cur_col++;
    modified = 1;
    return 1;
}

/* Does NOT request a redraw itself for the common intra-line case --
 * see do_backspace() below. The line-merge case still requests a
 * full redraw directly, since merging always affects more than one
 * line. */
void backspace(void)
{
    Line *prev, *cur;
    sel_clear();
    if (cur_col > 0) {
        Line *l = doc[cur_line];
        save_undo(cur_line);
        memmove(l->text + cur_col - 1, l->text + cur_col, (size_t) (l->len - cur_col + 1));
        l->len--;
        line_mark_dirty(l);
        cur_col--;
        modified = 1;
        return;
    }
    if (cur_line == 0) return;

    prev = doc[cur_line - 1];
    cur  = doc[cur_line];
    if (prev->len + cur->len > MAX_LINE_LEN) {
        flash_error("Can't merge: line too long.");
        return;
    }
    {
        int newcol = prev->len;
        strcat(prev->text, cur->text);
        prev->len += cur->len;
        line_mark_dirty(prev);
        free(cur);
        memmove(&doc[cur_line], &doc[cur_line + 1],
                (size_t) (doc_count - 1 - cur_line) * sizeof(Line *));
        doc_count--;
        cur_line--;
        cur_col = newcol;
        modified = 1;
        request_full_redraw();
        undo_line_no = -1;
    }
}

/* Does NOT request a redraw itself for the common intra-line case --
 * see do_delete_forward() below. The line-merge case still requests
 * a full redraw directly. */
void delete_forward(void)
{
    Line *l = doc[cur_line];
    sel_clear();
    if (cur_col < l->len) {
        save_undo(cur_line);
        memmove(l->text + cur_col, l->text + cur_col + 1, (size_t) (l->len - cur_col));
        l->len--;
        line_mark_dirty(l);
        modified = 1;
        return;
    }
    if (cur_line < doc_count - 1) {
        Line *next = doc[cur_line + 1];
        if (l->len + next->len <= MAX_LINE_LEN) {
            strcat(l->text, next->text);
            l->len += next->len;
            line_mark_dirty(l);
            free(next);
            memmove(&doc[cur_line + 1], &doc[cur_line + 2],
                    (size_t) (doc_count - 2 - cur_line) * sizeof(Line *));
            doc_count--;
            modified = 1;
            request_full_redraw();
            undo_line_no = -1;
        }
    }
}

/* Returns 1 on success, 0 if the document was already full
 * (status_msg is set to explain why in that case). Enter ignores
 * the return value; cmd_paste uses it to notice a truncated paste. */
int split_line(void)
{
    Line *l = doc[cur_line];
    Line *nl;
    if (doc_count >= MAX_LINES) { flash_error("Document full."); return 0; }
    sel_clear();
    nl = new_line();
    strcpy(nl->text, l->text + cur_col);
    nl->len = l->len - cur_col;
    l->text[cur_col] = '\0';
    l->len = cur_col;
    line_mark_dirty(l);   /* nl is already dirty fresh out of new_line() */
    memmove(&doc[cur_line + 2], &doc[cur_line + 1],
            (size_t) (doc_count - cur_line - 1) * sizeof(Line *));
    doc[cur_line + 1] = nl;
    doc_count++;
    cur_line++;
    cur_col = 0;
    modified = 1;
    request_full_redraw();
    undo_line_no = -1;
    return 1;
}

/* Removes the whole current line -- used by vim's "dd". Not exposed
 * outside vim mode since there's no non-vim key bound to it. Like
 * split_line/merge, this is a structural change the single-line undo
 * can't represent, so it invalidates it rather than misrepresenting it. */
void delete_current_line(void)
{
    sel_clear();
    if (doc_count <= 1) {
        doc[0]->text[0] = '\0';
        doc[0]->len = 0;
        line_mark_dirty(doc[0]);
        cur_col = 0;
        modified = 1;
        request_full_redraw();
        return;
    }
    free(doc[cur_line]);
    memmove(&doc[cur_line], &doc[cur_line + 1],
            (size_t) (doc_count - cur_line - 1) * sizeof(Line *));
    doc_count--;
    if (cur_line >= doc_count) cur_line = doc_count - 1;
    cur_col = 0;
    modified = 1;
    request_full_redraw();
    undo_line_no = -1;
}

/* Maps a raw buffer column (an index into doc[line]->text, including
 * hidden markdown delimiters) to the screen column it actually shows
 * up at in Writer view. Mirrors render_line's hiding logic exactly --
 * whenever render_line consumes a delimiter without drawing it, this
 * walks past it without counting a column either, so the two stay in
 * sync. Only meaningful in Writer view; raw Markdown view is already
 * 1:1 (minus horizontal scroll) and doesn't need this.
 *
 * Known imprecision: a [link](url) collapses ALL of its raw
 * positions -- brackets, label, and url alike -- to a single screen
 * column, the same simplification noted where this is used for
 * cursor placement. That means Left/Right (below) can't currently
 * stop inside a link's visible label in Writer view; they'll jump
 * clean over the whole link in one step, same as they now do for
 * bold/italic/etc. Worth fixing properly if that turns out to
 * matter in practice -- it would need this function to walk the
 * label's characters individually instead of returning early.
 *
 * This is still the function used for one-off "where is raw column
 * X on screen" queries (cursor placement after a single Left/Right
 * step, rendering). For anything that needs the answer for MANY
 * columns of the SAME line at once (word wrap, vertical cursor
 * movement across a wrap), use build_screen_col_table() below
 * instead -- calling this in a loop over every column of a line is
 * exactly the O(len^2) pattern that used to make word wrap slow on
 * long lines. */
int writer_screen_col(const char *text, int raw_col)
{
    int i = 0, col = 0, len = (int) strlen(text);

    if (raw_col > len) raw_col = len;

    if (len >= 3) {
        int all_dash = 1, ii;
        for (ii = 0; ii < len; ii++) if (text[ii] != '-') { all_dash = 0; break; }
        if (all_dash) return raw_col;
    }

    if (text[0] == '#') {
        while (i < len && text[i] == '#' && i < raw_col) i++;
        if (i < len && text[i] == ' ' && i < raw_col) i++;
        if (raw_col <= i) return 0;
        return raw_col - i;
    }

    if (text[0] == '>') {
        i = 1;
        if (i < len && text[i] == ' ') i++;
        if (raw_col <= i) return 0;
        return raw_col - i;
    }

    if (text[0] == '-' && len > 1 && text[1] == ' ') {
        if (raw_col < 2) return 0;
        col = 1;
        i = 2;
    }

    while (i < raw_col && i < len) {
        if (text[i] == '*' && i + 1 < len && text[i + 1] == '*') { i += 2; continue; }
        if (text[i] == '*') { i++; continue; }
        if (text[i] == '`') { i++; continue; }
        if (text[i] == '~' && i + 1 < len && text[i + 1] == '~') { i += 2; continue; }
        if (text[i] == '[') {
            int j = i + 1;
            while (j < len && text[j] != ']') j++;
            if (j < len && j + 1 < len && text[j + 1] == '(') {
                int k = j + 2;
                while (k < len && text[k] != ')') k++;
                if (k < len) {
                    if (raw_col <= k) return col;  /* cursor inside the link markup */
                    col += (j - (i + 1));           /* label length */
                    i = k + 1;
                    continue;
                }
            }
        }
        i++;
        col++;
    }
    return col;
}

/* Builds, in a single O(len) forward pass, a table mapping every raw
 * column 0..len of `text` to the Writer-view screen column that
 * writer_screen_col(text, that_column) would return -- same rules
 * (heading/blockquote prefix, list bullet, bold/italic/code/strike
 * delimiters hidden, [link](url) collapsing to one column), just
 * computed once for the whole line instead of re-walking from column
 * 0 for every query. `table` must have room for len+1 ints (indices
 * 0..len inclusive); it's meant to be a transient, stack/static
 * scratch buffer supplied by the caller, NOT something stored per
 * line -- a full int-per-column table would be a few KB per line,
 * which adds up fast against DOS's tight conventional memory once
 * you multiply by a document's worth of lines. (The per-line cache
 * that IS kept around, wrap_starts[], is only a handful of ints --
 * see the comment on Line.)
 * Returns len.
 *
 * This is the fix for the #1 hotspot: compute_wrap_starts() and
 * col_for_target_screen() used to call writer_screen_col() -- itself
 * an O(len) scan from column 0 -- once per candidate column, making
 * wrap computation O(len^2) per line. Both now build this table once
 * (O(len)) and do O(1) lookups into it instead. */
int build_screen_col_table(const char *text, int *table)
{
    int len = (int) strlen(text);
    int i, col;

    if (len >= 3) {
        int all_dash = 1, ii;
        for (ii = 0; ii < len; ii++) if (text[ii] != '-') { all_dash = 0; break; }
        if (all_dash) {
            for (i = 0; i <= len; i++) table[i] = i;
            return len;
        }
    }

    if (text[0] == '#') {
        int prefix = 0;
        while (prefix < len && text[prefix] == '#') prefix++;
        if (prefix < len && text[prefix] == ' ') prefix++;
        for (i = 0; i <= len; i++) table[i] = (i <= prefix) ? 0 : i - prefix;
        return len;
    }

    if (text[0] == '>') {
        int prefix = 1;
        if (prefix < len && text[prefix] == ' ') prefix++;
        for (i = 0; i <= len; i++) table[i] = (i <= prefix) ? 0 : i - prefix;
        return len;
    }

    i = 0; col = 0;
    table[0] = 0;
    if (text[0] == '-' && len > 1 && text[1] == ' ') {
        if (len >= 1) table[1] = 0;
        col = 1;
        i = 2;
        if (i <= len) table[i] = col;
    }

    while (i < len) {
        int i_start = i;
        if (text[i] == '*' && i + 1 < len && text[i + 1] == '*') {
            i += 2;
        } else if (text[i] == '*') {
            i += 1;
        } else if (text[i] == '`') {
            i += 1;
        } else if (text[i] == '~' && i + 1 < len && text[i + 1] == '~') {
            i += 2;
        } else if (text[i] == '[') {
            int j = i_start + 1;
            while (j < len && text[j] != ']') j++;
            if (j < len && j + 1 < len && text[j + 1] == '(') {
                int k = j + 2, m;
                while (k < len && text[k] != ')') k++;
                if (k < len) {
                    /* every raw column from just past '[' through the
                     * closing ')' collapses to the pre-link column --
                     * mirrors writer_screen_col's "if (raw_col <= k)
                     * return col" early-out */
                    for (m = i_start + 1; m <= k; m++) table[m] = col;
                    col += (j - (i_start + 1));   /* label length */
                    i = k + 1;
                    if (i <= len) table[i] = col;
                    continue;
                }
            }
            /* '[' with no valid following (label)(url): ordinary char */
            i = i_start + 1;
            col++;
            if (i <= len) table[i] = col;
            continue;
        } else {
            i = i_start + 1;
            col++;
            if (i <= len) table[i] = col;
            continue;
        }
        /* hidden delimiter (bold/italic/code/strike): every raw column
         * it spans maps to the same col -- these never change col */
        { int m; for (m = i_start + 1; m <= i && m <= len; m++) table[m] = col; }
    }
    return len;
}

/* Right-arrow step for Writer view: advances raw_col past however
 * many raw characters share the current screen column (i.e. past a
 * whole hidden run -- a heading/blockquote prefix, a list bullet's
 * "- ", or a bold/code/strike delimiter pair) so one keypress
 * always lands on the next actually-different on-screen position,
 * never mid-hidden-run. Built entirely on writer_screen_col above
 * rather than re-parsing the line a third time, so it automatically
 * covers every hidden-markup case that function does (see its own
 * note re: links). Falls straight through to a plain +1 wherever
 * there's nothing hidden to skip. */
int writer_move_right(const char *text, int raw_col)
{
    int len = (int) strlen(text);
    int start_screen = writer_screen_col(text, raw_col);
    int new_col = raw_col + 1;
    while (new_col < len && writer_screen_col(text, new_col) == start_screen) new_col++;
    return new_col;
}

/* Left-arrow step for Writer view: the mirror image of
 * writer_move_right above, and its exact inverse -- writer_move_left
 * (writer_move_right(text, x)) == x and vice versa. Walks back to
 * the start of whatever screen-column "slot" the character just
 * left of raw_col belongs to. */
int writer_move_left(const char *text, int raw_col)
{
    int target_screen = writer_screen_col(text, raw_col - 1);
    int new_col = raw_col - 1;
    while (new_col > 0 && writer_screen_col(text, new_col - 1) == target_screen) new_col--;
    return new_col;
}

/* ================= word wrap (Writer view only) ================= */

/* Scratch buffer for build_screen_col_table(), reused across calls
 * instead of a MAX_LINE_LEN-sized array on the stack each time
 * (stack space is precious in DOS's small/medium/large-model, no-MMU
 * world). Fine to share as one global: compute_wrap_starts() and
 * col_for_target_screen() each fill it and consume it before
 * returning, with no reentrancy between those fill/consume pairs. */
static int g_col_table[MAX_LINE_LEN + 1];

/* Raw-column offsets at which each wrapped visual row of `text`
 * begins, when rendered in Writer view at SCREEN_COLS width.
 * starts[0] is always 0. Returns the number of visual rows (>= 1),
 * capped at MAX_WRAP_ROWS -- a document line is at most MAX_LINE_LEN
 * raw chars, which can never wrap into more than a handful of
 * 80-column rows, so the cap is never actually hit in practice.
 *
 * Mirrors the construct-by-construct parsing writer_screen_col and
 * render_writer_line use (HR, heading, blockquote, list, then the
 * shared bold/italic/code/strike/link inline scanner) closely enough
 * that all three always agree on where a line's visible text sits --
 * but instead of hard-clipping at column 80 like the old renderer
 * did, it prefers the most recent space, giving real word-wrap
 * instead of a mid-word chop. A run with no space to break at (e.g.
 * one very long "word") still hard-breaks at the column limit, same
 * as before.
 *
 * Screen columns for every raw column of `text` are computed ONCE up
 * front via build_screen_col_table() (O(len)), then looked up here
 * with plain array indexing -- this used to call writer_screen_col()
 * (itself an O(len) from-scratch scan) once per candidate column,
 * which made this function O(len^2) per line. See that function's
 * comment for the full story; this was the #1 hotspot fix.
 *
 * Most callers don't want this called at all if the line hasn't
 * changed since last time -- see get_line_wraps() below, which wraps
 * this with a per-line cache and is what everything except this
 * function itself should call. */
int compute_wrap_starts(const char *text, int *starts)
{
    int len = (int) strlen(text);
    int seg_start = 0, nstarts = 1;
    int i, base_col, screen_col, last_space, brk;
    int wrapped;

    starts[0] = 0;

    if (len >= 3) {
        int all_dash = 1, ii;
        for (ii = 0; ii < len; ii++) if (text[ii] != '-') { all_dash = 0; break; }
        if (all_dash) return 1;   /* horizontal rule: always one row */
    }

    build_screen_col_table(text, g_col_table);

    while (seg_start < len && nstarts < MAX_WRAP_ROWS) {
        base_col = g_col_table[seg_start];
        last_space = -1;
        wrapped = 0;

        for (i = seg_start; i <= len; i++) {
            screen_col = g_col_table[i] - base_col;

            if (screen_col >= SCREEN_COLS) {
                brk = (last_space >= seg_start) ? last_space + 1 : i;

                /* Always make progress, even on an unusually long token. */
                if (brk <= seg_start) brk = i;
                if (brk <= seg_start) break;

                starts[nstarts++] = brk;
                seg_start = brk;
                wrapped = 1;
                break;
            }

            if (i < len && text[i] == ' ') last_space = i;
        }

        if (!wrapped) break;
    }

    return nstarts;
}

/* Cache-aware front end for compute_wrap_starts(): returns line
 * `line_no`'s wrap starts, recomputing only if that Line's text has
 * changed since the cache was last filled (wrap_dirty, set by
 * line_mark_dirty() from every place that mutates a line's text --
 * see the comment on Line). This is what makes repeated per-redraw,
 * per-arrow-key wrap queries for an UNCHANGED line effectively free
 * (an array copy) instead of redoing the O(len) scan every time --
 * on a full-screen redraw, only lines that actually changed since
 * the last frame do any wrap work at all. Every call site that used
 * to call compute_wrap_starts(doc[N]->text, ...) directly should call
 * this instead. */
int get_line_wraps(int line_no, int *starts)
{
    Line *l = doc[line_no];
    int i;
    if (l->wrap_dirty) {
        l->wrap_nstarts = compute_wrap_starts(l->text, l->wrap_starts);
        l->wrap_dirty = 0;
    }
    for (i = 0; i < l->wrap_nstarts; i++) starts[i] = l->wrap_starts[i];
    return l->wrap_nstarts;
}

/* Number of visual rows `line_no` occupies in the current view --
 * always 1 in raw Markdown view, which still shows one buffer line
 * per screen row and scrolls horizontally instead (see left_col). */
int line_rows(int line_no)
{
    int starts[MAX_WRAP_ROWS];
    if (view_mode == 1) return 1;
    return get_line_wraps(line_no, starts);
}

/* Which wrap segment (0-based) raw column `col` falls into. */
int wrap_seg_of_col(int *starts, int nstarts, int col)
{
    int k;
    for (k = nstarts - 1; k >= 0; k--) if (col >= starts[k]) return k;
    return 0;
}

/* The raw column in [lo, hi] whose Writer-view screen column,
 * relative to `lo`, is the closest to target_col without exceeding
 * it. This is what keeps the cursor's screen column stable when
 * Up/Down crosses a wrapped row -- the vertical equivalent of what
 * writer_move_left/right already do for horizontal steps.
 *
 * Like compute_wrap_starts, this builds the line's screen-column
 * table once (O(len)) via build_screen_col_table() rather than
 * calling writer_screen_col() once per candidate column in the
 * [lo, hi] scan (which used to make this O(len * (hi-lo))). */
int col_for_target_screen(const char *text, int lo, int hi, int target_col)
{
    int base, best = lo, c;
    build_screen_col_table(text, g_col_table);
    base = g_col_table[lo];
    for (c = lo; c <= hi; c++) {
        if (g_col_table[c] - base > target_col) break;
        best = c;
    }
    return best;
}

/* ================= cursor movement ================= */
/* These just update cur_line/cur_col (and, for Up/Down, the wrap
 * bookkeeping needed to keep the on-screen column stable) -- they
 * don't request any redraw themselves. Every call site goes through
 * do_move() below, which decides the cheapest redraw that's actually
 * correct: pure cursor movement never changes any line's rendered
 * content, so as long as the viewport doesn't need to scroll, only
 * the status bar's position readout and the hardware cursor itself
 * need touching -- see request_cursor_redraw(). */

void move_left(void)
{
    if (cur_col > 0) {
        cur_col = (view_mode == 0) ? writer_move_left(doc[cur_line]->text, cur_col)
                                    : cur_col - 1;
    } else if (cur_line > 0) {
        cur_line--;
        cur_col = doc[cur_line]->len;
    }
}
void move_right(void)
{
    if (cur_col < doc[cur_line]->len) {
        cur_col = (view_mode == 0) ? writer_move_right(doc[cur_line]->text, cur_col)
                                    : cur_col + 1;
    } else if (cur_line < doc_count - 1) {
        cur_line++;
        cur_col = 0;
    }
}
/* Up/Down in Writer view step by *visual* row, not buffer line: on a
 * wrapped line they move between wrap segments first, only crossing
 * into the previous/next buffer line once they're on its first/last
 * segment -- same convention as everything else here (a text editor,
 * not a form field). Raw Markdown view is unwrapped, so it keeps the
 * old one-buffer-line-per-row behavior unchanged. */
void move_up(void)
{
    int starts[MAX_WRAP_ROWS], n, seg, target;
    if (view_mode == 1) {
        if (cur_line > 0) {
            cur_line--;
            if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
        }
        return;
    }
    n = get_line_wraps(cur_line, starts);
    seg = wrap_seg_of_col(starts, n, cur_col);
    target = writer_screen_col(doc[cur_line]->text, cur_col)
           - writer_screen_col(doc[cur_line]->text, starts[seg]);
    if (seg > 0) {
        cur_col = col_for_target_screen(doc[cur_line]->text, starts[seg - 1],
                                          starts[seg] - 1, target);
    } else if (cur_line > 0) {
        int pstarts[MAX_WRAP_ROWS], pn;
        cur_line--;
        pn = get_line_wraps(cur_line, pstarts);
        cur_col = col_for_target_screen(doc[cur_line]->text, pstarts[pn - 1],
                                          doc[cur_line]->len, target);
    }
}
void move_down(void)
{
    int starts[MAX_WRAP_ROWS], n, seg, target;
    if (view_mode == 1) {
        if (cur_line < doc_count - 1) {
            cur_line++;
            if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
        }
        return;
    }
    n = get_line_wraps(cur_line, starts);
    seg = wrap_seg_of_col(starts, n, cur_col);
    target = writer_screen_col(doc[cur_line]->text, cur_col)
           - writer_screen_col(doc[cur_line]->text, starts[seg]);
    if (seg + 1 < n) {
        int seg_end = (seg + 2 < n) ? starts[seg + 2] - 1 : doc[cur_line]->len;
        cur_col = col_for_target_screen(doc[cur_line]->text, starts[seg + 1], seg_end, target);
    } else if (cur_line < doc_count - 1) {
        int nstarts[MAX_WRAP_ROWS], nn, hi;
        cur_line++;
        nn = get_line_wraps(cur_line, nstarts);
        hi = (nn > 1) ? nstarts[1] - 1 : doc[cur_line]->len;
        cur_col = col_for_target_screen(doc[cur_line]->text, 0, hi, target);
    }
}
void move_home(void) { cur_col = 0; }
void move_end(void)  { cur_col = doc[cur_line]->len; }

void page_up(void)
{
    if (view_mode == 1) {
        cur_line -= TEXT_ROWS;
        if (cur_line < 0) cur_line = 0;
    } else {
        int rows = 0;
        while (cur_line > 0 && rows < TEXT_ROWS) {
            cur_line--;
            rows += line_rows(cur_line);
        }
    }
    if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
}
void page_down(void)
{
    if (view_mode == 1) {
        cur_line += TEXT_ROWS;
        if (cur_line >= doc_count) cur_line = doc_count - 1;
    } else {
        int rows = 0;
        while (cur_line < doc_count - 1 && rows < TEXT_ROWS) {
            rows += line_rows(cur_line);
            cur_line++;
        }
    }
    if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
}

/* Requests the cheapest possible redraw: just the status bar (its
 * line:col/percent readout) and the hardware cursor position, with
 * every on-screen character left completely alone. Only valid when
 * NOTHING about document text changed -- see redraw_after_move()
 * below, the only caller. Never downgrades an already-pending
 * line/full request. */
void request_cursor_redraw(void)
{
    if (screen_dirty == 0) screen_dirty = 3;
}

/* Wraps any of the move_ or page_ functions above: calls it, then
 * figures out the cheapest correct redraw for the result. A true
 * no-op (arrow key already at the edge of the document) requests
 * nothing at all. Otherwise: if the viewport had to scroll
 * (top_line or, in raw view, left_col changed) the whole screen's
 * contents shifted and needs a full repaint; if it didn't, no line's
 * rendered text changed at all -- cursor movement never touches
 * document content -- so only the status bar and cursor need
 * updating. */
void do_move(void (*move_fn)(void))
{
    int old_line = cur_line, old_col = cur_col;
    int old_top = top_line, old_left = left_col;

    move_fn();
    if (cur_line == old_line && cur_col == old_col) return;   /* true no-op */

    scroll_to_cursor();
    if (top_line != old_top) { request_full_redraw(); return; }
    if (view_mode == 1) {
        int new_left = old_left;
        if (cur_col < new_left) new_left = cur_col;
        if (cur_col >= new_left + SCREEN_COLS) new_left = cur_col - SCREEN_COLS + 1;
        if (new_left != old_left) { request_full_redraw(); return; }
    }
    request_cursor_redraw();
}

/* Grows top_line (in whole buffer lines -- a line's visual rows
 * always scroll onto/off screen together) until the cursor's visual
 * row lands within the TEXT_ROWS window. Raw Markdown view is
 * unwrapped, so it keeps the old exact-line-count logic. */
void scroll_to_cursor(void)
{
    if (view_mode == 1) {
        if (cur_line < top_line) top_line = cur_line;
        if (cur_line >= top_line + TEXT_ROWS) top_line = cur_line - TEXT_ROWS + 1;
        return;
    }
    if (cur_line < top_line) top_line = cur_line;
    for (;;) {
        int rows = 0, ln, starts[MAX_WRAP_ROWS], n, seg;
        for (ln = top_line; ln < cur_line; ln++) rows += line_rows(ln);
        n = get_line_wraps(cur_line, starts);
        seg = wrap_seg_of_col(starts, n, cur_col);
        rows += seg;
        if (rows < TEXT_ROWS || top_line >= cur_line) break;
        top_line++;
    }
}

/* ================= confined single-line edit wrappers ================= */

/* After a character-level edit confined to one line (typing,
 * backspacing, or deleting a single character -- never a line
 * split/merge, which always request a full redraw themselves),
 * decides whether the effect really stayed confined to that one
 * on-screen line, and requests the cheap redraw_line_only() path if
 * so. Falls back to a full redraw whenever the edit had any side
 * effect reaching past `line_no`:
 *   - re-wrapping that line into a different number of visual rows
 *     shifts every line below it up/down a row in Writer view;
 *   - scroll_to_cursor() deciding to move top_line means the whole
 *     viewport's contents shifted;
 *   - in raw Markdown view, the cursor crossing the horizontal-scroll
 *     boundary (left_col) means every row's visible slice shifted.
 * Getting any of these wrong in the "should be full but we picked
 * line" direction would just be a rendering bug, not a crash, but
 * the checks above are exactly what redraw_screen() itself already
 * uses to decide top_line/left_col, so there's no separate logic to
 * keep in sync. */
void redraw_after_char_edit(int line_no, int old_nrows, int old_top, int old_left)
{
    int new_nrows;

    scroll_to_cursor();
    if (top_line != old_top) { request_full_redraw(); return; }

    new_nrows = line_rows(line_no);
    if (new_nrows != old_nrows) { request_full_redraw(); return; }

    if (view_mode == 1) {
        int new_left = old_left;
        if (cur_col < new_left) new_left = cur_col;
        if (cur_col >= new_left + SCREEN_COLS) new_left = cur_col - SCREEN_COLS + 1;
        if (new_left != old_left) { request_full_redraw(); return; }
    }

    request_line_redraw(line_no);
}

/* Call this instead of insert_char() directly from key dispatch (not
 * from cmd_paste, which wants exactly one redraw for the whole
 * paste, not one per character -- see insert_char's own comment). A
 * selection being active forces a full redraw regardless, since
 * insert_char() clears it and the highlight it's removing can span
 * more than one line. */
void do_insert_char(int ch)
{
    int line_no = cur_line;
    int old_nrows, old_top, old_left;
    if (sel_active) { insert_char(ch); request_full_redraw(); return; }
    old_nrows = line_rows(line_no);
    old_top = top_line;
    old_left = left_col;
    if (!insert_char(ch)) return;   /* flash_error already requested full */
    redraw_after_char_edit(line_no, old_nrows, old_top, old_left);
}

/* Call this instead of backspace() directly from key dispatch. Only
 * the plain intra-line case (deleting the character just left of the
 * cursor, cursor not at column 0) is eligible for the cheap path --
 * backspace() itself still requests a full redraw for the
 * line-merge case, so this just needs to not interfere with that. */
void do_backspace(void)
{
    int line_no = cur_line;
    int old_nrows, old_top, old_left;
    if (sel_active || cur_col == 0) {
        int had_sel = sel_active;
        backspace();
        if (had_sel) request_full_redraw();
        return;
    }
    old_nrows = line_rows(line_no);
    old_top = top_line;
    old_left = left_col;
    backspace();
    redraw_after_char_edit(line_no, old_nrows, old_top, old_left);
}

/* Call this instead of delete_forward() directly from key dispatch.
 * Only the plain intra-line case (deleting the character under the
 * cursor, cursor not already at end of line) is eligible -- the
 * merge-with-next-line case still requests a full redraw itself. */
void do_delete_forward(void)
{
    int line_no = cur_line;
    int old_nrows, old_top, old_left;
    if (sel_active || cur_col >= doc[cur_line]->len) {
        int had_sel = sel_active;
        delete_forward();
        if (had_sel) request_full_redraw();
        return;
    }
    old_nrows = line_rows(line_no);
    old_top = top_line;
    old_left = left_col;
    delete_forward();
    redraw_after_char_edit(line_no, old_nrows, old_top, old_left);
}

/* ================= rendering ================= */

/* ---- selection highlight helpers ----
 *
 * These are called from inside render_line at the exact point each
 * visible glyph is emitted, passing the raw buffer column that glyph
 * came from. That's the "refactor" the Writer renderer needed: every
 * put_char site now knows and reports the raw source column that
 * produced it, so highlighting is decided per visible character
 * against the *raw* selection range -- never by converting selection
 * endpoints into a screen-column span and painting between them.
 * Hidden delimiters (the "**" of bold, the "#" of a heading, etc.)
 * are simply never drawn and never asked about, so they can't shift
 * or gap the highlight the way a screen-span approach would risk.
 */

/* True if raw column range [r0, r1) overlaps the row's selected raw
 * range [sel_start, sel_end). sel_start == -1 means nothing is
 * selected on this row. */
int sel_overlaps(int r0, int r1, int sel_start, int sel_end)
{
    return sel_start >= 0 && r0 < sel_end && r1 > sel_start;
}

/* Reverse-video swap: highlights a cell regardless of its current
 * fg/bg attribute, so selection reads consistently over normal text,
 * bold/italic/code/strike runs, headings, and blockquotes alike. */
unsigned char swap_attr(unsigned char attr)
{
    return (unsigned char) (((attr & 0x0F) << 4) | ((attr & 0xF0) >> 4));
}

/* attr, highlighted if the single raw column raw_col is selected. */
unsigned char apply_sel(unsigned char attr, int raw_col, int sel_start, int sel_end)
{
    return sel_overlaps(raw_col, raw_col + 1, sel_start, sel_end) ? swap_attr(attr) : attr;
}

/* Renders one buffer line into one screen row.
 *
 * Raw/Markdown view: shows the text exactly as typed, honoring
 * 'offset' for horizontal scroll.
 *
 * Writer view: a single-pass scanner. Heading / blockquote / list /
 * horizontal-rule are whole-line markers checked first; everything
 * else (bold/italic/code/strike/link) toggles as it scans left to
 * right. See the file header for what's simplified and why.
 *
 * sel_start/sel_end give this row's selected raw column range as
 * [sel_start, sel_end); sel_start == -1 means no selection touches
 * this row. Callers get these from sel_line_range().
 */
/* Raw Markdown view: one buffer line per screen row, unwrapped,
 * scrolled horizontally by `offset` (see left_col).
 *
 * Writes through row_ptr() directly instead of clear_row()-then-
 * put_char(): the old version blanked all 80 cells and then
 * overwrote however many actually have text, so every covered cell
 * was written twice per redraw for no reason. This writes each cell
 * exactly once -- content where the line has it, a blank cell where
 * it doesn't -- with no put_char multiply/bounds-check per glyph
 * either, since the whole row is already known to be in range. */
void render_line(const char *text, int row, int offset, int sel_start, int sel_end)
{
    unsigned int far *rp = row_ptr(row);
    int i, len = (int) strlen(text);
    int col = 0;
    for (i = offset; i < len && col < SCREEN_COLS; i++, col++)
        rp[col] = CELL(text[i], apply_sel(ATTR_NORMAL, i, sel_start, sel_end));
    for (; col < SCREEN_COLS; col++) rp[col] = CELL(' ', ATTR_NORMAL);
}

/* Writer view: draws one *visual* row -- the word-wrapped raw-column
 * slice [seg_start, seg_end) of `text` that compute_wrap_starts
 * decided belongs on this screen row. Because that slice was already
 * chosen to fit within SCREEN_COLS at a word boundary, this never
 * needs to clip mid-word the way the old single-row-per-line
 * render_line did.
 *
 * For a continuation row (seg_start > 0) that lands inside a run of
 * inline markup, the initial bold/italic/code/strike state is
 * recovered by silently replaying the same scanner from just after
 * any heading/quote/list prefix up to seg_start, without drawing
 * anything -- the same trick writer_screen_col already uses to
 * answer "where is raw column X on screen" without keeping any
 * separate persistent state. (A `[link](url)` that itself straddles
 * a wrap point is the one case this replay can lose sync with -- an
 * accepted rough edge, same spirit as the other Known Limitations.) */
void render_writer_line(const char *text, int row, int seg_start, int seg_end,
                         int sel_start, int sel_end)
{
    unsigned int far *rp = row_ptr(row);
    int i, col = 0, len = (int) strlen(text);
    int bold = 0, italic = 0, code = 0, strike = 0;
    unsigned char attr;
    int last_col = -1;
    char last_ch = ' ';
    unsigned char last_attr = ATTR_NORMAL;

    /* horizontal rule: a line that is nothing but 3+ hyphens (always one row) */
    if (len >= 3) {
        int all_dash = 1, ii;
        for (ii = 0; ii < len; ii++) if (text[ii] != '-') { all_dash = 0; break; }
        if (all_dash) {
            for (col = 0; col < SCREEN_COLS; col++) {
                attr = (col < len) ? apply_sel(ATTR_NORMAL, col, sel_start, sel_end)
                                    : ATTR_NORMAL;
                rp[col] = CELL((char) 196, attr);  /* CP437 horizontal line */
            }
            return;
        }
    }

    /* heading -- flat attribute, no inline toggles to replay */
    if (text[0] == '#') {
        static const unsigned char head_attr[6] = {
            ATTR_HEAD1, ATTR_HEAD2, ATTR_HEAD3, ATTR_HEAD4, ATTR_HEAD5, ATTR_HEAD6
        };
        int level;
        unsigned char hattr;
        i = 0;
        while (i < len && text[i] == '#') i++;
        level = i;
        if (level < 1) level = 1;
        if (level > 6) level = 6;
        hattr = head_attr[level - 1];
        if (i < len && text[i] == ' ') i++;
        if (seg_start > i) i = seg_start;
        for (; i < len && i < seg_end && col < SCREEN_COLS; i++, col++)
            rp[col] = CELL(text[i], apply_sel(hattr, i, sel_start, sel_end));
        for (; col < SCREEN_COLS; col++) rp[col] = CELL(' ', ATTR_NORMAL);
        return;
    }

    /* blockquote -- flat attribute, no inline toggles to replay */
    if (text[0] == '>') {
        i = 1;
        if (i < len && text[i] == ' ') i++;
        if (seg_start > i) i = seg_start;
        for (; i < len && i < seg_end && col < SCREEN_COLS; i++, col++)
            rp[col] = CELL(text[i], apply_sel(ATTR_QUOTE, i, sel_start, sel_end));
        for (; col < SCREEN_COLS; col++) rp[col] = CELL(' ', ATTR_NORMAL);
        return;
    }

    i = 0;
    if (text[0] == '-' && len > 1 && text[1] == ' ') {
        if (seg_start == 0) {
            /* the bullet glyph stands in for raw columns 0-1 (the "- "
             * prefix), so it's checked as a 2-wide range rather than a
             * single raw column like everything else here */
            attr = sel_overlaps(0, 2, sel_start, sel_end) ? swap_attr(ATTR_LISTMARK) : ATTR_LISTMARK;
            rp[0] = CELL((char) 7, attr);  /* CP437 bullet glyph */
            col = 1;
            i = 2;
        } else {
            i = 2;   /* continuation row: replay starts right after "- " */
        }
    }

    /* continuation row: silently replay inline toggles up to seg_start */
    if (seg_start > i) {
        int r = i;
        while (r < seg_start) {
            if (text[r] == '*' && r + 1 < len && text[r + 1] == '*') { bold = !bold; r += 2; continue; }
            if (text[r] == '*') { italic = !italic; r++; continue; }
            if (text[r] == '`') { code = !code; r++; continue; }
            if (text[r] == '~' && r + 1 < len && text[r + 1] == '~') { strike = !strike; r += 2; continue; }
            if (text[r] == '[') {
                int j = r + 1;
                while (j < len && text[j] != ']') j++;
                if (j < len && j + 1 < len && text[j + 1] == '(') {
                    int k = j + 2;
                    while (k < len && text[k] != ')') k++;
                    if (k < len) { r = k + 1; continue; }
                }
            }
            r++;
        }
        i = seg_start;
        col = 0;
    }

    while (i < len && i < seg_end) {
        if (text[i] == '*' && i + 1 < len && text[i + 1] == '*') {
            if (last_col >= 0 && sel_overlaps(i, i + 2, sel_start, sel_end))
                rp[last_col] = CELL(last_ch, swap_attr(last_attr));
            bold = !bold; i += 2; continue;
        }
        if (text[i] == '*') {
            if (last_col >= 0 && sel_overlaps(i, i + 1, sel_start, sel_end))
                rp[last_col] = CELL(last_ch, swap_attr(last_attr));
            italic = !italic; i++; continue;
        }
        if (text[i] == '`') {
            if (last_col >= 0 && sel_overlaps(i, i + 1, sel_start, sel_end))
                rp[last_col] = CELL(last_ch, swap_attr(last_attr));
            code = !code; i++; continue;
        }
        if (text[i] == '~' && i + 1 < len && text[i + 1] == '~') {
            if (last_col >= 0 && sel_overlaps(i, i + 2, sel_start, sel_end))
                rp[last_col] = CELL(last_ch, swap_attr(last_attr));
            strike = !strike; i += 2; continue;
        }
        if (text[i] == '[') {
            int j = i + 1;
            while (j < len && text[j] != ']') j++;
            if (j < len && j + 1 < len && text[j + 1] == '(') {
                int k = j + 2;
                while (k < len && text[k] != ')') k++;
                if (k < len) {
                    int m;
                    if (last_col >= 0 && sel_overlaps(i, i + 1, sel_start, sel_end))
                        rp[last_col] = CELL(last_ch, swap_attr(last_attr));  /* leading '[' */
                    for (m = i + 1; m < j; m++) {
                        if (col < SCREEN_COLS) {
                            rp[col] = CELL(text[m], apply_sel(ATTR_LINK, m, sel_start, sel_end));
                            last_col = col; last_ch = text[m]; last_attr = ATTR_LINK;
                        }
                        col++;
                    }
                    if (last_col >= 0 && sel_overlaps(j, k + 1, sel_start, sel_end))
                        rp[last_col] = CELL(last_ch, swap_attr(last_attr));  /* "](url)" */
                    i = k + 1;
                    continue;
                }
            }
        }
        if (col < SCREEN_COLS) {
            attr = code   ? ATTR_CODE
                 : strike ? ATTR_STRIKE
                 : bold   ? ATTR_BOLD
                 : italic ? ATTR_ITALIC
                 : ATTR_NORMAL;
            rp[col] = CELL(text[i], apply_sel(attr, i, sel_start, sel_end));
            last_col = col; last_ch = text[i]; last_attr = attr;
        }
        i++;
        col++;
    }

    /* blank out whatever's left of the row past the rendered text --
     * see render_line's comment for why this replaces the old
     * clear-then-overwrite (every covered cell written once now, not
     * twice) */
    for (; col < SCREEN_COLS; col++) rp[col] = CELL(' ', ATTR_NORMAL);
}

/* Status bar, laid out like a Neovim statusline:
 *   [NORMAL/INSERT]  filename* | status msg          Rich  42%  17:6
 * The mode block only appears while vim_mode is on -- it always shows
 * whichever sub-mode is current (never faded/hidden away, so it stays
 * trustworthy at a glance) and sits in its own color, distinct from the
 * rest of the bar. Everything on the right is right-aligned, same spot
 * every time regardless of how long the left side gets. */
void draw_status_bar(void)
{
    char left[SCREEN_COLS + 1];
    char right[32];
    int col = 0, rlen, rcol, percent;
    const char *sel_tag = sel_active ? " Sel" : "";
    /* flash_active only controls how long a routine confirmation
     * sticks around (see main()'s poll loop) and never affects color.
     * error_flash_active is the only thing that puts the bar on the
     * eye-catching ATTR_STATUS_FLASH background, for the first few
     * seconds of an error message (see flash_error()); once it times
     * out the bar drops back to plain ATTR_STATUS while status_msg
     * (the error text) stays displayed. */
    unsigned char attr = error_flash_active ? ATTR_STATUS_FLASH : ATTR_STATUS;

    clear_row(STATUS_ROW, attr);

    if (vim_mode) {
        const char *mtxt = vim_insert ? " INSERT " : " NORMAL ";
        unsigned char mattr = vim_insert ? ATTR_MODE_INSERT : ATTR_MODE_NORMAL;
        put_string(col, STATUS_ROW, mtxt, mattr);
        col += (int) strlen(mtxt) + 1;
    }

    sprintf(left, "%s%s%s | %s",
            filename[0] ? filename : "untitled",
            modified ? "*" : "",
            sel_tag,
            status_msg);
    put_string(col, STATUS_ROW, left, attr);

    percent = (doc_count > 1) ? ((cur_line * 100) / (doc_count - 1)) : 100;
    sprintf(right, "%s  %3d%%  %d:%d",
            view_mode ? "Markdown" : "Rich",
            percent, cur_line + 1, cur_col + 1);
    rlen = (int) strlen(right);
    rcol = SCREEN_COLS - rlen - 1;
    if (rcol < col + (int) strlen(left) + 2) rcol = col + (int) strlen(left) + 2;
    put_string(rcol, STATUS_ROW, right, attr);
}

/* Bottom row: menu category names, Alt-navigable. Replaces the old
 * static hint line -- same row, interactive now. */
void draw_menu_bar(void)
{
    int i, c = 1, rlen;
    char right[80 + 32];
    const char *fname = filename[0] ? filename : "untitled";
    clear_row(CMDBAR_ROW, ATTR_CMDBAR);
    for (i = 0; i < MENU_COUNT; i++) {
        unsigned char base = (menu_open == i) ? ATTR_CMDBAR_SEL : ATTR_CMDBAR;
        unsigned char hot  = (menu_open == i) ? ATTR_CMDBAR_SEL : ATTR_CMDBAR_HOT;
        menu_col[i] = c;
        put_char(c, CMDBAR_ROW, menus[i].label[0], hot);
        put_string(c + 1, CMDBAR_ROW, menus[i].label + 1, base);
        c += (int) strlen(menus[i].label) + 3;
    }
    sprintf(right, "%s | %s %s", fname, MDRITE_NAME, MDRITE_VERSION);
    rlen = (int) strlen(right);
    put_string(SCREEN_COLS - rlen - 1, CMDBAR_ROW, right, ATTR_CMDBAR);
}

void draw_menu_popup(void)
{
    MenuCategory *m = &menus[menu_open];
    int width = 0, i, r, col, top;
    for (i = 0; i < m->item_count; i++) {
        int l = (int) strlen(m->items[i]);
        if (l > width) width = l;
    }
    width += 2;
    col = menu_col[menu_open];
    if (col + width > SCREEN_COLS) col = SCREEN_COLS - width;
    if (col < 0) col = 0;
    top = CMDBAR_ROW - m->item_count;
    if (top < 0) top = 0;

    for (i = 0; i < m->item_count; i++) {
        unsigned char base = (i == menu_sel) ? ATTR_CMDBAR_SEL : ATTR_POPUP;
        /* Selected row is already fully highlighted (ATTR_CMDBAR_SEL),
         * same as the top bar's own hot-letter-on-selected-category
         * case, so the mnemonic only needs its own color when the row
         * ISN'T selected. */
        unsigned char hot = (i == menu_sel) ? ATTR_CMDBAR_SEL : ATTR_POPUP_HOT;
        r = top + i;
        fill_rect(col, r, width, base);
        put_string(col + 1, r, m->items[i], base);
        put_char(col + 1 + m->mnemonic_idx[i], r, m->items[i][m->mnemonic_idx[i]], hot);
    }
}

/* Draws the shared bottom row (menu bar+popup, or the status bar)
 * and places the hardware cursor. Shared by redraw_screen() and
 * redraw_line_only() -- every redraw, full or partial, ends the same
 * way, since the status bar's contents (modified flag, position,
 * mode) and the cursor position can change on any edit regardless of
 * how much of the document view itself needed to move. */
void draw_bottom_and_cursor(void)
{
    if (menu_open >= 0) {
        draw_menu_bar();
        draw_menu_popup();
    } else if (alt_held) {
        draw_menu_bar();   /* preview only -- no category selected/opened */
    } else {
        draw_status_bar();
    }
    {
        int screen_col, screen_row;
        if (view_mode == 1) {
            screen_col = cur_col - left_col;
            screen_row = cur_line - top_line;
        } else {
            int starts[MAX_WRAP_ROWS], n, seg, rows, l;
            n = get_line_wraps(cur_line, starts);
            seg = wrap_seg_of_col(starts, n, cur_col);
            rows = 0;
            for (l = top_line; l < cur_line; l++) rows += line_rows(l);
            screen_row = rows + seg;
            screen_col = writer_screen_col(doc[cur_line]->text, cur_col)
                       - writer_screen_col(doc[cur_line]->text, starts[seg]);
        }
        if (screen_col < 0) screen_col = 0;
        if (screen_col >= SCREEN_COLS) screen_col = SCREEN_COLS - 1;
        if (screen_row < 0) screen_row = 0;
        if (screen_row >= TEXT_ROWS) screen_row = TEXT_ROWS - 1;
        set_cursor(screen_col, screen_row);
    }
}

/* Cheap redraw path for an edit request_line_redraw() confirmed is
 * confined to `line_no`: repaints just that buffer line's on-screen
 * row(s) (1 in raw view, 1+ wrap segments in Writer view) plus the
 * status bar and cursor -- the other TEXT_ROWS-1-ish rows on screen
 * are left completely untouched, since by construction (see
 * redraw_after_char_edit) nothing about them changed. If `line_no`
 * isn't currently visible (shouldn't happen -- it's always cur_line,
 * and scroll_to_cursor keeps cur_line on screen -- but a defensive
 * fallback costs nothing) this just falls back to a full redraw. */
void redraw_line_only(int line_no)
{
    int sel_start, sel_end, r;

    if (view_mode == 1) {
        r = line_no - top_line;
        if (r < 0 || r >= TEXT_ROWS) { redraw_screen(); return; }
        sel_line_range(line_no, doc[line_no]->len, &sel_start, &sel_end);
        render_line(doc[line_no]->text, r, left_col, sel_start, sel_end);
    } else {
        int starts[MAX_WRAP_ROWS], n, seg, rows_above, l;
        rows_above = 0;
        for (l = top_line; l < line_no; l++) rows_above += line_rows(l);
        if (rows_above >= TEXT_ROWS) { redraw_screen(); return; }
        n = get_line_wraps(line_no, starts);
        sel_line_range(line_no, doc[line_no]->len, &sel_start, &sel_end);
        for (seg = 0; seg < n; seg++) {
            r = rows_above + seg;
            if (r >= TEXT_ROWS) break;
            {
                int seg_end = (seg + 1 < n) ? starts[seg + 1] : doc[line_no]->len;
                render_writer_line(doc[line_no]->text, r, starts[seg], seg_end, sel_start, sel_end);
            }
        }
    }

    draw_bottom_and_cursor();
}

void redraw_screen(void)
{
    int r, ln;
    scroll_to_cursor();

    if (view_mode == 1) {
        if (cur_col < left_col) left_col = cur_col;
        if (cur_col >= left_col + SCREEN_COLS) left_col = cur_col - SCREEN_COLS + 1;

        for (r = 0; r < TEXT_ROWS; r++) {
            ln = top_line + r;
            if (ln < doc_count) {
                int sel_start, sel_end;
                sel_line_range(ln, doc[ln]->len, &sel_start, &sel_end);
                render_line(doc[ln]->text, r, left_col, sel_start, sel_end);
            } else {
                clear_row(r, ATTR_NORMAL);
            }
        }
    } else {
        /* Writer view: each buffer line may occupy several word-wrapped
         * visual rows, so walk lines starting at top_line and, for
         * each, draw all of its wrap segments until the screen fills. */
        left_col = 0;
        r = 0; ln = top_line;
        while (r < TEXT_ROWS) {
            if (ln >= doc_count) {
                clear_row(r, ATTR_NORMAL);
                r++;
                continue;
            }
            {
                int starts[MAX_WRAP_ROWS], n, seg, sel_start, sel_end;
                n = get_line_wraps(ln, starts);
                sel_line_range(ln, doc[ln]->len, &sel_start, &sel_end);
                for (seg = 0; seg < n && r < TEXT_ROWS; seg++, r++) {
                    int seg_end = (seg + 1 < n) ? starts[seg + 1] : doc[ln]->len;
                    render_writer_line(doc[ln]->text, r, starts[seg], seg_end, sel_start, sel_end);
                }
            }
            ln++;
        }
    }

    draw_bottom_and_cursor();
}

/* ================= input helpers ================= */

int prompt_input(const char *prompt, char *buf, int maxlen)
{
    int i = 0, ch, plen = (int) strlen(prompt);
    buf[0] = '\0';
    clear_row(CMDBAR_ROW, ATTR_CMDBAR);
    put_string(0, CMDBAR_ROW, prompt, ATTR_CMDBAR);
    set_cursor(plen, CMDBAR_ROW);
    for (;;) {
        ch = _bios_keybrd(_KEYBRD_READ);
        if ((ch & 0xFF) == 13) { buf[i] = '\0'; return 1; }
        if ((ch & 0xFF) == 27) { return 0; }
        if ((ch & 0xFF) == 8) {
            if (i > 0) { i--; put_char(plen + i, CMDBAR_ROW, ' ', ATTR_CMDBAR); }
        } else if ((ch & 0xFF) >= 32 && (ch & 0xFF) < 127 && i < maxlen - 1) {
            buf[i] = (char) (ch & 0xFF);
            put_char(plen + i, CMDBAR_ROW, buf[i], ATTR_CMDBAR);
            i++;
        }
        set_cursor(plen + i, CMDBAR_ROW);
    }
}

int confirm(const char *msg)
{
    int ch;
    clear_row(CMDBAR_ROW, ATTR_CMDBAR);
    put_string(0, CMDBAR_ROW, msg, ATTR_CMDBAR);
    for (;;) {
        ch = _bios_keybrd(_KEYBRD_READ) & 0xFF;
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N' || ch == 27) return 0;
    }
}

/* ================= file I/O ================= */

/* Reads one full source line from f into a heap-allocated,
 * NUL-terminated buffer -- however long it actually is, no fixed cap
 * -- with any trailing '\n' stripped. *out_len receives its length.
 * Returns 0 at EOF with nothing read, 1 otherwise; caller owns *out
 * and must free() it.
 *
 * This exists so load_file() below never repeats the bug a fixed
 * fgets(buf, MAX_LINE_LEN+1, f) buffer has: silently chopping a
 * too-long real line at a fixed byte count, mid-word, and treating
 * the leftover as an unrelated new line. */
int read_full_line(FILE *f, char **out, int *out_len)
{
    int cap = 256, len = 0, ch;
    char *buf = (char *) malloc(cap);
    if (!buf) { *out = NULL; *out_len = 0; return 0; }
    for (;;) {
        ch = fgetc(f);
        if (ch == EOF) {
            if (len == 0) { free(buf); *out = NULL; *out_len = 0; return 0; }
            break;
        }
        if (ch == '\n') break;
        if (len + 1 >= cap) {
            char *grown;
            cap *= 2;
            grown = (char *) realloc(buf, cap);
            if (!grown) break;   /* out of memory: keep what we have so far */
            buf = grown;
        }
        buf[len++] = (char) ch;
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 1;
}

void load_file(const char *fname)
{
    FILE *f = fopen(fname, "r");
    int i;
    char *raw;
    int raw_len;
    if (!f) { flash_error("Could not open file."); return; }
    for (i = 0; i < doc_count; i++) free(doc[i]);
    doc_count = 0;
    while (doc_count < MAX_LINES && read_full_line(f, &raw, &raw_len)) {
        int pos = 0;
        do {
            int chunk = raw_len - pos;
            if (chunk > MAX_LINE_LEN) {
                /* Only reached if a single real source line is
                 * longer than MAX_LINE_LEN even after the bump above
                 * -- back up to the last space so the forced split
                 * lands between words, not inside one, the same
                 * word-boundary preference compute_wrap_starts uses
                 * for on-screen wrapping. */
                int brk = pos + MAX_LINE_LEN, k;
                for (k = brk; k > pos; k--) if (raw[k] == ' ') break;
                chunk = (k > pos) ? (k - pos) : MAX_LINE_LEN;
            }
            doc[doc_count] = new_line();
            memcpy(doc[doc_count]->text, raw + pos, chunk);
            doc[doc_count]->text[chunk] = '\0';
            doc[doc_count]->len = chunk;
            doc_count++;
            pos += chunk;
            while (pos < raw_len && raw[pos] == ' ') pos++;  /* drop the break space itself */
        } while (pos < raw_len && doc_count < MAX_LINES);
        free(raw);
    }
    if (doc_count == 0) { doc[0] = new_line(); doc_count = 1; }
    fclose(f);
    strcpy(filename, fname);
    cur_line = cur_col = top_line = left_col = 0;
    modified = 0;
    undo_line_no = -1;
    sel_clear();
    request_full_redraw();
    flash_status("Loaded.");
}

void save_file(const char *fname)
{
    FILE *f = fopen(fname, "w");
    int i;
    if (!f) { flash_error("Could not save file."); return; }
    for (i = 0; i < doc_count; i++) fprintf(f, "%s\n", doc[i]->text);
    fclose(f);
    strcpy(filename, fname);
    modified = 0;
    flash_status("Saved.");
}

/* ================= commands ================= */

/* Copies the selection into the clipboard buffer, lines joined with
 * '\n'. Doesn't touch the document or clear the selection -- Copy
 * is read-only, unlike Cut. */
void cmd_copy(void)
{
    int sl, sc, el, ec, ln, from, to, n;
    if (!sel_active) { flash_status_for("Nothing selected.", FLASH_SECS(2)); return; }
    sel_bounds(&sl, &sc, &el, &ec);
    if (sl == el && sc == ec) { flash_status_for("Nothing selected.", FLASH_SECS(2)); return; }

    clip_len = 0;
    for (ln = sl; ln <= el; ln++) {
        from = (ln == sl) ? sc : 0;
        to   = (ln == el) ? ec : doc[ln]->len;
        if (from > doc[ln]->len) from = doc[ln]->len;
        if (to   > doc[ln]->len) to   = doc[ln]->len;
        n = to - from;
        if (n > 0 && clip_len + n < CLIP_MAX - 1) {
            memcpy(clipboard + clip_len, doc[ln]->text + from, n);
            clip_len += n;
        }
        if (ln < el && clip_len < CLIP_MAX - 1) clipboard[clip_len++] = '\n';
    }
    clipboard[clip_len] = '\0';
    flash_status("Copied.");
}

void cmd_cut(void)
{
    if (!sel_active) { flash_status_for("Nothing selected.", FLASH_SECS(2)); return; }
    cmd_copy();
    /* Only claim "Cut." if sel_delete actually removed something.
     * When it doesn't (an active-but-empty selection, or a
     * cross-line selection too long to merge) it has already left
     * an explanatory status_msg -- overwriting that with a blanket
     * "Cut." would tell the user text was removed when it wasn't. */
    if (sel_delete()) flash_status("Cut.");
}

/* Inserts the clipboard at the cursor. A selection active at paste
 * time is replaced rather than typed over -- same convention as
 * every other editor's paste. Embedded '\n's become real line
 * splits via split_line(), one clipboard char at a time through
 * insert_char(), so paste inherits exactly the same MAX_LINE_LEN/
 * MAX_LINES guards (and the same per-line single-level undo
 * behavior) as ordinary typing -- no separate bounds-checking to
 * keep in sync. */
void cmd_paste(void)
{
    int i, ok = 1;
    if (clip_len == 0) { flash_status_for("Clipboard empty.", FLASH_SECS(2)); return; }
    if (sel_active) sel_delete();
    for (i = 0; i < clip_len && ok; i++) {
        if (clipboard[i] == '\n') ok = split_line();
        else ok = insert_char((unsigned char) clipboard[i]);
    }
    /* Stop and leave split_line/insert_char's own "Line full."/
     * "Document full." message in place if the paste ran out of
     * room partway through, rather than papering over a truncated
     * paste with a blanket "Pasted." */
    if (ok) flash_status("Pasted.");
}

void cmd_save_as(void)
{
    char buf[80];
    if (prompt_input("Save as: ", buf, sizeof(buf)) && buf[0]) save_file(buf);
}

void cmd_save(void)
{
    if (filename[0]) save_file(filename);
    else cmd_save_as();
}

void cmd_new(void)
{
    if (modified && !confirm("Discard unsaved changes? (Y/N)")) return;
    doc_reset();
    filename[0] = '\0';
    request_full_redraw();
    flash_status("New file.");
}

void cmd_open(void)
{
    char buf[80];
    if (modified && !confirm("Discard unsaved changes? (Y/N)")) return;
    if (prompt_input("Open file: ", buf, sizeof(buf)) && buf[0]) load_file(buf);
}

int find_from(int start_line, int start_col, const char *needle)
{
    int ln, from;
    char *found;
    for (ln = start_line; ln < doc_count; ln++) {
        from = (ln == start_line) ? start_col : 0;
        if (from > doc[ln]->len) continue;
        found = strstr(doc[ln]->text + from, needle);
        if (found) {
            cur_line = ln;
            cur_col = (int) (found - doc[ln]->text);
            request_full_redraw();
            return 1;
        }
    }
    return 0;
}

void cmd_find(void)
{
    char buf[80];
    if (!prompt_input("Find: ", buf, sizeof(buf)) || !buf[0]) return;
    strcpy(last_search, buf);
    if (find_from(cur_line, cur_col + 1, buf) || find_from(0, 0, buf))
        flash_status("Found.");
    else
        flash_status("Not found.");
}

void cmd_find_next(void)
{
    if (!last_search[0]) { cmd_find(); return; }
    if (find_from(cur_line, cur_col + 1, last_search) || find_from(0, 0, last_search))
        flash_status("Found.");
    else
        flash_status("Not found.");
}

/* Guards Replace All against a replacement that contains the search
 * string itself (e.g. replacing "a" with "aab"), which would
 * otherwise keep creating new matches forever. Comfortably above
 * anything a real document here would ever need. */
#define REPLACE_LIMIT 5000

/* WordStar-style per-match confirmation, same spirit as Alt+X's
 * DOS-editor convention elsewhere in this file: (Y)es replaces just
 * this one, (N)o skips it, (A)ll stops asking and replaces every
 * remaining match, anything else (including Esc) stops. Blocks for
 * one keypress, same as confirm(). */
int prompt_replace_choice(void)
{
    int ch;
    clear_row(CMDBAR_ROW, ATTR_CMDBAR);
    put_string(0, CMDBAR_ROW, "Replace? (Y)es (N)o (A)ll (Esc) Quit", ATTR_CMDBAR);
    ch = tolower(_bios_keybrd(_KEYBRD_READ) & 0xFF);
    if (ch == 'y' || ch == 'n' || ch == 'a') return ch;
    return 'q';
}

/* Find and replace. A match never spans lines -- find_from() only
 * ever searches within one line's text -- so every accepted replace
 * is exactly a sel_delete() followed by an insert_char() loop: the
 * same pair cmd_paste() already uses to replace a selection. That
 * means it inherits the same single-line-undo caveat cmd_paste does
 * -- Undo only reverts the last character of the last accepted
 * replace, not the whole operation (see README's Known Limitations).
 *
 * Search position bookkeeping mirrors cmd_find(): search strictly
 * after the cursor first, wrapping to the top of the document if
 * nothing turns up before it. After an accepted replace, the next
 * search resumes right where the replacement text ends, so a
 * replacement that doesn't contain the search string can never be
 * found again by accident. */
void cmd_replace(void)
{
    char find_buf[80], repl_buf[80], msg[32];
    int replace_all = 0, had_match = 0, count = 0, guard = 0;
    int rlen, i, ch = 0;
    int sline, scol;

    if (!prompt_input("Find: ", find_buf, sizeof(find_buf)) || !find_buf[0]) return;
    if (!prompt_input("Replace with: ", repl_buf, sizeof(repl_buf))) return;
    strcpy(last_search, find_buf);
    rlen = (int) strlen(repl_buf);

    sline = cur_line;
    scol  = cur_col + 1;   /* same "search strictly after cursor" rule as cmd_find */

    for (;;) {
        if (!find_from(sline, scol, find_buf) && !find_from(0, 0, find_buf)) break;
        had_match = 1;

        if (++guard > REPLACE_LIMIT) {
            flash_error("Replace stopped: too many matches (check replacement text).");
            break;
        }

        /* find_from() only marks the screen dirty -- it doesn't
         * actually repaint, since normally that's left to main()'s
         * loop. But cmd_replace() never returns to main() between
         * matches (it loops right here, blocking on
         * prompt_replace_choice() below), so without an explicit
         * repaint now the view never scrolls to the match and the
         * cursor never visibly jumps to it before we ask Y/N/A. */
        if (!replace_all) {
            scroll_to_cursor();
            redraw_screen();
            screen_dirty = 0;
            ch = prompt_replace_choice();
            if (ch == 'q') break;
            if (ch == 'a') replace_all = 1;
        }

        if (!replace_all && ch == 'n') {
            sline = cur_line;
            scol  = cur_col + 1;    /* skip past this match's start next pass */
            continue;
        }

        /* find_from already left cur_line/cur_col at the match's
         * start -- select exactly the matched span, then reuse the
         * same delete+insert pair cmd_paste() uses over a selection */
        anchor_line = cur_line;
        anchor_col  = cur_col;
        cur_col += (int) strlen(find_buf);
        sel_active = 1;
        sel_delete();
        for (i = 0; i < rlen; i++) {
            if (!insert_char((unsigned char) repl_buf[i])) break;
        }
        count++;
        sline = cur_line;
        scol  = cur_col;    /* resume right after the inserted text */
    }

    if (count > 0) { sprintf(msg, "Replaced %d.", count); flash_status(msg); }
    else if (had_match) flash_status("No replacements made.");
    else flash_status("Not found.");
    request_full_redraw();
}

void cmd_goto(void)
{
    char buf[16];
    int n;
    if (!prompt_input("Go to line: ", buf, sizeof(buf)) || !buf[0]) return;
    n = atoi(buf) - 1;
    if (n < 0) n = 0;
    if (n >= doc_count) n = doc_count - 1;
    cur_line = n;
    cur_col = 0;
    request_full_redraw();
}

void cmd_quit_request(void)
{
    if (!modified || confirm("Unsaved changes -- quit anyway? (Y/N)")) want_quit = 1;
}

void toggle_vim_mode(void)
{
    vim_mode = !vim_mode;
    vim_insert = 0;    /* always start in Normal sub-mode when turning it on */
    vim_pending = 0;
    request_full_redraw();
    flash_status(vim_mode ? "Vim keys ON." : "Vim keys OFF.");
}

/* Minimal ":" command line: :w save, :q quit, :wq save+quit,
 * :q! quit without saving. Nothing beyond these four. */
void vim_command_line(void)
{
    char buf[40];
    if (!prompt_input(":", buf, sizeof(buf))) return;
    if (strcmp(buf, "w") == 0) cmd_save();
    else if (strcmp(buf, "q") == 0) cmd_quit_request();
    else if (strcmp(buf, "wq") == 0) { cmd_save(); want_quit = 1; }
    else if (strcmp(buf, "q!") == 0) want_quit = 1;
    else flash_error("Unknown command.");
}

/* About screen: a standalone centered box (double-line CP437 border,
 * same ATTR_POPUP color as the dropdown menus) rather than another
 * dropdown item, since it's a whole screen the user reads and
 * dismisses, not a list they pick from. Blocks on a single keypress
 * -- any key closes it, same "any key" convention as confirm()'s
 * Y/N prompt except it doesn't care which key -- then asks for a
 * full redraw since the box was painted straight over the document
 * area. */
void cmd_about(void)
{
    const char *lines[] = {
        "",
        MDRITE_NAME " " MDRITE_VERSION,
        "",
        MDRITE_DESC,
        "",
        MDRITE_COPYRIGHT,
        MDRITE_LICENSE,
        MDRITE_REPO_URL,
        "",
        "Press any key to close",
        ""
    };
    int n = (int) (sizeof(lines) / sizeof(lines[0]));
    int i, w = 0, len, col;
    int box_w, box_h, box_col, box_row;

    for (i = 0; i < n; i++) {
        len = (int) strlen(lines[i]);
        if (len > w) w = len;
    }
    box_w = w + 4;                      /* 1-col border + 1-col pad each side */
    if (box_w > SCREEN_COLS) box_w = SCREEN_COLS;
    box_h = n + 2;                      /* +1 border row top and bottom */
    box_col = (SCREEN_COLS - box_w) / 2;
    box_row = (TEXT_ROWS - box_h) / 2;
    if (box_row < 0) box_row = 0;

    for (i = 0; i < box_h; i++) fill_rect(box_col, box_row + i, box_w, ATTR_POPUP);

    put_char(box_col, box_row, (char) 0xC9, ATTR_POPUP);                        /* top-left */
    put_char(box_col + box_w - 1, box_row, (char) 0xBB, ATTR_POPUP);            /* top-right */
    put_char(box_col, box_row + box_h - 1, (char) 0xC8, ATTR_POPUP);            /* bottom-left */
    put_char(box_col + box_w - 1, box_row + box_h - 1, (char) 0xBC, ATTR_POPUP);/* bottom-right */
    for (i = 1; i < box_w - 1; i++) {
        put_char(box_col + i, box_row, (char) 0xCD, ATTR_POPUP);
        put_char(box_col + i, box_row + box_h - 1, (char) 0xCD, ATTR_POPUP);
    }
    for (i = 1; i < box_h - 1; i++) {
        put_char(box_col, box_row + i, (char) 0xBA, ATTR_POPUP);
        put_char(box_col + box_w - 1, box_row + i, (char) 0xBA, ATTR_POPUP);
    }

    for (i = 0; i < n; i++) {
        len = (int) strlen(lines[i]);
        col = box_col + (box_w - len) / 2;
        put_string(col, box_row + 1 + i, lines[i], (i == 1) ? ATTR_POPUP_HOT : ATTR_POPUP);
    }

    set_cursor(box_col + 1, box_row + box_h - 1);
    _bios_keybrd(_KEYBRD_READ);         /* any key dismisses -- don't care which */
    request_full_redraw();
}

void execute_menu_item(int cat, int idx)
{
    if (cat == 0) {
        switch (idx) {
            case 0: cmd_new();          break;
            case 1: cmd_open();         break;
            case 2: cmd_save();         break;
            case 3: cmd_save_as();      break;
            case 4: cmd_quit_request(); break;
        }
    } else if (cat == 1) {
        switch (idx) {
            case 0: do_undo();  break;
            case 1: cmd_cut();  break;
            case 2: cmd_copy(); break;
            case 3: cmd_paste(); break;
        }
    } else if (cat == 2) {
        switch (idx) {
            case 0: cmd_find();      break;
            case 1: cmd_find_next(); break;
            case 2: cmd_goto();      break;
            case 3: cmd_replace();   break;
        }
    } else if (cat == 3) {
        if (idx == 0) { view_mode = !view_mode; request_full_redraw(); }
        else if (idx == 1) toggle_vim_mode();
    } else if (cat == 4) {
        if (idx == 0) cmd_about();
    }
}

/* ================= main ================= */

int main(int argc, char **argv)
{
    int key, lo, hi, k, matched;

    doc[0] = new_line();
    if (argc > 1) load_file(argv[1]);

    clear_screen(ATTR_NORMAL);

    flash_status("Hold Alt for the menu bar");
    request_full_redraw();

    for (;;) {
        if (screen_dirty == 3) {
            draw_bottom_and_cursor();
            screen_dirty = 0;
        } else if (screen_dirty == 2) {
            redraw_line_only(dirty_line_no);
            screen_dirty = 0;
        } else if (screen_dirty) {
            redraw_screen();
            screen_dirty = 0;
        }

        /* No key is waiting yet: keep polling, redrawing only when
         * Alt's held/released state actually changes, so the menu
         * bar preview appears the instant Alt goes down and the
         * status bar comes back the instant it's released -- with
         * no keystroke required either way. Same poll also clears
         * a flash message once its timer runs out. */
        while (!_bios_keybrd(_KEYBRD_READY)) {
            int now_alt = alt_down();
            if (now_alt != alt_held) {
                alt_held = now_alt;
                redraw_screen();
            }
            if (flash_active) {
                long now;
                _bios_timeofday(_TIME_GETCLOCK, &now);
                if (now >= flash_expire) {
                    flash_active = 0;
                    strcpy(status_msg, "Ready.");
                    redraw_screen();
                }
            }
            if (error_flash_active) {
                long now;
                _bios_timeofday(_TIME_GETCLOCK, &now);
                if (now >= error_flash_expire) {
                    /* Just drop the attention-flash color -- the error
                     * text in status_msg is sticky and stays put. */
                    error_flash_active = 0;
                    redraw_screen();
                }
            }
            _asm { hlt }
        }
        alt_held = 0;
        if (flash_active) {
            /* Any real keystroke dismisses a flash message right
             * away too, even if the 5 seconds haven't elapsed --
             * whatever the key goes on to do below (including
             * starting a new flash of its own, e.g. F4) is free to
             * set its own status_msg afterward, same as normal. */
            flash_active = 0;
            strcpy(status_msg, "Ready.");
            request_full_redraw();
        }
        if (error_flash_active) {
            /* Unlike a routine flash, the error's attention-color is
             * NOT cut short by a keystroke -- it always gets its full
             * FLASH_SECS(5) to be noticed. Typing fast enough to skip
             * the poll loop above just means we check the clock here
             * instead; the message itself was never going anywhere. */
            long now;
            _bios_timeofday(_TIME_GETCLOCK, &now);
            if (now >= error_flash_expire) { error_flash_active = 0; request_full_redraw(); }
        }

        key = _bios_keybrd(_KEYBRD_READ);
        lo = key & 0xFF;
        hi = (key >> 8) & 0xFF;

        if (menu_open >= 0) {
            /* ---- menu navigation mode ---- */
            request_full_redraw();   /* menu nav always repaints the popup/bar */
            if (lo == 27) {
                menu_open = -1;
            } else if (lo == 13) {
                execute_menu_item(menu_open, menu_sel);
                menu_open = -1;
            } else if (lo == 0) {
                switch (hi) {
                    case 0x48:  /* Up */
                        menu_sel--;
                        if (menu_sel < 0) menu_sel = menus[menu_open].item_count - 1;
                        break;
                    case 0x50:  /* Down */
                        menu_sel++;
                        if (menu_sel >= menus[menu_open].item_count) menu_sel = 0;
                        break;
                    case 0x4B:  /* Left */
                        menu_open = (menu_open - 1 + MENU_COUNT) % MENU_COUNT;
                        menu_sel = 0;
                        break;
                    case 0x4D:  /* Right */
                        menu_open = (menu_open + 1) % MENU_COUNT;
                        menu_sel = 0;
                        break;
                    default:
                        for (k = 0; k < MENU_COUNT; k++) {
                            if (menus[k].altkey_scan == hi) {
                                menu_open = k; menu_sel = 0; break;
                            }
                        }
                        break;
                }
            } else {
                /* Any other (non-Alt) key: check it against this menu's
                 * mnemonic letters -- typing the highlighted letter runs
                 * that item immediately, same as arrowing to it and
                 * pressing Enter. Arrow/Enter/Esc handling above already
                 * covers everything else, so this only ever sees plain
                 * character keys. */
                MenuCategory *m = &menus[menu_open];
                int typed = tolower((unsigned char) lo);
                for (k = 0; k < m->item_count; k++) {
                    int letter = tolower((unsigned char) m->items[k][m->mnemonic_idx[k]]);
                    if (letter == typed) {
                        execute_menu_item(menu_open, k);
                        menu_open = -1;
                        break;
                    }
                }
            }
            if (want_quit) break;
            continue;
        }

        /* ---- normal editing mode ---- */
        if (vim_mode && !vim_insert && lo != 0) {
            /* Vim Normal sub-mode. Plain letter/punctuation keys are
             * commands here, not text -- Ctrl-shortcuts, Enter, and
             * Backspace still work underneath exactly like non-vim
             * mode; only the bare unmodified keys change meaning. */
            if (lo == 13) split_line();
            else if (lo == 8)  do_backspace();
            else if (lo == 1)  cmd_save_as();
            else if (lo == 6)  cmd_find();
            else if (lo == 7)  cmd_goto();
            else if (lo == 18) cmd_replace();
            else if (lo == 14) cmd_new();
            else if (lo == 15) cmd_open();
            else if (lo == 19) cmd_save();
            else if (lo == 3)  cmd_copy();
            else if (lo == 24) cmd_cut();
            else if (lo == 22) cmd_paste();
            else if (lo == 26) do_undo();
            else if (vim_pending == 'd' && lo == 'd') { delete_current_line(); vim_pending = 0; }
            else if (lo == 'h') { do_move(move_left);  vim_pending = 0; }
            else if (lo == 'l') { do_move(move_right); vim_pending = 0; }
            else if (lo == 'k') { do_move(move_up);    vim_pending = 0; }
            else if (lo == 'j') { do_move(move_down);  vim_pending = 0; }
            else if (lo == '0') { do_move(move_home);  vim_pending = 0; }
            else if (lo == '$') { do_move(move_end);   vim_pending = 0; }
            else if (lo == 'x') { do_delete_forward(); vim_pending = 0; }
            else if (lo == 'u') { do_undo();        vim_pending = 0; }
            else if (lo == 'i') { vim_insert = 1;   vim_pending = 0; request_full_redraw(); }
            else if (lo == 'a') { do_move(move_right); vim_insert = 1; vim_pending = 0; request_full_redraw(); }
            else if (lo == 'd') { vim_pending = 'd'; }
            else if (lo == ':') { vim_command_line(); vim_pending = 0; request_full_redraw(); }
            else vim_pending = 0;   /* unrecognized: swallow, don't insert -- no screen change */
        }
        else if (lo == 27) {
            /* Esc no longer quits (see Alt+X below) -- it now only
             * clears an active selection, or in vim mode drops back
             * from Insert to Normal. With neither applicable, it's
             * simply a no-op, same as most editors treat a bare Esc. */
            if (sel_active) {
                sel_clear();
                request_full_redraw();
            } else if (vim_mode && vim_insert) {
                vim_insert = 0; vim_pending = 0;  /* Insert -> Normal */
                request_full_redraw();
            }
        } else if (lo == 0) {
            matched = 0;
            for (k = 0; k < MENU_COUNT; k++) {
                if (menus[k].altkey_scan == hi) {
                    menu_open = k; menu_sel = 0; matched = 1; request_full_redraw(); break;
                }
            }
            if (!matched) {
                switch (hi) {
                    case 0x4B: case 0x4D: case 0x48: case 0x50:
                    case 0x47: case 0x4F: case 0x49: case 0x51: {
                        /* cursor-movement keys: Shift held extends
                         * (or starts) the selection; unmodified,
                         * they clear it -- selection tracks
                         * cur_line/cur_col, which the move below
                         * updates as always. */
                        if (shift_down()) { sel_begin(); request_full_redraw(); }
                        else if (sel_active) { sel_clear(); request_full_redraw(); }
                        switch (hi) {
                            case 0x4B: do_move(move_left);  break;
                            case 0x4D: do_move(move_right); break;
                            case 0x48: do_move(move_up);    break;
                            case 0x50: do_move(move_down);  break;
                            case 0x47: do_move(move_home);  break;
                            case 0x4F: do_move(move_end);   break;
                            case 0x49: do_move(page_up);    break;
                            case 0x51: do_move(page_down);  break;
                        }
                        break;
                    }
                    case 0x53: do_delete_forward(); break;
                    case 0x3B: cmd_about(); break;                 /* F1 */
                    case 0x3C: view_mode = !view_mode; request_full_redraw(); break;  /* F2 */
                    case 0x3D: cmd_find_next();  break;         /* F3 */
                    case 0x3E: toggle_vim_mode(); break;        /* F4 */
                    case 0x2D: cmd_quit_request(); break;       /* Alt+X */
                    default: break;
                }
            }
        } else if (lo == 13) split_line();
        else if (lo == 8)  do_backspace();
        else if (lo == 1)  cmd_save_as();
        else if (lo == 6)  cmd_find();
        else if (lo == 7)  cmd_goto();
        else if (lo == 18) cmd_replace();
        else if (lo == 14) cmd_new();
        else if (lo == 15) cmd_open();
        else if (lo == 19) cmd_save();
        else if (lo == 3)  cmd_copy();
        else if (lo == 24) cmd_cut();
        else if (lo == 22) cmd_paste();
        else if (lo == 26) do_undo();
        else if (lo >= 32 && lo < 127) do_insert_char(lo);

        if (want_quit) break;
    }

    clear_screen(ATTR_NORMAL);
    set_cursor(0, 0);
    return 0;
}

