// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// terminal grid implementation, see term_grid.h

#include "term_grid.h"

#include <stdlib.h>
#include <string.h>

static struct x12term_cell *cell_at(const struct x12term_grid *g,
                                    uint32_t x, uint32_t y) {
    return &g->cells[(size_t)y * g->cols + x];
}

static void mark_dirty(struct x12term_grid *g, uint32_t y) {
    if (y < g->rows) {
        g->dirty_rows[y] = 1;
    }
}

static void mark_all_dirty(struct x12term_grid *g) {
    memset(g->dirty_rows, 1, g->rows);
}

// erased cells carry the current background, that is the bce convention
// programs rely on when they clear screens with a color set
static void erase_cells(struct x12term_grid *g, uint32_t x0, uint32_t y0,
                        uint32_t x1, uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            struct x12term_cell *c = cell_at(g, x, y);
            c->cp = 0;
            c->fg = g->cur_fg;
            c->bg = g->cur_bg;
            c->attrs = 0;
        }
        mark_dirty(g, y);
    }
}

static void clear_row_edge(struct x12term_grid *g, uint32_t y) {
    for (uint32_t x = 0; x < g->cols; x++) {
        cell_at(g, x, y)->cp = 0;
    }
    mark_dirty(g, y);
}

// a never touched cell: blank text on default colors, calloc alone would
// leave fg/bg at 0 and paint would draw black on black
static void blank_cell(struct x12term_cell *c) {
    c->cp = ' ';
    c->fg = TERM_COLOR_DEFAULT;
    c->bg = TERM_COLOR_DEFAULT;
    c->attrs = 0;
}

// archives one live row into the ring; a failed allocation loses one
// line instead of corrupting the ring, the grid lived without
// scrollback before and stays correct on loss
static void hist_push_row(struct x12term_grid *g, uint32_t y) {
    struct x12term_cell *copy = malloc(g->cols * sizeof *copy);
    if (!copy) {
        return;
    }
    memcpy(copy, g->cells + (size_t)y * g->cols, g->cols * sizeof *copy);
    uint32_t slot;
    if (g->hist_count == TERM_HIST_MAX) {
        // the ring is full: the oldest line dies for the new one
        slot = g->hist_head;
        free(g->hist[slot].cells);
        g->hist_head = (g->hist_head + 1) % TERM_HIST_MAX;
    } else {
        slot = (g->hist_head + g->hist_count) % TERM_HIST_MAX;
        g->hist_count++;
    }
    g->hist[slot].cells = copy;
    g->hist[slot].width = g->cols;
}

// takes the newest archived line back out, the resize grow feeds on it
static int hist_pop_newest(struct x12term_grid *g,
                           struct x12term_hist_line *out) {
    if (g->hist_count == 0) {
        return 0;
    }
    uint32_t slot = (g->hist_head + g->hist_count - 1) % TERM_HIST_MAX;
    *out = g->hist[slot];
    g->hist_count--;
    return 1;
}

void term_grid_init(struct x12term_grid *g, uint32_t cols, uint32_t rows) {
    memset(g, 0, sizeof *g);
    g->cols = cols;
    g->rows = rows;
    g->cells = calloc(cols * rows, sizeof *g->cells);
    for (size_t i = 0; i < (size_t)cols * rows; i++) {
        blank_cell(&g->cells[i]);
    }
    g->dirty_rows = calloc(rows, 1);
    g->cur_fg = TERM_COLOR_DEFAULT;
    g->cur_bg = TERM_COLOR_DEFAULT;
    g->cur_visible = 1;
    mark_all_dirty(g);
}

void term_grid_free(struct x12term_grid *g) {
    for (uint32_t i = 0; i < g->hist_count; i++) {
        free(g->hist[(g->hist_head + i) % TERM_HIST_MAX].cells);
    }
    free(g->cells);
    free(g->dirty_rows);
    memset(g, 0, sizeof *g);
}

void term_grid_resize(struct x12term_grid *g, uint32_t cols, uint32_t rows) {
    // vertical shrink archives the rows above the cursor and cuts the
    // deficit at the bottom edge, grow pulls archived lines back above
    // the screen and pads with blanks where the archive runs dry; the
    // cursor line survives every shape change. horizontal: the top
    // left corner of each surviving row carries over, archived lines
    // keep the width they died with
    int32_t drows = (int32_t)rows - (int32_t)g->rows;
    uint32_t arch = 0;
    if (drows < 0) {
        uint32_t k = (uint32_t)-drows;
        arch = g->cur_y < k ? g->cur_y : k;
        for (uint32_t y = 0; y < arch; y++) {
            hist_push_row(g, y);
        }
    }
    struct x12term_cell *cells = calloc(cols * rows, sizeof *cells);
    uint8_t *dirty = calloc(rows, 1);
    uint32_t keep_x = cols < g->cols ? cols : g->cols;
    uint32_t pulled = 0;
    if (drows > 0) {
        // grow: the newest archived lines return in chronological
        // order above the screen, the newest of them sits right above
        struct x12term_hist_line line;
        uint32_t place = (uint32_t)drows;
        while (place > 0 && hist_pop_newest(g, &line)) {
            place--;
            uint32_t lw = line.width < cols ? line.width : cols;
            for (uint32_t x = 0; x < cols; x++) {
                struct x12term_cell *dst =
                    &cells[(size_t)place * cols + x];
                if (x < lw) {
                    *dst = line.cells[x];
                } else {
                    blank_cell(dst);
                }
            }
            free(line.cells);
            pulled++;
        }
        for (uint32_t yy = 0; yy < drows - pulled; yy++) {
            for (uint32_t x = 0; x < cols; x++) {
                blank_cell(&cells[(size_t)yy * cols + x]);
            }
        }
    }
    for (uint32_t yy = 0; yy < rows; yy++) {
        dirty[yy] = 1;
        int32_t sy = drows > 0 ? (int32_t)yy - drows
                               : (int32_t)yy + (int32_t)arch;
        if (sy < 0 || sy >= (int32_t)g->rows) {
            // pulled history rows and the blank gap are done already
            continue;
        }
        struct x12term_cell *dst = cells + (size_t)yy * cols;
        memcpy(dst, g->cells + (size_t)sy * g->cols,
               keep_x * sizeof *dst);
        for (uint32_t x = keep_x; x < cols; x++) {
            blank_cell(&dst[x]);
        }
    }
    free(g->cells);
    free(g->dirty_rows);
    g->cells = cells;
    g->dirty_rows = dirty;
    g->cols = cols;
    g->rows = rows;
    if (g->cur_x >= cols) {
        g->cur_x = cols - 1;
    }
    // the cursor line rides the same shift as the block it sits in
    int32_t cy = (int32_t)g->cur_y +
                 (drows > 0 ? drows : -(int32_t)arch);
    if (cy < 0) {
        cy = 0;
    }
    if (cy >= (int32_t)rows) {
        cy = (int32_t)rows - 1;
    }
    g->cur_y = (uint32_t)cy;
    // a parked view cannot point past what survived the resize
    if (g->view_off > g->hist_count) {
        g->view_off = g->hist_count;
    }
    g->wrap_pending = 0;
}

void term_grid_put(struct x12term_grid *g, uint32_t cp) {
    if (g->wrap_pending) {
        g->wrap_pending = 0;
        g->cur_x = 0;
        term_grid_lf(g);
    }
    struct x12term_cell *c = cell_at(g, g->cur_x, g->cur_y);
    c->cp = cp;
    c->fg = g->cur_fg;
    c->bg = g->cur_bg;
    c->attrs = g->cur_attrs;
    mark_dirty(g, g->cur_y);
    if (g->cur_x == g->cols - 1) {
        // the cursor logically sits past the edge until the next
        // printable, that is the deferred wrap convention
        g->wrap_pending = 1;
    } else {
        g->cur_x++;
    }
}

// the caret is part of the row visual state: a row the cursor left and
// the row it entered both need a damage rect or the old block lingers on
// screen while the buffer underneath is already clean
static void cursor_to(struct x12term_grid *g, uint32_t x, uint32_t y) {
    if (x == g->cur_x && y == g->cur_y) {
        return;
    }
    mark_dirty(g, g->cur_y);
    g->cur_x = x;
    g->cur_y = y;
    mark_dirty(g, g->cur_y);
}

void term_grid_cr(struct x12term_grid *g) {
    g->wrap_pending = 0;
    cursor_to(g, 0, g->cur_y);
}

void term_grid_lf(struct x12term_grid *g) {
    g->wrap_pending = 0;
    if (g->cur_y == g->rows - 1) {
        term_grid_scroll_up(g);
    } else {
        cursor_to(g, g->cur_x, g->cur_y + 1);
    }
}

void term_grid_bs(struct x12term_grid *g) {
    g->wrap_pending = 0;
    if (g->cur_x > 0) {
        cursor_to(g, g->cur_x - 1, g->cur_y);
    }
}

void term_grid_tab(struct x12term_grid *g) {
    g->wrap_pending = 0;
    uint32_t next = (g->cur_x / 8 + 1) * 8;
    g->cur_x = next < g->cols ? next : g->cols - 1;
}

void term_grid_cursor_move(struct x12term_grid *g, int dx, int dy) {
    g->wrap_pending = 0;
    int32_t x = (int32_t)g->cur_x + dx;
    int32_t y = (int32_t)g->cur_y + dy;
    if (x < 0) {
        x = 0;
    }
    if (x >= (int32_t)g->cols) {
        x = (int32_t)g->cols - 1;
    }
    if (y < 0) {
        y = 0;
    }
    if (y >= (int32_t)g->rows) {
        y = (int32_t)g->rows - 1;
    }
    cursor_to(g, (uint32_t)x, (uint32_t)y);
}

void term_grid_cursor_set(struct x12term_grid *g, uint32_t x, uint32_t y) {
    g->wrap_pending = 0;
    cursor_to(g, x < g->cols ? x : g->cols - 1, y < g->rows ? y : g->rows - 1);
}

void term_grid_ed(struct x12term_grid *g, uint32_t mode) {
    if (mode == 0) {
        erase_cells(g, g->cur_x, g->cur_y, g->cols, g->cur_y + 1);
        erase_cells(g, 0, g->cur_y + 1, g->cols, g->rows);
    } else if (mode == 1) {
        erase_cells(g, 0, g->cur_y, g->cur_x + 1, g->cur_y + 1);
        erase_cells(g, 0, 0, g->cols, g->cur_y);
    } else {
        erase_cells(g, 0, 0, g->cols, g->rows);
    }
}

void term_grid_el(struct x12term_grid *g, uint32_t mode) {
    if (mode == 0) {
        erase_cells(g, g->cur_x, g->cur_y, g->cols, g->cur_y + 1);
    } else if (mode == 1) {
        erase_cells(g, 0, g->cur_y, g->cur_x + 1, g->cur_y + 1);
    } else {
        erase_cells(g, 0, g->cur_y, g->cols, g->cur_y + 1);
    }
}

void term_grid_scroll_up(struct x12term_grid *g) {
    hist_push_row(g, 0);
    memmove(g->cells, g->cells + g->cols,
            (size_t)(g->rows - 1) * g->cols * sizeof *g->cells);
    clear_row_edge(g, g->rows - 1);
    mark_all_dirty(g);
    // a shifted view anchors to the line it shows: the content grew
    // one line at the bottom, the offset follows or the text jumps
    if (g->view_off > 0) {
        g->view_off++;
        if (g->view_off > g->hist_count) {
            // the ring evicted the line the view was parked on
            g->view_off = g->hist_count;
        }
    }
}

void term_grid_scroll_down(struct x12term_grid *g) {
    memmove(g->cells + g->cols, g->cells,
            (size_t)(g->rows - 1) * g->cols * sizeof *g->cells);
    clear_row_edge(g, 0);
    mark_all_dirty(g);
}

void term_grid_view_cell(const struct x12term_grid *g, uint32_t x,
                         uint32_t y, struct x12term_cell *out) {
    if (y >= g->view_off) {
        uint32_t live = y - g->view_off;
        if (live < g->rows && x < g->cols) {
            *out = g->cells[(size_t)live * g->cols + x];
        } else {
            blank_cell(out);
        }
        return;
    }
    uint32_t idx = g->hist_count - g->view_off + y;
    if (idx >= g->hist_count || x >= g->cols) {
        blank_cell(out);
        return;
    }
    const struct x12term_hist_line *ln =
        &g->hist[(g->hist_head + idx) % TERM_HIST_MAX];
    if (x < ln->width) {
        *out = ln->cells[x];
    } else {
        blank_cell(out);
    }
}

void term_grid_scroll_view(struct x12term_grid *g, int delta) {
    int64_t v = (int64_t)g->view_off + delta;
    // the deepest view parks the viewport fully inside the archive
    if (v > (int64_t)g->hist_count) {
        v = g->hist_count;
    }
    if (v < 0) {
        v = 0;
    }
    if ((uint32_t)v == g->view_off) {
        return;
    }
    g->view_off = (uint32_t)v;
    mark_all_dirty(g);
}

void term_grid_view_home(struct x12term_grid *g) {
    if (g->view_off == 0) {
        return;
    }
    g->view_off = 0;
    mark_all_dirty(g);
}

// utf8 worst case four bytes per codepoint, returns the advanced end
static char *put_utf8(char *w, uint32_t cp) {
    if (cp < 0x80) {
        *w++ = (char)cp;
    } else if (cp < 0x800) {
        *w++ = (char)(0xC0 | (cp >> 6));
        *w++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *w++ = (char)(0xE0 | (cp >> 12));
        *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *w++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *w++ = (char)(0xF0 | (cp >> 18));
        *w++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *w++ = (char)(0x80 | (cp & 0x3F));
    }
    return w;
}

// one text dump of the grid, view selects the user viewport (with the
// scrollback shifted in) over the raw live rows
static char *dump_text(const struct x12term_grid *g, int view) {
    // utf8 worst case four bytes per codepoint plus the row separator
    size_t cap = (size_t)g->rows * (g->cols * 4 + 1) + 1;
    char *out = malloc(cap);
    if (!out) {
        return 0;
    }
    char *w = out;
    for (uint32_t y = 0; y < g->rows; y++) {
        if (y) {
            *w++ = '\n';
        }
        for (uint32_t x = 0; x < g->cols; x++) {
            uint32_t cp;
            if (view) {
                struct x12term_cell c;
                term_grid_view_cell(g, x, y, &c);
                cp = c.cp;
            } else {
                cp = cell_at(g, x, y)->cp;
            }
            w = put_utf8(w, cp ? cp : ' ');
        }
    }
    *w = 0;
    return out;
}

char *term_grid_dump(const struct x12term_grid *g) {
    return dump_text(g, 0);
}

char *term_grid_dump_view(const struct x12term_grid *g) {
    return dump_text(g, 1);
}
