// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// terminal grid: the cell matrix the vt parser writes into and the
// renderer reads from. pure logic over explicit state, no io anywhere,
// so the unit tests drive it headless. damage is tracked per row, the
// renderer turns dirty rows into surface damage rectangles

#ifndef X12_TERM_GRID_H
#define X12_TERM_GRID_H

#include <stddef.h>
#include <stdint.h>

// attribute bits stored per cell
#define TERM_A_BOLD 0x1
#define TERM_A_REVERSE 0x2

// color sentinels: 0xff means the theme default, 0..15 index the
// xterm-ish palette resolved by the renderer
#define TERM_COLOR_DEFAULT 0xffu

// scrollback depth: archived lines above the live grid, the ring
// keeps the newest ones when the cap overflows
#define TERM_HIST_MAX 512

// one archived line: cells captured at the width the line had when
// it scrolled off the top
struct x12term_hist_line {
    struct x12term_cell *cells;
    uint32_t width;
};

struct x12term_cell {
    uint32_t cp;   // codepoint, 0 means an erased cell
    uint8_t fg;
    uint8_t bg;
    uint8_t attrs;
};

struct x12term_grid {
    uint32_t cols, rows;
    struct x12term_cell *cells;
    uint32_t cur_x, cur_y;
    uint8_t cur_fg, cur_bg;
    uint8_t cur_attrs;
    int cur_visible;
    // deferred wrap: the last printable landed in the last column and
    // the cursor logically sits past the edge, the next printable wraps
    int wrap_pending;
    // one byte per row, nonzero means the renderer must repaint it
    uint8_t *dirty_rows;
    // scrollback ring: hist[hist_head] is the oldest archived line,
    // view_off counts lines the viewport is shifted up from the live
    // bottom, zero means the user sees the live grid
    struct x12term_hist_line hist[TERM_HIST_MAX];
    uint32_t hist_head, hist_count;
    uint32_t view_off;
};

void term_grid_init(struct x12term_grid *g, uint32_t cols, uint32_t rows);
void term_grid_free(struct x12term_grid *g);
// copies the overlapping top left corner, clears the rest, marks all
// rows dirty; no reflow at this stage
void term_grid_resize(struct x12term_grid *g, uint32_t cols, uint32_t rows);

// viewport read: y counts viewport rows, archived lines show through
// while the view is shifted up; out gets a blank cell for anything
// beyond an archived line width
void term_grid_view_cell(const struct x12term_grid *g, uint32_t x,
                         uint32_t y, struct x12term_cell *out);
// shift the view delta lines, positive up into the history, clamped
// to the archive depth; a change repaints every row
void term_grid_scroll_view(struct x12term_grid *g, int delta);
// back to the live bottom, user input belongs there
void term_grid_view_home(struct x12term_grid *g);
// utf8 text of the current viewport, rows joined with newlines; the
// caller frees the buffer, the scrollback e2e reads it through the
// sigusr1 dump
char *term_grid_dump_view(const struct x12term_grid *g);

// printable codepoint at the cursor with deferred wrap
void term_grid_put(struct x12term_grid *g, uint32_t cp);
void term_grid_cr(struct x12term_grid *g);
// pure index: down one row, scrolls at the bottom edge, column kept
void term_grid_lf(struct x12term_grid *g);
void term_grid_bs(struct x12term_grid *g);
void term_grid_tab(struct x12term_grid *g);

void term_grid_cursor_move(struct x12term_grid *g, int dx, int dy);
void term_grid_cursor_set(struct x12term_grid *g, uint32_t x, uint32_t y);

// erase in display, mode 0 cursor to end, 1 start to cursor, 2 all
void term_grid_ed(struct x12term_grid *g, uint32_t mode);
// erase in line, same modes inside the cursor row
void term_grid_el(struct x12term_grid *g, uint32_t mode);

// scroll the whole screen up or down one row, content pushed out is
// lost, the freed edge row is erased
void term_grid_scroll_up(struct x12term_grid *g);
void term_grid_scroll_down(struct x12term_grid *g);

// ascii text of the whole grid, rows joined with newlines; the caller
// frees the buffer, used by the unit tests
char *term_grid_dump(const struct x12term_grid *g);

#endif
