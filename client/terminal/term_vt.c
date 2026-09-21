// Copyright (c) lezginskiy <xanother-x12@proton.me>
// SPDX-License-Identifier: MIT

// vt parser implementation, see term_vt.h

#include "term_vt.h"

#include <string.h>

#define ST_GROUND 0
#define ST_ESC 1
#define ST_CSI 2
#define ST_OSC 3
#define ST_ESC_INTERM 4

// xterm-ish default: light gray on black, 0xff keeps the theme choice
// with the renderer instead of hardcoding colors here
#define VT_MAX_PARAMS 8

void term_vt_reset(struct x12term_vt *vt) {
    memset(vt, 0, sizeof *vt);
    vt->state = ST_GROUND;
}

static void vt_put(struct x12term_grid *g, uint32_t cp) {
    if (cp >= 0x80 && cp < 0xA0) {
        // c1 range decoded through utf8, no printable
        return;
    }
    term_grid_put(g, cp);
}

static void start_param(struct x12term_vt *vt) {
    vt->n_params = 0;
    vt->cur_param = 0;
    vt->seen_digit = 0;
    vt->private = 0;
}

static void push_param(struct x12term_vt *vt) {
    if (!vt->seen_digit) {
        vt->cur_param = 0;
    }
    if (vt->n_params < VT_MAX_PARAMS) {
        vt->params[vt->n_params++] = vt->cur_param;
    }
    vt->cur_param = 0;
    vt->seen_digit = 0;
}

// param n with a default when it was never written, csi params are
// 1 based for cursor addressing and 0 for attributes
static uint32_t param(struct x12term_vt *vt, uint32_t i, uint32_t def) {
    if (i >= vt->n_params) {
        return def;
    }
    uint32_t v = vt->params[i];
    return v ? v : def;
}

static void csi_dispatch(struct x12term_grid *g, struct x12term_vt *vt,
                         uint8_t final) {
    uint32_t p0 = param(vt, 0, 0);
    switch (final) {
    case 'A':
        term_grid_cursor_move(g, 0, -(int32_t)(p0 ? p0 : 1));
        break;
    case 'B':
        term_grid_cursor_move(g, 0, (int32_t)(p0 ? p0 : 1));
        break;
    case 'C':
        term_grid_cursor_move(g, (int32_t)(p0 ? p0 : 1), 0);
        break;
    case 'D':
        term_grid_cursor_move(g, -(int32_t)(p0 ? p0 : 1), 0);
        break;
    case 'G':
        // column absolute, 1 based
        term_grid_cursor_set(g, p0 ? p0 - 1 : 0, g->cur_y);
        break;
    case 'H':
    case 'f':
        term_grid_cursor_set(g, param(vt, 1, 1) - 1, param(vt, 0, 1) - 1);
        break;
    case 'J':
        term_grid_ed(g, p0);
        break;
    case 'K':
        term_grid_el(g, p0);
        break;
    case 'd':
        term_grid_cursor_set(g, g->cur_x, p0 ? p0 - 1 : 0);
        break;
    case 's':
        vt->saved_x = g->cur_x;
        vt->saved_y = g->cur_y;
        break;
    case 'u':
        term_grid_cursor_set(g, vt->saved_x, vt->saved_y);
        break;
    case 'h':
    case 'l':
        if (vt->private == '?' && vt->n_params == 1 && vt->params[0] == 25) {
            g->cur_visible = final == 'h';
        }
        break;
    case 'm':
        for (uint32_t i = 0; i < vt->n_params; i++) {
            uint32_t v = vt->params[i];
            if (v == 0) {
                g->cur_fg = TERM_COLOR_DEFAULT;
                g->cur_bg = TERM_COLOR_DEFAULT;
                g->cur_attrs = 0;
            } else if (v == 1) {
                g->cur_attrs |= TERM_A_BOLD;
            } else if (v == 22) {
                g->cur_attrs &= ~TERM_A_BOLD;
            } else if (v == 7) {
                g->cur_attrs |= TERM_A_REVERSE;
            } else if (v == 27) {
                g->cur_attrs &= ~TERM_A_REVERSE;
            } else if (v >= 30 && v <= 37) {
                g->cur_fg = (uint8_t)(v - 30);
            } else if (v == 39) {
                g->cur_fg = TERM_COLOR_DEFAULT;
            } else if (v >= 40 && v <= 47) {
                g->cur_bg = (uint8_t)(v - 40);
            } else if (v == 49) {
                g->cur_bg = TERM_COLOR_DEFAULT;
            } else if (v >= 90 && v <= 97) {
                g->cur_fg = (uint8_t)(v - 90 + 8);
            } else if (v >= 100 && v <= 107) {
                g->cur_bg = (uint8_t)(v - 100 + 8);
            }
        }
        break;
    default:
        // everything outside the advertised terminfo subset lands here,
        // silent because it was never supposed to be printed
        break;
    }
}

static void esc_dispatch(struct x12term_grid *g, struct x12term_vt *vt,
                         uint8_t c) {
    switch (c) {
    case 'c':
        // full reset, content stays
        g->cur_fg = TERM_COLOR_DEFAULT;
        g->cur_bg = TERM_COLOR_DEFAULT;
        g->cur_attrs = 0;
        g->cur_visible = 1;
        g->wrap_pending = 0;
        term_vt_reset(vt);
        break;
    case 'D':
        term_grid_lf(g);
        break;
    case 'E':
        term_grid_cr(g);
        term_grid_lf(g);
        break;
    case 'M':
        // reverse index, scroll down when at the top edge
        if (g->cur_y == 0) {
            term_grid_scroll_down(g);
        } else {
            term_grid_cursor_move(g, 0, -1);
        }
        break;
    case '7':
        vt->saved_x = g->cur_x;
        vt->saved_y = g->cur_y;
        break;
    case '8':
        term_grid_cursor_set(g, vt->saved_x, vt->saved_y);
        break;
    default:
        break;
    }
}

// decodes utf8 into the grid, malformed sequences surface as the
// replacement character rather than desyncing the stream
static void feed_ground(struct x12term_grid *g, struct x12term_vt *vt,
                        uint8_t c) {
    if (vt->utf8_need) {
        if ((c & 0xC0) != 0x80) {
            vt_put(g, 0xFFFD);
            vt->utf8_need = 0;
            // fallthrough with the byte itself when it is ascii
            if (c < 0x80) {
                feed_ground(g, vt, c);
            }
            return;
        }
        vt->utf8[vt->utf8_len++] = c;
        vt->utf8_cp = (vt->utf8_cp << 6) | (c & 0x3F);
        if (--vt->utf8_need == 0) {
            vt_put(g, vt->utf8_cp);
            vt->utf8_len = 0;
            vt->utf8_cp = 0;
        }
        return;
    }
    if (c < 0x80) {
        switch (c) {
        case '\r':
            term_grid_cr(g);
            return;
        case '\n':
        case '\v':
        case '\f':
            term_grid_lf(g);
            return;
        case '\b':
            term_grid_bs(g);
            return;
        case '\t':
            term_grid_tab(g);
            return;
        case '\x07':
            return;
        case '\x1b':
            vt->state = ST_ESC;
            return;
        default:
            vt_put(g, c);
            return;
        }
    }
    uint32_t need;
    if ((c & 0xE0) == 0xC0) {
        need = 1;
        vt->utf8_cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        vt->utf8_cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        vt->utf8_cp = c & 0x07;
    } else {
        vt_put(g, 0xFFFD);
        return;
    }
    vt->utf8_need = need;
    vt->utf8_len = 0;
}

void term_vt_feed(struct x12term_grid *g, struct x12term_vt *vt,
                  const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        switch (vt->state) {
        case ST_GROUND:
            feed_ground(g, vt, c);
            break;
        case ST_ESC:
            if (c == '[') {
                vt->state = ST_CSI;
                start_param(vt);
            } else if (c == ']') {
                // osc through to bel or st, titles and colors never print
                vt->state = ST_OSC;
            } else if (c >= 0x20 && c <= 0x2F) {
                // intermediate byte of a two byte escape, the next byte
                // finishes it and everything stays unhandled
                vt->state = ST_ESC_INTERM;
            } else {
                esc_dispatch(g, vt, c);
                vt->state = ST_GROUND;
            }
            break;
        case ST_ESC_INTERM:
            vt->state = ST_GROUND;
            break;
        case ST_CSI:
            if (c >= '0' && c <= '9') {
                vt->cur_param = vt->cur_param * 10 + (c - '0');
                vt->seen_digit = 1;
            } else if (c == ';') {
                push_param(vt);
            } else if (c == '?') {
                vt->private = '?';
            } else if (c >= 0x40 && c <= 0x7E) {
                push_param(vt);
                csi_dispatch(g, vt, c);
                vt->state = ST_GROUND;
            }
            // the dispatcher already grew the table before the prefix
            // ever mattered, private markers just ride along
            break;
        case ST_OSC:
            if (c == '\x07') {
                vt->state = ST_GROUND;
            } else if (c == '\x1b') {
                // st is esc backslash, the backslash is swallowed by the
                // ground state handler as an unhandled escape
                vt->state = ST_ESC;
            }
            break;
        }
    }
}
