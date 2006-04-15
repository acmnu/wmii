/*
 * (C)opyright MMIV-MMVI Anselm R. Garbe <garbeam at gmail dot com>
 * See LICENSE file for license details.
 */

#include <stdlib.h>
#include <string.h>

#include "wm.h"

static Vector *
vector_of_areas(AreaVector *av)
{
	return (Vector *) av;
}

Area *
create_area(View *v)
{
	unsigned int w;
	if(def.colw)
		w = def.colw;
	else if(v->area.size > 1)
		w = rect.width / v->area.size;
	else
		w = rect.width;
	if(v->area.size >= 2 && (v->area.size - 1) * MIN_COLWIDTH + w > rect.width)
		return nil;
	else
	{
		static unsigned short id = 1;
		Area *a = cext_emallocz(sizeof(Area));
		a->view = v;
		a->id = id++;
		a->rect = rect;
		a->rect.height = rect.height - brect.height;
		a->mode = def.colmode;
		if(v->area.size > 1)
			w = rect.width / ((float)rect.width / a->rect.width - 1);
		a->rect.width = w;
		cext_vattach(vector_of_areas(&v->area), a);
		v->sel = v->area.size - 1;
		return a;
	}
}

void
destroy_area(Area *a)
{
	unsigned int i;
	View *v = a->view;
	if(a->frame.size)
		return;
	if(a->frame.data)
		free(a->frame.data);
	if(v->revert == idx_of_area(a))
		v->revert = 0;
	for(i = 0; i < client.size; i++)
		if(client.data[i]->revert == a)
			client.data[i]->revert = 0;
	cext_vdetach(vector_of_areas(&v->area), a);
	if(v->sel > 1)
		v->sel--;
	free(a);
}

int
idx_of_area(Area *a)
{
	int i;
	View *v = a->view;
	for(i = 0; i < v->area.size; i++)
		if(v->area.data[i] == a)
			return i;
	return -1;
}

int
idx_of_area_id(View *v, unsigned short id)
{
	int i;
	for(i = 0; i < v->area.size; i++)
		if(v->area.data[i]->id == id)
			return i;
	return -1;
}

void
select_area(Area *a, char *arg)
{
	Area *new;
	View *v = a->view;
	int i = idx_of_area(a);

	if(i == -1)
		return;
	if(i)
		v->revert = i;

	if(!strncmp(arg, "toggle", 7)) {
		if(i)
			i = 0;
		else if(v->revert > 0 && v->revert < v->area.size)
			i = v->revert;
		else
			i = 1;
	} else if(!strncmp(arg, "prev", 5)) {
		if(i > 0) {
			if(i == 1)
				i = v->area.size - 1;
			else
				i--;
		}
	} else if(!strncmp(arg, "next", 5)) {
		if(i > 0) {
			if(i + 1 < v->area.size)
				i++;
			else
				i = 1;
		}
	}
	else {
		const char *errstr;
		i = cext_strtonum(arg, 0, v->area.size - 1, &errstr);
		if(errstr)
			return;
	}
	new = v->area.data[i];
	if(new->frame.size)
		focus_client(new->frame.data[new->sel]->client);
	v->sel = i;
	for(i = 0; i < a->frame.size; i++)
		draw_client(a->frame.data[i]->client);
}

void
send_to_area(Area *to, Area *from, Client *c)
{
	c->revert = from;
	detach_from_area(from, c);
	attach_to_area(to, c);
	focus_client(c);
}

void
place_client(Area *a, Client *c)
{
	static unsigned int mx, my;
	static Bool *field = nil;
	Bool fit = False;
	unsigned int i, j, k, x, y, maxx, maxy, dx, dy, cx, cy;
	XPoint p1 = {0, 0}, p2 = {0, 0};
	Frame *f = c->frame.data[c->sel];

	if(c->trans || f->rect.width >= a->rect.width
			|| f->rect.height >= a->rect.height)
		return;

	if(!field) {
		mx = rect.width / 8;
		my = rect.height / 8;
		field = cext_emallocz(my * mx * sizeof(Bool));
	}

	for(y = 0; y < my; y++)
		for(x = 0; x < mx; x++)
			field[y*mx + x] = True;

	dx = rect.width / mx;
	dy = rect.height / my;
	for(k = 0; k < a->frame.size; k++) {
		Frame *fr = a->frame.data[k];
		if(fr == f) {
			cx = f->rect.width / dx;
			cy = f->rect.height / dy;
			continue;
		}
		if(fr->rect.x < 0)
			x = 0;
		else
			x = fr->rect.x / dx;
		if(fr->rect.y < 0)
			y = 0;
		else
			y = fr->rect.y / dy;
		maxx = (fr->rect.x + fr->rect.width) / dx;
		maxy = (fr->rect.y + fr->rect.height) / dy;
		for(j = y; j < my && j < maxy; j++)
			for(i = x; i < mx && i < maxx; i++)
				field[j*mx + i] = False;
	}

	for(y = 0; y < my; y++)
		for(x = 0; x < mx; x++) {
			if(field[y*mx + x]) {
				for(i = x; (i < mx) && field[y*mx + i]; i++);
				for(j = y; (j < my) && field[j*mx + x]; j++);
				if(((i - x) * (j - y) > (p2.x - p1.x) * (p2.y - p1.y)) 
					&& (i - x > cx) && (j - y > cy))
				{
					fit = True;
					p1.x = x;
					p1.y = y;
					p2.x = i;
					p2.y = j;
				}
			}
		}

	if(fit) {
		p1.x *= dx;
		p1.y *= dy;
	}

	if(fit && (p1.x + f->rect.width < a->rect.x + a->rect.width))
		f->rect.x = p1.x;
	else 
		f->rect.x = a->rect.x + (random()%(a->rect.width - f->rect.width));

	if(fit && (p1.y + f->rect.height < a->rect.y + a->rect.height))
		f->rect.y = p1.y;
	else
		f->rect.y = a->rect.y + (random()%(a->rect.height - f->rect.height));
}

void
attach_to_area(Area *a, Client *c)
{
	unsigned int aidx = idx_of_area(a);
	Frame *f = create_frame(a, c);

	c->floating = !aidx;
	if(aidx) { /* column */
		if(a->frame.size > 1)
			f->rect.height = a->rect.height / (a->frame.size - 1);
		arrange_column(a, False);
	}
	else { /* floating */
		place_client(a, c);
		resize_client(c, &f->rect,  False);
	}
}

void
detach_from_area(Area *a, Client *c)
{
	View *v = a->view;
	int i;

	for(i = 0; i < c->frame.size; i++)
		if(c->frame.data[i]->area == a) {
			destroy_frame(c->frame.data[i]);
			break;
		}

	i = idx_of_area(a);
	if(i && a->frame.size)
		arrange_column(a, False);
	else {
		if(i) {
		    if(v->area.size > 2)
				destroy_area(a);
			else if(!a->frame.size && v->area.data[0]->frame.size)
				v->sel = 0; /* focus floating area if it contains something */
			arrange_view(v, True);
		}
		else if(!i && !a->frame.size) {
			if(c->trans) {
				/* focus area of transient, if possible */
				Client *cl = client_of_win(c->trans);
				if(cl && cl->frame.size) {
				   a = cl->frame.data[cl->sel]->area;
				   if(a->view == v)
					   v->sel = idx_of_area(a);
				}
			}
			else if(v->area.data[1]->frame.size)
				v->sel = 1; /* focus first col as fallback */
		}
	}
}

Bool
is_of_area(Area *a, Client *c)
{
	unsigned int i;
	for(i = 0; i < a->frame.size; i++)
		if(a->frame.data[i]->client == c)
			return True;
	return False;
}
