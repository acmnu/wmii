/*
 * (C)opyright MMIV-MMV Anselm R. Garbe <garbeam at gmail dot com>
 * See LICENSE file for license details.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wm.h"
#include "layoutdef.h"

typedef struct Acme Acme;
typedef struct Column Column;

struct Acme {
	Container columns;
	Container frames;
};

struct Column {
	Bool refresh;
	Container frames;
	XRectangle rect;
};

static void init_col(Area * a);
static void deinit_col(Area * a);
static void arrange_col(Area * a);
static Bool attach_col(Area * a, Client * c);
static void detach_col(Area * a, Client * c, Bool unmap);
static void resize_col(Frame *f, XRectangle * new, XPoint * pt);
static void select_col(Frame *f, Bool raise);
static Container *get_frames_col(Area *a);
static Action *get_actions_col(Area *a);

static void select_frame(void *obj, char *arg);
static void swap_frame(void *obj, char *arg);
static void new_col(void *obj, char *arg);
static void destroy_col(void *obj, char *arg);

static Action lcol_acttbl[] = {
	{"select", select_frame},
	{"swap", swap_frame},
	{"new", new_col},
	{"destroy", destroy_col},
	{0, 0}
};

static Layout lcol = { "col", init_col, deinit_col, arrange_col, attach_col, detach_col,
   				       resize_col, select_col, get_frames_col, get_actions_col };

void init_layout_column()
{
	cext_attach_item(&layouts, &lcol);
}

static Column *get_sel_column(Acme *acme)
{
	return cext_stack_get_top_item(&acme->columns);
}

static void iter_arrange_column_frame(void *frame, void *height)
{
	Frame *f = frame;
	Column *col = f->aux;
	size_t size = cext_sizeof(&col->frames);
	unsigned int h = *(unsigned int *)height;
	int idx = cext_list_get_item_index(&col->frames, f) ;

	if (col->refresh) {
		f->rect = col->rect;
		f->rect.y = idx * h;
		if (idx + 1 == size)
			f->rect.height = f->area->rect.height - f->rect.y;
		else
			f->rect.height = h;
	}
	resize_frame(f, &f->rect, 0);
}

static void iter_arrange_column(void *column, void *area)
{
	Column *col = column;
	size_t size = cext_sizeof(&col->frames);
	unsigned int height;
   
	if (size) {
		height= ((Area *)area)->rect.height / size;
		cext_list_iterate(&col->frames, &height, iter_arrange_column_frame);
		col->refresh = False;
	}
}

static void arrange_col(Area *a)
{
	Acme *acme = a->aux;
	cext_list_iterate(&acme->columns, a, iter_arrange_column);
	XSync(dpy, False);
}

static void iter_attach_col(void *client, void *area)
{
	attach_col(area, client);
}

static void init_col(Area *a)
{
	Acme *acme = cext_emallocz(sizeof(Acme));
	Column *col = cext_emallocz(sizeof(Column));
	a->aux = acme;
	col->rect = a->rect;
	cext_attach_item(&acme->columns, col);
	cext_list_iterate(&a->clients, a, iter_attach_col);
}

static void iter_detach_client(void *client, void *area)
{
	Area *a = area;
	detach_col(a, (Client *)client, a->page != get_sel_page());
}

static void deinit_col(Area *a)
{
	Acme *acme = a->aux;
	Column *col;
	cext_list_iterate(&a->clients, a, iter_detach_client);
	while ((col = get_sel_column(acme)))
	{
		cext_detach_item(&acme->columns, col);
		free(col);
	}
	free(acme);
	a->aux = 0;
}

static Bool attach_col(Area *a, Client *c)
{
	Acme *acme = a->aux;
	Column *col = get_sel_column(acme);
	Frame *f = get_sel_frame_of_area(a);

	/* check for tabbing? */
	if (f && (((char *) f->file[F_LOCKED]->content)[0] == '1'))
		f = 0;
	if (!f) {
		f = alloc_frame(&c->rect);
		attach_frame_to_area(a, f);
		f->aux = col;
		cext_attach_item(&acme->frames, f);
		cext_attach_item(&col->frames, f);
	}
	attach_client_to_frame(f, c);
	if (a->page == get_sel_page())
		XMapWindow(dpy, f->win);
	col->refresh = True;
	arrange_col(a);
	select_col(f, True);
	return True;
}

static void detach_col(Area *a, Client *c, Bool unmap)
{
	Acme *acme = a->aux;
	Frame *f = c->frame;
	Column *col = f->aux;

	detach_client_from_frame(c, unmap);
	if (!cext_sizeof(&f->clients)) {
		detach_frame_from_area(f);
		cext_detach_item(&acme->frames, f);
		cext_detach_item(&col->frames, f);
		destroy_frame(f);
	}
	col->refresh = True;
	arrange_col(a);
}

static void iter_match_frame_horiz(void *frame, void *rect)
{
	Frame *f = frame;
	XRectangle *r = rect;
	f->rect.x = r->x;
	f->rect.width = r->width;
	resize_frame(f, &f->rect, nil);
}

static void drop_resize(Frame *f, XRectangle *new)
{
	Column *west = 0, *east = 0, *col = f->aux;
	Frame *north = 0, *south = 0;
	Acme *acme = f->area->aux;
	size_t ncol = cext_sizeof(&acme->columns);
	size_t nfr = cext_sizeof(&col->frames);
	int colidx = cext_list_get_item_index(&acme->columns, col);
	int fidx = cext_list_get_item_index(&col->frames, f);

	if (colidx > 0)
		west = cext_list_get_item(&acme->columns, colidx - 1);
	if (colidx + 1 < ncol)
		east = cext_list_get_item(&acme->columns, colidx + 1);
	if (fidx > 0)
		north = cext_list_get_item(&col->frames, fidx - 1);
	if (fidx + 1 < nfr)
		south = cext_list_get_item(&col->frames, fidx + 1);

	/* horizontal resize */
	if (new->x < f->rect.x) {
		if (west && new->x > west->rect.x) {
			west->rect.width = new->x - west->rect.x;
			col->rect.width += f->rect.x - new->x;
			col->rect.x = new->x;
			cext_list_iterate(&west->frames, &west->rect, iter_match_frame_horiz);
			cext_list_iterate(&col->frames, &col->rect, iter_match_frame_horiz);
		}
	}
	if (new->x + new->width > f->rect.x + f->rect.width) {
		if (east && (new->x + new->width < east->rect.x + east->rect.width)) {
			east->rect.width -= new->x + new->width - east->rect.x;
			east->rect.x = new->x + new->width;
			col->rect.x = new->x;
			col->rect.width = new->width;
			cext_list_iterate(&col->frames, &col->rect, iter_match_frame_horiz);
			cext_list_iterate(&east->frames, &east->rect, iter_match_frame_horiz);
		}
	}

	/* vertical resize */
	if (new->y < f->rect.y) {
		if (north && new->y > north->rect.y) {
			north->rect.height = new->y - north->rect.y;
			f->rect.height += new->y - north->rect.y;
			f->rect.y = new->y;
			resize_frame(north, &north->rect, nil);
			resize_frame(f, &f->rect, nil);
		}
	}
	if (new->y + new->height > f->rect.y + f->rect.height) {
		if (south && (new->y + new->height < south->rect.y + south->rect.height)) {
			south->rect.height -= new->y + new->height - south->rect.y;
			south->rect.y = new->y + new->height;
			f->rect.y = new->y;
			f->rect.height = new->height;
			resize_frame(f, &f->rect, nil);
			resize_frame(south, &south->rect, nil);
		}
	}
}

static int comp_pointer(void *point, void *column)
{
	Column *col = column;
	XPoint *pt = point;

	return blitz_ispointinrect(pt->x, pt->y, &col->rect);
}

static void drop_moving(Frame *f, XRectangle *new, XPoint *pt)
{
	Acme *acme = f->area->aux;
	Column *src = f->aux, *tgt = 0;

	if (!pt)
		return;

	tgt = cext_find_item(&acme->columns, pt, comp_pointer);

	if (tgt && tgt != src)
	{
		cext_detach_item(&src->frames, f);
		cext_attach_item(&tgt->frames, f);
		f->aux = tgt;
		tgt->refresh = src->refresh = True;
		cext_stack_top_item(&acme->columns, tgt);
		arrange_col(f->area);
	}
}

static void resize_col(Frame *f, XRectangle *new, XPoint *pt)
{
	if ((f->rect.width == new->width)
		&& (f->rect.height == new->height))
		drop_moving(f, new, pt);
	else
		drop_resize(f, new);
	draw_area(f->area);
}

static void select_col(Frame *f, Bool raise)
{
	Area *a = f->area;
	Acme *acme = a->aux;
	Column *col = f->aux;
	cext_stack_top_item(&acme->columns, col);
	sel_client(cext_stack_get_top_item(&f->clients));
	cext_stack_top_item(&col->frames, f);
	cext_stack_top_item(&acme->frames, f);
	a->file[A_SEL_FRAME]->content = f->file[F_PREFIX]->content;
	if (raise)
		center_pointer(f);
}

static Container *get_frames_col(Area *a)
{
	Acme *acme = a->aux;
	return &acme->frames;
}

static Action *get_actions_col(Area *a)
{
	return lcol_acttbl;
}	

static void select_frame(void *obj, char *arg)
{
	Area *a = obj;
	Acme *acme = a->aux;
	Column *col = get_sel_column(acme);
	Frame *f, *old;

	f = old = cext_stack_get_top_item(&col->frames);
	if (!f || !arg)
		return;
	if (!strncmp(arg, "prev", 5))
		f = cext_list_get_prev_item(&col->frames, f);
	else if (!strncmp(arg, "next", 5))
		f = cext_list_get_next_item(&col->frames, f);
	else if (!strncmp(arg, "west", 5)) {
		col = cext_list_get_prev_item(&acme->columns, col);
		cext_stack_top_item(&acme->columns, col);
		f = cext_stack_get_top_item(&col->frames);
	}
	else if (!strncmp(arg, "east", 5)) {
		col = cext_list_get_next_item(&acme->columns, col);
		cext_stack_top_item(&acme->columns, col);
		f = cext_stack_get_top_item(&col->frames);
	}
	else 
		f = cext_list_get_item(&col->frames, blitz_strtonum(arg, 0, cext_sizeof(&col->frames) - 1));
	if (f && old != f) {
		select_col(f, True);
		draw_frame(old, nil);
		draw_frame(f, nil);
	}
}

static void swap_frame(void *obj, char *arg)
{
	Area *a = obj;
	Acme *acme = a->aux;
	Column *col = get_sel_column(acme);
	Frame *f = cext_stack_get_top_item(&col->frames);
	if (!f || !arg)
		return;
}

static void update_column_width(Area *a)
{
	Acme *acme = a->aux;
	size_t size = cext_sizeof(&acme->columns);
	unsigned int i, width = a->rect.width / cext_sizeof(&acme->columns);

	for (i = 0; i < size; i++) {
		Column *col = cext_list_get_item(&acme->columns, i);
		col->refresh = True;
		col->rect.x = i * width;
		col->rect.width = width;
	}
	arrange_col(a);
}

static void new_col(void *obj, char *arg)
{
	Area *a = obj;
	Acme *acme = a->aux;
	Column *col = cext_emallocz(sizeof(Column));
	cext_attach_item(&acme->columns, col);
	update_column_width(a);
}

static void destroy_col(void *obj, char *arg)
{
	Area *a = obj;
	Acme *acme = a->aux;
	Column *col = get_sel_column(acme);
	size_t size;

	if (cext_sizeof(&acme->columns) > 1) {
		while (cext_sizeof(&col->frames)) {
			Frame *f = cext_stack_get_top_item(&col->frames);
			while ((size = cext_sizeof(&f->clients))) {
				detach_col(a, cext_stack_get_top_item(&f->clients), a->page != get_sel_page());
			    if (size == 1)
				 	break;	
			}
		}
		cext_detach_item(&acme->columns, col);
		free(col);
		update_column_width(a);
	}
}
