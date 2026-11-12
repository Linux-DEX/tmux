/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <sys/statvfs.h>
#include <sys/types.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <stdio.h>
#else
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tmux.h"

/*
 * Vertical sidebar. Labels are drawn with a 5x7 bitmap rotated 90 degrees
 * clockwise, two pixel rows per terminal row. The column stops sidebar-gap
 * rows above the status line; panes still run down to the status line.
 *
 * ponytail: ASCII 5x7 only. A larger bitmap (or a terminal that can rotate
 * glyphs) is the upgrade if labels need non-ASCII.
 */

#define SIDEBAR_GLYPH_W 5
#define SIDEBAR_GLYPH_H 7
#define SIDEBAR_GLYPH_GAP 1
#define SIDEBAR_GLYPH_MAX 48

/* nf-md-cpu_64_bit, nf-md-memory, nf-md-harddisk */
static const char *sidebar_stat_icon[] = {
	"\U000F0EE0",
	"\U000F035B",
	"\U000F02CA"
};

static int sidebar_cpu = -1;
static int sidebar_ram = -1;
static struct event sidebar_timer;

/* 5x7, bit 4 is the left pixel. Index is ASCII - 0x20. */
static const u_char sidebar_font[95][7] = {
	{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /*   */
	{ 0x04,0x04,0x04,0x04,0x04,0x00,0x04 }, /* ! */
	{ 0x0a,0x0a,0x14,0x00,0x00,0x00,0x00 }, /* " */
	{ 0x0a,0x1f,0x0a,0x0a,0x1f,0x0a,0x0a }, /* # */
	{ 0x04,0x0f,0x14,0x0e,0x05,0x1e,0x04 }, /* $ */
	{ 0x19,0x1a,0x02,0x04,0x08,0x0b,0x13 }, /* % */
	{ 0x0c,0x12,0x14,0x08,0x15,0x12,0x0d }, /* & */
	{ 0x04,0x04,0x08,0x00,0x00,0x00,0x00 }, /* ' */
	{ 0x02,0x04,0x08,0x08,0x08,0x04,0x02 }, /* ( */
	{ 0x08,0x04,0x02,0x02,0x02,0x04,0x08 }, /* ) */
	{ 0x00,0x04,0x15,0x0e,0x15,0x04,0x00 }, /* * */
	{ 0x00,0x04,0x04,0x1f,0x04,0x04,0x00 }, /* + */
	{ 0x00,0x00,0x00,0x00,0x04,0x04,0x08 }, /* , */
	{ 0x00,0x00,0x00,0x1f,0x00,0x00,0x00 }, /* - */
	{ 0x00,0x00,0x00,0x00,0x00,0x0c,0x0c }, /* . */
	{ 0x01,0x02,0x02,0x04,0x08,0x08,0x10 }, /* / */
	{ 0x0e,0x11,0x13,0x15,0x19,0x11,0x0e }, /* 0 */
	{ 0x04,0x0c,0x04,0x04,0x04,0x04,0x0e }, /* 1 */
	{ 0x0e,0x11,0x01,0x02,0x04,0x08,0x1f }, /* 2 */
	{ 0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e }, /* 3 */
	{ 0x02,0x06,0x0a,0x12,0x1f,0x02,0x02 }, /* 4 */
	{ 0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e }, /* 5 */
	{ 0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e }, /* 6 */
	{ 0x1f,0x01,0x02,0x04,0x08,0x08,0x08 }, /* 7 */
	{ 0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e }, /* 8 */
	{ 0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e }, /* 9 */
	{ 0x00,0x04,0x04,0x00,0x04,0x04,0x00 }, /* : */
	{ 0x00,0x04,0x04,0x00,0x04,0x04,0x08 }, /* ; */
	{ 0x02,0x04,0x08,0x10,0x08,0x04,0x02 }, /* < */
	{ 0x00,0x1f,0x00,0x1f,0x00,0x00,0x00 }, /* = */
	{ 0x08,0x04,0x02,0x01,0x02,0x04,0x08 }, /* > */
	{ 0x0e,0x11,0x01,0x02,0x04,0x00,0x04 }, /* ? */
	{ 0x0e,0x11,0x17,0x15,0x17,0x10,0x0e }, /* @ */
	{ 0x0e,0x11,0x11,0x1f,0x11,0x11,0x11 }, /* A */
	{ 0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e }, /* B */
	{ 0x0f,0x10,0x10,0x10,0x10,0x10,0x0f }, /* C */
	{ 0x1e,0x11,0x11,0x11,0x11,0x11,0x1e }, /* D */
	{ 0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f }, /* E */
	{ 0x1f,0x10,0x10,0x1e,0x10,0x10,0x10 }, /* F */
	{ 0x0f,0x10,0x10,0x17,0x11,0x11,0x0f }, /* G */
	{ 0x11,0x11,0x11,0x1f,0x11,0x11,0x11 }, /* H */
	{ 0x0e,0x04,0x04,0x04,0x04,0x04,0x0e }, /* I */
	{ 0x07,0x02,0x02,0x02,0x12,0x12,0x0c }, /* J */
	{ 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }, /* K */
	{ 0x10,0x10,0x10,0x10,0x10,0x10,0x1f }, /* L */
	{ 0x11,0x1b,0x15,0x15,0x11,0x11,0x11 }, /* M */
	{ 0x11,0x19,0x15,0x13,0x11,0x11,0x11 }, /* N */
	{ 0x0e,0x11,0x11,0x11,0x11,0x11,0x0e }, /* O */
	{ 0x1e,0x11,0x11,0x1e,0x10,0x10,0x10 }, /* P */
	{ 0x0e,0x11,0x11,0x11,0x15,0x12,0x0d }, /* Q */
	{ 0x1e,0x11,0x11,0x1e,0x14,0x12,0x11 }, /* R */
	{ 0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e }, /* S */
	{ 0x1f,0x04,0x04,0x04,0x04,0x04,0x04 }, /* T */
	{ 0x11,0x11,0x11,0x11,0x11,0x11,0x0e }, /* U */
	{ 0x11,0x11,0x11,0x11,0x11,0x0a,0x04 }, /* V */
	{ 0x11,0x11,0x11,0x15,0x15,0x15,0x0a }, /* W */
	{ 0x11,0x11,0x0a,0x04,0x0a,0x11,0x11 }, /* X */
	{ 0x11,0x11,0x0a,0x04,0x04,0x04,0x04 }, /* Y */
	{ 0x1f,0x01,0x02,0x04,0x08,0x10,0x1f }, /* Z */
	{ 0x0e,0x08,0x08,0x08,0x08,0x08,0x0e }, /* [ */
	{ 0x10,0x08,0x08,0x04,0x02,0x02,0x01 }, /* \ */
	{ 0x0e,0x02,0x02,0x02,0x02,0x02,0x0e }, /* ] */
	{ 0x04,0x0a,0x11,0x00,0x00,0x00,0x00 }, /* ^ */
	{ 0x00,0x00,0x00,0x00,0x00,0x00,0x1f }, /* _ */
	{ 0x08,0x04,0x02,0x00,0x00,0x00,0x00 }, /* ` */
	{ 0x00,0x00,0x0e,0x01,0x0f,0x11,0x0f }, /* a */
	{ 0x10,0x10,0x1e,0x11,0x11,0x11,0x1e }, /* b */
	{ 0x00,0x00,0x0e,0x10,0x10,0x10,0x0e }, /* c */
	{ 0x01,0x01,0x0f,0x11,0x11,0x11,0x0f }, /* d */
	{ 0x00,0x00,0x0e,0x11,0x1f,0x10,0x0e }, /* e */
	{ 0x06,0x09,0x08,0x1c,0x08,0x08,0x08 }, /* f */
	{ 0x00,0x0f,0x11,0x11,0x0f,0x01,0x0e }, /* g */
	{ 0x10,0x10,0x1e,0x11,0x11,0x11,0x11 }, /* h */
	{ 0x04,0x00,0x0c,0x04,0x04,0x04,0x0e }, /* i */
	{ 0x02,0x00,0x06,0x02,0x02,0x12,0x0c }, /* j */
	{ 0x10,0x10,0x12,0x14,0x18,0x14,0x12 }, /* k */
	{ 0x0c,0x04,0x04,0x04,0x04,0x04,0x0e }, /* l */
	{ 0x00,0x00,0x1a,0x15,0x15,0x11,0x11 }, /* m */
	{ 0x00,0x00,0x16,0x19,0x11,0x11,0x11 }, /* n */
	{ 0x00,0x00,0x0e,0x11,0x11,0x11,0x0e }, /* o */
	{ 0x00,0x00,0x1e,0x11,0x1e,0x10,0x10 }, /* p */
	{ 0x00,0x00,0x0f,0x11,0x0f,0x01,0x01 }, /* q */
	{ 0x00,0x00,0x16,0x19,0x10,0x10,0x10 }, /* r */
	{ 0x00,0x00,0x0f,0x10,0x0e,0x01,0x1e }, /* s */
	{ 0x08,0x08,0x1c,0x08,0x08,0x09,0x06 }, /* t */
	{ 0x00,0x00,0x11,0x11,0x11,0x13,0x0d }, /* u */
	{ 0x00,0x00,0x11,0x11,0x11,0x0a,0x04 }, /* v */
	{ 0x00,0x00,0x11,0x11,0x15,0x15,0x0a }, /* w */
	{ 0x00,0x00,0x11,0x0a,0x04,0x0a,0x11 }, /* x */
	{ 0x00,0x00,0x11,0x11,0x0f,0x01,0x0e }, /* y */
	{ 0x00,0x00,0x1f,0x02,0x04,0x08,0x1f }, /* z */
	{ 0x06,0x04,0x04,0x18,0x04,0x04,0x06 }, /* { */
	{ 0x04,0x04,0x04,0x04,0x04,0x04,0x04 }, /* | */
	{ 0x0c,0x04,0x04,0x03,0x04,0x04,0x0c }, /* } */
	{ 0x00,0x00,0x08,0x15,0x02,0x00,0x00 }, /* ~ */
};

struct sidebar_item {
	char			*text;
	char			 compact[16];
	int			 window_id;
	int			 stat;
	int			 active;
	int			 hidden;
	int			 pct;
	u_int			 rows;
	u_int			 y;
	struct grid_cell	 gc;
};

static u_int	sidebar_glyphs(const char *, int *);
static u_int	sidebar_text_rows(u_int);
static u_char  *rotate_clockwise(const u_char *, u_int, u_int, u_int *,
		    u_int *);
static void	sidebar_render_text(struct screen *, u_int, u_int, u_int,
		    const char *, const struct grid_cell *, int);
static u_int	sidebar_chars(const char *);
static void	sidebar_self_check(void);

static u_int
sidebar_glyphs(const char *text, int *out)
{
	const u_char	*p;
	u_int		 n = 0;

	if (text == NULL)
		return (0);
	p = (const u_char *)text;
	while (*p != '\0' && n < SIDEBAR_GLYPH_MAX) {
		if (*p >= 0x20 && *p < 0x7f) {
			if (out != NULL)
				out[n] = *p - 0x20;
			n++;
			p++;
			continue;
		}
		if (*p < 0x20) {
			p++;
			continue;
		}
		if (out != NULL)
			out[n] = '?' - 0x20;
		n++;
		p++;
		while ((*p & 0xc0) == 0x80)
			p++;
	}
	return (n);
}

/* Terminal rows for nglyphs rotated characters, including the 1px gap. */
static u_int
sidebar_text_rows(u_int nglyphs)
{
	u_int	px;

	if (nglyphs == 0)
		return (0);
	px = nglyphs * SIDEBAR_GLYPH_W + (nglyphs - 1) * SIDEBAR_GLYPH_GAP;
	return ((px + 1) / 2);
}

/* Rotate a row-major 0/1 grid 90 degrees clockwise. */
static u_char *
rotate_clockwise(const u_char *src, u_int sw, u_int sh, u_int *dw, u_int *dh)
{
	u_char	*dst;
	u_int	 x, y;

	*dw = sh;
	*dh = sw;
	dst = xcalloc((size_t)sh * sw, 1);
	for (y = 0; y < sh; y++) {
		for (x = 0; x < sw; x++) {
			if (src[y * sw + x])
				dst[x * sh + (sh - 1 - y)] = 1;
		}
	}
	return (dst);
}

static void
sidebar_self_check(void)
{
	static int	 done;
	u_char		 src[4] = { 1, 0, 0, 0 };
	u_char		*dst;
	u_int		 dw, dh;
	int		 glyphs[4];

	if (done)
		return;
	done = 1;
	if (sidebar_font['A' - 0x20][0] != 0x0e || sidebar_font[0][0] != 0)
		fatalx("sidebar font");
	if (sidebar_glyphs("A", glyphs) != 1 ||
	    sidebar_glyphs("AB", glyphs) != 2)
		fatalx("sidebar glyphs");
	if (sidebar_text_rows(1) != 3 || sidebar_text_rows(2) != 6)
		fatalx("sidebar rows");
	dst = rotate_clockwise(src, 2, 2, &dw, &dh);
	if (dw != 2 || dh != 2 || dst[0] != 0 || dst[1] != 1 || dst[2] != 0 ||
	    dst[3] != 0)
		fatalx("sidebar rotate");
	free(dst);
}

/* Nerd icons are drawn one step larger than the letters and digits. */
static int
sidebar_codepoint_is_icon(utf8_char cp)
{
	switch (cp) {
	case 0xF179:	/* fa-apple */
	case 0xF17C:	/* fa-linux */
	case 0xF0EE0:	/* md-cpu_64_bit */
	case 0xF035B:	/* md-memory */
	case 0xF02CA:	/* md-harddisk */
		return (1);
	}
	return (0);
}

static int
sidebar_data_is_icon(const struct utf8_data *ud)
{
	utf8_char	cp;

	if (utf8_from_data(ud, &cp) != UTF8_DONE)
		return (0);
	return (sidebar_codepoint_is_icon(cp));
}

/* Icons take two rows; everything else takes one. */
static u_int
sidebar_chars(const char *text)
{
	struct utf8_data	*ud, *next;
	u_int			 n = 0;

	if (text == NULL || *text == '\0')
		return (0);
	ud = utf8_fromcstr(text);
	for (next = ud; next->size != 0; next++)
		n += sidebar_data_is_icon(next) ? 2 : 1;
	free(ud);
	return (n);
}

/* Draw a label downwards, one character per row, in the status style. */
static void
sidebar_render_text(struct screen *s, u_int y0, u_int width, u_int rows,
    const char *text, const struct grid_cell *gc, int utf8)
{
	struct utf8_data	*ud, *next;
	struct grid_cell	 cell;
	u_int			 x, y, cw;

	if (width == 0 || rows == 0 || text == NULL)
		return;
	memcpy(&cell, gc, sizeof cell);
	utf8_set(&cell.data, ' ');
	for (y = 0; y < rows; y++) {
		for (x = 0; x < width; x++)
			grid_view_set_cell(s->grid, x, y0 + y, &cell);
	}
	ud = utf8_fromcstr(text);
	y = 0;
	for (next = ud; next->size != 0 && y < rows; next++) {
		int	icon = sidebar_data_is_icon(next);

		cw = next->width == 0 ? 1 : next->width;
		if (!utf8 || cw > width)
			utf8_set(&cell.data, (next->size == 1) ? next->data[0] :
			    '?');
		else
			memcpy(&cell.data, next, sizeof cell.data);
		cw = cell.data.width == 0 ? 1 : cell.data.width;
		if (icon && width >= 2)
			x = (width - 2) / 2;
		else {
			if (cw > width)
				cw = width;
			x = (width - cw) / 2;
		}
		grid_view_set_cell(s->grid, x, y0 + y, &cell);
		y += icon ? 2 : 1;
	}
	free(ud);
}

int
sidebar_icon_at(struct screen *s, u_int py, u_int width, u_int *cx,
    struct grid_cell *gc)
{
	u_int	x;

	if (py >= screen_size_y(s) || width == 0)
		return (0);
	if (width > screen_size_x(s))
		width = screen_size_x(s);
	for (x = 0; x < width; x++) {
		grid_view_get_cell(s->grid, x, py, gc);
		if (sidebar_data_is_icon(&gc->data)) {
			*cx = x;
			return (1);
		}
	}
	return (0);
}

/* Horizontal rule, the vertical-bar separator from the status line. */
static void
sidebar_render_rule(struct screen *s, u_int y, u_int width,
    const struct grid_cell *gc, int utf8)
{
	struct grid_cell	 cell;
	u_int			 x;

	if (width == 0)
		return;
	memcpy(&cell, gc, sizeof cell);
	cell.attr |= GRID_ATTR_DIM;
	if (utf8)
		utf8_fromwc(0x2500, &cell.data);
	else
		utf8_set(&cell.data, '-');
	if (cell.data.width == 0)
		cell.data.width = 1;
	for (x = 0; x < width; x++)
		grid_view_set_cell(s->grid, x, y, &cell);
}

static int
sidebar_pct(uint64_t part, uint64_t total)
{
	if (total == 0)
		return (-1);
	if (part > total)
		part = total;
	return ((int)((part * 100) / total));
}

static int
sidebar_sample_cpu(void)
{
#if defined(__linux__)
	FILE			*f;
	unsigned long long	 user, nice, sys, idle, iowait, irq, soft, steal;
	unsigned long long	 total, idleall, dtotal, didle;
	static unsigned long long ptot, pidle;
	static int		 have;
	int			 pct;

	f = fopen("/proc/stat", "r");
	if (f == NULL)
		return (-1);
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
	    &user, &nice, &sys, &idle, &iowait, &irq, &soft, &steal) != 8) {
		fclose(f);
		return (-1);
	}
	fclose(f);
	idleall = idle + iowait;
	total = user + nice + sys + idleall + irq + soft + steal;
	if (!have || total <= ptot) {
		ptot = total;
		pidle = idleall;
		have = 1;
		return (-1);
	}
	dtotal = total - ptot;
	didle = (idleall > pidle) ? idleall - pidle : 0;
	if (didle > dtotal)
		didle = dtotal;
	pct = (int)(((dtotal - didle) * 100) / dtotal);
	ptot = total;
	pidle = idleall;
	return (pct);
#elif defined(__APPLE__)
	host_cpu_load_info_data_t	 load;
	mach_msg_type_number_t		 count = HOST_CPU_LOAD_INFO_COUNT;
	unsigned long long		 user, sys, idle, nice, total, dtotal;
	unsigned long long		 didle;
	static unsigned long long	 pidle, ptot;
	static int			 have;
	int				 pct;

	if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
	    (host_info_t)&load, &count) != KERN_SUCCESS)
		return (-1);
	user = load.cpu_ticks[CPU_STATE_USER];
	sys = load.cpu_ticks[CPU_STATE_SYSTEM];
	idle = load.cpu_ticks[CPU_STATE_IDLE];
	nice = load.cpu_ticks[CPU_STATE_NICE];
	total = user + sys + idle + nice;
	if (!have || total <= ptot) {
		pidle = idle;
		ptot = total;
		have = 1;
		return (-1);
	}
	dtotal = total - ptot;
	didle = (idle > pidle) ? idle - pidle : 0;
	if (didle > dtotal)
		didle = dtotal;
	pct = (int)(((dtotal - didle) * 100) / dtotal);
	pidle = idle;
	ptot = total;
	return (pct);
#else
#ifdef CPUSTATES
	long		 cp[CPUSTATES];
	size_t		 len = sizeof cp;
	static long	 prev[CPUSTATES];
	static int	 have;
	unsigned long long total = 0, idle = 0, ptot = 0, pidle = 0;
	unsigned long long dtotal, didle;
	int		 i, pct;

	if (sysctlbyname("kern.cp_time", cp, &len, NULL, 0) != 0)
		return (-1);
	for (i = 0; i < CPUSTATES; i++) {
		total += (unsigned long long)cp[i];
		if (have)
			ptot += (unsigned long long)prev[i];
	}
	idle = (unsigned long long)cp[CP_IDLE];
	if (have)
		pidle = (unsigned long long)prev[CP_IDLE];
	memcpy(prev, cp, sizeof prev);
	if (!have || total <= ptot) {
		have = 1;
		return (-1);
	}
	dtotal = total - ptot;
	didle = (idle > pidle) ? idle - pidle : 0;
	if (didle > dtotal)
		didle = dtotal;
	pct = (int)(((dtotal - didle) * 100) / dtotal);
	have = 1;
	return (pct);
#else
	return (-1);
#endif
#endif
}

static int
sidebar_sample_ram(void)
{
#if defined(__linux__)
	FILE			*f;
	char			 line[128];
	unsigned long long	 total = 0, avail = 0;
	int			 got = 0;

	f = fopen("/proc/meminfo", "r");
	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof line, f) != NULL) {
		if (sscanf(line, "MemTotal: %llu kB", &total) == 1)
			got |= 1;
		else if (sscanf(line, "MemAvailable: %llu kB", &avail) == 1)
			got |= 2;
	}
	fclose(f);
	if (got != 3 || avail > total)
		return (-1);
	return (sidebar_pct(total - avail, total));
#elif defined(__APPLE__)
	vm_statistics64_data_t	 vm;
	mach_msg_type_number_t	 count = HOST_VM_INFO64_COUNT;
	vm_size_t		 pagesize;
	uint64_t		 mem = 0, used;
	size_t			 len = sizeof mem;

	if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
	    (host_info64_t)&vm, &count) != KERN_SUCCESS)
		return (-1);
	if (host_page_size(mach_host_self(), &pagesize) != KERN_SUCCESS)
		return (-1);
	if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0 || mem == 0)
		return (-1);
	used = (uint64_t)(vm.active_count + vm.wire_count +
	    vm.compressor_page_count) * pagesize;
	return (sidebar_pct(used, mem));
#else
	uint32_t	 pages = 0, free_pages = 0, pagesize = 0;
	size_t		 len;

	len = sizeof pages;
	if (sysctlbyname("vm.stats.vm.v_page_count", &pages, &len, NULL, 0) != 0)
		return (-1);
	len = sizeof free_pages;
	if (sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &len, NULL,
	    0) != 0)
		return (-1);
	len = sizeof pagesize;
	if (sysctlbyname("hw.pagesize", &pagesize, &len, NULL, 0) != 0)
		return (-1);
	if (pages == 0 || free_pages > pages)
		return (-1);
	return (sidebar_pct((uint64_t)(pages - free_pages) * pagesize,
	    (uint64_t)pages * pagesize));
#endif
}

static int
sidebar_sample_disk(const char *path)
{
	struct statvfs	 fs;

	if (path == NULL || *path == '\0' || statvfs(path, &fs) != 0 ||
	    fs.f_blocks == 0) {
		if (statvfs("/", &fs) != 0 || fs.f_blocks == 0)
			return (-1);
	}
	if (fs.f_bfree > fs.f_blocks)
		return (-1);
	return (sidebar_pct(fs.f_blocks - fs.f_bfree, fs.f_blocks));
}

static void
sidebar_sample(void)
{
	struct session	*s;

	sidebar_cpu = sidebar_sample_cpu();
	sidebar_ram = sidebar_sample_ram();
	RB_FOREACH(s, sessions, &sessions)
		s->sidebar_disk = sidebar_sample_disk(s->cwd);
	log_debug("%s: cpu %d ram %d", __func__, sidebar_cpu, sidebar_ram);
}

static void
sidebar_timer_callback(__unused int fd, __unused short events, __unused void *arg)
{
	struct session	*s;
	struct timeval	 tv;
	long long	 interval = -1, n;

	if (event_initialized(&sidebar_timer))
		evtimer_del(&sidebar_timer);

	RB_FOREACH(s, sessions, &sessions) {
		if (!options_get_number(s->options, "sidebar"))
			continue;
		n = options_get_number(s->options, "sidebar-interval");
		if (n <= 0)
			continue;
		if (interval < 0 || n < interval)
			interval = n;
	}
	if (interval < 0)
		return;

	sidebar_sample();
	RB_FOREACH(s, sessions, &sessions) {
		if (options_get_number(s->options, "sidebar"))
			server_status_session(s);
	}
	timerclear(&tv);
	tv.tv_sec = interval;
	evtimer_add(&sidebar_timer, &tv);
}

void
sidebar_timer_start(void)
{
	if (event_initialized(&sidebar_timer))
		evtimer_del(&sidebar_timer);
	else
		evtimer_set(&sidebar_timer, sidebar_timer_callback, NULL);
	sidebar_timer_callback(-1, 0, NULL);
}

/* Reverse would invert half-block ink. Swap colours and drop the attribute. */
static void
sidebar_unreverse(struct grid_cell *gc)
{
	int	tmp;

	if (~gc->attr & GRID_ATTR_REVERSE)
		return;
	tmp = gc->fg;
	gc->fg = gc->bg;
	gc->bg = tmp;
	gc->attr &= ~GRID_ATTR_REVERSE;
	if (COLOUR_DEFAULT(gc->fg) && COLOUR_DEFAULT(gc->bg)) {
		gc->fg = 0;
		gc->bg = 7;
	}
}

static void
sidebar_apply_status_colours(struct session *s, struct grid_cell *gc)
{
	int	fg, bg;

	fg = options_get_number(s->options, "status-fg");
	if (!COLOUR_DEFAULT(fg))
		gc->fg = fg;
	bg = options_get_number(s->options, "status-bg");
	if (!COLOUR_DEFAULT(bg))
		gc->bg = bg;
}

static void
sidebar_status_style(struct grid_cell *gc, struct client *c,
    struct format_tree *ft)
{
	style_apply(gc, c->session->options, "status-style", ft);
	sidebar_apply_status_colours(c->session, gc);
	sidebar_unreverse(gc);
}

/* Active session uses the status bar's current-window colour. */
static void
sidebar_session_style(struct grid_cell *gc, struct client *c, int active,
    struct format_tree *ft)
{
	struct options	*oo;

	sidebar_status_style(gc, c, ft);
	if (!active)
		return;
	if (c->session->curw != NULL)
		oo = c->session->curw->window->options;
	else
		oo = global_w_options;
	style_add(gc, oo, "window-status-current-style", ft);
	sidebar_unreverse(gc);
}

static char *
sidebar_strip_styles(const char *src)
{
	char		*dst, *out;
	const char	*p;

	dst = out = xmalloc(strlen(src) + 1);
	p = src;
	while (*p != '\0') {
		if (p[0] == '#' && p[1] == '[') {
			p += 2;
			while (*p != '\0' && *p != ']')
				p++;
			if (*p == ']')
				p++;
			continue;
		}
		*out++ = *p++;
	}
	*out = '\0';
	return (dst);
}

static void
sidebar_trim(char *s)
{
	char	*p = s;
	size_t	 n;

	while (*p == ' ')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	n = strlen(s);
	while (n > 0 && s[n - 1] == ' ') {
		n--;
		s[n] = '\0';
	}
}

static void
sidebar_stat_label(char *buf, size_t len, int id, int pct, int compact)
{
	if (pct < 0) {
		xsnprintf(buf, len, "%s--", sidebar_stat_icon[id]);
		return;
	}
	if (compact)
		xsnprintf(buf, len, "%s%d", sidebar_stat_icon[id], pct);
	else
		xsnprintf(buf, len, "%s%d%%", sidebar_stat_icon[id], pct);
}

static int
sidebar_stat_value(struct session *s, int id)
{
	if (id == 0)
		return (sidebar_cpu);
	if (id == 1)
		return (sidebar_ram);
	if (id == 2) {
		if (s->sidebar_disk < 0)
			s->sidebar_disk = sidebar_sample_disk(s->cwd);
		return (s->sidebar_disk);
	}
	return (-1);
}

static void
sidebar_parse_stats(const char *list, int *ids, u_int *n)
{
	char	*copy, *next, *tok, *end;
	int	 id;
	u_int	 i;

	*n = 0;
	if (list == NULL || *list == '\0')
		return;
	copy = xstrdup(list);
	next = copy;
	while ((tok = strsep(&next, ",")) != NULL) {
		while (*tok == ' ')
			tok++;
		end = tok + strlen(tok);
		while (end > tok && end[-1] == ' ') {
			end--;
			*end = '\0';
		}
		if (*tok == '\0')
			continue;
		id = -1;
		if (strcasecmp(tok, "cpu") == 0)
			id = 0;
		else if (strcasecmp(tok, "ram") == 0)
			id = 1;
		else if (strcasecmp(tok, "disk") == 0)
			id = 2;
		if (id < 0)
			continue;
		for (i = 0; i < *n; i++) {
			if (ids[i] == id)
				break;
		}
		if (i != *n || *n == 3)
			continue;
		ids[(*n)++] = id;
	}
	free(copy);
}

static void
sidebar_free_items(struct sidebar_item *items, u_int n)
{
	u_int	i;

	for (i = 0; i < n; i++)
		free(items[i].text);
	free(items);
}

static int
sidebar_hide_last(struct sidebar_item *items, u_int n, int stat)
{
	u_int	i;

	for (i = n; i > 0; i--) {
		if (items[i - 1].hidden)
			continue;
		if (stat) {
			if (!items[i - 1].stat)
				continue;
		} else if (items[i - 1].stat || items[i - 1].active)
			continue;
		items[i - 1].hidden = 1;
		return (1);
	}
	return (0);
}

static u_int
sidebar_measure(struct sidebar_item *items, u_int n, int compact)
{
	u_int	i, sum = 0;
	const char *text;

	for (i = 0; i < n; i++) {
		if (items[i].hidden) {
			items[i].rows = 0;
			continue;
		}
		if (compact && items[i].stat)
			text = items[i].compact;
		else
			text = items[i].text;
		items[i].rows = sidebar_chars(text);
		if (items[i].rows > 0)
			items[i].rows++;
		sum += items[i].rows;
	}
	return (sum);
}

static void
sidebar_place(struct sidebar_item *items, u_int n, u_int height)
{
	u_int	i, y = 0, bot = 0;

	/* Leave one blank row so the OS logo is not flush with the top. */
	if (height > 1)
		y = 1;

	for (i = 0; i < n; i++) {
		if (items[i].hidden || items[i].stat || items[i].rows == 0)
			continue;
		items[i].y = y;
		y += items[i].rows;
	}
	for (i = 0; i < n; i++) {
		if (!items[i].hidden && items[i].stat)
			bot += items[i].rows;
	}
	y = (height > bot) ? height - bot : 0;
	for (i = 0; i < n; i++) {
		if (items[i].hidden || !items[i].stat || items[i].rows == 0)
			continue;
		items[i].y = y;
		y += items[i].rows;
	}
}

static void
sidebar_fill(struct screen *s, const struct grid_cell *gc)
{
	struct grid_cell	 cell;
	u_int			 x, y, sx, sy;

	sx = screen_size_x(s);
	sy = screen_size_y(s);
	memcpy(&cell, gc, sizeof cell);
	utf8_set(&cell.data, ' ');
	for (y = 0; y < sy; y++) {
		for (x = 0; x < sx; x++)
			grid_view_set_cell(s->grid, x, y, &cell);
	}
}

static void
sidebar_add_hit(struct sidebar_line *sb, u_int y, u_int sy, int id)
{
	if (sy == 0 || id < 0)
		return;
	sb->hits = xreallocarray(sb->hits, sb->nhits + 1, sizeof *sb->hits);
	sb->hits[sb->nhits].y = y;
	sb->hits[sb->nhits].sy = sy;
	sb->hits[sb->nhits].window_id = id;
	sb->nhits++;
}

static int printflike(4, 5)
sidebar_sig_add(char **buf, size_t *len, size_t *cap, const char *fmt, ...)
{
	va_list	 ap;
	int	 n;
	char	*next;

	for (;;) {
		va_start(ap, fmt);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
		n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
		va_end(ap);
		if (n < 0)
			return (-1);
		if (*len + (size_t)n < *cap) {
			*len += (size_t)n;
			return (0);
		}
		*cap *= 2;
		next = xrealloc(*buf, *cap);
		*buf = next;
	}
}

u_int
sidebar_size(struct client *c)
{
	struct session	*s = c->session;
	u_int		 width, side;

	if (c->flags & CLIENT_CONTROL)
		return (0);
	if (s == NULL || !options_get_number(s->options, "sidebar"))
		return (0);
	width = options_get_number(s->options, "sidebar-width");
	side = status_side_size(c);
	if (c->tty.sx <= side || c->tty.sx - side <= width)
		return (0);
	return (width);
}

int
sidebar_at_column(struct client *c)
{
	u_int	width;

	width = sidebar_size(c);
	if (width == 0 || c->session == NULL)
		return (-1);
	if (options_get_number(c->session->options, "sidebar-position") == 1)
		return (0);
	return (c->tty.sx - width);
}

u_int
sidebar_view_xoff(struct client *c)
{
	u_int	xoff = 0;

	if (status_side_at_column(c) == 0)
		xoff += status_side_size(c);
	if (sidebar_at_column(c) == 0)
		xoff += sidebar_size(c);
	return (xoff);
}

int
sidebar_side_status_x(struct client *c)
{
	int	x = status_side_at_column(c);
	u_int	sb = sidebar_size(c);

	if (x < 0 || sb == 0)
		return (x);
	if (x == 0 && sidebar_at_column(c) == 0)
		return ((int)sb);
	if (x > 0 && sidebar_at_column(c) > 0)
		return (x - (int)sb);
	return (x);
}

u_int
sidebar_pane_x(struct client *c, u_int x)
{
	u_int	left, right;

	left = sidebar_view_xoff(c);
	right = c->tty.sx;
	if (status_side_at_column(c) > 0)
		right -= status_side_size(c);
	if (sidebar_at_column(c) > 0)
		right -= sidebar_size(c);
	if (right <= left)
		return (0);
	if (x < left)
		return (0);
	if (x >= right)
		return (right - left - 1);
	return (x - left);
}

int
sidebar_pane_outside(struct client *c, u_int x)
{
	u_int	left, right;

	left = sidebar_view_xoff(c);
	right = c->tty.sx;
	if (status_side_at_column(c) > 0)
		right -= status_side_size(c);
	if (sidebar_at_column(c) > 0)
		right -= sidebar_size(c);
	return (right <= left || x < left || x >= right);
}

void
sidebar_geometry(struct client *c, u_int *x, u_int *y, u_int *w, u_int *h,
    u_int *gap_y, u_int *gap_h)
{
	struct session	*s = c->session;
	u_int		 gap, lines, room;
	int		 at;

	*x = *y = *w = *h = *gap_y = *gap_h = 0;
	*w = sidebar_size(c);
	if (*w == 0 || s == NULL)
		return;
	at = sidebar_at_column(c);
	if (at < 0) {
		*w = 0;
		return;
	}
	*x = (u_int)at;
	gap = options_get_number(s->options, "sidebar-gap");
	lines = status_line_size(c);
	if (c->tty.sy <= lines)
		return;
	room = c->tty.sy - lines;
	if (gap > room)
		gap = room;
	*gap_h = gap;
	if (status_at_line(c) == 0) {
		*gap_y = lines;
		*y = lines + gap;
		*h = room - gap;
	} else {
		*y = 0;
		*h = room - gap;
		*gap_y = *h;
	}
}

void
sidebar_init(struct client *c)
{
	screen_init(&c->sidebar.screen, 1, 1, 0);
	c->sidebar.hits = NULL;
	c->sidebar.nhits = 0;
	c->sidebar.signature = NULL;
	sidebar_self_check();
}

void
sidebar_status_all(void)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL)
			c->flags |= CLIENT_REDRAWSTATUS;
	}
}

void
sidebar_free(struct client *c)
{
	screen_free(&c->sidebar.screen);
	free(c->sidebar.hits);
	free(c->sidebar.signature);
	c->sidebar.hits = NULL;
	c->sidebar.signature = NULL;
	c->sidebar.nhits = 0;
}

static struct sidebar_item *
sidebar_collect(struct client *c, u_int *nitems, int *utf8)
{
	struct session		*s = c->session, *loop;
	struct sidebar_item	*items;
	struct format_tree	*ft;
	char			*expanded, *stripped, buf[32];
	const char		*list;
	int			 ids[3], pct, active;
	u_int			 n = 0, nstats = 0, i, ns;
	int			 flags;

	*nitems = 0;
	flags = FORMAT_STATUS;
	if (c->flags & CLIENT_STATUSFORCE)
		flags |= FORMAT_FORCE;
	*utf8 = (c->flags & CLIENT_UTF8) ? 1 : 0;
	ns = 0;
	RB_FOREACH(loop, sessions, &sessions)
		ns++;
	items = xcalloc(ns + 4, sizeof *items);

	/*
	 * OS logo above the session names. fa-apple on macOS, fa-linux (Tux)
	 * on Linux.
	 */
#if defined(__APPLE__) || defined(__linux__)
#if defined(__APPLE__)
	items[n].text = xstrdup("\uF179");
#else
	items[n].text = xstrdup("\uF17C");
#endif
	ft = format_create(c, NULL, FORMAT_NONE, flags);
	format_defaults(ft, c, s, NULL, NULL);
	items[n].window_id = -1;
	items[n].active = 1;
	items[n].stat = 0;
	sidebar_status_style(&items[n].gc, c, ft);
	n++;
	format_free(ft);
#endif

	RB_FOREACH(loop, sessions, &sessions) {
		ft = format_create(c, NULL, FORMAT_NONE, flags);
		format_defaults(ft, c, loop, NULL, NULL);
		expanded = format_expand(ft, "#S");
		if (expanded == NULL)
			expanded = xstrdup("");
		stripped = sidebar_strip_styles(expanded);
		free(expanded);
		sidebar_trim(stripped);
		if (*stripped != '\0') {
			active = (loop == s);
			items[n].text = stripped;
			items[n].window_id = loop->id;
			items[n].active = active;
			items[n].stat = 0;
			sidebar_session_style(&items[n].gc, c, active, ft);
			n++;
		} else
			free(stripped);
		format_free(ft);
	}

	ft = format_create(c, NULL, FORMAT_NONE, flags);
	format_defaults(ft, c, s, NULL, NULL);
	list = options_get_string(s->options, "sidebar-stats");
	sidebar_parse_stats(list, ids, &nstats);
	for (i = 0; i < nstats; i++) {
		pct = sidebar_stat_value(s, ids[i]);
		sidebar_stat_label(buf, sizeof buf, ids[i], pct, 0);
		items[n].text = xstrdup(buf);
		sidebar_stat_label(items[n].compact, sizeof items[n].compact,
		    ids[i], pct, 1);
		items[n].stat = 1;
		items[n].window_id = -1;
		items[n].pct = pct;
		sidebar_status_style(&items[n].gc, c, ft);
		n++;
	}
	format_free(ft);
	*nitems = n;
	return (items);
}

int
sidebar_redraw(struct client *c)
{
	struct sidebar_line	*sb = &c->sidebar;
	struct sidebar_item	*items;
	struct grid_cell	 bg;
	struct format_tree	*ft;
	char			*sig;
	size_t			 len = 0, cap = 256;
	u_int			 x, y, w, h, gy, gh, n = 0, i, j, sum, room;
	int			 utf8 = 0, compact = 0, progress, flags;

	sidebar_geometry(c, &x, &y, &w, &h, &gy, &gh);
	if (w == 0 || h == 0) {
		free(sb->signature);
		sb->signature = NULL;
		sb->nhits = 0;
		return (0);
	}
	if (c->session == NULL)
		return (0);

	items = sidebar_collect(c, &n, &utf8);
	room = (h > 1) ? h - 1 : h;
	progress = 1;
	while (progress) {
		sum = sidebar_measure(items, n, compact);
		if (sum <= room)
			break;
		progress = 0;
		if (!compact) {
			compact = 1;
			progress = 1;
			continue;
		}
		if (sidebar_hide_last(items, n, 1)) {
			progress = 1;
			continue;
		}
		if (sidebar_hide_last(items, n, 0)) {
			progress = 1;
			continue;
		}
	}
	sum = sidebar_measure(items, n, compact);
	if (sum > room) {
		for (i = 0; i < n; i++) {
			if (!items[i].active || items[i].hidden ||
			    items[i].window_id < 0)
				continue;
			if (sum - items[i].rows >= room)
				items[i].rows = (room > 0) ? 1 : 0;
			else
				items[i].rows -= sum - room;
			if (items[i].rows > room)
				items[i].rows = room;
			break;
		}
	}
	sidebar_place(items, n, h);

	sig = xmalloc(cap);
	sig[0] = '\0';
	sidebar_sig_add(&sig, &len, &cap, "g:%u,%u,%u,%u,%d;", w, h, x, y,
	    utf8);
	for (i = 0; i < n; i++) {
		sidebar_sig_add(&sig, &len, &cap,
		    "i:%d:%d:%u:%u:%d:%d:%d:%s;",
		    items[i].hidden, items[i].stat, items[i].y, items[i].rows,
		    items[i].gc.fg, items[i].gc.bg, items[i].gc.attr,
		    items[i].hidden ? "" :
		    (compact && items[i].stat ? items[i].compact :
		    items[i].text));
	}
	if (sb->signature != NULL && strcmp(sb->signature, sig) == 0 &&
	    screen_size_x(&sb->screen) == w && screen_size_y(&sb->screen) == h) {
		free(sig);
		sidebar_free_items(items, n);
		return (0);
	}

	if (screen_size_x(&sb->screen) != w || screen_size_y(&sb->screen) != h)
		screen_resize(&sb->screen, w, h, 0);
	flags = FORMAT_STATUS;
	ft = format_create(c, NULL, FORMAT_NONE, flags);
	format_defaults(ft, c, c->session, NULL, NULL);
	style_apply(&bg, c->session->options, "status-style", ft);
	sidebar_apply_status_colours(c->session, &bg);
	sidebar_unreverse(&bg);
	format_free(ft);
	sidebar_fill(&sb->screen, &bg);

	sb->nhits = 0;
	for (i = 0; i < n; i++) {
		const char	*text;

		if (items[i].hidden || items[i].rows == 0)
			continue;
		if (compact && items[i].stat)
			text = items[i].compact;
		else
			text = items[i].text;
		sidebar_render_text(&sb->screen, items[i].y, w,
		    items[i].rows > 0 ? items[i].rows - 1 : 0, text,
		    &items[i].gc, utf8);
		if (items[i].rows > 1) {
			for (j = i + 1; j < n; j++) {
				if (!items[j].hidden && items[j].rows > 0)
					break;
			}
			if (j < n)
				sidebar_render_rule(&sb->screen,
				    items[i].y + items[i].rows - 1, w, &bg, utf8);
		}
		if (!items[i].stat)
			sidebar_add_hit(sb, items[i].y,
			    items[i].rows > 0 ? items[i].rows - 1 : 0,
			    items[i].window_id);
	}
	free(sb->signature);
	sb->signature = sig;
	sidebar_free_items(items, n);
	return (1);
}

int
sidebar_mouse(struct client *c, u_int x, u_int y, int *window_id)
{
	struct sidebar_line	*sb = &c->sidebar;
	u_int			 sx, sy, sw, sh, gy, gh, i, rel;

	*window_id = -1;
	sidebar_geometry(c, &sx, &sy, &sw, &sh, &gy, &gh);
	if (sw == 0 || x < sx || x >= sx + sw)
		return (0);
	if (gh != 0 && y >= gy && y < gy + gh)
		return (1);
	if (sh == 0 || y < sy || y >= sy + sh)
		return (0);
	rel = y - sy;
	for (i = 0; i < sb->nhits; i++) {
		if (rel >= sb->hits[i].y &&
		    rel < sb->hits[i].y + sb->hits[i].sy) {
			*window_id = sb->hits[i].window_id;
			break;
		}
	}
	return (1);
}
