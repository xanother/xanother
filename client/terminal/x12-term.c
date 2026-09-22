// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// the x12 terminal: forkpty into the user shell, one event loop over the
// x12 socket, the pty master and a self pipe, psf glyphs into a shm
// buffer. the client renders itself, the server never draws text. the
// vt parser and the grid live in term_vt and term_grid, this file is
// session, input encoding and painting

#include "../libx12.h"
#include "term_font.h"
#include "term_grid.h"
#include "term_vt.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

// one pool covers the largest window the terminal accepts, buffers are
// carved per resize; each resize takes a fresh buffer id, the count is
// bounded by the server object table which is fine for now
#define TERM_MAX_COLS 200
#define TERM_MAX_ROWS 60
#define TERM_MAX_W (TERM_MAX_COLS * FONT_W)
#define TERM_MAX_H (TERM_MAX_ROWS * FONT_H)

// xterm-ish palette, index 0..15 plus theme defaults
static const uint8_t palette[16][3] = {
    { 0, 0, 0 },       { 205, 0, 0 },     { 0, 205, 0 },   { 205, 205, 0 },
    { 0, 0, 238 },     { 205, 0, 205 },   { 0, 205, 205 }, { 229, 229, 229 },
    { 127, 127, 127 }, { 255, 0, 0 },     { 0, 255, 0 },   { 255, 255, 0 },
    { 92, 92, 255 },   { 255, 0, 255 },   { 0, 255, 255 }, { 255, 255, 255 },
};
static const uint8_t theme_fg[3] = { 200, 200, 210 };
static const uint8_t theme_bg[3] = { 16, 16, 32 };

static struct x12term_grid grid;
static struct x12term_vt vt;
static x12c *x12;
static uint32_t comp_id, bufman_id, surf_id, tl_id;
static uint32_t pool_id;
static int memfd = -1;
static uint8_t *pix;
static uint32_t cur_w, cur_h;
static int commits_out;
// the current buffer plus retired ones waiting for the server to stop
// reading them; a retired id dies on its first release, without this
// the object table would fill up on every resize and kill the session
static uint32_t buf_cur;
static uint32_t buf_retired[8];
static int buf_retired_n;

// configure handshake, one pending offer at a time
static int have_config;
static uint32_t conf_w, conf_h, conf_serial;

// pointer selection: cell coordinates of the drag rect, the text is
// re-extracted at copy time from the grid. the button events carry no
// coordinates, the anchor comes from the last seen motion position
static int sel_valid;
static int mouse_b1;
static uint32_t mouse_cx, mouse_cy;
// wheel notches queue up on axis events and apply on the frame that
// closes the batch, the protocol forbids acting before it
static int pend_scroll;
static uint32_t sel_ax, sel_ay, sel_bx, sel_by;

// the primary lives in the terminal until the selection protocol lands
// with the data party: a copy fills sel_text and a middle click feeds it
// back to the pty
static char sel_text[65536];
static size_t sel_len;

// keyboard model
static struct xkb_context *kb_ctx;
static struct xkb_keymap *keymap;
static struct xkb_state *kstate;
static uint32_t repeat_rate = 25, repeat_delay = 600;
static struct { int active; uint32_t code; int64_t next_ms; } rpt;

static int pty_fd = -1;
static int selfpipe[2] = { -1, -1 };
static pid_t child;
static int can_paint = 1;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// signal context: the handlers only push bytes into the self pipe, the
// loop does the real work on the other end
static int cell_in_sel(uint32_t x, uint32_t y);
static void sel_invalidate(void);

static void sig_child(int signo) {
    (void)signo;
    if (write(selfpipe[1], "c", 1) < 0) {
        // the pipe fills at most once per signal, nothing to do
    }
}

static void sig_dump(int signo) {
    (void)signo;
    if (write(selfpipe[1], "d", 1) < 0) {
        // same as above
    }
}

// fallback for codepoints the font does not carry
#define TERM_FALLBACK_CP '?'

// binary search over the generated codepoint map
static uint16_t font_glyph(uint32_t cp) {
    uint32_t lo = 0, hi = X12TERM_MAP_LEN;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (x12term_map[mid].cp < cp) {
            lo = mid + 1;
        } else if (x12term_map[mid].cp > cp) {
            hi = mid;
        } else {
            return x12term_map[mid].glyph;
        }
    }
    // the fallback glyph is guaranteed by the converter
    uint32_t q = TERM_FALLBACK_CP;
    for (uint32_t i = 0; i < X12TERM_MAP_LEN; i++) {
        if (x12term_map[i].cp == q) {
            return x12term_map[i].glyph;
        }
    }
    return 0;
}

static void cell_colors(const struct x12term_cell *c, int cursor,
                        uint8_t fg_out[3], uint8_t bg_out[3]) {
    uint8_t fg = c->fg, bg = c->bg;
    uint8_t attrs = c->attrs;
    if (cursor) {
        // the cursor inverts the cell, simplest visible caret
        uint8_t t = fg;
        fg = bg;
        bg = t;
        attrs ^= TERM_A_REVERSE;
        if (fg == TERM_COLOR_DEFAULT && bg == TERM_COLOR_DEFAULT) {
            // both default on an empty cell: force a visible bar
            fg = 0;
            bg = 15;
            attrs = TERM_A_REVERSE;
        }
    }
    const uint8_t *f, *b;
    if (fg == TERM_COLOR_DEFAULT) {
        f = theme_fg;
    } else {
        f = palette[fg & 15];
        if ((attrs & TERM_A_BOLD) && fg < 8) {
            f = palette[fg + 8];
        }
    }
    if (bg == TERM_COLOR_DEFAULT) {
        b = theme_bg;
    } else {
        b = palette[bg & 15];
    }
    if (attrs & TERM_A_REVERSE) {
        const uint8_t *t = f;
        f = b;
        b = t;
    }
    memcpy(fg_out, f, 3);
    memcpy(bg_out, b, 3);
}

// premultiplied argb word from an rgb triple, alpha full
static uint32_t color_word(const uint8_t *rgb) {
    return 0xFF000000u | ((uint32_t)rgb[0] << 16) |
           ((uint32_t)rgb[1] << 8) | rgb[2];
}

static void paint(void) {
    // the buffer covers the whole pool, only the grid part is written
    for (uint32_t y = 0; y < grid.rows; y++) {
        for (uint32_t x = 0; x < grid.cols; x++) {
            struct x12term_cell vc;
            term_grid_view_cell(&grid, x, y, &vc);
            const struct x12term_cell *c = &vc;
            uint8_t fg[3], bg[3];
            // the caret marks the live bottom, a scrolled back view
            // hides it instead of pointing at text nothing edits
            int cursor = grid.view_off == 0 &&
                         x == grid.cur_x && y == grid.cur_y &&
                         grid.cur_visible;
            cell_colors(c, cursor, fg, bg);
            if (cell_in_sel(x, y)) {
                // the selection is a plain color swap, the caret and the
                // selection use the same visual language
                uint8_t t0 = fg[0], t1 = fg[1], t2 = fg[2];
                fg[0] = bg[0];
                fg[1] = bg[1];
                fg[2] = bg[2];
                bg[0] = t0;
                bg[1] = t1;
                bg[2] = t2;
            }
            uint32_t fgw = color_word(fg), bgw = color_word(bg);
            if (c->cp == 0) {
                // an erased cell carries colors only: fill it with the
                // background, the fallback glyph belongs to unknown text
                for (uint32_t gy = 0; gy < FONT_H; gy++) {
                    uint32_t *row = (uint32_t *)(pix +
                        ((size_t)y * FONT_H + gy) * cur_w * 4 +
                        x * FONT_W * 4);
                    for (uint32_t gx = 0; gx < FONT_W; gx++) {
                        row[gx] = bgw;
                    }
                }
                continue;
            }
            for (uint32_t gy = 0; gy < FONT_H; gy++) {
                uint32_t *row = (uint32_t *)(pix +
                    ((size_t)y * FONT_H + gy) * cur_w * 4 +
                    x * FONT_W * 4);
                uint8_t bits = x12term_glyphs
                    [(size_t)font_glyph(c->cp) * FONT_H + gy];
                for (uint32_t gx = 0; gx < FONT_W; gx++) {
                    // premultiplied argb, alpha full
                    row[gx] = bits & (0x80 >> gx) ? fgw : bgw;
                }
            }
        }
    }
}

// one damage rect per dirty row, the renderer never repaints a quiet row
static void flush_damage(void) {
    for (uint32_t y = 0; y < grid.rows; y++) {
        if (!grid.dirty_rows[y]) {
            continue;
        }
        grid.dirty_rows[y] = 0;
        x12_arg d[4] = {
            { .i64 = 0 },
            { .i64 = (int32_t)(y * FONT_H) },
            { .i64 = (int32_t)cur_w },
            { .i64 = FONT_H },
        };
        if (x12c_call(x12, "x12_surface", "damage", surf_id, d, 4) != 0) {
            return;
        }
    }
    if (x12c_call(x12, "x12_surface", "commit", surf_id, 0, 0) == 0) {
        commits_out++;
    }
}

// a retired buffer is off the surface, its first release is the
// permission to destroy; a full ring just leaks the id, the release
// contract still holds
static void buf_retire(uint32_t id) {
    if (!id || buf_retired_n >= (int)(sizeof buf_retired / sizeof *buf_retired)) {
        return;
    }
    buf_retired[buf_retired_n++] = id;
}

// applies a configure offer: resize the grid, the pty window and the
// buffer, then repaint. the server applies the new view size on commit
static int apply_configure(void) {
    uint32_t w = conf_w < TERM_MAX_W ? conf_w : TERM_MAX_W;
    uint32_t h = conf_h < TERM_MAX_H ? conf_h : TERM_MAX_H;
    uint32_t cols = w / FONT_W, rows = h / FONT_H;
    if (!cols || !rows) {
        // degenerate size, keep the old geometry and just ack
        x12_arg ack[1] = { { .u64 = conf_serial } };
        return x12c_call(x12, "x12_toplevel", "ack_configure", tl_id,
                         ack, 1);
    }
    cur_w = cols * FONT_W;
    cur_h = rows * FONT_H;
    term_grid_resize(&grid, cols, rows);
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    if (pty_fd >= 0) {
        ioctl(pty_fd, TIOCSWINSZ, &ws);
    }
    paint();
    x12_arg ack[1] = { { .u64 = conf_serial } };
    if (x12c_call(x12, "x12_toplevel", "ack_configure", tl_id, ack, 1) != 0) {
        return -1;
    }
    uint32_t buf;
    int rc = x12c_buffer_create(x12, pool_id, 0, cur_w, cur_h,
                                0x34325258u, &buf, 0);
    if (rc != 0) {
        return -1;
    }
    x12_arg at[1] = { { .u64 = buf } };
    if (x12c_call(x12, "x12_surface", "set_buffer", surf_id, at, 1) != 0) {
        return -1;
    }
    buf_retire(buf_cur);
    buf_cur = buf;
    flush_damage();
    return 0;
}

static void write_all(const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(pty_fd, buf + off, n - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        off += (size_t)w;
    }
}

// keysym to the byte sequence the shell expects, arrows and function
// keys go out as csi, control combinations as their ascii roots
static void encode_key(uint32_t code, int pressed) {
    if (!kstate) {
        return;
    }
    xkb_state_update_key(kstate, code + 8,
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (!pressed) {
        if (rpt.active && rpt.code == code) {
            rpt.active = 0;
        }
        return;
    }
    xkb_keysym_t ks = xkb_state_key_get_one_sym(kstate, code + 8);
    if (ks == XKB_KEY_NoSymbol) {
        return;
    }
    int ctrl = xkb_state_mod_name_is_active(kstate, XKB_MOD_NAME_CTRL,
                                            XKB_STATE_MODS_EFFECTIVE) > 0;
    int alt = xkb_state_mod_name_is_active(kstate, XKB_MOD_NAME_ALT,
                                           XKB_STATE_MODS_EFFECTIVE) > 0;
    char out[8];
    size_t n = 0;
    int suppress_text = 0;
    switch (ks) {
    case XKB_KEY_Return:
        out[n++] = '\r';
        suppress_text = 1;
        break;
    case XKB_KEY_BackSpace:
        out[n++] = 0x7f;
        suppress_text = 1;
        break;
    case XKB_KEY_Tab:
        out[n++] = '\t';
        suppress_text = 1;
        break;
    case XKB_KEY_Escape:
        out[n++] = 0x1b;
        suppress_text = 1;
        break;
    case XKB_KEY_Up:
        memcpy(out, "\x1b[A", 3);
        n = 3;
        suppress_text = 1;
        break;
    case XKB_KEY_Down:
        memcpy(out, "\x1b[B", 3);
        n = 3;
        suppress_text = 1;
        break;
    case XKB_KEY_Right:
        memcpy(out, "\x1b[C", 3);
        n = 3;
        suppress_text = 1;
        break;
    case XKB_KEY_Left:
        memcpy(out, "\x1b[D", 3);
        n = 3;
        suppress_text = 1;
        break;
    default:
        break;
    }
    if (!suppress_text && ctrl && ks >= 0x40 && ks <= 0x7E) {
        // the control root of an ascii letter or symbol, exactly what a
        // readline expects; keys outside ascii keep their text
        out[n++] = (char)(ks & 0x1F);
        suppress_text = 1;
    }
    if (!suppress_text) {
        char utf[8];
        int got = xkb_state_key_get_utf8(kstate, code + 8, utf, sizeof utf);
        if (got > 0) {
            memcpy(out, utf, (size_t)got);
            n = (size_t)got;
        }
    }
    if (!n) {
        return;
    }
    if (alt) {
        char esc[2] = { 0x1b, 0 };
        write_all(esc, 1);
    }
    write_all(out, n);
    // repeats schedule from the press, the loop timer drives them
    rpt.active = 1;
    rpt.code = code;
    rpt.next_ms = now_ms() + (int64_t)repeat_delay;
}

// utf8 encode of one codepoint, returns the byte count
static size_t utf8_put(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// normalized selection bounds, true when the drag covers real cells
static int sel_rect(uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1) {
    if (!sel_valid) {
        return 0;
    }
    *x0 = sel_ax < sel_bx ? sel_ax : sel_bx;
    *x1 = (sel_ax < sel_bx ? sel_bx : sel_ax) + 1;
    *y0 = sel_ay < sel_by ? sel_ay : sel_by;
    *y1 = (sel_ay < sel_by ? sel_by : sel_ay) + 1;
    return *x1 > *x0 && *y1 > *y0;
}

static int cell_in_sel(uint32_t x, uint32_t y) {
    uint32_t x0, y0, x1, y1;
    if (!sel_rect(&x0, &y0, &x1, &y1)) {
        return 0;
    }
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static void sel_invalidate(void) {
    if (!sel_valid) {
        return;
    }
    sel_valid = 0;
    memset(grid.dirty_rows, 1, grid.rows);
}

// reads the drag rect out of the grid: cells utf8 encoded, trailing
// spaces trimmed per row, rows joined with newlines
static void sel_extract(void) {
    uint32_t x0, y0, x1, y1;
    sel_len = 0;
    if (!sel_rect(&x0, &y0, &x1, &y1)) {
        return;
    }
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t end = x1;
        while (end > x0) {
            struct x12term_cell c;
            term_grid_view_cell(&grid, end - 1, y, &c);
            if (c.cp != 0 && c.cp != ' ') {
                break;
            }
            end--;
        }
        for (uint32_t x = x0; x < end; x++) {
            struct x12term_cell c;
            term_grid_view_cell(&grid, x, y, &c);
            uint32_t cp = c.cp;
            if (cp == 0) {
                cp = ' ';
            }
            char buf[4];
            size_t n = utf8_put(cp, buf);
            if (sel_len + n >= sizeof sel_text) {
                return;
            }
            memcpy(sel_text + sel_len, buf, n);
            sel_len += n;
        }
        if (y + 1 < y1 && sel_len < sizeof sel_text) {
            sel_text[sel_len++] = '\n';
        }
    }
}

// a copy just re-extracts the rect, the primary buffer is local until
// the selection protocol lands with the data party
static void copy_to_primary(void) {
    sel_extract();
}

static void paste_primary(void) {
    // pasting is input too, the view lands on the live bottom where
    // the echo will show up
    term_grid_view_home(&grid);
    sel_extract();
    if (sel_len > 0) {
        write_all(sel_text, sel_len);
    }
}

static void on_event(x12c *c, const x12c_proxy *p, const x12_msg_desc *desc,
                     const x12_arg *args, size_t nargs) {
    (void)c;
    (void)nargs;
    const char *iface = p->iface->name;
    if (strcmp(iface, "x12_keyboard") == 0) {
        if (strcmp(desc->name, "keymap") == 0) {
            // the lib remapped the wire slot into a real fd
            int fd = (int)args[1].u64;
            if (fd >= 0 && !kstate) {
                char text[262144];
                ssize_t got = read(fd, text, sizeof text - 1);
                if (got > 0) {
                    keymap = xkb_keymap_new_from_buffer(kb_ctx, text,
                                                        (size_t)got,
                                                        XKB_KEYMAP_FORMAT_TEXT_V1,
                                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
                    if (keymap) {
                        kstate = xkb_state_new(keymap);
                    }
                }
                close(fd);
            }
        } else if (strcmp(desc->name, "modifiers") == 0) {
            if (kstate) {
                xkb_state_update_mask(kstate,
                                      (xkb_mod_mask_t)args[1].u64,
                                      (xkb_mod_mask_t)args[3].u64,
                                      (xkb_mod_mask_t)args[2].u64,
                                      0, 0, (xkb_layout_index_t)args[4].u64);
            }
        } else if (strcmp(desc->name, "key") == 0) {
            if (args[2].u64 == 0) {
                sel_invalidate();
                // typed input belongs to the live bottom, a scrolled
                // back view jumps home before the bytes reach the pty
                term_grid_view_home(&grid);
            }
            encode_key((uint32_t)args[1].u64, args[2].u64 == 0);
        } else if (strcmp(desc->name, "repeat_info") == 0) {
            repeat_rate = (uint32_t)args[0].u64;
            repeat_delay = (uint32_t)args[1].u64;
        }
        return;
    }
    if (strcmp(iface, "x12_toplevel") == 0 &&
        strcmp(desc->name, "configure") == 0) {
        // the serial sits in the last argument
        conf_w = (uint32_t)args[0].u64;
        conf_h = (uint32_t)args[1].u64;
        conf_serial = (uint32_t)args[nargs - 1].u64;
        have_config = 1;
        return;
    }
    if (strcmp(iface, "x12_buffer") == 0 && strcmp(desc->name, "release") == 0) {
        // the server is done reading the last committed buffer
        if (commits_out > 0) {
            commits_out--;
        }
        can_paint = commits_out == 0;
        if (p->id != buf_cur) {
            for (int i = 0; i < buf_retired_n; i++) {
                if (buf_retired[i] == p->id) {
                    x12c_call(x12, "x12_buffer", "destroy", p->id, 0, 0);
                    buf_retired[i] = buf_retired[--buf_retired_n];
                    break;
                }
            }
        }
        return;
    }
    if (strcmp(iface, "x12_pointer") == 0) {
        // surface local fixed24p8 coordinates, cells come from the pixel
        // position
        if (strcmp(desc->name, "motion") == 0 && nargs >= 3) {
            int32_t px = (int32_t)(args[1].i64 >> 8);
            int32_t py = (int32_t)(args[2].i64 >> 8);
            uint32_t cx = px / FONT_W;
            uint32_t cy = py / FONT_H;
            if (cx < grid.cols && cy < grid.rows) {
                mouse_cx = cx;
                mouse_cy = cy;
            }
            if (mouse_b1 && cx < grid.cols && cy < grid.rows &&
                (cx != sel_bx || cy != sel_by)) {
                sel_bx = cx;
                sel_by = cy;
                sel_valid = 1;
                memset(grid.dirty_rows, 1, grid.rows);
            }
        } else if (strcmp(desc->name, "button") == 0 && nargs >= 3) {
            uint32_t button = (uint32_t)args[1].u64;
            int pressed = args[2].u64 == 0;
            if (button == 1 && pressed) {
                // a new drag starts: the old selection dies here and the
                // anchor pins to the cell under the pointer
                mouse_b1 = 1;
                sel_invalidate();
                sel_ax = sel_bx = mouse_cx;
                sel_ay = sel_by = mouse_cy;
            } else if (button == 1 && !pressed) {
                mouse_b1 = 0;
                if (sel_valid) {
                    copy_to_primary();
                }
            } else if (button == 2 && pressed) {
                paste_primary();
            }
        } else if (strcmp(desc->name, "axis") == 0 && nargs >= 2) {
            // fixed24p8 notches, positive turns the wheel down which
            // moves the view toward the live bottom; three lines per
            // notch queue up until the frame closes the batch
            if (args[0].u64 == 0) {
                pend_scroll -= (int)(args[1].i64 * 3 / 256);
            }
        } else if (strcmp(desc->name, "frame") == 0) {
            if (pend_scroll != 0) {
                term_grid_scroll_view(&grid, pend_scroll);
                // selection coordinates are viewport bound, the shift
                // would silently move them onto different text
                sel_invalidate();
                pend_scroll = 0;
            }
        }
        return;
    }
    if (strcmp(desc->name, "error") == 0) {
        fprintf(stderr, "x12-term: server error on object %u\n",
                (unsigned)args[1].u64);
    }
}

static void dump_grid(void) {
    char *text = term_grid_dump_view(&grid);
    if (!text) {
        return;
    }
    printf("TERM-DUMP-BEGIN\n%s\nTERM-DUMP-END\n", text);
    fflush(stdout);
    free(text);
}

static int run_shell(const char *cmd) {
    struct winsize ws = { 0 };
    child = forkpty(&pty_fd, 0, 0, &ws);
    if (child < 0) {
        perror("forkpty");
        return -1;
    }
    if (child == 0) {
        setenv("TERM", "x12-term", 1);
        if (cmd) {
            // the test harness drives a scripted session through this
            execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        } else {
            const char *shell = getenv("SHELL");
            if (!shell || !*shell) {
                shell = "/bin/sh";
            }
            execlp(shell, shell, (char *)0);
        }
        _exit(127);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *sock = 0;
    const char *cmd = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--exec") == 0 && i + 1 < argc) {
            cmd = argv[++i];
        } else if (!sock) {
            sock = argv[i];
        }
    }
    if (!sock) {
        fprintf(stderr, "usage: x12-term <socket> [--exec command]\n");
        return 1;
    }
    if (pipe2(selfpipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        return 1;
    }
    kb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_child;
    sigaction(SIGCHLD, &sa, 0);
    sa.sa_handler = sig_dump;
    sigaction(SIGUSR1, &sa, 0);

    x12 = x12c_connect(sock, on_event);
    if (!x12) {
        fprintf(stderr, "x12-term: cannot connect to %s\n", sock);
        return 1;
    }
    comp_id = x12c_bind(x12, "x12_compositor", 0);
    bufman_id = x12c_bind(x12, "x12_buffer_manager", 0);
    uint32_t seat = x12c_bind(x12, "x12_seat", 0);
    if (!comp_id || !bufman_id || !seat) {
        fprintf(stderr, "x12-term: bind ids missing\n");
        return 1;
    }
    while (x12c_proxy_iface(x12, comp_id) == 0 ||
           x12c_proxy_iface(x12, bufman_id) == 0 ||
           x12c_proxy_iface(x12, seat) == 0) {
        if (x12c_dispatch(x12, 2000) < 0) {
            fprintf(stderr, "x12-term: server hung up before globals\n");
            return 1;
        }
    }
    uint32_t ptr = x12c_reserve(x12, "x12_pointer", 0);
    uint32_t kbd = x12c_reserve(x12, "x12_keyboard", 0);
    x12_arg pa[1] = { { .u64 = ptr } };
    x12_arg ka[1] = { { .u64 = kbd } };
    if (!ptr || !kbd ||
        x12c_call(x12, "x12_seat", "get_pointer", seat, pa, 1) != 0 ||
        x12c_call(x12, "x12_seat", "get_keyboard", seat, ka, 1) != 0) {
        fprintf(stderr, "x12-term: seat devices refused\n");
        return 1;
    }

    surf_id = x12c_reserve(x12, "x12_surface", 0);
    x12_arg cr[1] = { { .u64 = surf_id } };
    if (!surf_id ||
        x12c_call(x12, "x12_compositor", "create_surface", comp_id, cr, 1) != 0) {
        fprintf(stderr, "x12-term: create_surface refused\n");
        return 1;
    }

    // one pool for the whole session, buffers carve out of it
    size_t pool_size = (size_t)TERM_MAX_W * TERM_MAX_H * 4;
    memfd = x12c_pool_create(x12, bufman_id, pool_size, &pool_id, 0);
    if (memfd < 0) {
        fprintf(stderr, "x12-term: pool refused\n");
        return 1;
    }
    pix = mmap(0, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (pix == MAP_FAILED) {
        fprintf(stderr, "x12-term: pool mmap failed (errno=%s)\n",
                strerror(errno));
        return 1;
    }

    // the initial geometry is the wish, the server clamps to the output
    // and answers with a configure offer that drives the real sizes
    uint32_t cols = 80, rows = 24;
    cur_w = cols * FONT_W;
    cur_h = rows * FONT_H;
    term_grid_init(&grid, cols, rows);
    term_vt_reset(&vt);
    paint();
    uint32_t buf;
    if (x12c_buffer_create(x12, pool_id, 0, cur_w, cur_h, 0x34325258u,
                           &buf, 0) != 0) {
        fprintf(stderr, "x12-term: buffer refused\n");
        return 1;
    }
    x12_arg at[1] = { { .u64 = buf } };
    if (x12c_call(x12, "x12_surface", "set_buffer", surf_id, at, 1) != 0) {
        fprintf(stderr, "x12-term: set_buffer refused\n");
        return 1;
    }
    buf_cur = buf;
    x12_arg d0[4] = { { .i64 = 0 }, { .i64 = 0 },
                      { .i64 = -1 }, { .i64 = -1 } };
    if (x12c_call(x12, "x12_surface", "damage", surf_id, d0, 4) != 0) {
        fprintf(stderr, "x12-term: initial damage refused\n");
        return 1;
    }
    if (x12c_call(x12, "x12_surface", "commit", surf_id, 0, 0) != 0) {
        fprintf(stderr, "x12-term: initial commit refused\n");
        return 1;
    }
    int tl = x12c_toplevel_map(x12, comp_id, surf_id);
    if (tl <= 0) {
        fprintf(stderr, "x12-term: map handshake failed\n");
        return 1;
    }
    tl_id = (uint32_t)tl;
    commits_out = 0;
    can_paint = 1;

    if (run_shell(cmd) != 0) {
        return 1;
    }
    printf("TERM: mapped cols=%u rows=%u\n", grid.cols, grid.rows);
    fflush(stdout);

    int rc = 0;
    int exit_code = 0;
    int exited = 0;
    for (;;) {
        struct pollfd pfds[3];
        int n = 0;
        pfds[n++] = (struct pollfd){ x12c_fd(x12), POLLIN, 0 };
        pfds[n++] = (struct pollfd){ pty_fd, POLLIN, 0 };
        pfds[n++] = (struct pollfd){ selfpipe[0], POLLIN, 0 };
        int timeout = -1;
        if (rpt.active) {
            timeout = (int)(rpt.next_ms - now_ms());
            if (timeout < 0) {
                timeout = 0;
            }
        }
        // calls are batched in libx12, poll on our own fd would block on
        // bytes that never left the process without this flush
        if (x12c_flush(x12) != 0) {
            rc = 1;
            break;
        }
        int pr = poll(pfds, (nfds_t)n, timeout);
        if (pr < 0 && errno != EINTR) {
            rc = 1;
            break;
        }
        if (pfds[0].revents & POLLIN) {
            if (x12c_dispatch(x12, 0) < 0) {
                // server hangup ends a quiet life
                exited = 1;
                exit_code = 0;
                break;
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR)) {
            exited = 1;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            uint8_t chunk[4096];
            ssize_t got;
            while ((got = read(pty_fd, chunk, sizeof chunk)) > 0) {
                term_vt_feed(&grid, &vt, chunk, (size_t)got);
                if ((size_t)got < sizeof chunk) {
                    break;
                }
            }
            if (got < 0 && errno != EAGAIN && errno != EINTR) {
                // the pty died with the shell
                exited = 1;
            }
        }
        if (pfds[1].revents & (POLLHUP | POLLERR)) {
            exited = 1;
        }
        if (pfds[2].revents & POLLIN) {
            char sigs[16];
            ssize_t got;
            while ((got = read(selfpipe[0], sigs, sizeof sigs)) > 0) {
                for (ssize_t i = 0; i < got; i++) {
                    if (sigs[i] == 'c') {
                        int status = 0;
                        pid_t w = waitpid(child, &status, WNOHANG);
                        if (w == child) {
                            exited = 1;
                            exit_code = WIFEXITED(status)
                                ? WEXITSTATUS(status) : 1;
                        }
                    } else if (sigs[i] == 'd') {
                        dump_grid();
                    }
                }
            }
        }
        if (have_config) {
            have_config = 0;
            if (apply_configure() != 0) {
                rc = 1;
                break;
            }
        }
        if (rpt.active && rpt.next_ms <= now_ms()) {
            encode_key(rpt.code, 1);
            rpt.next_ms = now_ms() +
                (repeat_rate ? 1000 / repeat_rate : 33);
        }
        if (grid.dirty_rows && memchr(grid.dirty_rows, 1, grid.rows) &&
            can_paint) {
            paint();
            flush_damage();
        }
        if (exited) {
            break;
        }
    }
    if (exited) {
        printf("TERM: shell exited code=%d\n", exit_code);
        fflush(stdout);
    }
    term_grid_free(&grid);
    if (kstate) {
        xkb_state_unref(kstate);
    }
    if (keymap) {
        xkb_keymap_unref(keymap);
    }
    if (kb_ctx) {
        xkb_context_unref(kb_ctx);
    }
    if (pix) {
        munmap(pix, (size_t)TERM_MAX_W * TERM_MAX_H * 4);
    }
    if (memfd >= 0) {
        close(memfd);
    }
    x12c_close(x12);
    return rc;
}
