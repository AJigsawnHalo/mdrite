/* mdrite.c - small DOS/FreeDOS markdown writer, inspired by ArtfulType for 68k Mac. See README.md for build/run instructions, keys, and limitations. */

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
/* Status and menu bars share the bottom row: it shows the status line normally, and swaps to the File/Edit/Search/View bar while Alt is held. That frees up a row, giving 24 lines of text instead of 23. */
#define STATUS_ROW    (SCREEN_ROWS - 1)
#define CMDBAR_ROW    (SCREEN_ROWS - 1)
#define TEXT_ROWS     (SCREEN_ROWS - 1)
#define MAX_LINE_LEN  1024
#define MAX_LINES     2000

/* Max word-wrapped visual rows a single buffer line can occupy in Writer view. Used by Line's wrap cache below. */
#define MAX_WRAP_ROWS 16

/* Sublist indent step in spaces, and max nesting depth. do_list_indent() moves indentation by LIST_INDENT_UNIT at a time; MAX_LIST_INDENT caps it so indentation can't eat into a wrapped line's usable width. */
#define LIST_INDENT_UNIT   4
#define MAX_LIST_INDENT    24

/* ---------- color attributes ---------- */
#define ATTR_NORMAL      0x07   /* light grey / black */
#define ATTR_BOLD        0x0F   /* bright white / black */
#define ATTR_ITALIC      0x0B   /* bright cyan / black */
#define ATTR_CODE        0x0D   /* bright magenta / black */
#define ATTR_STRIKE      0x0C   /* bright red / black -- color only, no literal strikethrough line */
#define ATTR_HEAD        0x1E   /* legacy alias for ATTR_HEAD2, kept so old references still work */
/* Per-level heading colors share one blue background but step down in foreground brightness from H1 to H6, so level is visible at a glance. */
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
#define ATTR_STATUS_FLASH 0x4E  /* reserved for the error attention-flash; routine flashes (Saved., etc.) use plain ATTR_STATUS instead, so a normal confirmation doesn't look like something broke */
#define ATTR_CMDBAR      0x30   /* black / cyan */
#define ATTR_CMDBAR_HOT  0x3E   /* bright yellow / cyan -- menu hotkey letter */
#define ATTR_CMDBAR_SEL  0x4F   /* open/selected menu color, kept distinct from the status bar so they don't visually fuse */
#define ATTR_POPUP       0x1F   /* bright white / blue -- dropdown body */
#define ATTR_POPUP_HOT   0x1E   /* dropdown item's mnemonic letter, same trick as ATTR_CMDBAR_HOT */
#define ATTR_MODE_NORMAL 0x2F   /* bright white / green -- vim Normal indicator */
#define ATTR_MODE_INSERT 0x6F   /* bright white / brown(orange) -- vim Insert indicator */

unsigned char far *video = (unsigned char far *) 0xB8000000L;

/* Forward declarations needed because a few functions call each other across sections before their own definitions appear. */
void scroll_to_cursor(void);
void redraw_screen(void);
int list_indent_of(const char *text, int len);
void do_list_indent(int dir);

typedef struct {
    char text[MAX_LINE_LEN + 1];
    int  len;
    /* Cached Writer-view word-wrap offsets for this line. Wrap only depends on the line's own text, so it's safe to compute once and reuse until the line is edited (wrap_dirty tracks that). */
    int  wrap_starts[MAX_WRAP_ROWS];
    int  wrap_nstarts;
    int  wrap_dirty;
} Line;

Line *doc[MAX_LINES];
int  doc_count = 1;

/* Fenced code block state. code_before[i] is 1 if line i sits inside an open ``` fence (state entering that line). Recomputed in one pass whenever code_state_valid is 0 -- structural edits (line insert/delete) always invalidate; single-line edits only invalidate if that line's own fence-ness actually flipped. */
int code_before[MAX_LINES];
int code_state_valid = 0;

int  cur_line = 0, cur_col = 0;
int  top_line = 0;
int  left_col = 0;
int  modified = 0;
char filename[80] = "";
int  view_mode = 0;     /* 0 = Rich (rendered), 1 = raw Markdown. Kept as an int, not a bool, since a Graphics mode may be added later. */
char status_msg[80] = "Ready.";
char last_search[80] = "";
int  want_quit = 0;

/* Redraw request level for the current keystroke: 0 = nothing changed, 1 = full repaint (anything touching more than one line), 2 = just one line (dirty_line_no) plus the status bar -- the cheap path for ordinary typing. */
int screen_dirty = 1;
int dirty_line_no = -1;

void request_full_redraw(void)
{
    screen_dirty = 1;
}

/* Requests a cheap single-line redraw for `line_no`, upgrading to a full redraw instead if one is already pending or a different line was requested. */
void request_line_redraw(int line_no)
{
    if (screen_dirty == 1) return;
    if (screen_dirty == 2 && dirty_line_no != line_no) { screen_dirty = 1; return; }
    screen_dirty = 2;
    dirty_line_no = line_no;
}

/* Optional vim-lite keymapping, off by default, toggled with F4. Intentionally a small subset, not a full emulation -- see the file header. */
int vim_mode = 0;
int vim_insert = 0;    /* 0 = Normal sub-mode, 1 = Insert sub-mode */
int vim_pending = 0;   /* holds the first key of a two-key command, e.g. 'd' of dd */

/* Single-level undo: remembers only one line's previous contents. */
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
    int  mnemonic_idx[MAX_MENU_ITEMS]; /* Index into items[i] of the mnemonic letter to highlight/match -- not always item[i][0] (e.g. 'Save As' highlights the 'A'). */
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

/* Set while Alt is held with no letter pressed yet, previewing the menu bar without entering navigation mode. Updated by main()'s poll loop. */
int alt_held = 0;

/* Status messages come in two kinds: sticky (stays until the next update, for things the user must notice) and flash (shown briefly, then reverts to 'Ready.' or clears on the next keystroke). */
int  flash_active = 0;
long flash_expire = 0;

/* Sticky error message with a brief attention-flash: the bar shows ATTR_STATUS_FLASH for FLASH_SECS(5), then settles back to the plain background while the message stays put. */
int  error_flash_active = 0;
long error_flash_expire = 0;

/* ---------- clipboard ---------- */
/* Flat buffer holding a copy of the selected text, lines joined with '\n'. One clipboard slot, no history, like a classic DOS editor. */
#define CLIP_MAX 4000
char clipboard[CLIP_MAX] = "";
int  clip_len = 0;

/* ================= selection (raw document coordinates) ================= */

/* A selection is an anchor plus the live cursor position, both in raw buffer coordinates -- so it renders correctly in either view with no translation needed. */
int sel_active   = 0;
int anchor_line  = 0, anchor_col = 0;

void sel_clear(void) { sel_active = 0; }

/* Starts a selection at the cursor if one isn't already active; safe to call on every Shift-move since later calls in a run are no-ops. */
void sel_begin(void)
{
    if (!sel_active) {
        anchor_line = cur_line;
        anchor_col  = cur_col;
        sel_active  = 1;
    }
}

/* Normalizes the anchor/cursor pair into an ordered (start <= end) pair, comparing line first, then column. */
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

/* Raw [start,end) selected range on line_no, clipped to the line length. out_start is -1 when the line isn't touched by the selection at all. */
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

/* True if Shift is held, read from the BIOS keyboard flag byte rather than the key code, since arrow/Home/End/etc. report the same scan code either way. */
int shift_down(void)
{
    return (_bios_keybrd(_KEYBRD_SHIFTSTATUS) & 0x03) != 0;
}

/* True if Alt is held, via the same BIOS flag byte. A bare Alt press generates no keystroke, so main() polls this instead of blocking on a key read. */
int alt_down(void)
{
    return (_bios_keybrd(_KEYBRD_SHIFTSTATUS) & 0x08) != 0;
}

/* BIOS clock ticks run at ~18.2/sec, rounded to 18, so callers can write FLASH_SECS(2) instead of a raw tick count. */
#define FLASH_SECS(n) ((long) (n) * 18L)

/* Sticky status message: replaces status_msg and stays until the next update. Use for anything the user needs to notice and may act on. */
void set_status(const char *msg)
{
    strcpy(status_msg, msg);
    flash_active = 0;
    error_flash_active = 0;
    request_full_redraw();
}

/* Flash message shown for `ticks` BIOS clock ticks, then reverts to 'Ready.' on its own or on the next keystroke, whichever comes first. */
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

/* Sticky error message with an attention flash: the text stays put like set_status(), but the bar flashes ATTR_STATUS_FLASH for 5 seconds first to make sure it's noticed. */
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

/* Packs a glyph+attribute pair as one 16-bit value (char in the low byte, attribute in the high byte), written with a single far store instead of two. */
#define CELL(ch, attr) ((unsigned int) (unsigned char) (ch) | ((unsigned int) (attr) << 8))

/* Row base address as a cell-sized far pointer, so indexing by column avoids a general multiply. Callers touching many cells in one row should fetch this once and index through it rather than calling put_char per glyph. */
unsigned int far *row_ptr(int row)
{
    return (unsigned int far *) (video + (long) row * SCREEN_COLS * 2);
}

/* Safe, bounds-checked single-glyph write for scattered, non-hot-loop call sites (menu bar, status bar, prompts). Hot per-row loops bypass this and write through row_ptr() instead. */
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

/* Blanks [col, col+w) of `row` with `attr` by computing the row's base pointer once and writing through it, avoiding put_char's per-cell multiply and bounds check. */
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

/* Call this whenever a Line's text[] changes, to invalidate its cached word-wrap so the next get_line_wraps() call recomputes. */
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
    code_state_valid = 0;
}

/* A line opens/closes a fence if it starts with ``` -- no leading spaces allowed, same as CommonMark. */
int is_fence_line(Line *l)
{
    return l->len >= 3 && l->text[0] == '`' && l->text[1] == '`' && l->text[2] == '`';
}

/* One O(doc_count) pass, only run when code_state_valid is 0. Also drops every line's wrap cache, since wrap columns are computed differently in and out of a fence and this is the one place that knows fence membership might have shifted. */
void recompute_code_state(void)
{
    int i, state = 0;
    for (i = 0; i < doc_count; i++) {
        code_before[i] = state;
        if (is_fence_line(doc[i])) state = !state;
        doc[i]->wrap_dirty = 1;
    }
    code_state_valid = 1;
}

void ensure_code_state(void)
{
    if (!code_state_valid) recompute_code_state();
}

/* True if line_no should render as code: either it's inside an open fence, or it's the fence line itself. Callers must have called ensure_code_state() first. */
int line_in_code(int line_no)
{
    return code_before[line_no] || is_fence_line(doc[line_no]);
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
    code_state_valid = 0;
    cur_line = undo_line_no;
    cur_col = undo_col;
    undo_line_no = -1;
    flash_status("Undid last edit.");
    modified = 1;
}

/* ================= editing ops ================= */

/* Removes the selected text (single- or multi-line), leaves the cursor at the collapsed start, and clears the selection. A multi-line delete invalidates the single-line undo rather than misrepresenting it. Returns 1 if it deleted anything, 0 for a no-op or a bailed-out cross-line merge, with status_msg already explaining why. */
int sel_delete(void)
{
    int sl, sc, el, ec, i;
    if (!sel_active) return 0;
    sel_bounds(&sl, &sc, &el, &ec);
    if (sl == el && sc == ec) { sel_clear(); return 0; }  /* nothing actually selected */

    if (sl == el) {
        Line *l = doc[sl];
        int n = ec - sc;
        int was_fence = is_fence_line(l);
        save_undo(sl);
        /* shifts text left by n bytes via memmove instead of a per-byte loop -- see insert_char's comment for why */
        memmove(l->text + sc, l->text + sc + n, (size_t) (l->len - n - sc + 1));
        l->len -= n;
        line_mark_dirty(l);
        if (is_fence_line(l) != was_fence) code_state_valid = 0;
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
        code_state_valid = 0;
    }
    cur_line = sl;
    cur_col = sc;
    sel_clear();
    modified = 1;
    request_full_redraw();
    return 1;
}

/* Returns 1 on success, 0 if the line was already full. Doesn't request a redraw itself -- callers (do_insert_char, or cmd_paste for a whole paste) decide the scope. */
int insert_char(int ch)
{
    Line *l = doc[cur_line];
    int was_fence;
    if (l->len >= MAX_LINE_LEN) { flash_error("Line full."); return 0; }
    sel_clear();
    save_undo(cur_line);
    was_fence = is_fence_line(l);
    /* Shifts text right by one byte to open a gap at cur_col, via memmove instead of a byte-at-a-time loop for speed. */
    memmove(l->text + cur_col + 1, l->text + cur_col, (size_t) (l->len - cur_col + 1));
    l->text[cur_col] = (char) ch;
    l->len++;
    line_mark_dirty(l);
    if (is_fence_line(l) != was_fence) code_state_valid = 0;
    cur_col++;
    modified = 1;
    return 1;
}

/* Doesn't request a redraw itself for the intra-line case -- see do_backspace(). The line-merge case requests a full redraw directly. */
void backspace(void)
{
    Line *prev, *cur;
    sel_clear();
    if (cur_col > 0) {
        Line *l = doc[cur_line];
        int was_fence = is_fence_line(l);
        save_undo(cur_line);
        memmove(l->text + cur_col - 1, l->text + cur_col, (size_t) (l->len - cur_col + 1));
        l->len--;
        line_mark_dirty(l);
        if (is_fence_line(l) != was_fence) code_state_valid = 0;
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
        code_state_valid = 0;
    }
}

/* Doesn't request a redraw itself for the intra-line case -- see do_delete_forward(). The line-merge case requests a full redraw directly. */
void delete_forward(void)
{
    Line *l = doc[cur_line];
    sel_clear();
    if (cur_col < l->len) {
        int was_fence = is_fence_line(l);
        save_undo(cur_line);
        memmove(l->text + cur_col, l->text + cur_col + 1, (size_t) (l->len - cur_col));
        l->len--;
        line_mark_dirty(l);
        if (is_fence_line(l) != was_fence) code_state_valid = 0;
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
            code_state_valid = 0;
            modified = 1;
            request_full_redraw();
            undo_line_no = -1;
        }
    }
}

/* Returns 1 on success, 0 if the document was already full. Enter ignores the return value; cmd_paste uses it to notice a truncated paste. */
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
    code_state_valid = 0;
    return 1;
}

/* Call this instead of split_line() directly for a real Enter keypress (not from cmd_paste). If Enter is pressed inside a list item's content, the new line inherits the same indent + '- ' prefix, continuing the list. */
void do_split_line(void)
{
    Line *l = doc[cur_line];
    int indent = list_indent_of(l->text, l->len);
    int had_split_col = cur_col;

    if (!split_line()) return;   /* "Document full." already flashed */

    if (indent >= 0 && had_split_col >= indent + 2) {
        Line *nl = doc[cur_line];   /* split_line() left cur_line on the new line */
        if (nl->len + indent + 2 <= MAX_LINE_LEN) {
            memmove(nl->text + indent + 2, nl->text, (size_t) (nl->len + 1));
            memset(nl->text, ' ', (size_t) indent);
            nl->text[indent] = '-';
            nl->text[indent + 1] = ' ';
            nl->len += indent + 2;
            line_mark_dirty(nl);
            cur_col += indent + 2;
        } else {
            flash_error("Line too long to continue list.");
        }
    }
}

/* Removes the whole current line, used by vim's 'dd'. Like split_line/merge, this invalidates the single-line undo rather than misrepresenting it. */
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
        code_state_valid = 0;
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
    code_state_valid = 0;
}

/* Maps a raw buffer column to its Writer-view screen column, mirroring render_line's hiding logic so cursor placement and rendering stay in sync. A [link](url) collapses entirely to one screen column. For one-off column queries only -- use build_screen_col_table() for many columns of the same line. */
/* Scans a possibly-indented list line's marker (leading spaces, then '- '), returning the indent depth or -1 if it isn't a list line. Shared by writer_screen_col, build_screen_col_table, render_writer_line, and do_list_indent() so they all agree on where content starts. */
int list_indent_of(const char *text, int len)
{
    int i = 0;
    while (i < len && text[i] == ' ') i++;
    if (i < len - 1 && text[i] == '-' && text[i + 1] == ' ') return i;
    return -1;
}

int writer_screen_col(const char *text, int raw_col, int in_code)
{
    int i = 0, col = 0, len = (int) strlen(text);

    if (raw_col > len) raw_col = len;
    if (in_code) return raw_col;   /* verbatim: no hiding, no markers */

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

    {
        int indent = list_indent_of(text, len);
        if (indent >= 0) {
            if (raw_col <= indent) return raw_col;
            if (raw_col < indent + 2) return indent;
            col = indent + 1;
            i = indent + 2;
        }
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

/* Builds, in one O(len) pass, a table mapping every raw column to its Writer-view screen column -- the same rules as writer_screen_col but computed once instead of per query. `table` needs len+1 ints and is meant as transient scratch, not stored per line. Fixes the old O(len^2) wrap computation. */
int build_screen_col_table(const char *text, int *table, int in_code)
{
    int len = (int) strlen(text);
    int i, col;

    if (in_code) {
        for (i = 0; i <= len; i++) table[i] = i;
        return len;
    }

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
    {
        int indent = list_indent_of(text, len);
        if (indent >= 0) {
            int k;
            for (k = 1; k <= indent; k++) table[k] = k;
            table[indent + 1] = indent;
            col = indent + 1;
            i = indent + 2;
            if (i <= len) table[i] = col;
        }
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
                    /* Raw columns from '[' through the closing ')' all collapse to the pre-link column, mirroring writer_screen_col's early-out. */
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
        /* hidden delimiter (bold/italic/code/strike) -- every raw column it spans maps to the same screen column */
        { int m; for (m = i_start + 1; m <= i && m <= len; m++) table[m] = col; }
    }
    return len;
}

/* Right-arrow step for Writer view: skips a whole hidden run (heading prefix, list bullet, or a markup delimiter pair) in one keypress, built on writer_screen_col so it covers every hidden-markup case that function does. */
int writer_move_right(const char *text, int raw_col, int in_code)
{
    int len = (int) strlen(text);
    int start_screen, new_col;
    if (in_code) return raw_col + 1;
    start_screen = writer_screen_col(text, raw_col, 0);
    new_col = raw_col + 1;
    while (new_col < len && writer_screen_col(text, new_col, 0) == start_screen) new_col++;
    return new_col;
}

/* Left-arrow step for Writer view: mirror image of writer_move_right, its exact inverse. */
int writer_move_left(const char *text, int raw_col, int in_code)
{
    int target_screen, new_col;
    if (in_code) return raw_col - 1;
    target_screen = writer_screen_col(text, raw_col - 1, 0);
    new_col = raw_col - 1;
    while (new_col > 0 && writer_screen_col(text, new_col - 1, 0) == target_screen) new_col--;
    return new_col;
}

/* ================= word wrap (Writer view only) ================= */

/* Scratch buffer for build_screen_col_table(), reused across calls instead of a stack array each time, since DOS stack space is precious. */
static int g_col_table[MAX_LINE_LEN + 1];

/* Raw-column offsets where each wrapped visual row of `text` begins in Writer view. Prefers breaking at the most recent space for real word-wrap; a run with no space hard-breaks at the column limit. Screen columns are computed once via build_screen_col_table() rather than per candidate column. Most callers should use get_line_wraps() below instead, which caches per line. */
int compute_wrap_starts(const char *text, int *starts, int in_code)
{
    int len = (int) strlen(text);
    int seg_start = 0, nstarts = 1;
    int i, base_col, screen_col, last_space, brk;
    int wrapped;

    starts[0] = 0;

    if (!in_code && len >= 3) {
        int all_dash = 1, ii;
        for (ii = 0; ii < len; ii++) if (text[ii] != '-') { all_dash = 0; break; }
        if (all_dash) return 1;   /* horizontal rule: always one row */
    }

    build_screen_col_table(text, g_col_table, in_code);

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

/* Cache-aware front end for compute_wrap_starts(): recomputes only if the line's text changed since the cache was last filled, making repeated wrap queries for an unchanged line effectively free. */
int get_line_wraps(int line_no, int *starts)
{
    Line *l = doc[line_no];
    int i;
    ensure_code_state();
    if (l->wrap_dirty) {
        l->wrap_nstarts = compute_wrap_starts(l->text, l->wrap_starts, line_in_code(line_no));
        l->wrap_dirty = 0;
    }
    for (i = 0; i < l->wrap_nstarts; i++) starts[i] = l->wrap_starts[i];
    return l->wrap_nstarts;
}

/* Number of visual rows `line_no` occupies in the current view -- always 1 in raw Markdown view, which scrolls horizontally instead. */
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

/* The raw column whose Writer-view screen column is closest to target_col without exceeding it -- keeps the cursor's screen column stable when Up/Down crosses a wrapped row. Builds the line's screen-column table once instead of scanning per candidate column. */
int col_for_target_screen(const char *text, int lo, int hi, int target_col, int in_code)
{
    int base, best = lo, c;
    build_screen_col_table(text, g_col_table, in_code);
    base = g_col_table[lo];
    for (c = lo; c <= hi; c++) {
        if (g_col_table[c] - base > target_col) break;
        best = c;
    }
    return best;
}

/* ================= cursor movement ================= */
/* These just update cur_line/cur_col and wrap bookkeeping; they don't request a redraw themselves. Every call site goes through do_move() below, which picks the cheapest correct redraw. */

void move_left(void)
{
    if (cur_col > 0) {
        ensure_code_state();
        cur_col = (view_mode == 0) ? writer_move_left(doc[cur_line]->text, cur_col, line_in_code(cur_line))
                                    : cur_col - 1;
    } else if (cur_line > 0) {
        cur_line--;
        cur_col = doc[cur_line]->len;
    }
}
void move_right(void)
{
    if (cur_col < doc[cur_line]->len) {
        ensure_code_state();
        cur_col = (view_mode == 0) ? writer_move_right(doc[cur_line]->text, cur_col, line_in_code(cur_line))
                                    : cur_col + 1;
    } else if (cur_line < doc_count - 1) {
        cur_line++;
        cur_col = 0;
    }
}
/* Up/Down in Writer view step by visual row, not buffer line, moving between wrap segments before crossing into the next buffer line. Raw Markdown view keeps the old one-line-per-row behavior. */
void move_up(void)
{
    int starts[MAX_WRAP_ROWS], n, seg, target, in_code;
    if (view_mode == 1) {
        if (cur_line > 0) {
            cur_line--;
            if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
        }
        return;
    }
    ensure_code_state();
    in_code = line_in_code(cur_line);
    n = get_line_wraps(cur_line, starts);
    seg = wrap_seg_of_col(starts, n, cur_col);
    target = writer_screen_col(doc[cur_line]->text, cur_col, in_code)
           - writer_screen_col(doc[cur_line]->text, starts[seg], in_code);
    if (seg > 0) {
        cur_col = col_for_target_screen(doc[cur_line]->text, starts[seg - 1],
                                          starts[seg] - 1, target, in_code);
    } else if (cur_line > 0) {
        int pstarts[MAX_WRAP_ROWS], pn;
        cur_line--;
        in_code = line_in_code(cur_line);
        pn = get_line_wraps(cur_line, pstarts);
        cur_col = col_for_target_screen(doc[cur_line]->text, pstarts[pn - 1],
                                          doc[cur_line]->len, target, in_code);
    }
}
void move_down(void)
{
    int starts[MAX_WRAP_ROWS], n, seg, target, in_code;
    if (view_mode == 1) {
        if (cur_line < doc_count - 1) {
            cur_line++;
            if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
        }
        return;
    }
    ensure_code_state();
    in_code = line_in_code(cur_line);
    n = get_line_wraps(cur_line, starts);
    seg = wrap_seg_of_col(starts, n, cur_col);
    target = writer_screen_col(doc[cur_line]->text, cur_col, in_code)
           - writer_screen_col(doc[cur_line]->text, starts[seg], in_code);
    if (seg + 1 < n) {
        int seg_end = (seg + 2 < n) ? starts[seg + 2] - 1 : doc[cur_line]->len;
        cur_col = col_for_target_screen(doc[cur_line]->text, starts[seg + 1], seg_end, target, in_code);
    } else if (cur_line < doc_count - 1) {
        int nstarts[MAX_WRAP_ROWS], nn, hi;
        cur_line++;
        in_code = line_in_code(cur_line);
        nn = get_line_wraps(cur_line, nstarts);
        hi = (nn > 1) ? nstarts[1] - 1 : doc[cur_line]->len;
        cur_col = col_for_target_screen(doc[cur_line]->text, 0, hi, target, in_code);
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

/* Requests the cheapest redraw: just the status bar readout and the hardware cursor, leaving every on-screen character alone. Only valid when no document text changed. */
void request_cursor_redraw(void)
{
    if (screen_dirty == 0) screen_dirty = 3;
}

/* Wraps a move_/page_ function, then picks the cheapest correct redraw: nothing for a true no-op, a full repaint if the viewport scrolled, otherwise just the status bar and cursor since movement never touches document content. */
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

/* Grows top_line until the cursor's visual row lands within the TEXT_ROWS window. Raw Markdown view keeps the old exact-line-count logic. */
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

/* After a single-character edit, decides whether the effect stayed confined to one on-screen line and requests the cheap redraw_line_only() path if so, falling back to a full redraw if the line rewrapped, the viewport scrolled, or horizontal scroll shifted. */
void redraw_after_char_edit(int line_no, int old_nrows, int old_top, int old_left)
{
    int new_nrows;

    if (!code_state_valid) { request_full_redraw(); return; }

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

/* Call this instead of insert_char() directly from key dispatch (not from cmd_paste, which wants one redraw per whole paste). An active selection forces a full redraw regardless, since it can span more than one line. */
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

/* Call this instead of backspace() directly from key dispatch. Backspace on a list line that's nothing but its own marker steps the item back out one indent level instead of deleting a character -- same convention as most outliner/Markdown editors. */
void do_backspace(void)
{
    int line_no = cur_line;
    int old_nrows, old_top, old_left;
    Line *l = doc[cur_line];

    if (!sel_active && cur_col > 0 && cur_col == l->len) {
        int indent = list_indent_of(l->text, l->len);
        if (indent >= 0 && l->len == indent + 2) {
            if (indent > 0) {
                do_list_indent(-1);
            } else {
                old_nrows = line_rows(line_no);
                old_top = top_line;
                old_left = left_col;
                save_undo(line_no);
                l->text[0] = '\0';
                l->len = 0;
                line_mark_dirty(l);
                cur_col = 0;
                modified = 1;
                redraw_after_char_edit(line_no, old_nrows, old_top, old_left);
            }
            return;
        }
    }

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

/* Call this instead of delete_forward() directly from key dispatch. Only the plain intra-line case is eligible for the cheap redraw path; the merge-with-next-line case requests a full redraw itself. */
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

/* ================= list indent (Tab / Shift+Tab) ================= */

/* Indent depth of the nearest list item at or above `line_no`, skipping blank spacer lines. Returns -1 if it hits a non-blank non-list line or the top of the document. Only used when indenting; Shift+Tab doesn't need it. */
int prev_list_indent(int line_no)
{
    int i;
    for (i = line_no - 1; i >= 0; i--) {
        if (doc[i]->len == 0) continue;
        return list_indent_of(doc[i]->text, doc[i]->len);
    }
    return -1;
}

/* Tab/Shift+Tab. On a list line, shifts the marker one LIST_INDENT_UNIT level in or out, capped to one level past the nearest parent item and by MAX_LIST_INDENT. On any other line, falls back to ordinary editor Tab/Shift+Tab. Only touches the current line, not a multi-line selection. */
void do_list_indent(int dir)
{
    int line_no = cur_line;
    Line *l = doc[line_no];
    int indent = list_indent_of(l->text, l->len);
    int new_indent, delta;
    int old_nrows, old_top, old_left;

    if (indent < 0) {
        /* Not a list line: plain editor indent/outdent instead of the sublist marker-shifting logic below. */
        if (dir > 0) {
            int n;
            if (l->len + LIST_INDENT_UNIT > MAX_LINE_LEN) {
                flash_error("Line too long to indent.");
                return;
            }
            for (n = 0; n < LIST_INDENT_UNIT; n++) insert_char(' ');
            request_full_redraw();
        } else {
            int lead = 0;
            int was_fence = is_fence_line(l);
            while (lead < l->len && l->text[lead] == ' ') lead++;
            if (lead == 0) return;   /* no leading whitespace to strip */
            delta = (lead < LIST_INDENT_UNIT) ? lead : LIST_INDENT_UNIT;
            save_undo(line_no);
            memmove(l->text, l->text + delta, (size_t) (l->len - delta + 1));
            l->len -= delta;
            line_mark_dirty(l);
            if (is_fence_line(l) != was_fence) code_state_valid = 0;
            cur_col -= delta;
            if (cur_col < 0) cur_col = 0;
            modified = 1;
            request_full_redraw();
        }
        return;
    }

    if (dir > 0) {
        int p_indent = prev_list_indent(line_no);
        int cap = (p_indent >= 0) ? p_indent + LIST_INDENT_UNIT : indent;
        new_indent = indent + LIST_INDENT_UNIT;
        if (new_indent > MAX_LIST_INDENT) {
            flash_error("Can't indent further: max list depth reached.");
            return;
        }
        if (new_indent > cap) {
            flash_error("Can't indent further: no parent item at that level.");
            return;
        }
    } else {
        new_indent = indent - LIST_INDENT_UNIT;
        if (new_indent < 0) return;   /* already at the left margin */
    }

    if (l->len + LIST_INDENT_UNIT > MAX_LINE_LEN) {
        flash_error("Line too long to indent.");
        return;
    }

    old_nrows = line_rows(line_no);
    old_top = top_line;
    old_left = left_col;

    delta = new_indent - indent;
    save_undo(line_no);
    if (delta > 0) {
        /* Opens a `delta`-wide gap at the start of the line, then fills it with spaces, using the same memmove idiom as insert_char. */
        memmove(l->text + delta, l->text, (size_t) (l->len + 1));
        memset(l->text, ' ', (size_t) delta);
    } else {
        /* remove -delta leading spaces */
        memmove(l->text, l->text - delta, (size_t) (l->len + 1 + delta));
    }
    l->len += delta;
    line_mark_dirty(l);
    cur_col += delta;
    if (cur_col < 0) cur_col = 0;
    modified = 1;

    redraw_after_char_edit(line_no, old_nrows, old_top, old_left);
}

/* ================= rendering ================= */

/* Selection highlight helpers, called from render_line at the point each visible glyph is emitted with its raw source column, so highlighting is decided per character against the raw selection range -- hidden delimiters are never drawn or asked about, so they can't shift the highlight. */

/* True if raw range [r0, r1) overlaps the row's selected range [sel_start, sel_end); sel_start == -1 means nothing selected. */
int sel_overlaps(int r0, int r1, int sel_start, int sel_end)
{
    return sel_start >= 0 && r0 < sel_end && r1 > sel_start;
}

/* Reverse-video swap: highlights a cell regardless of its current attribute, so selection reads consistently over every style. */
unsigned char swap_attr(unsigned char attr)
{
    return (unsigned char) (((attr & 0x0F) << 4) | ((attr & 0xF0) >> 4));
}

/* attr, highlighted if the single raw column raw_col is selected. */
unsigned char apply_sel(unsigned char attr, int raw_col, int sel_start, int sel_end)
{
    return sel_overlaps(raw_col, raw_col + 1, sel_start, sel_end) ? swap_attr(attr) : attr;
}

/* Renders one buffer line into one screen row. Raw view shows text as typed with horizontal scroll; Writer view is a single-pass scanner checking whole-line markers (heading/quote/list/HR) first, then toggling inline styles left to right. sel_start/sel_end give the row's selected raw range, from sel_line_range(). */
/* Raw Markdown view: one buffer line per screen row, unwrapped, scrolled by `offset`. Writes each cell exactly once through row_ptr(), instead of the old clear-then-overwrite which touched covered cells twice. */
void render_line(const char *text, int row, int offset, int sel_start, int sel_end)
{
    unsigned int far *rp = row_ptr(row);
    int i, len = (int) strlen(text);
    int col = 0;
    for (i = offset; i < len && col < SCREEN_COLS; i++, col++)
        rp[col] = CELL(text[i], apply_sel(ATTR_NORMAL, i, sel_start, sel_end));
    for (; col < SCREEN_COLS; col++) rp[col] = CELL(' ', ATTR_NORMAL);
}

/* Writer view: draws one visual row -- the word-wrapped slice [seg_start, seg_end) compute_wrap_starts chose, which already fits within SCREEN_COLS at a word boundary. For a continuation row, inline style state is recovered by silently replaying the scanner up to seg_start without drawing anything. */
void render_writer_line(const char *text, int row, int seg_start, int seg_end,
                         int sel_start, int sel_end, int in_code)
{
    unsigned int far *rp = row_ptr(row);
    int i, col = 0, len = (int) strlen(text);
    int bold = 0, italic = 0, code = 0, strike = 0;
    unsigned char attr;
    int last_col = -1;
    char last_ch = ' ';
    unsigned char last_attr = ATTR_NORMAL;

    /* code fence / code block content: verbatim characters, ATTR_CODE, no markdown parsing at all. */
    if (in_code) {
        for (i = seg_start, col = 0; i < len && i < seg_end && col < SCREEN_COLS; i++, col++)
            rp[col] = CELL(text[i], apply_sel(ATTR_CODE, i, sel_start, sel_end));
        for (; col < SCREEN_COLS; col++) rp[col] = CELL(' ', ATTR_CODE);
        return;
    }

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
    {
        int indent = list_indent_of(text, len);
        if (indent >= 0) {
            if (seg_start == 0) {
                /* Leading indent spaces render as literal columns, so a sublist actually looks indented; only the '- ' marker itself collapses into the bullet glyph. */
                int s;
                for (s = 0; s < indent && col < SCREEN_COLS; s++, col++)
                    rp[col] = CELL(' ', apply_sel(ATTR_NORMAL, s, sel_start, sel_end));
                if (col < SCREEN_COLS) {
                    attr = sel_overlaps(indent, indent + 2, sel_start, sel_end)
                         ? swap_attr(ATTR_LISTMARK) : ATTR_LISTMARK;
                    rp[col] = CELL((char) 7, attr);  /* CP437 bullet glyph */
                    col++;
                }
                i = indent + 2;
            } else {
                i = indent + 2;   /* continuation row: replay starts right after "<indent>- " */
            }
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

    /* Blanks whatever's left of the row past the rendered text, same one-write-per-cell approach as render_line. */
    for (; col < SCREEN_COLS; col++) rp[col] = CELL(' ', ATTR_NORMAL);
}

/* Status bar, laid out like a Neovim statusline: [NORMAL/INSERT] filename* | status msg | Rich 42% 17:6. The mode block only shows while vim_mode is on; everything on the right stays right-aligned. */
void draw_status_bar(void)
{
    char left[SCREEN_COLS + 1];
    char right[32];
    int col = 0, rlen, rcol, percent;
    const char *sel_tag = sel_active ? " Sel" : "";
    /* flash_active only controls how long a routine confirmation sticks around; error_flash_active is what puts the bar on the eye-catching ATTR_STATUS_FLASH background for the first few seconds of an error, then it drops back to plain while the message stays displayed. */
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

/* Bottom row: menu category names, Alt-navigable. Replaces the old static hint line -- same row, interactive now. */
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
        /* Selected row is already fully highlighted, so the mnemonic only needs its own color when the row isn't selected. */
        unsigned char hot = (i == menu_sel) ? ATTR_CMDBAR_SEL : ATTR_POPUP_HOT;
        r = top + i;
        fill_rect(col, r, width, base);
        put_string(col + 1, r, m->items[i], base);
        put_char(col + 1 + m->mnemonic_idx[i], r, m->items[i][m->mnemonic_idx[i]], hot);
    }
}

/* Draws the shared bottom row (menu bar/popup or status bar) and places the hardware cursor. Shared by redraw_screen() and redraw_line_only(), since both need it regardless of how much of the document view moved. */
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
            int starts[MAX_WRAP_ROWS], n, seg, rows, l, in_code;
            ensure_code_state();
            in_code = line_in_code(cur_line);
            n = get_line_wraps(cur_line, starts);
            seg = wrap_seg_of_col(starts, n, cur_col);
            rows = 0;
            for (l = top_line; l < cur_line; l++) rows += line_rows(l);
            screen_row = rows + seg;
            screen_col = writer_screen_col(doc[cur_line]->text, cur_col, in_code)
                       - writer_screen_col(doc[cur_line]->text, starts[seg], in_code);
        }
        if (screen_col < 0) screen_col = 0;
        if (screen_col >= SCREEN_COLS) screen_col = SCREEN_COLS - 1;
        if (screen_row < 0) screen_row = 0;
        if (screen_row >= TEXT_ROWS) screen_row = TEXT_ROWS - 1;
        set_cursor(screen_col, screen_row);
    }
}

/* Cheap redraw path for an edit confirmed confined to `line_no`: repaints just that line's row(s) plus the status bar and cursor, leaving everything else untouched. Falls back to a full redraw if the line isn't currently visible. */
void redraw_line_only(int line_no)
{
    int sel_start, sel_end, r;

    if (view_mode == 1) {
        r = line_no - top_line;
        if (r < 0 || r >= TEXT_ROWS) { redraw_screen(); return; }
        sel_line_range(line_no, doc[line_no]->len, &sel_start, &sel_end);
        render_line(doc[line_no]->text, r, left_col, sel_start, sel_end);
    } else {
        int starts[MAX_WRAP_ROWS], n, seg, rows_above, l, in_code;
        ensure_code_state();
        in_code = line_in_code(line_no);
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
                render_writer_line(doc[line_no]->text, r, starts[seg], seg_end, sel_start, sel_end, in_code);
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
        /* Writer view: each buffer line may span several wrapped visual rows, so walk from top_line drawing every wrap segment until the screen fills. */
        left_col = 0;
        r = 0; ln = top_line;
        ensure_code_state();
        while (r < TEXT_ROWS) {
            if (ln >= doc_count) {
                clear_row(r, ATTR_NORMAL);
                r++;
                continue;
            }
            {
                int starts[MAX_WRAP_ROWS], n, seg, sel_start, sel_end, in_code;
                in_code = line_in_code(ln);
                n = get_line_wraps(ln, starts);
                sel_line_range(ln, doc[ln]->len, &sel_start, &sel_end);
                for (seg = 0; seg < n && r < TEXT_ROWS; seg++, r++) {
                    int seg_end = (seg + 1 < n) ? starts[seg + 1] : doc[ln]->len;
                    render_writer_line(doc[ln]->text, r, starts[seg], seg_end, sel_start, sel_end, in_code);
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

/* Reads one full source line from f into a heap-allocated buffer, however long it actually is, with any trailing '\n' stripped. Avoids the fixed-fgets bug of silently chopping a too-long line mid-word. */
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
                /* Only reached if a line is longer than MAX_LINE_LEN even after the bump above; backs up to the last space so the forced split lands between words, same as compute_wrap_starts prefers. */
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
    code_state_valid = 0;
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

/* Copies the selection into the clipboard buffer, lines joined with '\n'. Doesn't touch the document or clear the selection -- Copy is read-only, unlike Cut. */
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
    /* Only claims 'Cut.' if sel_delete actually removed something; otherwise it leaves sel_delete's own explanatory status_msg in place instead of overwriting it. */
    if (sel_delete()) flash_status("Cut.");
}

/* Inserts the clipboard at the cursor, replacing an active selection first. Embedded '\n's become real line splits, so paste inherits the same length/line-count guards as ordinary typing. */
void cmd_paste(void)
{
    int i, ok = 1;
    if (clip_len == 0) { flash_status_for("Clipboard empty.", FLASH_SECS(2)); return; }
    if (sel_active) sel_delete();
    for (i = 0; i < clip_len && ok; i++) {
        if (clipboard[i] == '\n') ok = split_line();
        else ok = insert_char((unsigned char) clipboard[i]);
    }
    /* Stops and leaves split_line/insert_char's own error message in place if the paste ran out of room, rather than papering over it with a blanket "Pasted." */
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

/* Guards Replace All against a replacement containing the search string itself, which would create matches forever. Set high enough to never fire on realistic usage. */
#define REPLACE_LIMIT 200000

/* WordStar-style per-match confirmation: (Y)es replaces this one, (N)o skips it, (A)ll replaces every remaining match, anything else (including Esc) stops. */
int prompt_replace_choice(void)
{
    int ch;
    clear_row(CMDBAR_ROW, ATTR_CMDBAR);
    put_string(0, CMDBAR_ROW, "Replace? (Y)es (N)o (A)ll (Esc) Quit", ATTR_CMDBAR);
    ch = tolower(_bios_keybrd(_KEYBRD_READ) & 0xFF);
    if (ch == 'y' || ch == 'n' || ch == 'a') return ch;
    return 'q';
}

/* Find and replace. Each accepted replace is a sel_delete() plus insert_char() loop, so it inherits the same single-line-undo limit as cmd_paste. Search resumes after the cursor, wrapping to the top; hitting the first-seen match again means the whole document was covered, so it stops instead of re-prompting forever. */
void cmd_replace(void)
{
    char find_buf[80], repl_buf[80], msg[32];
    int replace_all = 0, had_match = 0, count = 0;
    long guard = 0;
    int rlen, i, ch = 0, truncated = 0;
    int sline, scol;
    int first_line = -1, first_col = -1;

    if (!prompt_input("Find: ", find_buf, sizeof(find_buf)) || !find_buf[0]) return;
    if (!prompt_input("Replace with: ", repl_buf, sizeof(repl_buf))) return;
    strcpy(last_search, find_buf);
    rlen = (int) strlen(repl_buf);

    sline = cur_line;
    scol  = cur_col + 1;   /* same "search strictly after cursor" rule as cmd_find */

    for (;;) {
        if (!find_from(sline, scol, find_buf) && !find_from(0, 0, find_buf)) break;
        had_match = 1;

        if (first_line < 0) {
            first_line = cur_line;
            first_col  = cur_col;
        } else if (cur_line == first_line && cur_col == first_col) {
            break;   /* wrapped all the way back to the first match: done */
        }

        if (++guard > REPLACE_LIMIT) {
            flash_error("Replace stopped: too many matches (check replacement text).");
            break;
        }

        /* find_from() only marks the screen dirty; since cmd_replace() loops here without returning to main(), it needs an explicit repaint so the view scrolls to each match before asking Y/N/A. */
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

        /* find_from already left the cursor at the match's start; select exactly the matched span and reuse the same delete+insert pair cmd_paste() uses. */
        anchor_line = cur_line;
        anchor_col  = cur_col;
        cur_col += (int) strlen(find_buf);
        sel_active = 1;
        sel_delete();
        for (i = 0; i < rlen; i++) {
            if (!insert_char((unsigned char) repl_buf[i])) { truncated = 1; break; }
        }
        count++;
        if (truncated) break;   /* insert_char already set its own "Line full." error */
        sline = cur_line;
        scol  = cur_col;    /* resume right after the inserted text */
    }

    if (truncated) {
        /* Leaves insert_char's own "Line full." message in place -- more actionable than a blanket success message. */
    } else if (count > 0) {
        sprintf(msg, "Replaced %d.", count);
        flash_status(msg);
    } else if (had_match) {
        flash_status("No replacements made.");
    } else {
        flash_status("Not found.");
    }
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

/* Minimal ":" command line: :w save, :q quit, :wq save+quit, :q! quit without saving. Nothing beyond these four. */
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

/* About screen: a standalone centered box, since it's a whole screen to read and dismiss rather than a list to pick from. Any key closes it, then requests a full redraw since the box was painted over the document. */
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

        /* No key waiting yet: keep polling, redrawing only when Alt's held/released state actually changes, so the menu preview and status bar react instantly with no keystroke required. Also clears a flash message once its timer runs out. */
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
                    /* Just drop the attention-flash color -- the error text in status_msg is sticky and stays put. */
                    error_flash_active = 0;
                    redraw_screen();
                }
            }
            _asm { hlt }
        }
        alt_held = 0;
        if (flash_active) {
            /* Any real keystroke dismisses a flash message right away too, even if the 5 seconds haven't elapsed. */
            flash_active = 0;
            strcpy(status_msg, "Ready.");
            request_full_redraw();
        }
        if (error_flash_active) {
            /* Unlike a routine flash, the error's attention-color always gets its full FLASH_SECS(5) before fading, regardless of keystrokes -- it's just checked here instead of in the poll loop if typing outran it. */
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
                /* Any other (non-Alt) key: check it against this menu's mnemonic letters -- typing the highlighted letter runs that item immediately, same as arrowing to it and pressing Enter. */
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
            /* Vim Normal sub-mode. Plain letter/punctuation keys are commands here, not text -- Ctrl-shortcuts, Enter, and Backspace still work the same as non-vim mode. */
            if (lo == 13) do_split_line();
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
            else if (lo == 9)  { do_list_indent(1); vim_pending = 0; }
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
            /* Esc no longer quits (see Alt+X below); it now only clears an active selection, or in vim mode drops back from Insert to Normal, otherwise it's a no-op. */
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
                        /* cursor-movement keys: Shift held extends (or starts) the selection; unmodified, they clear it. */
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
                    case 0x0F: do_list_indent(-1); break;       /* Shift+Tab */
                    case 0x3B: cmd_about(); break;                 /* F1 */
                    case 0x3C: view_mode = !view_mode; request_full_redraw(); break;  /* F2 */
                    case 0x3D: cmd_find_next();  break;         /* F3 */
                    case 0x3E: toggle_vim_mode(); break;        /* F4 */
                    case 0x2D: cmd_quit_request(); break;       /* Alt+X */
                    default: break;
                }
            }
        } else if (lo == 13) do_split_line();
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
        else if (lo == 9)  do_list_indent(1);
        else if (lo >= 32 && lo < 127) do_insert_char(lo);

        if (want_quit) break;
    }

    clear_screen(ATTR_NORMAL);
    set_cursor(0, 0);
    return 0;
}

