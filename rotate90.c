/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Linux-DEX
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/* 3x5 pictures. Bit 2 is the left pixel. \1 is a down arrow, \2 an up arrow. */
static const u_char rotate90_font[128][5] = {
	[' '] = { 0, 0, 0, 0, 0 },
	['\1'] = { 2, 2, 2, 7, 2 },
	['\2'] = { 2, 7, 2, 2, 2 },
	['%'] = { 5, 1, 2, 4, 5 },
	['-'] = { 0, 0, 7, 0, 0 },
	['/'] = { 1, 1, 2, 4, 4 },
	['0'] = { 7, 5, 5, 5, 7 },
	['1'] = { 2, 6, 2, 2, 7 },
	['2'] = { 7, 1, 7, 4, 7 },
	['3'] = { 7, 1, 7, 1, 7 },
	['4'] = { 5, 5, 7, 1, 1 },
	['5'] = { 7, 4, 7, 1, 7 },
	['6'] = { 7, 4, 7, 5, 7 },
	['7'] = { 7, 1, 1, 1, 1 },
	['8'] = { 7, 5, 7, 5, 7 },
	['9'] = { 7, 5, 7, 1, 7 },
	['A'] = { 2, 5, 7, 5, 5 },
	['B'] = { 6, 5, 6, 5, 6 },
	['C'] = { 7, 4, 4, 4, 7 },
	['D'] = { 6, 5, 5, 5, 6 },
	['E'] = { 7, 4, 6, 4, 7 },
	['F'] = { 7, 4, 6, 4, 4 },
	['G'] = { 7, 4, 5, 5, 7 },
	['H'] = { 5, 5, 7, 5, 5 },
	['I'] = { 7, 2, 2, 2, 7 },
	['J'] = { 1, 1, 1, 5, 7 },
	['K'] = { 5, 5, 6, 5, 5 },
	['L'] = { 4, 4, 4, 4, 7 },
	['M'] = { 5, 7, 5, 5, 5 },
	['N'] = { 5, 7, 7, 5, 5 },
	['O'] = { 7, 5, 5, 5, 7 },
	['P'] = { 7, 5, 7, 4, 4 },
	['Q'] = { 7, 5, 5, 7, 1 },
	['R'] = { 7, 5, 7, 6, 5 },
	['S'] = { 7, 4, 7, 1, 7 },
	['T'] = { 7, 2, 2, 2, 2 },
	['U'] = { 5, 5, 5, 5, 7 },
	['V'] = { 5, 5, 5, 5, 2 },
	['W'] = { 5, 5, 5, 7, 5 },
	['X'] = { 5, 5, 2, 5, 5 },
	['Y'] = { 5, 5, 7, 2, 2 },
	['Z'] = { 7, 1, 2, 4, 7 },
	['a'] = { 0, 3, 5, 5, 3 },
	['b'] = { 4, 6, 5, 5, 6 },
	['c'] = { 0, 3, 4, 4, 3 },
	['d'] = { 1, 3, 5, 5, 3 },
	['e'] = { 0, 3, 7, 4, 3 },
	['f'] = { 3, 4, 6, 4, 4 },
	['g'] = { 0, 3, 5, 3, 1 },
	['h'] = { 4, 6, 5, 5, 5 },
	['i'] = { 2, 0, 2, 2, 2 },
	['j'] = { 1, 0, 1, 1, 6 },
	['k'] = { 4, 5, 6, 5, 5 },
	['l'] = { 6, 2, 2, 2, 3 },
	['m'] = { 0, 6, 7, 5, 5 },
	['n'] = { 0, 6, 5, 5, 5 },
	['o'] = { 0, 2, 5, 5, 2 },
	['p'] = { 0, 6, 5, 6, 4 },
	['q'] = { 0, 3, 5, 3, 1 },
	['r'] = { 0, 5, 6, 4, 4 },
	['s'] = { 0, 3, 6, 1, 6 },
	['t'] = { 2, 7, 2, 2, 3 },
	['u'] = { 0, 5, 5, 5, 3 },
	['v'] = { 0, 5, 5, 5, 2 },
	['w'] = { 0, 5, 5, 7, 5 },
	['x'] = { 0, 5, 2, 5, 5 },
	['y'] = { 0, 5, 5, 3, 1 },
	['z'] = { 0, 7, 2, 4, 7 },
};

/* 2x2 pixel blocks. Bit 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right. */
static const wchar_t rotate90_quads[16] = {
	' ',
	0x2598, 0x259d, 0x2580,
	0x2596, 0x258c, 0x259e, 0x259b,
	0x2597, 0x259a, 0x2590, 0x259c,
	0x2584, 0x2599, 0x259f, 0x2588
};

#define ROTATE90_GW 3
#define ROTATE90_GH 5

static int
rotate90_colour(struct client *c, const char *key, const char *fallback)
{
	struct format_tree	*ft;
	char			 fmt[32], *out;
	int			 colour;

	xsnprintf(fmt, sizeof fmt, "#{@%s}", key);
	ft = format_create(c, NULL, FORMAT_NONE, 0);
	format_defaults(ft, c, NULL, NULL, NULL);
	out = format_expand(ft, fmt);
	format_free(ft);
	colour = colour_fromstring(out);
	free(out);
	if (colour == -1)
		colour = colour_fromstring(fallback);
	return (colour);
}

/* Build the word as one picture, then turn that picture 90 degrees clockwise. */
static u_char *
rotate90_picture(const char *text, u_int *w, u_int *h)
{
	const u_char	*glyph;
	u_char		*flat, *turned, bit;
	u_int		 i, n, x, y, fw, nx, ny;
	u_char		 ch;

	n = 0;
	for (i = 0; text[i] != '\0'; i++) {
		ch = (u_char)text[i];
		if (ch < 128 && (ch == '\1' || ch == '\2' ||
		    rotate90_font[ch][0] || rotate90_font[ch][1] ||
		    rotate90_font[ch][2] || rotate90_font[ch][3] ||
		    rotate90_font[ch][4] || ch == ' '))
			n++;
	}
	if (n == 0) {
		*w = *h = 0;
		return (NULL);
	}
	fw = n * ROTATE90_GW + (n - 1);
	flat = xcalloc(fw * ROTATE90_GH, 1);
	x = 0;
	for (i = 0; text[i] != '\0'; i++) {
		ch = (u_char)text[i];
		if (ch >= 128)
			continue;
		if (ch != ' ' && ch != '\1' && ch != '\2' &&
		    rotate90_font[ch][0] == 0 && rotate90_font[ch][1] == 0 &&
		    rotate90_font[ch][2] == 0 && rotate90_font[ch][3] == 0 &&
		    rotate90_font[ch][4] == 0)
			continue;
		glyph = rotate90_font[ch];
		for (y = 0; y < ROTATE90_GH; y++) {
			for (bit = 0; bit < ROTATE90_GW; bit++) {
				if (glyph[y] & (1 << (ROTATE90_GW - 1 - bit)))
					flat[y * fw + x + bit] = 1;
			}
		}
		x += ROTATE90_GW + 1;
	}
	*w = ROTATE90_GH;
	*h = fw;
	turned = xcalloc((*w) * (*h), 1);
	for (y = 0; y < ROTATE90_GH; y++) {
		for (x = 0; x < fw; x++) {
			if (flat[y * fw + x] == 0)
				continue;
			nx = ROTATE90_GH - 1 - y;
			ny = x;
			turned[ny * (*w) + nx] = 1;
		}
	}
	free(flat);
	return (turned);
}

static void
rotate90_putc(struct screen_write_ctx *ctx, struct grid_cell *gc, u_int x,
    u_int y, wchar_t wc)
{
	memcpy(&gc->data, &grid_default_cell.data, sizeof gc->data);
	if (utf8_fromwc(wc, &gc->data) != UTF8_DONE)
		return;
	screen_write_cursormove(ctx, x, y, 0);
	screen_write_cell(ctx, gc);
}

void
rotate90_draw(struct screen_write_ctx *ctx, struct client *c,
    struct grid_cell *base, u_int ox, u_int sx, u_int sy)
{
	struct session		*s = c->session;
	struct host_stat	 st;
	struct grid_cell	 gc;
	char			 cpu[8], down[20], up[20];
	const char		*cols[7];
	int			 colours[7];
	u_char			*cells, *pic;
	u_int			 i, px, py, cw, ch, x, bit, w, h;
	u_int			 colw, colh;

	if (s == NULL || s->curw == NULL || sx == 0 || sy == 0)
		return;
	host_stat_sample(&st);
	xsnprintf(cpu, sizeof cpu, "%d%%", st.cpu);
	xsnprintf(down, sizeof down, "\1%s", st.net_down);
	xsnprintf(up, sizeof up, "\2%s", st.net_up);
	cols[0] = up;
	cols[1] = down;
	cols[2] = st.disk_usage;
	cols[3] = st.ram_usage;
	cols[4] = cpu;
	cols[5] = s->curw->window->name;
	cols[6] = s->name;
	colours[0] = rotate90_colour(c, "thm_lavender", "themeyellow");
	colours[1] = rotate90_colour(c, "thm_teal", "themecyan");
	colours[2] = rotate90_colour(c, "thm_blue", "themeblue");
	colours[3] = rotate90_colour(c, "thm_teal", "themecyan");
	colours[4] = rotate90_colour(c, "thm_peach", "themeyellow");
	colours[5] = rotate90_colour(c, "thm_blue", "themeblue");
	colours[6] = rotate90_colour(c, "thm_peach", "themeyellow");

	cells = xcalloc(sx * sy, 1);
	{
		u_char		*paint = xcalloc(sx * sy, 1);
		u_int		 y = 0, order[] = { 6, 5, 4, 3, 2, 1, 0 };

		for (i = 0; i < 7; i++) {
			pic = rotate90_picture(cols[order[i]], &w, &h);
			if (pic == NULL)
				continue;
			colw = (w + 1) / 2;
			colh = (h + 1) / 2;
			if (colw > sx || y >= sy) {
				free(pic);
				break;
			}
			x = ox + sx - colw;
			for (py = 0; py < h && y + py / 2 < sy; py++) {
				for (px = 0; px < w; px++) {
					if (pic[py * w + px] == 0)
						continue;
					cw = x + px / 2;
					ch = y + py / 2;
					if (cw < ox || cw >= ox + sx || ch >= sy)
						continue;
					bit = (py & 1) ? 2 : 0;
					bit += (px & 1) ? 1 : 0;
					cells[ch * sx + (cw - ox)] |= 1 << bit;
					paint[ch * sx + (cw - ox)] = order[i];
				}
			}
			free(pic);
			y += colh + 1;
		}
		for (ch = 0; ch < sy; ch++) {
			for (cw = 0; cw < sx; cw++) {
				bit = cells[ch * sx + cw];
				if (bit == 0)
					continue;
				memcpy(&gc, base, sizeof gc);
				gc.fg = colours[paint[ch * sx + cw]];
				rotate90_putc(ctx, &gc, ox + cw, ch,
				    rotate90_quads[bit & 15]);
			}
		}
		free(paint);
	}
	free(cells);
}
