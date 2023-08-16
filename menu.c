/*
 * (C) Gražvydas "notaz" Ignotas, 2006-2012
 *
 * This work is licensed under the terms of any of these licenses
 * (at your option):
 *  - GNU GPL, version 2 or later.
 *  - GNU LGPL, version 2.1 or later.
 *  - MAME license.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <locale.h> // savestate date
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>

#include "menu.h"
#include "fonts.h"
#include "readpng.h"
#include "lprintf.h"
#include "input.h"
#include "plat.h"
#include "posix.h"
#include "core.h"

#if defined(__GNUC__) && __GNUC__ >= 7
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

static char static_buff[64];
static int menu_error_time = 0;
char menu_error_msg[64] = {0};

// g_menuscreen is the current output buffer the menu is rendered to.
SDL_Surface *g_menuscreen_surface;
// g_menubg is the menu background and has the same w/h as g_menuscreen, but
// pp=w. It is filled on menu entry from file or from g_menubg_src if available.
void *g_menubg_ptr;
// g_menubg_src points to a buffer containing a bg image. This is usually either
// the emulator screen buffer or the host frame buffer.
void *g_menubg_src_ptr;

int g_menuscreen_w;
int g_menuscreen_h;
int g_menuscreen_pp;
int g_menubg_src_w;
int g_menubg_src_h;
int g_menubg_src_pp;

int g_autostateld_opt;

static TTF_Font *me_mfont = NULL;
static TTF_Font *me_sfont = NULL;
static int menu_text_color = 0xfffe; // default to white
static int menu_sel_color = -1;		 // disabled

/* note: these might become non-constant in future */
#if MENU_X2
static const int me_mfont_size = 16;
static const int me_sfont_size = 12;
static const int me_mfont_w = 16, me_mfont_h = 20;
static const int me_sfont_w = 12, me_sfont_h = 16;
#else
static const int me_mfont_size = 12;
static const int me_sfont_size = 10;
static const int me_mfont_w = 12, me_mfont_h = 14;
static const int me_sfont_w = 10, me_sfont_h = 12;
#endif

static int g_menu_filter_off;
static int g_border_style;
static int border_left, border_right, border_top, border_bottom;

void menuscreen_memset_lines(unsigned short *dst, int c, int l)
{
	for (; l > 0; l--, dst += g_menuscreen_pp)
		memset(dst, c, g_menuscreen_w * 2);
}

// draws text to current bbp16 screen
static int text_out16_(int x, int y, unsigned short color, const char *text)
{
	if (!text || x < 0 || x >= g_menuscreen_w ||
		y < 0 || y >= g_menuscreen_h)
		return 0;

	int text_w = 0, text_h = 0;
	SDL_Color text_color = {(color & 0xf800) >> 8, (color & 0x07e0) >> 3, (color & 0x001f) << 3};
	SDL_Surface *text_surface = TTF_RenderUTF8_Blended(me_mfont, text, text_color);
	if (text_surface)
	{
		text_w = text_surface->w;
		text_h = text_surface->h;
		SDL_Rect text_pos = {x, y, text_surface->w, text_surface->h};
		SDL_BlitSurface(text_surface, NULL, g_menuscreen_surface, &text_pos);
		SDL_FreeSurface(text_surface);
	}

	if (x < border_left)
		border_left = x;
	if (x + text_w > border_right)
		border_right = x + text_w;
	if (y < border_top)
		border_top = y;
	if (y + text_h > border_bottom)
		border_bottom = y + text_h;

	return text_w;
}

int text_out16(int x, int y, unsigned short color, const char *textf, ...)
{
	va_list args;
	char buffer[512];

	va_start(args, textf);
	vsnprintf(buffer, sizeof(buffer), textf, args);
	va_end(args);

	return text_out16_(x, y, color, buffer);
}

static int smalltext_out16_(int x, int y, unsigned short color, const char *text)
{
	if (!text || x < 0 || x >= g_menuscreen_w ||
		y < 0 || y >= g_menuscreen_h)
		return 0;

	int text_w = 0;
	SDL_Color text_color = {(color & 0xf800) >> 8, (color & 0x07e0) >> 3, (color & 0x001f) << 3};
	SDL_Surface *text_surface = TTF_RenderUTF8_Blended(me_sfont, text, text_color);
	if (text_surface)
	{
		text_w = text_surface->w;
		SDL_Rect text_pos = {x, y, text_surface->w, text_surface->h};
		SDL_BlitSurface(text_surface, NULL, g_menuscreen_surface, &text_pos);
		SDL_FreeSurface(text_surface);
	}
	return text_w;
}

static int smalltext_out16(int x, int y, unsigned short color, const char *textf, ...)
{
	va_list args;
	char buffer[512];

	va_start(args, textf);
	vsnprintf(buffer, sizeof(buffer), textf, args);
	va_end(args);

	return smalltext_out16_(x, y, color, buffer);
}

static int menu_draw_selection(int x, int y)
{
	return text_out16_(x, y, (menu_sel_color < 0) ? menu_text_color : menu_sel_color, ">");

}

static int parse_hex_color(char *buff)
{
	char *endp = buff;
	int t = (int)strtoul(buff, &endp, 16);
	if (endp != buff)
#ifdef PSP
		return ((t << 8) & 0xf800) | ((t >> 5) & 0x07e0) | ((t >> 19) & 0x1f);
#else
		return ((t >> 8) & 0xf800) | ((t >> 5) & 0x07e0) | ((t >> 3) & 0x1f);
#endif
	return -1;
}

static char tolower_simple(char c)
{
	if ('A' <= c && c <= 'Z')
		c = c - 'A' + 'a';
	return c;
}

void menu_init_base(void)
{
	int pos;
	char buff[256];
	FILE *f;

	pos = plat_get_skin_dir(buff, sizeof(buff));
	strcpy(buff + pos, "font.ttf");

	if (me_mfont != NULL)
		TTF_CloseFont(me_mfont);
	me_mfont = TTF_OpenFont(buff, me_mfont_size);
	if (me_mfont == NULL)
		printf("ERROR in menu_init_base: Could not open menu font %s, %s\n", buff, SDL_GetError());

	if (me_sfont != NULL)
		TTF_CloseFont(me_sfont);
	me_sfont = TTF_OpenFont(buff, me_sfont_size);
	if (me_sfont == NULL)
		printf("ERROR in menu_init_base: Could not open menu font %s, %s\n", buff, SDL_GetError());

	// load custom colors
	strcpy(buff + pos, "skin.txt");
	f = fopen(buff, "r");
	if (f != NULL)
	{
		lprintf("found skin.txt\n");
		while (!feof(f))
		{
			if (fgets(buff, sizeof(buff), f) == NULL)
				break;
			if (buff[0] == '#' || buff[0] == '/')
				continue; // comment
			if (buff[0] == '\r' || buff[0] == '\n')
				continue; // empty line
			if (strncmp(buff, "text_color=", 11) == 0)
			{
				int tmp = parse_hex_color(buff + 11);
				if (tmp >= 0)
					menu_text_color = tmp;
				else
					lprintf("skin.txt: parse error for text_color\n");
			}
			else if (strncmp(buff, "selection_color=", 16) == 0)
			{
				int tmp = parse_hex_color(buff + 16);
				if (tmp >= 0)
					menu_sel_color = tmp;
				else
					lprintf("skin.txt: parse error for selection_color\n");
			}
			else
				lprintf("skin.txt: parse error: %s\n", buff);
		}
		fclose(f);
	}

	// use user's locale for savestate date display
	setlocale(LC_TIME, "");
}

static void menu_darken_bg(void *dst, void *src, int pixels, int darker)
{
	unsigned int *dest = dst;
	unsigned int *sorc = src;
	pixels /= 2;
	if (darker)
	{
		while (pixels--)
		{
			unsigned int p = *sorc++;
			*dest++ = ((p & 0xf79ef79e) >> 1) - ((p & 0xc618c618) >> 3);
		}
	}
	else
	{
		while (pixels--)
		{
			unsigned int p = *sorc++;
			*dest++ = (p & 0xf79ef79e) >> 1;
		}
	}
}

static void menu_darken_text_bg(void)
{
	int x, y, xmin, xmax, ymax, ls;
	unsigned short *screen = g_menuscreen_surface->pixels;

	SDL_LockSurface(g_menuscreen_surface);

	xmin = border_left - 3;
	if (xmin < 0)
		xmin = 0;
	xmax = border_right + 2;
	if (xmax > g_menuscreen_w - 1)
		xmax = g_menuscreen_w - 1;

	y = border_top - 3;
	if (y < 0)
		y = 0;
	ymax = border_bottom + 2;
	if (ymax > g_menuscreen_h - 1)
		ymax = g_menuscreen_h - 1;

	for (x = xmin; x <= xmax; x++)
		screen[y * g_menuscreen_pp + x] = 0xa514;
	for (y++; y < ymax; y++)
	{
		ls = y * g_menuscreen_pp;
		screen[ls + xmin] = 0xffff;
		for (x = xmin + 1; x < xmax; x++)
		{
			unsigned int p = screen[ls + x];
			if (p != menu_text_color)
				screen[ls + x] = ((p & 0xf79e) >> 1) - ((p & 0xc618) >> 3);
		}
		screen[ls + xmax] = 0xffff;
	}
	ls = y * g_menuscreen_pp;
	for (x = xmin; x <= xmax; x++)
		screen[ls + x] = 0xffff;

	SDL_UnlockSurface(g_menuscreen_surface);
}

static int borders_pending;

static void menu_reset_borders(void)
{
	border_left = g_menuscreen_w;
	border_right = 0;
	border_top = g_menuscreen_h;
	border_bottom = 0;
}

static void menu_draw_begin(int need_bg, int no_borders)
{
	int y;

	plat_video_menu_begin();

	menu_reset_borders();
	borders_pending = g_border_style && !no_borders;

	if (need_bg)
	{
		SDL_LockSurface(g_menuscreen_surface);

		if (g_border_style && no_borders)
		{
			for (y = 0; y < g_menuscreen_h; y++)
				menu_darken_bg((short *)g_menuscreen_surface->pixels + g_menuscreen_pp * y,
							   (short *)g_menubg_ptr + g_menuscreen_w * y, g_menuscreen_w, 1);
		}
		else
		{
			for (y = 0; y < g_menuscreen_h; y++)
				memcpy((short *)g_menuscreen_surface->pixels + g_menuscreen_pp * y,
					   (short *)g_menubg_ptr + g_menuscreen_w * y, g_menuscreen_w * 2);
		}

		SDL_UnlockSurface(g_menuscreen_surface);
	}
}

static void menu_draw_end(void)
{
	if (borders_pending)
		menu_darken_text_bg();
	plat_video_menu_end();
}

static void menu_separation(void)
{
	if (borders_pending)
	{
		menu_darken_text_bg();
		menu_reset_borders();
	}
}

static int me_id2offset(const menu_entry *ent, menu_id id)
{
	int i;
	for (i = 0; ent->name; ent++, i++)
	{
		if (ent->id == id)
			return i;
	}

	lprintf("%s: id %i not found\n", __FUNCTION__, id);
	return 0;
}

static void me_enable(menu_entry *entries, menu_id id, int enable)
{
	int i = me_id2offset(entries, id);
	entries[i].enabled = enable;
}

static int me_count(const menu_entry *ent)
{
	int ret;

	for (ret = 0; ent->name; ent++, ret++)
		;

	return ret;
}

static unsigned int me_read_onoff(const menu_entry *ent)
{
	// guess var size based on mask to avoid reading too much
	if (ent->mask & 0xffff0000)
		return *(unsigned int *)ent->var & ent->mask;
	else if (ent->mask & 0xff00)
		return *(unsigned short *)ent->var & ent->mask;
	else
		return *(unsigned char *)ent->var & ent->mask;
}

static void me_toggle_onoff(menu_entry *ent)
{
	// guess var size based on mask to avoid reading too much
	if (ent->mask & 0xffff0000)
		*(unsigned int *)ent->var ^= ent->mask;
	else if (ent->mask & 0xff00)
		*(unsigned short *)ent->var ^= ent->mask;
	else
		*(unsigned char *)ent->var ^= ent->mask;
}

static void me_draw(const char *title, menu_entry *entries, int sel, void (*draw_more)(void))
{
	menu_entry *ent;
	const char **names;
	const char *name;
	menu_entry **enable_entries = NULL;
	int n_enable_entries;
	int n_entries;
	int m_sel;

	int wt;
	int title_x, title_y;
	int msg_x, msg_sy;
	int sel_x, sel_y;
	int listview_h;
	int listview_x, listview_x2, listview_sy, listview_dy;
	int n_draw_items, half_draw_items, top_idx;
	int item_left_w = 0;
	int i, j, y;

	menu_draw_begin(1, 0);

	n_entries = me_count(entries);
	enable_entries = calloc(n_entries, sizeof(menu_entry *));
	if (!enable_entries)
		goto finish;
	n_enable_entries = 0;

	// 获取enabled entries
	for (i = 0; i < n_entries; i++)
	{
		ent = entries + i;

		if (sel == i)
			m_sel = n_enable_entries;

		if (ent->enabled)
		{
			enable_entries[n_enable_entries] = ent;
			n_enable_entries++;
		}
	}
	wt = 0;
	TTF_SizeUTF8(me_mfont, title, &wt, NULL);
	title_x = (g_menuscreen_w - wt) / 2;
	title_y = 5;
	text_out16(title_x, title_y, menu_sel_color, title);

	// 留3行显示help
	msg_x = 5;
	msg_sy = g_menuscreen_h - me_sfont_h * 3;

	// 获取列表最小起始y位置,与title隔一行
	listview_sy = title_y + 2 * me_mfont_h;
	// 获取绘画列表高度
	listview_h = msg_sy - listview_sy;
	// 获取列表绘画行数
	n_draw_items = listview_h / me_mfont_h;
	half_draw_items = n_draw_items / 2;
	// 获取列表终止dy位置
	listview_dy = listview_sy + n_draw_items * me_mfont_h;
	// 获取第一个绘画item index
	top_idx = m_sel - half_draw_items;
	if (top_idx > n_enable_entries - n_draw_items)
		top_idx = n_enable_entries - n_draw_items;
	if (top_idx < 0)
		top_idx = 0;

	for (i = 0; i < n_enable_entries; i++)
	{
		ent = enable_entries[i];
		wt = 0;
		name = ent->name;
		if (strlen(name) == 0)
		{
			if (ent->generate_name)
				name = ent->generate_name(ent->id);
		}
		if (name != NULL)
			TTF_SizeUTF8(me_mfont, ent->name, &wt, NULL);

		if (item_left_w < wt)

			item_left_w = wt;
	}

	sel_x = 5;
	sel_y = listview_sy + (m_sel - top_idx) * me_mfont_h;
	listview_x = sel_x + menu_draw_selection(sel_x, sel_y) + 5;
	listview_x2 = listview_x + item_left_w + me_mfont_w * 2;

	y = listview_sy;
	for (i = top_idx; i < n_enable_entries; i++)
	{
		if (y >= listview_dy)
			break;

		ent = enable_entries[i];

		name = ent->name;
		if (strlen(name) == 0)
		{
			if (ent->generate_name)
				name = ent->generate_name(ent->id);
		}
		if (name != NULL)
			text_out16(listview_x, y, menu_sel_color, name);

		switch (ent->beh)
		{
		case MB_NONE:
			break;
		case MB_OPT_ONOFF:
			text_out16(listview_x2, y, menu_sel_color, me_read_onoff(ent) ? "开启" : "关闭");
			break;
		case MB_OPT_RANGE:
			text_out16(listview_x2, y, menu_sel_color, "%i", *(int *)ent->var);
			break;
		case MB_OPT_CUSTOM:
		case MB_OPT_CUSTONOFF:
		case MB_OPT_CUSTRANGE:
			name = NULL;
			if (ent->generate_name)
				name = ent->generate_name(ent->id);
			if (name != NULL)
				text_out16(listview_x2, y, menu_sel_color, "%s", name);
			break;
		case MB_OPT_ENUM:
			names = (const char **)ent->data;
			for (j = 0; names[j] != NULL; j++)
			{
				if (j == *(unsigned char *)ent->var)
				{
					text_out16(listview_x2, y, menu_sel_color, "%s", names[j]);
					break;
				}
			}
			break;
		}

		y += me_mfont_h;
	}

	menu_separation();

	ent = enable_entries[m_sel];

	if (menu_error_msg[0] != 0)
	{
		smalltext_out16(msg_x, msg_sy, menu_sel_color, menu_error_msg);

		if (plat_get_ticks_ms() - menu_error_time > 2048)
			menu_error_msg[0] = 0;
	}
	else if (ent->help != NULL)
	{
		int len = strlen(ent->help);
		int i, l = 1;
		for (i = 0; i < len; i++)
		{
			if (ent->help[i] == '\n')
				l++;
		}
		if (l <= 3)
		{
			y = msg_sy + me_sfont_h * (3 - l);

			char *buf = malloc(len + 1);
			strcpy(buf, ent->help);
			char *p = buf;
			char *p2;
			for (i = 0; i < l; i++)
			{
				p2 = strchr(p, '\n');
				if (p2)
					*p2++ = '\0';
				smalltext_out16(msg_x, y, 0xffff, p);
				p = p2;
				y += me_sfont_h;
			}
			free(buf);
		}
		else
		{
			lprintf("menu msg doesn't fit!\n");
		}
	}

	menu_separation();

finish:
	if (enable_entries)
		free(enable_entries);

	if (draw_more != NULL)
		draw_more();

	menu_draw_end();
}

static int me_process(menu_entry *entry, int is_next, int is_lr)
{
	const char **names;
	int c;
	switch (entry->beh)
	{
	case MB_OPT_ONOFF:
	case MB_OPT_CUSTONOFF:
		me_toggle_onoff(entry);
		return 1;
	case MB_OPT_RANGE:
	case MB_OPT_CUSTRANGE:
		c = is_lr ? 10 : 1;
		*(int *)entry->var += is_next ? c : -c;
		if (*(int *)entry->var < (int)entry->min)
			*(int *)entry->var = (int)entry->max;
		if (*(int *)entry->var > (int)entry->max)
			*(int *)entry->var = (int)entry->min;
		return 1;
	case MB_OPT_ENUM:
		names = (const char **)entry->data;
		for (c = 0; names[c] != NULL; c++)
			;
		*(signed char *)entry->var += is_next ? 1 : -1;
		if (*(signed char *)entry->var < 0)
			*(signed char *)entry->var = 0;
		if (*(signed char *)entry->var >= c)
			*(signed char *)entry->var = c - 1;
		return 1;
	default:
		return 0;
	}
}

static void debug_menu_loop(void);

static int me_loop_d(const char *title, menu_entry *menu, int *menu_sel, void (*draw_prep)(void), void (*draw_more)(void))
{
	int ret = 0, inp, sel = *menu_sel, menu_sel_max;

	menu_sel_max = me_count(menu) - 1;
	if (menu_sel_max < 0)
	{
		lprintf("no enabled menu entries\n");
		return 0;
	}

	while ((!menu[sel].enabled || !menu[sel].selectable) && sel < menu_sel_max)
		sel++;

	/* make sure action buttons are not pressed on entering menu */
	me_draw(title, menu, sel, NULL);
	while (in_menu_wait_any(NULL, 50) & (PBTN_MOK | PBTN_MBACK | PBTN_MENU))
		;

	for (;;)
	{
		if (draw_prep != NULL)
			draw_prep();

		me_draw(title, menu, sel, draw_more);
		inp = in_menu_wait(PBTN_UP | PBTN_DOWN | PBTN_LEFT | PBTN_RIGHT |
							   PBTN_MOK | PBTN_MBACK | PBTN_MENU | PBTN_L | PBTN_R,
						   NULL, 70);
		if (inp & (PBTN_MENU | PBTN_MBACK))
			break;

		if (inp & PBTN_UP)
		{
			do
			{
				sel--;
				if (sel < 0)
					sel = menu_sel_max;
			} while (!menu[sel].enabled || !menu[sel].selectable);
		}
		if (inp & PBTN_DOWN)
		{
			do
			{
				sel++;
				if (sel > menu_sel_max)
					sel = 0;
			} while (!menu[sel].enabled || !menu[sel].selectable);
		}

		/* a bit hacky but oh well */
		if ((inp & (PBTN_L | PBTN_R)) == (PBTN_L | PBTN_R))
			debug_menu_loop();

		if (inp & (PBTN_LEFT | PBTN_RIGHT | PBTN_L | PBTN_R))
		{ /* multi choice */
			if (me_process(&menu[sel], (inp & (PBTN_RIGHT | PBTN_R)) ? 1 : 0,
						   inp & (PBTN_L | PBTN_R)))
				continue;
		}

		if (inp & (PBTN_MOK | PBTN_LEFT | PBTN_RIGHT | PBTN_L | PBTN_R))
		{
			/* require PBTN_MOK for MB_NONE */
			if (menu[sel].handler != NULL && (menu[sel].beh != MB_NONE || (inp & PBTN_MOK)))
			{
				ret = menu[sel].handler(menu[sel].id, inp);
				if (ret)
					break;
				menu_sel_max = me_count(menu) - 1; /* might change, so update */
			}
		}
	}
	*menu_sel = sel;

	return ret;
}

static int me_loop(const char *title, menu_entry *menu, int *menu_sel)
{
	return me_loop_d(title, menu, menu_sel, NULL, NULL);
}

/* ***************************************** */

static void draw_menu_message(const char *msg, void (*draw_more)(void))
{
	int x, y, h, w, wt;
	char *buf = NULL;
	char *p, *p2;
	int len, lines, i;

	menu_draw_begin(1, 0);

	len = strlen(msg);
	buf = malloc(len + 1);
	if (!buf)
		goto finish;
	strcpy(buf, msg);

	p = buf;
	lines = 1;
	w = 0;

	for (i = 0; i < len; i++)
	{
		if (buf[i] == '\n')
		{
			wt = 0;
			buf[i] = '\0';
			TTF_SizeUTF8(me_mfont, p, &wt, NULL);
			buf[i] = '\n';
			if (w < wt)
				w = wt;
			p = buf + i + 1;
			lines++;
		}
	}

	h = me_sfont_h * lines;
	x = (g_menuscreen_w - w) / 2;
	y = (g_menuscreen_h - h) / 2;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;

	for (i = 0; i < len; i++)
	{
		if (buf[i] == '\n')
		{
			wt = 0;
			buf[i] = '\0';
			TTF_SizeUTF8(me_mfont, p, &wt, NULL);
			buf[i] = '\n';
			if (w < wt)
				w = wt;
			p = buf + i + 1;
			lines++;
		}
	}

	p = buf;
	for (i = 0; i < lines; i++)
	{
		p2 = strchr(p, '\n');
		if (p2)
			*p2++ = '\0';
		smalltext_out16(x, y, 0xffff, p);
		p = p2;
		y += me_sfont_h;
	}

	menu_separation();

finish:
	if (buf)
		free(buf);

	if (draw_more != NULL)
		draw_more();

	menu_draw_end();
}

// -------------- del confirm ---------------

static void do_delete(const char *fpath, const char *fname)
{
	int len, mid, inp;
	const char *nm;
	char tmp[64];

	menu_draw_begin(1, 0);

	len = strlen(fname);
	if (len > g_menuscreen_w / me_sfont_w)
		len = g_menuscreen_w / me_sfont_w;

	mid = g_menuscreen_w / 2;
	text_out16(mid - me_mfont_w * 15 / 2, 8 * me_mfont_h, menu_sel_color, "About to delete");
	smalltext_out16(mid - len * me_sfont_w / 2, 9 * me_mfont_h + 5, 0xbdff, fname);
	text_out16(mid - me_mfont_w * 13 / 2, 11 * me_mfont_h, menu_sel_color, "Are you sure?");

	nm = in_get_key_name(-1, -PBTN_MA3);
	snprintf(tmp, sizeof(tmp), "(%s - confirm, ", nm);
	len = strlen(tmp);
	nm = in_get_key_name(-1, -PBTN_MBACK);
	snprintf(tmp + len, sizeof(tmp) - len, "%s - cancel)", nm);
	len = strlen(tmp);

	text_out16(mid - me_mfont_w * len / 2, 12 * me_mfont_h, menu_sel_color, tmp);
	menu_draw_end();

	while (in_menu_wait_any(NULL, 50) & (PBTN_MENU | PBTN_MA2))
		;
	inp = in_menu_wait(PBTN_MA3 | PBTN_MBACK, NULL, 100);
	if (inp & PBTN_MA3)
		remove(fpath);
}

// -------------- ROM selector --------------

static void draw_dirlist(char *curdir, struct dirent **namelist,
						 int list_len, int sel, int show_help)
{
	int wt;
	char *pcurdir;
	char curdir_buf[256];
	int curdir_x, curdir_y;
	int msg_x, msg_sy;
	int sel_x, sel_y;
	int listview_h;
	int listview_x, listview_sy, listview_dy;
	int n_draw_items, half_draw_items, top_idx;

	void *darken_ptr;
	int i, x, y;

	menu_draw_begin(1, 0);

	curdir_x = 5;
	curdir_y = 5;
	if (curdir)
	{
		pcurdir = curdir;
		TTF_SizeUTF8(me_mfont, curdir, &wt, NULL);
		// 过长截断
		if (wt > g_menuscreen_w - 10)
		{
			char *p = strrchr(curdir, '/');
			if (p)
			{
				snprintf(curdir_buf, sizeof(curdir_buf), "...%s", p);
				pcurdir = curdir_buf;
			}
		}
		text_out16(curdir_x, curdir_y, 0xffff, pcurdir);
	}

	// help
	msg_sy = g_menuscreen_h;
	if (show_help)
		msg_sy -= me_sfont_h * 3;

	// 获取列表起始y位置,与title隔一行
	listview_sy = curdir_y + 2 * me_mfont_h;
	// 获取绘画列表高度
	listview_h = msg_sy - listview_sy;
	// 获取列表绘画行数
	n_draw_items = listview_h / me_mfont_h;
	half_draw_items = n_draw_items / 2;
	// 获取列表终止dy位置
	listview_dy = listview_sy + n_draw_items * me_mfont_h;
	// 获取第一个绘画item index
	top_idx = sel - half_draw_items;
	if (top_idx > list_len - n_draw_items)
		top_idx = list_len - n_draw_items;
	if (top_idx < 0)
		top_idx = 0;

	sel_x = 5;
	sel_y = listview_sy + (sel - top_idx) * me_mfont_h;
	darken_ptr = (short *)g_menuscreen_surface->pixels + g_menuscreen_pp * (sel_y - 2);
	SDL_LockSurface(g_menuscreen_surface);
	menu_darken_bg(darken_ptr, darken_ptr, g_menuscreen_pp * (me_mfont_h + 4), 1);
	SDL_UnlockSurface(g_menuscreen_surface);
	listview_x = sel_x + menu_draw_selection(sel_x, sel_y) + 5;

	y = listview_sy;
	for (i = top_idx; i < list_len; i++)
	{
		if (y >= listview_dy)
			break;

		if (namelist[i]->d_type == DT_DIR)
		{
			x = listview_x;
			x += text_out16(x, y, 0xffff, namelist[i]->d_name);
			if (strcmp(namelist[i]->d_name, "..") != 0)
				text_out16(x, y, 0xffff, "/");
		}
		else
		{
			text_out16(listview_x, y, 0xfff6, namelist[i]->d_name);
		}
		y += me_mfont_h;
	}

	if (show_help)
	{
		y = msg_sy;
		smalltext_out16(msg_x, y, 0xe78c, "%s - 選擇, %s - 返回",
						in_get_key_name(-1, -PBTN_MOK), in_get_key_name(-1, -PBTN_MBACK));
		y += me_sfont_h;
		smalltext_out16(msg_x, y, 0xe78c, g_menu_filter_off ? "%s - 隱藏未知文件" : "%s - 顯示全部文件",
						in_get_key_name(-1, -PBTN_MA3));
		y += me_sfont_h;
		smalltext_out16(msg_x, y, 0xe78c, g_autostateld_opt ? "%s - 已開啟自動加載存檔" : "%s - 已關閉自動加載存檔",
						in_get_key_name(-1, -PBTN_MA2));
	}

	menu_draw_end();
}

static int scandir_cmp(const void *p1, const void *p2)
{
	const struct dirent **d1 = (const struct dirent **)p1;
	const struct dirent **d2 = (const struct dirent **)p2;
	const char *p;
	if ((p = (*d1)->d_name)[0] == '.' && p[1] == '.' && p[2] == 0)
		return -1; // ".." first
	if ((p = (*d2)->d_name)[0] == '.' && p[1] == '.' && p[2] == 0)
		return 1;
	if ((*d1)->d_type == (*d2)->d_type)
		return alphasort(d1, d2);
	if ((*d1)->d_type == DT_DIR)
		return -1; // directories before files/links
	if ((*d2)->d_type == DT_DIR)
		return 1;

	return alphasort(d1, d2);
}

static const char **filter_exts_internal;

static int scandir_filter(const struct dirent *ent)
{
	const char **filter = filter_exts_internal;
	const char *ext;
	int i;

	if (ent == NULL)
		return 0;

	switch (ent->d_type)
	{
	case DT_DIR:
		// leave out useless reference to current directory
		return strcmp(ent->d_name, ".") != 0;
	case DT_LNK:
	case DT_UNKNOWN:
		// could be a dir, deal with it later..
		return 1;
	}

	ext = strrchr(ent->d_name, '.');
	if (ext == NULL)
		return 0;

	ext++;
	for (i = 0; filter[i] != NULL; i++)
		if (strcasecmp(ext, filter[i]) == 0)
			return 1;

	return 0;
}

static int dirent_seek_char(struct dirent **namelist, int len, int sel, char c)
{
	int i;

	sel++;
	for (i = sel + 1;; i++)
	{
		if (i >= len)
			i = 1;
		if (i == sel)
			break;

		if (tolower_simple(namelist[i]->d_name[0]) == c)
			break;
	}

	return i - 1;
}

static int menu_make_file_name(char *name, char *path, int size)
{
	if (!name || !path)
		return -1;

	int len = strlen(path);
	if (len <= 0)
		goto failed;

	if (len == 1 && strcmp(path, "/") == 0)
		goto failed;

	char *p2 = path + len - 1;
	while (*p2 == '/' && p2 > path)
		p2--;

	char *p = p2;
	while (*p != '/' && p > path)
		p--;

	len = p2 - p;
	if (size <= len)
		goto failed;
	strncpy(name, p + 1, len);
	name[len] = '\0';
	return 0;

failed:
	name[0] = '\0';
	return -1;
}

static const char *menu_loop_romsel(char *curr_path, int len,
									const char **filter_exts,
									int (*extra_filter)(struct dirent **namelist, int count,
														const char *basedir))
{
	static char rom_fname_reload[256]; // used for scratch and return
	static char sel_fname[256] = {0};
	int (*filter)(const struct dirent *);
	struct dirent **namelist = NULL;
	int n = 0, inp = 0, sel = 0, show_help = 0;
	char *curr_path_restore = NULL;
	const char *ret = NULL;
	char cinp;
	int r, i;

	filter_exts_internal = filter_exts;

	// is this a dir or a full path?
	if (!plat_is_dir(curr_path))
	{
		char *p = strrchr(curr_path, '/');
		if (p != NULL)
		{
			*p = 0;
			curr_path_restore = p;
			snprintf(sel_fname, sizeof(sel_fname), "%s", p + 1);
		}

		if (rom_fname_reload[0] == 0)
			show_help = 2;
	}

rescan:
	if (namelist != NULL)
	{
		while (n-- > 0)
			free(namelist[n]);
		free(namelist);
		namelist = NULL;
	}

	filter = NULL;
	if (!g_menu_filter_off)
		filter = scandir_filter;

	n = scandir(curr_path, &namelist, filter, (void *)scandir_cmp);
	if (n < 0)
	{
		lprintf("menu_loop_romsel failed, dir: %s\n", curr_path);

		// try data root
		plat_get_data_dir(curr_path, len);
		n = scandir(curr_path, &namelist, filter, (void *)scandir_cmp);
		if (n < 0)
		{
			// oops, we failed
			lprintf("menu_loop_romsel failed, dir: %s\n", curr_path);
			return NULL;
		}
	}

	// try to resolve DT_UNKNOWN and symlinks
	for (i = 0; i < n; i++)
	{
		struct stat st;
		char *slash;

		if (namelist[i]->d_type == DT_REG || namelist[i]->d_type == DT_DIR)
			continue;

		r = strlen(curr_path);
		slash = (r && curr_path[r - 1] == '/') ? "" : "/";
		snprintf(rom_fname_reload, sizeof(rom_fname_reload),
				 "%s%s%s", curr_path, slash, namelist[i]->d_name);
		r = stat(rom_fname_reload, &st);
		if (r == 0)
		{
			if (S_ISREG(st.st_mode))
				namelist[i]->d_type = DT_REG;
			else if (S_ISDIR(st.st_mode))
				namelist[i]->d_type = DT_DIR;
		}
	}

	if (!g_menu_filter_off && extra_filter != NULL)
		n = extra_filter(namelist, n, curr_path);

	if (n > 1)
		qsort(namelist, n, sizeof(namelist[0]), scandir_cmp);

	// try to find selected file
	sel = 0;
	if (sel_fname[0] != 0)
	{
		for (i = 0; i < n; i++)
		{
			char *dname = namelist[i]->d_name;
			if (dname[0] == sel_fname[0] && strcmp(dname, sel_fname) == 0)
			{
				sel = i;
				break;
			}
		}
	}

	/* make sure action buttons are not pressed on entering menu */
	draw_dirlist(curr_path, namelist, n, sel, show_help);
	while (in_menu_wait_any(NULL, 50) & (PBTN_MOK | PBTN_MBACK | PBTN_MENU))
		;

	for (;;)
	{
		draw_dirlist(curr_path, namelist, n, sel, show_help);
		inp = in_menu_wait(PBTN_UP | PBTN_DOWN | PBTN_LEFT | PBTN_RIGHT | PBTN_L | PBTN_R | PBTN_MA2 | PBTN_MA3 | PBTN_MOK | PBTN_MBACK | PBTN_MENU | PBTN_CHAR, &cinp, 33);
		if (inp & PBTN_MA3)
		{
			g_menu_filter_off = !g_menu_filter_off;
			snprintf(sel_fname, sizeof(sel_fname), "%s",
					 namelist[sel]->d_name);
			goto rescan;
		}
		if (inp & PBTN_UP)
		{
			sel--;
			if (sel < 0)
				sel = n - 1;
		}
		if (inp & PBTN_DOWN)
		{
			sel++;
			if (sel > n - 1)
				sel = 0;
		}
		if (inp & PBTN_LEFT)
		{
			sel -= 10;
			if (sel < 0)
				sel = 0;
		}
		if (inp & PBTN_L)
		{
			sel -= 24;
			if (sel < 0)
				sel = 0;
		}
		if (inp & PBTN_RIGHT)
		{
			sel += 10;
			if (sel > n - 1)
				sel = n - 1;
		}
		if (inp & PBTN_R)
		{
			sel += 24;
			if (sel > n - 1)
				sel = n - 1;
		}

		if ((inp & PBTN_MOK) || (inp & (PBTN_MENU | PBTN_MA2)) == (PBTN_MENU | PBTN_MA2))
		{
			if (namelist[sel]->d_type == DT_REG)
			{
				int l = strlen(curr_path);
				char *slash = l && curr_path[l - 1] == '/' ? "" : "/";
				snprintf(rom_fname_reload, sizeof(rom_fname_reload),
						 "%s%s%s", curr_path, slash, namelist[sel]->d_name);

				if (inp & PBTN_MOK)
				{ // return sel
					ret = rom_fname_reload;
					break;
				}
				do_delete(rom_fname_reload, namelist[sel]->d_name);
				goto rescan;
			}
			else if (namelist[sel]->d_type == DT_DIR)
			{
				int newlen;
				char *p, *newdir;
				if (!(inp & PBTN_MOK))
					continue;
				newlen = strlen(curr_path) + strlen(namelist[sel]->d_name) + 2;
				newdir = malloc(newlen);
				if (newdir == NULL)
					break;
				if (strcmp(namelist[sel]->d_name, "..") == 0)
				{
					menu_make_file_name(sel_fname, curr_path, sizeof(sel_fname));

					char *start = curr_path;
					p = start + strlen(start) - 1;
					while (*p == '/' && p > start)
						p--;
					while (*p != '/' && p > start)
						p--;
					if (p <= start)
						plat_get_data_dir(newdir, newlen);
					else
						strncpy(newdir, start, p - start);
				}
				else
				{
					strcpy(newdir, curr_path);
					p = newdir + strlen(newdir) - 1;
					while (*p == '/' && p >= newdir)
						*p-- = 0;
					strcat(newdir, "/");
					strcat(newdir, namelist[sel]->d_name);
					sel_fname[0] = '\0';
				}
				ret = menu_loop_romsel(newdir, newlen, filter_exts, extra_filter);
				free(newdir);
				break;
			}
		}
		else if (inp & PBTN_MA2)
		{
			g_autostateld_opt = !g_autostateld_opt;
			show_help = 3;
		}
		else if (inp & PBTN_CHAR)
		{
			// must be last
			sel = dirent_seek_char(namelist, n, sel, cinp);
		}

		if (inp & PBTN_MBACK)
		{
			sel_fname[0] = '\0';
			break;
		}

		if (show_help > 0)
			show_help--;
	}

	if (n > 0)
	{
		while (n-- > 0)
			free(namelist[n]);
		free(namelist);
	}

	// restore curr_path
	if (curr_path_restore != NULL)
		*curr_path_restore = '/';

	return ret;
}

// ------------ savestate loader ------------

#define STATE_SLOT_COUNT 20

static int state_slot_flags = 0;
static int state_slot_times[STATE_SLOT_COUNT];

static void state_check_slots(void)
{
	int slot;

	state_slot_flags = 0;

	for (slot = 0; slot < STATE_SLOT_COUNT; slot++)
	{
		state_slot_times[slot] = 0;
		if (emu_check_save_file(slot, &state_slot_times[slot]))
			state_slot_flags |= 1 << slot;
	}
}

static void draw_savestate_bg(int slot);

static void draw_savestate_menu(int menu_sel, int is_loading)
{
	int wt;
	int title_x, title_y;
	int sel_x, sel_y;
	int listview_h;
	int listview_x, listview_sy, listview_dy;
	int n_draw_items, half_draw_items, top_idx;
	char *title;
	char text_buf[64];

	if (state_slot_flags & (1 << menu_sel))
		draw_savestate_bg(menu_sel);

	menu_draw_begin(1, 1);

	title = is_loading ? "即時讀檔" : "即時存檔";
	wt = 0;
	TTF_SizeUTF8(me_mfont, title, &wt, NULL);
	title_x = (g_menuscreen_w - wt) / 2;
	title_y = 5;
	text_out16(title_x, title_y, menu_sel_color, title);

	// 获取列表起始y位置,与title隔一行
	listview_sy = title_y + 2 * me_mfont_h;
	// 获取绘画列表高度,底部留一行
	listview_h = g_menuscreen_h - me_mfont_h - listview_sy;
	// 获取列表绘画行数
	n_draw_items = listview_h / me_mfont_h;
	half_draw_items = n_draw_items / 2;
	// 获取列表终止dy位置
	listview_dy = listview_sy + n_draw_items * me_mfont_h;
	// 获取第一个绘画item index
	top_idx = menu_sel - half_draw_items;
	if (top_idx > STATE_SLOT_COUNT - n_draw_items)
		top_idx = STATE_SLOT_COUNT - n_draw_items;
	if (top_idx < 0)
		top_idx = 0;

	sel_x = 5;
	sel_y = listview_sy + (menu_sel - top_idx) * me_mfont_h;
	listview_x = sel_x + menu_draw_selection(sel_x, sel_y) + 5;

	int i, y = listview_sy;
	for (i = top_idx; i < STATE_SLOT_COUNT; i++)
	{
		if (y >= listview_dy)
			break;

		if (!(state_slot_flags & (1 << i)))
		{
			sprintf(text_buf, "未存檔 %02i", i + 1);
		}
		else
		{
			sprintf(text_buf, "已存檔 %02i", i + 1);
			if (state_slot_times[i] != 0)
			{
				time_t time = state_slot_times[i];
				struct tm *t = localtime(&time);
				strftime(text_buf, sizeof(text_buf), "%x %R", t);
			}
		}

		text_out16(listview_x, y, menu_sel_color, text_buf);
		y += me_mfont_h;
	}

	menu_draw_end();
}

static int menu_loop_savestate(int is_loading)
{
	static int menu_sel = 0;
	int menu_sel_max = STATE_SLOT_COUNT - 1;
	unsigned long inp = 0;
	int ret = 0;

	state_check_slots();

	for (;;)
	{
		draw_savestate_menu(menu_sel, is_loading);
		inp = in_menu_wait(PBTN_UP | PBTN_DOWN | PBTN_MOK | PBTN_MBACK, NULL, 100);
		if (inp & PBTN_UP)
		{
			menu_sel--;
			if (menu_sel < 0)
				menu_sel = menu_sel_max;
		}
		if (inp & PBTN_DOWN)
		{
			menu_sel++;
			if (menu_sel > menu_sel_max)
				menu_sel = 0;
		}
		if (inp & PBTN_MOK)
		{ // save/load
			if (menu_sel < STATE_SLOT_COUNT)
			{
				if (!is_loading || (state_slot_flags & (1 << menu_sel)))
				{
					state_slot = menu_sel;
					if (emu_save_load_game(is_loading, 0))
					{
						menu_update_msg(is_loading ? "Load failed" : "Save failed");
						break;
					}
					ret = 1;
					break;
				}
			}
		}
		if (inp & PBTN_MBACK)
			break;
	}

	return ret;
}

// -------------- key config --------------

static char *action_binds(int player_idx, int action_mask, int dev_id)
{
	int dev = 0, dev_last = IN_MAX_DEVS - 1;
	int can_combo = 1, type;

	static_buff[0] = 0;

	type = IN_BINDTYPE_EMU;
	if (player_idx >= 0)
	{
		can_combo = 0;
		type = IN_BINDTYPE_PLAYER12;
	}
	if (player_idx == 1)
		action_mask <<= 16;

	if (dev_id >= 0)
		dev = dev_last = dev_id;

	for (; dev <= dev_last; dev++)
	{
		int k, count = 0, combo = 0;
		const int *binds;

		binds = in_get_dev_binds(dev);
		if (binds == NULL)
			continue;

		in_get_config(dev, IN_CFG_BIND_COUNT, &count);
		in_get_config(dev, IN_CFG_DOES_COMBOS, &combo);
		combo = combo && can_combo;

		for (k = 0; k < count; k++)
		{
			const char *xname;
			int len;

			if (!(binds[IN_BIND_OFFS(k, type)] & action_mask))
				continue;

			xname = in_get_key_name(dev, k);
			len = strlen(static_buff);
			if (len)
			{
				strncat(static_buff, combo ? " + " : ", ",
						sizeof(static_buff) - len - 1);
				len += combo ? 3 : 2;
			}
			strncat(static_buff, xname, sizeof(static_buff) - len - 1);
		}
	}

	return static_buff;
}

static int count_bound_keys(int dev_id, int action_mask, int bindtype)
{
	const int *binds;
	int k, keys = 0;
	int count = 0;

	binds = in_get_dev_binds(dev_id);
	if (binds == NULL)
		return 0;

	in_get_config(dev_id, IN_CFG_BIND_COUNT, &count);
	for (k = 0; k < count; k++)
	{
		if (binds[IN_BIND_OFFS(k, bindtype)] & action_mask)
			keys++;
	}

	return keys;
}

static void draw_key_config(const me_bind_action *opts, int opt_cnt, int player_idx,
							int sel, int dev_id, int dev_count, int is_bind)
{
	int wt;
	int title_x, title_y;
	int msg_x, msg_sy;
	int sel_x, sel_y;
	int listview_h;
	int listview_x, listview_x2, listview_sy, listview_dy;
	int n_draw_items, half_draw_items, top_idx;
	int item_left_w = 0;
	const char *dev_name;
	char *val;
	char text_buf[64];
	int i, y;

	menu_draw_begin(1, 0);

	if (player_idx >= 0)
		sprintf(text_buf, "玩家%i按鍵設置", player_idx + 1);
	else
		strcpy(text_buf, "快捷鍵設置");
	wt = 0;
	TTF_SizeUTF8(me_mfont, text_buf, &wt, NULL);
	title_x = (g_menuscreen_w - wt) / 2;
	title_y = 5;
	text_out16(title_x, title_y, menu_sel_color, text_buf);

	// help
	msg_sy = g_menuscreen_h - me_sfont_h;
	if (dev_count > 1)
		msg_sy -= me_sfont_h * 2;

	// 获取列表起始y位置,与title隔一行
	listview_sy = title_y + 2 * me_mfont_h;
	// 获取绘画列表高度
	listview_h = msg_sy - listview_sy;
	// 获取列表绘画行数
	n_draw_items = listview_h / me_mfont_h;
	half_draw_items = n_draw_items / 2;
	// 获取列表终止dy位置
	listview_dy = listview_sy + n_draw_items * me_mfont_h;
	// 获取第一个绘画item index
	top_idx = sel - half_draw_items;
	if (top_idx > opt_cnt - n_draw_items)
		top_idx = opt_cnt - n_draw_items;
	if (top_idx < 0)
		top_idx = 0;

	for (i = 0; i < opt_cnt; i++)
	{
		wt = 0;
		TTF_SizeUTF8(me_mfont, opts[i].name, &wt, NULL);
		if (item_left_w < wt)
			item_left_w = wt;
	}

	sel_x = 5;
	sel_y = listview_sy + (sel - top_idx) * me_mfont_h;
	listview_x = sel_x + menu_draw_selection(sel_x, sel_y) + 5;
	listview_x2 = listview_x + item_left_w + me_mfont_w * 2;

	y = listview_sy;

	for (i = top_idx; i < opt_cnt; i++)
	{
		if (y >= listview_dy)
			break;

		text_out16(listview_x, y, menu_sel_color, opts[i].name);
		val = action_binds(player_idx, opts[i].mask, dev_id);
		if (strlen(val) > 0)
			text_out16(listview_x2, y, menu_sel_color, val);
		else
			text_out16(listview_x2, y, menu_sel_color, "(無)");
		y += me_mfont_h;
	}

	menu_separation();

	msg_x = 5;
	y = msg_sy;
	if (!is_bind)
	{
		snprintf(text_buf, sizeof(text_buf), "%s - 綁定, %s - 清除",
				 in_get_key_name(-1, -PBTN_MOK), in_get_key_name(-1, -PBTN_MA2));
		smalltext_out16(msg_x, y, 0xffff, text_buf);
	}
	else
	{
		smalltext_out16(msg_x, y, 0xffff, "請輸入一个按鍵綁定/解綁");
	}
	y += me_sfont_h;

	if (dev_count > 1)
	{
		if (dev_id < 0)
			dev_name = "(所有設備)";
		else
			dev_name = in_get_dev_name(dev_id, 0, 1);

		smalltext_out16(msg_x, y, 0xffff, dev_name);
		y += me_sfont_h;
		smalltext_out16(msg_x, y, 0xffff, "輸入左/右切換其他設備");
	}

	menu_draw_end();
}

static void key_config_loop(const me_bind_action *opts, int opt_cnt, int player_idx)
{
	int i, sel = 0, menu_sel_max = opt_cnt - 1, does_combos = 0;
	int dev_id, bind_dev_id, dev_count, kc, is_down, mkey;
	int unbind, bindtype, mask_shift;
	int allow_unbound_mods[IN_MAX_DEVS] = {0};

	for (i = 0, dev_id = -1, dev_count = 0; i < IN_MAX_DEVS; i++)
	{
		if (in_get_dev_name(i, 1, 0) != NULL)
		{
			dev_count++;
			if (dev_id < 0)
				dev_id = i;
		}
	}

	if (dev_id == -1)
	{
		lprintf("no devs, can't do config\n");
		return;
	}

	dev_id = -1; // show all
	mask_shift = 0;
	if (player_idx == 1)
		mask_shift = 16;
	bindtype = player_idx >= 0 ? IN_BINDTYPE_PLAYER12 : IN_BINDTYPE_EMU;

	for (i = 0; i < IN_MAX_DEVS; i++)
	{
		in_get_config(i, IN_CFG_ALLOW_UNBOUND_MOD_KEYS, &allow_unbound_mods[i]);
		in_set_config_int(i, IN_CFG_ALLOW_UNBOUND_MOD_KEYS, 1);
	}

	for (;;)
	{
		draw_key_config(opts, opt_cnt, player_idx, sel, dev_id, dev_count, 0);
		mkey = in_menu_wait(PBTN_UP | PBTN_DOWN | PBTN_LEFT | PBTN_RIGHT | PBTN_MBACK | PBTN_MOK | PBTN_MA2, NULL, 100);
		switch (mkey)
		{
		case PBTN_UP:
			sel--;
			if (sel < 0)
				sel = menu_sel_max;
			continue;
		case PBTN_DOWN:
			sel++;
			if (sel > menu_sel_max)
				sel = 0;
			continue;
		case PBTN_LEFT:
			for (i = 0, dev_id--; i < IN_MAX_DEVS + 1; i++, dev_id--)
			{
				if (dev_id < -1)
					dev_id = IN_MAX_DEVS - 1;
				if (dev_id == -1 || in_get_dev_name(dev_id, 0, 0) != NULL)
					break;
			}
			continue;
		case PBTN_RIGHT:
			for (i = 0, dev_id++; i < IN_MAX_DEVS; i++, dev_id++)
			{
				if (dev_id >= IN_MAX_DEVS)
					dev_id = -1;
				if (dev_id == -1 || in_get_dev_name(dev_id, 0, 0) != NULL)
					break;
			}
			continue;
		case PBTN_MBACK:
			goto finish;
		case PBTN_MOK:
			if (sel >= opt_cnt)
				goto finish;
			while (in_menu_wait_any(NULL, 30) & PBTN_MOK)
				;
			break;
		case PBTN_MA2:
			in_unbind_all(dev_id, opts[sel].mask << mask_shift, bindtype);
			continue;
		default:
			continue;
		}

		draw_key_config(opts, opt_cnt, player_idx, sel, dev_id, dev_count, 1);

		/* wait for some up event */
		for (is_down = 1; is_down;)
			kc = in_update_keycode(&bind_dev_id, &is_down, NULL, -1);

		i = count_bound_keys(bind_dev_id, opts[sel].mask << mask_shift, bindtype);
		unbind = (i > 0);

		/* allow combos if device supports them */
		in_get_config(bind_dev_id, IN_CFG_DOES_COMBOS, &does_combos);
		if (i == 1 && bindtype == IN_BINDTYPE_EMU && does_combos)
			unbind = 0;

		if (unbind)
			in_unbind_all(bind_dev_id, opts[sel].mask << mask_shift, bindtype);

		in_bind_key(bind_dev_id, kc, opts[sel].mask << mask_shift, bindtype, 0);

		// make sure bind change is displayed
		if (dev_id != -1)
			dev_id = bind_dev_id;
	}

finish:
	for (i = 0; i < IN_MAX_DEVS; i++)
	{
		in_set_config_int(i, IN_CFG_ALLOW_UNBOUND_MOD_KEYS, allow_unbound_mods[i]);
	}
}
