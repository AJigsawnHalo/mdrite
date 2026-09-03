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
#include <bios.h>

/* ---------- screen / buffer constants ---------- */
#define SCREEN_COLS   80
#define SCREEN_ROWS   25
#define STATUS_ROW    (SCREEN_ROWS - 2)
#define CMDBAR_ROW    (SCREEN_ROWS - 1)
#define TEXT_ROWS     (SCREEN_ROWS - 2)
#define MAX_LINE_LEN  200
#define MAX_LINES     2000

/* ---------- color attributes ---------- */
#define ATTR_NORMAL      0x07   /* light grey / black */
#define ATTR_BOLD        0x0F   /* bright white / black */
#define ATTR_ITALIC      0x0B   /* bright cyan / black */
#define ATTR_CODE        0x0D   /* bright magenta / black */
#define ATTR_STRIKE      0x0C   /* bright red / black -- color only, no literal line */
#define ATTR_HEAD        0x1E   /* bright yellow / blue background */
#define ATTR_QUOTE       0x5F   /* bright white / magenta background */
#define ATTR_LINK        0x09   /* bright blue / black */
#define ATTR_LISTMARK    0x0A   /* bright green / black, bullet glyph only */
#define ATTR_STATUS      0x70   /* black / light grey */
#define ATTR_CMDBAR      0x30   /* black / cyan */
#define ATTR_CMDBAR_HOT  0x3E   /* bright yellow / cyan -- menu hotkey letter */
#define ATTR_CMDBAR_SEL  0x4F   /* bright white / red -- open/selected menu (kept
                                    away from 0x70 so it doesn't fuse with the
                                    status bar directly above it) */
#define ATTR_POPUP       0x1F   /* bright white / blue -- dropdown body */

unsigned char far *video = (unsigned char far *) 0xB8000000L;

typedef struct {
    char text[MAX_LINE_LEN + 1];
    int  len;
} Line;

Line *doc[MAX_LINES];
int  doc_count = 1;

int  cur_line = 0, cur_col = 0;
int  top_line = 0;
int  left_col = 0;
int  modified = 0;
char filename[80] = "";
int  view_mode = 0;     /* 0 = Writer (rendered), 1 = raw Markdown */
char status_msg[80] = "Ready.";
char last_search[80] = "";
int  want_quit = 0;

/* optional vim-lite keymapping (off by default, toggle with F4
 * or View > Vim Keys). See the file header for exactly what subset
 * of vim this covers -- it's intentionally not a full emulation. */
int  vim_mode = 0;
int  vim_insert = 0;    /* 0 = Normal sub-mode, 1 = Insert sub-mode */
int  vim_pending = 0;   /* holds the first key of a two-key command, e.g. 'd' of dd */

/* single-level undo: remembers ONE line's previous contents.
 * See the file header for what it does and doesn't cover. */
Line undo_line;
int  undo_line_no = -1;
int  undo_col = 0;

/* ---------- Alt-driven pull-down menu ---------- */
#define MENU_COUNT      4
#define MAX_MENU_ITEMS  5

typedef struct {
    char label[12];
    unsigned char altkey_scan;   /* BIOS scan code for Alt+<letter> */
    char items[MAX_MENU_ITEMS][20];
    int  item_count;
} MenuCategory;

MenuCategory menus[MENU_COUNT] = {
    { "File",   0x21, { "New        ^N", "Open       ^O", "Save       ^S",
                         "Save As    ^A", "Quit       Alt+X" }, 5 },
    { "Edit",   0x12, { "Undo       ^Z", "Cut        ^X", "Copy       ^C",
                         "Paste      ^V" }, 4 },
    { "Search", 0x1F, { "Find       ^F", "Find Next  F3", "Go To Line ^G" }, 3 },
    { "View",   0x2F, { "Toggle View F2", "Vim Keys   F4" }, 2 }
};

int menu_open = -1;
int menu_sel  = 0;
int menu_col[MENU_COUNT];

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

/* ================= video primitives ================= */

void put_char(int col, int row, char ch, unsigned char attr)
{
    int off;
    if (col < 0 || col >= SCREEN_COLS || row < 0 || row >= SCREEN_ROWS) return;
    off = (row * SCREEN_COLS + col) * 2;
    video[off] = (unsigned char) ch;
    video[off + 1] = attr;
}

void put_string(int col, int row, const char *s, unsigned char attr)
{
    int i;
    for (i = 0; s[i]; i++) put_char(col + i, row, s[i], attr);
}

void fill_rect(int col, int row, int w, unsigned char attr)
{
    int c;
    for (c = 0; c < w; c++) put_char(col + c, row, ' ', attr);
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
    return l;
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
        strcpy(status_msg, "Nothing to undo.");
        return;
    }
    sel_clear();
    strcpy(doc[undo_line_no]->text, undo_line.text);
    doc[undo_line_no]->len = undo_line.len;
    cur_line = undo_line_no;
    cur_col = undo_col;
    undo_line_no = -1;
    strcpy(status_msg, "Undid last edit.");
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
        for (i = sc; i < l->len - n; i++) l->text[i] = l->text[i + n];
        l->len -= n;
        l->text[l->len] = '\0';
    } else {
        Line *startl = doc[sl];
        Line *endl = doc[el];
        int suffix_len = endl->len - ec;
        int shift;
        if (startl->len + suffix_len > MAX_LINE_LEN) {
            strcpy(status_msg, "Selection too long to delete across lines.");
            sel_clear();
            return 0;
        }
        startl->text[sc] = '\0';
        startl->len = sc;
        strcat(startl->text, endl->text + ec);
        startl->len += suffix_len;
        for (i = sl + 1; i <= el; i++) free(doc[i]);
        shift = el - sl;
        for (i = el + 1; i < doc_count; i++) doc[i - shift] = doc[i];
        doc_count -= shift;
        undo_line_no = -1;  /* spans lines: not representable by single-line undo */
    }
    cur_line = sl;
    cur_col = sc;
    sel_clear();
    modified = 1;
    return 1;
}

/* Returns 1 on success, 0 if the line was already full (status_msg
 * is set to explain why in that case). Ordinary typing ignores the
 * return value; cmd_paste uses it to notice a truncated paste. */
int insert_char(int ch)
{
    Line *l = doc[cur_line];
    int i;
    if (l->len >= MAX_LINE_LEN) { strcpy(status_msg, "Line full."); return 0; }
    sel_clear();
    save_undo(cur_line);
    for (i = l->len; i > cur_col; i--) l->text[i] = l->text[i - 1];
    l->text[cur_col] = (char) ch;
    l->len++;
    l->text[l->len] = '\0';
    cur_col++;
    modified = 1;
    return 1;
}

void backspace(void)
{
    Line *prev, *cur;
    int i;
    sel_clear();
    if (cur_col > 0) {
        Line *l = doc[cur_line];
        save_undo(cur_line);
        for (i = cur_col - 1; i < l->len - 1; i++) l->text[i] = l->text[i + 1];
        l->len--;
        l->text[l->len] = '\0';
        cur_col--;
        modified = 1;
        return;
    }
    if (cur_line == 0) return;

    prev = doc[cur_line - 1];
    cur  = doc[cur_line];
    if (prev->len + cur->len > MAX_LINE_LEN) {
        strcpy(status_msg, "Can't merge: line too long.");
        return;
    }
    {
        int newcol = prev->len;
        strcat(prev->text, cur->text);
        prev->len += cur->len;
        free(cur);
        for (i = cur_line; i < doc_count - 1; i++) doc[i] = doc[i + 1];
        doc_count--;
        cur_line--;
        cur_col = newcol;
        modified = 1;
        undo_line_no = -1;
    }
}

void delete_forward(void)
{
    Line *l = doc[cur_line];
    int i;
    sel_clear();
    if (cur_col < l->len) {
        save_undo(cur_line);
        for (i = cur_col; i < l->len - 1; i++) l->text[i] = l->text[i + 1];
        l->len--;
        l->text[l->len] = '\0';
        modified = 1;
        return;
    }
    if (cur_line < doc_count - 1) {
        Line *next = doc[cur_line + 1];
        int i2;
        if (l->len + next->len <= MAX_LINE_LEN) {
            strcat(l->text, next->text);
            l->len += next->len;
            free(next);
            for (i2 = cur_line + 1; i2 < doc_count - 1; i2++) doc[i2] = doc[i2 + 1];
            doc_count--;
            modified = 1;
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
    int i;
    if (doc_count >= MAX_LINES) { strcpy(status_msg, "Document full."); return 0; }
    sel_clear();
    nl = new_line();
    strcpy(nl->text, l->text + cur_col);
    nl->len = l->len - cur_col;
    l->text[cur_col] = '\0';
    l->len = cur_col;
    for (i = doc_count; i > cur_line + 1; i--) doc[i] = doc[i - 1];
    doc[cur_line + 1] = nl;
    doc_count++;
    cur_line++;
    cur_col = 0;
    modified = 1;
    undo_line_no = -1;
    return 1;
}

/* Removes the whole current line -- used by vim's "dd". Not exposed
 * outside vim mode since there's no non-vim key bound to it. Like
 * split_line/merge, this is a structural change the single-line undo
 * can't represent, so it invalidates it rather than misrepresenting it. */
void delete_current_line(void)
{
    int i;
    sel_clear();
    if (doc_count <= 1) {
        doc[0]->text[0] = '\0';
        doc[0]->len = 0;
        cur_col = 0;
        modified = 1;
        return;
    }
    free(doc[cur_line]);
    for (i = cur_line; i < doc_count - 1; i++) doc[i] = doc[i + 1];
    doc_count--;
    if (cur_line >= doc_count) cur_line = doc_count - 1;
    cur_col = 0;
    modified = 1;
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
 * label's characters individually instead of returning early. */
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

/* ================= cursor movement ================= */

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
void move_up(void)
{
    if (cur_line > 0) {
        cur_line--;
        if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
    }
}
void move_down(void)
{
    if (cur_line < doc_count - 1) {
        cur_line++;
        if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
    }
}
void move_home(void) { cur_col = 0; }
void move_end(void)  { cur_col = doc[cur_line]->len; }

void page_up(void)
{
    cur_line -= TEXT_ROWS;
    if (cur_line < 0) cur_line = 0;
    if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
}
void page_down(void)
{
    cur_line += TEXT_ROWS;
    if (cur_line >= doc_count) cur_line = doc_count - 1;
    if (cur_col > doc[cur_line]->len) cur_col = doc[cur_line]->len;
}

void scroll_to_cursor(void)
{
    if (cur_line < top_line) top_line = cur_line;
    if (cur_line >= top_line + TEXT_ROWS) top_line = cur_line - TEXT_ROWS + 1;
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
void render_line(const char *text, int row, int offset, int sel_start, int sel_end)
{
    int i = 0, col = 0, len = strlen(text);
    int bold = 0, italic = 0, code = 0, strike = 0, scr;
    unsigned char attr;
    /* Tracks the most recently drawn VISIBLE character, so that when
     * a hidden run right after it (a bold/code/strike delimiter, or
     * a link's closing ](url)) is part of the selection, we can retroactively
     * re-highlight that one already-drawn cell as a "selection
     * continues past here, invisibly" signal. Without this there's
     * no way to see on screen whether you've actually selected past
     * a closing delimiter or stopped just short of it -- both look
     * identical, since the hidden characters occupy zero screen
     * width either way. Deliberately just recolors an existing cell
     * rather than adding a new one, so it can't disturb
     * writer_screen_col's zero-width assumption for hidden runs. */
    int last_col = -1;
    char last_ch = ' ';
    unsigned char last_attr = ATTR_NORMAL;

    clear_row(row, ATTR_NORMAL);

    if (view_mode == 1) {
        for (i = 0; i < len; i++) {
            scr = i - offset;
            if (scr >= 0 && scr < SCREEN_COLS)
                put_char(scr, row, text[i], apply_sel(ATTR_NORMAL, i, sel_start, sel_end));
        }
        return;
    }

    /* horizontal rule: a line that is nothing but 3+ hyphens */
    if (len >= 3) {
        int all_dash = 1, ii;
        for (ii = 0; ii < len; ii++) if (text[ii] != '-') { all_dash = 0; break; }
        if (all_dash) {
            for (col = 0; col < SCREEN_COLS; col++) {
                attr = (col < len) ? apply_sel(ATTR_NORMAL, col, sel_start, sel_end)
                                    : ATTR_NORMAL;
                put_char(col, row, (char) 196, attr);  /* CP437 horizontal line */
            }
            return;
        }
    }

    /* heading */
    if (text[0] == '#') {
        while (i < len && text[i] == '#') i++;
        if (i < len && text[i] == ' ') i++;
        for (; i < len && col < SCREEN_COLS; i++, col++)
            put_char(col, row, text[i], apply_sel(ATTR_HEAD, i, sel_start, sel_end));
        return;
    }

    /* blockquote */
    if (text[0] == '>') {
        i = 1;
        if (i < len && text[i] == ' ') i++;
        for (; i < len && col < SCREEN_COLS; i++, col++)
            put_char(col, row, text[i], apply_sel(ATTR_QUOTE, i, sel_start, sel_end));
        return;
    }

    /* list item: hyphen bullet only (asterisk would collide with italic) */
    if (text[0] == '-' && len > 1 && text[1] == ' ') {
        /* the bullet glyph stands in for raw columns 0-1 (the "- "
         * prefix), so it's checked as a 2-wide range rather than a
         * single raw column like everything else here */
        attr = sel_overlaps(0, 2, sel_start, sel_end) ? swap_attr(ATTR_LISTMARK) : ATTR_LISTMARK;
        put_char(0, row, (char) 7, attr);  /* CP437 bullet glyph */
        col = 1;
        i = 2;
        /* falls through to the inline scanner below for the rest */
    }

    while (i < len) {
        if (text[i] == '*' && i + 1 < len && text[i + 1] == '*') {
            if (last_col >= 0 && sel_overlaps(i, i + 2, sel_start, sel_end))
                put_char(last_col, row, last_ch, swap_attr(last_attr));
            bold = !bold; i += 2; continue;
        }
        if (text[i] == '*') {
            if (last_col >= 0 && sel_overlaps(i, i + 1, sel_start, sel_end))
                put_char(last_col, row, last_ch, swap_attr(last_attr));
            italic = !italic; i++; continue;
        }
        if (text[i] == '`') {
            if (last_col >= 0 && sel_overlaps(i, i + 1, sel_start, sel_end))
                put_char(last_col, row, last_ch, swap_attr(last_attr));
            code = !code; i++; continue;
        }
        if (text[i] == '~' && i + 1 < len && text[i + 1] == '~') {
            if (last_col >= 0 && sel_overlaps(i, i + 2, sel_start, sel_end))
                put_char(last_col, row, last_ch, swap_attr(last_attr));
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
                        put_char(last_col, row, last_ch, swap_attr(last_attr));  /* leading '[' */
                    for (m = i + 1; m < j; m++) {
                        if (col < SCREEN_COLS) {
                            put_char(col, row, text[m], apply_sel(ATTR_LINK, m, sel_start, sel_end));
                            last_col = col; last_ch = text[m]; last_attr = ATTR_LINK;
                        }
                        col++;
                    }
                    if (last_col >= 0 && sel_overlaps(j, k + 1, sel_start, sel_end))
                        put_char(last_col, row, last_ch, swap_attr(last_attr));  /* "](url)" */
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
            put_char(col, row, text[i], apply_sel(attr, i, sel_start, sel_end));
            last_col = col; last_ch = text[i]; last_attr = attr;
        }
        i++;
        col++;
    }
}

void draw_status_bar(void)
{
    char buf[SCREEN_COLS + 1];
    int i;
    const char *sel_tag = sel_active ? " | Sel" : "";
    if (vim_mode)
        sprintf(buf, " %s%s | Ln %d/%d Col %d | %s | VIM:%s%s | %s",
                filename[0] ? filename : "untitled",
                modified ? "*" : "",
                cur_line + 1, doc_count, cur_col + 1,
                view_mode ? "Markdown" : "Writer",
                vim_insert ? "Insert" : "Normal",
                sel_tag,
                status_msg);
    else
        sprintf(buf, " %s%s | Ln %d/%d Col %d | %s%s | %s",
                filename[0] ? filename : "untitled",
                modified ? "*" : "",
                cur_line + 1, doc_count, cur_col + 1,
                view_mode ? "Markdown" : "Writer",
                sel_tag,
                status_msg);
    buf[SCREEN_COLS] = '\0';
    put_string(0, STATUS_ROW, buf, ATTR_STATUS);
    for (i = strlen(buf); i < SCREEN_COLS; i++) put_char(i, STATUS_ROW, ' ', ATTR_STATUS);
}

/* Bottom row: menu category names, Alt-navigable. Replaces the old
 * static hint line -- same row, interactive now. */
void draw_menu_bar(void)
{
    int i, c = 1;
    clear_row(CMDBAR_ROW, ATTR_CMDBAR);
    for (i = 0; i < MENU_COUNT; i++) {
        unsigned char base = (menu_open == i) ? ATTR_CMDBAR_SEL : ATTR_CMDBAR;
        unsigned char hot  = (menu_open == i) ? ATTR_CMDBAR_SEL : ATTR_CMDBAR_HOT;
        menu_col[i] = c;
        put_char(c, CMDBAR_ROW, menus[i].label[0], hot);
        put_string(c + 1, CMDBAR_ROW, menus[i].label + 1, base);
        c += (int) strlen(menus[i].label) + 3;
    }
    put_string(SCREEN_COLS - 9, CMDBAR_ROW, "Alt=Menu", ATTR_CMDBAR);
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
        r = top + i;
        fill_rect(col, r, width, (i == menu_sel) ? ATTR_CMDBAR_SEL : ATTR_POPUP);
        put_string(col + 1, r, m->items[i], (i == menu_sel) ? ATTR_CMDBAR_SEL : ATTR_POPUP);
    }
}

void redraw_screen(void)
{
    int r, ln;
    scroll_to_cursor();

    if (view_mode == 1) {
        if (cur_col < left_col) left_col = cur_col;
        if (cur_col >= left_col + SCREEN_COLS) left_col = cur_col - SCREEN_COLS + 1;
    } else {
        left_col = 0;
    }

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
    draw_status_bar();
    draw_menu_bar();
    if (menu_open >= 0) draw_menu_popup();
    {
        int screen_col;
        if (view_mode == 1) {
            screen_col = cur_col - left_col;
        } else {
            screen_col = writer_screen_col(doc[cur_line]->text, cur_col);
        }
        if (screen_col < 0) screen_col = 0;
        if (screen_col >= SCREEN_COLS) screen_col = SCREEN_COLS - 1;
        set_cursor(screen_col, cur_line - top_line);
    }
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

void load_file(const char *fname)
{
    FILE *f = fopen(fname, "r");
    char linebuf[MAX_LINE_LEN + 1];
    int i;
    if (!f) { strcpy(status_msg, "Could not open file."); return; }
    for (i = 0; i < doc_count; i++) free(doc[i]);
    doc_count = 0;
    while (fgets(linebuf, sizeof(linebuf), f) && doc_count < MAX_LINES) {
        int len = (int) strlen(linebuf);
        if (len > 0 && linebuf[len - 1] == '\n') linebuf[--len] = '\0';
        doc[doc_count] = new_line();
        strcpy(doc[doc_count]->text, linebuf);
        doc[doc_count]->len = len;
        doc_count++;
    }
    if (doc_count == 0) { doc[0] = new_line(); doc_count = 1; }
    fclose(f);
    strcpy(filename, fname);
    cur_line = cur_col = top_line = left_col = 0;
    modified = 0;
    undo_line_no = -1;
    sel_clear();
    strcpy(status_msg, "Loaded.");
}

void save_file(const char *fname)
{
    FILE *f = fopen(fname, "w");
    int i;
    if (!f) { strcpy(status_msg, "Could not save file."); return; }
    for (i = 0; i < doc_count; i++) fprintf(f, "%s\n", doc[i]->text);
    fclose(f);
    strcpy(filename, fname);
    modified = 0;
    strcpy(status_msg, "Saved.");
}

/* ================= commands ================= */

/* Copies the selection into the clipboard buffer, lines joined with
 * '\n'. Doesn't touch the document or clear the selection -- Copy
 * is read-only, unlike Cut. */
void cmd_copy(void)
{
    int sl, sc, el, ec, ln, from, to, n;
    if (!sel_active) { strcpy(status_msg, "Nothing selected."); return; }
    sel_bounds(&sl, &sc, &el, &ec);
    if (sl == el && sc == ec) { strcpy(status_msg, "Nothing selected."); return; }

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
    strcpy(status_msg, "Copied.");
}

void cmd_cut(void)
{
    if (!sel_active) { strcpy(status_msg, "Nothing selected."); return; }
    cmd_copy();
    /* Only claim "Cut." if sel_delete actually removed something.
     * When it doesn't (an active-but-empty selection, or a
     * cross-line selection too long to merge) it has already left
     * an explanatory status_msg -- overwriting that with a blanket
     * "Cut." would tell the user text was removed when it wasn't. */
    if (sel_delete()) strcpy(status_msg, "Cut.");
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
    if (clip_len == 0) { strcpy(status_msg, "Clipboard empty."); return; }
    if (sel_active) sel_delete();
    for (i = 0; i < clip_len && ok; i++) {
        if (clipboard[i] == '\n') ok = split_line();
        else ok = insert_char((unsigned char) clipboard[i]);
    }
    /* Stop and leave split_line/insert_char's own "Line full."/
     * "Document full." message in place if the paste ran out of
     * room partway through, rather than papering over a truncated
     * paste with a blanket "Pasted." */
    if (ok) strcpy(status_msg, "Pasted.");
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
    strcpy(status_msg, "New file.");
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
        strcpy(status_msg, "Found.");
    else
        strcpy(status_msg, "Not found.");
}

void cmd_find_next(void)
{
    if (!last_search[0]) { cmd_find(); return; }
    if (find_from(cur_line, cur_col + 1, last_search) || find_from(0, 0, last_search))
        strcpy(status_msg, "Found.");
    else
        strcpy(status_msg, "Not found.");
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
    strcpy(status_msg, vim_mode ? "Vim keys ON." : "Vim keys OFF.");
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
    else strcpy(status_msg, "Unknown command.");
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
        }
    } else if (cat == 3) {
        if (idx == 0) view_mode = !view_mode;
        else if (idx == 1) toggle_vim_mode();
    }
}

/* ================= main ================= */

int main(int argc, char **argv)
{
    int key, lo, hi, k, matched;

    doc[0] = new_line();
    if (argc > 1) load_file(argv[1]);

    clear_screen(ATTR_NORMAL);

    for (;;) {
        redraw_screen();
        key = _bios_keybrd(_KEYBRD_READ);
        lo = key & 0xFF;
        hi = (key >> 8) & 0xFF;

        if (menu_open >= 0) {
            /* ---- menu navigation mode ---- */
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
            else if (lo == 8)  backspace();
            else if (lo == 1)  cmd_save_as();
            else if (lo == 6)  cmd_find();
            else if (lo == 7)  cmd_goto();
            else if (lo == 14) cmd_new();
            else if (lo == 15) cmd_open();
            else if (lo == 19) cmd_save();
            else if (lo == 3)  cmd_copy();
            else if (lo == 24) cmd_cut();
            else if (lo == 22) cmd_paste();
            else if (lo == 26) do_undo();
            else if (vim_pending == 'd' && lo == 'd') { delete_current_line(); vim_pending = 0; }
            else if (lo == 'h') { move_left();      vim_pending = 0; }
            else if (lo == 'l') { move_right();     vim_pending = 0; }
            else if (lo == 'k') { move_up();        vim_pending = 0; }
            else if (lo == 'j') { move_down();      vim_pending = 0; }
            else if (lo == '0') { move_home();      vim_pending = 0; }
            else if (lo == '$') { move_end();       vim_pending = 0; }
            else if (lo == 'x') { delete_forward(); vim_pending = 0; }
            else if (lo == 'u') { do_undo();        vim_pending = 0; }
            else if (lo == 'i') { vim_insert = 1;   vim_pending = 0; }
            else if (lo == 'a') { move_right(); vim_insert = 1; vim_pending = 0; }
            else if (lo == 'd') { vim_pending = 'd'; }
            else if (lo == ':') { vim_command_line(); vim_pending = 0; }
            else vim_pending = 0;   /* unrecognized: swallow, don't insert */
        }
        else if (lo == 27) {
            /* Esc no longer quits (see Alt+X below) -- it now only
             * clears an active selection, or in vim mode drops back
             * from Insert to Normal. With neither applicable, it's
             * simply a no-op, same as most editors treat a bare Esc. */
            if (sel_active) {
                sel_clear();
            } else if (vim_mode) {
                vim_insert = 0; vim_pending = 0;  /* Insert -> Normal */
            }
        } else if (lo == 0) {
            matched = 0;
            for (k = 0; k < MENU_COUNT; k++) {
                if (menus[k].altkey_scan == hi) {
                    menu_open = k; menu_sel = 0; matched = 1; break;
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
                        if (shift_down()) sel_begin();
                        else sel_clear();
                        switch (hi) {
                            case 0x4B: move_left();  break;
                            case 0x4D: move_right(); break;
                            case 0x48: move_up();    break;
                            case 0x50: move_down();  break;
                            case 0x47: move_home();  break;
                            case 0x4F: move_end();   break;
                            case 0x49: page_up();    break;
                            case 0x51: page_down();  break;
                        }
                        break;
                    }
                    case 0x53: delete_forward(); break;
                    case 0x3C: view_mode = !view_mode; break;  /* F2 */
                    case 0x3D: cmd_find_next();  break;         /* F3 */
                    case 0x3E: toggle_vim_mode(); break;        /* F4 */
                    case 0x2D: cmd_quit_request(); break;       /* Alt+X */
                    default: break;
                }
            }
        } else if (lo == 13) split_line();
        else if (lo == 8)  backspace();
        else if (lo == 1)  cmd_save_as();
        else if (lo == 6)  cmd_find();
        else if (lo == 7)  cmd_goto();
        else if (lo == 14) cmd_new();
        else if (lo == 15) cmd_open();
        else if (lo == 19) cmd_save();
        else if (lo == 3)  cmd_copy();
        else if (lo == 24) cmd_cut();
        else if (lo == 22) cmd_paste();
        else if (lo == 26) do_undo();
        else if (lo >= 32 && lo < 127) insert_char(lo);

        if (want_quit) break;
    }

    clear_screen(ATTR_NORMAL);
    set_cursor(0, 0);
    return 0;
}
