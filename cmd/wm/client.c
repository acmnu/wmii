/*
 * (C)opyright MMIV-MMVI Anselm R. Garbe <garbeam at gmail dot com>
 * See LICENSE file for license details.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>

#include "wm.h"

#define CLIENT_MASK		(StructureNotifyMask | PropertyChangeMask)

Client *
alloc_client(Window w, XWindowAttributes *wa)
{
	XTextProperty name;
	Client *c = (Client *) cext_emallocz(sizeof(Client));
	XSetWindowAttributes fwa;
	XClassHint ch;
	long msize;
	static unsigned int id = 1;

	c->id = id++;
	c->win = w;
	c->rect.x = wa->x;
	c->rect.y = wa->y;
	c->border = wa->border_width;
	c->rect.width = wa->width + 2 * c->border;
	c->rect.height = wa->height + 2 * c->border;
	XSetWindowBorderWidth(dpy, c->win, 0);
	c->proto = win_proto(c->win);
	XGetTransientForHint(dpy, c->win, &c->trans);
	if(!XGetWMNormalHints(dpy, c->win, &c->size, &msize) || !c->size.flags)
		c->size.flags = PSize;
	XAddToSaveSet(dpy, c->win);
	XGetWMName(dpy, c->win, &name);
	if(name.value) {
		cext_strlcpy(c->name, (char *)name.value, sizeof(c->name));
		free(name.value);
	}
	if(XGetClassHint(dpy, c->win, &ch)) {
		snprintf(c->classinst, sizeof(c->classinst), "%s:%s", ch.res_class,
				ch.res_name);
		XFree(ch.res_class);
		XFree(ch.res_name);
	}
	fwa.override_redirect = 1;
	fwa.background_pixmap = ParentRelative;
	fwa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask
		| ExposureMask | ButtonPressMask;

	c->framewin = XCreateWindow(dpy, root, c->rect.x, c->rect.y,
			c->rect.width + 2 * def.border,
			c->rect.height + def.border + bar_height(), 0,
			DefaultDepth(dpy, screen), CopyFromParent,
			DefaultVisual(dpy, screen),
			CWOverrideRedirect | CWBackPixmap | CWEventMask, &fwa);
	c->gc = XCreateGC(dpy, c->framewin, 0, 0);
	XSync(dpy, False);
	client = (Client **)cext_array_attach((void **)client, c,
			sizeof(Client *), &clientsz);
	nclient++;

	return c;
}

void
set_client_state(Client * c, int state)
{
	long data[2];

	data[0] = (long) state;
	data[1] = (long) None;
	XChangeProperty(dpy, c->win, wm_atom[WMState], wm_atom[WMState], 32,
			PropModeReplace, (unsigned char *) data, 2);
}

void
focus_client(Client *c)
{
	Client *old = sel_client();
	Frame *f = c->frame[c->sel];
	int i = area2index(f->area);

	f->area->view->sel = i;
	f->area->sel = frame2index(f);
	if(old && (old != c)) {
		grab_mouse(old->win, AnyModifier, Button1);
		draw_client(old);
	}
	ungrab_mouse(c->win, AnyModifier, AnyButton);
	grab_mouse(c->win, Mod1Mask, Button1);
	grab_mouse(c->win, Mod1Mask, Button3);

	restack_view(f->area->view);

	XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
	draw_client(c);
	XSync(dpy, False);
	if(i > 0 && f->area->mode == Colstack)
		arrange_area(f->area);
}

void
map_client(Client *c)
{
	XSelectInput(dpy, c->win, CLIENT_MASK & ~StructureNotifyMask);
	XMapWindow(dpy, c->win);
	XSelectInput(dpy, c->win, CLIENT_MASK);
	set_client_state(c, NormalState);
}

void
unmap_client(Client *c)
{
	ungrab_mouse(c->win, AnyModifier, AnyButton);
	XSelectInput(dpy, c->win, CLIENT_MASK & ~StructureNotifyMask);
	XUnmapWindow(dpy, c->win);
	XSelectInput(dpy, c->win, CLIENT_MASK);
	set_client_state(c, WithdrawnState);
}

void
reparent_client(Client *c, Window w, int x, int y)
{
	XSelectInput(dpy, c->win, CLIENT_MASK & ~StructureNotifyMask);
	XReparentWindow(dpy, c->win, w, x, y);
	XSelectInput(dpy, c->win, CLIENT_MASK);
}

void
configure_client(Client *c)
{
	XConfigureEvent e;
	Frame *f = c->frame[c->sel];
	e.type = ConfigureNotify;
	e.event = c->win;
	e.window = c->win;
	e.x = c->rect.x;
	e.y = c->rect.y;
	if(f) {
		e.x += f->rect.x;
		e.y += f->rect.y;
	}
	e.width = c->rect.width;
	e.height = c->rect.height;
	e.border_width = c->border;
	e.above = None;
	e.override_redirect = False;
	XSelectInput(dpy, c->win, CLIENT_MASK & ~StructureNotifyMask);
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *) & e);
	XSelectInput(dpy, c->win, CLIENT_MASK);
	XSync(dpy, False);
}

static void
send_client_message(Window w, Atom a, long value)
{
	XEvent e;
	e.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = a;
	e.xclient.format = 32;
	e.xclient.data.l[0] = value;
	e.xclient.data.l[1] = CurrentTime;

	XSendEvent(dpy, w, False, NoEventMask, &e);
	XSync(dpy, False);
}

void
kill_client(Client * c)
{
	if(c->proto & WM_PROTOCOL_DELWIN)
		send_client_message(c->win, wm_atom[WMProtocols], wm_atom[WMDelete]);
	else
		XKillClient(dpy, c->win);
}

void
update_client_property(Client *c, XPropertyEvent *e)
{
	XTextProperty name;
	long msize;

	if(e->atom == wm_atom[WMProtocols]) {
		/* update */
		c->proto = win_proto(c->win);
		return;
	}
	switch (e->atom) {
	case XA_WM_NAME:
		XGetWMName(dpy, c->win, &name);
		if(name.value) {
			cext_strlcpy(c->name, (char*) name.value, sizeof(c->name));
			free(name.value);
		}
		if(c->frame)
			draw_client(c);
		break;
	case XA_WM_TRANSIENT_FOR:
		XGetTransientForHint(dpy, c->win, &c->trans);
		break;
	case XA_WM_NORMAL_HINTS:
		if(!XGetWMNormalHints(dpy, c->win, &c->size, &msize) || !c->size.flags) {
			c->size.flags = PSize;
		}
		break;
	}
}

/* speed reasoned function for client property change */
void
draw_client(Client *c)
{
	Draw d = { 0 };
	char buf[512];

	if(!c->nframe)
		return; /* might not have been attached atm */

	d.align = WEST;
	d.drawable = c->framewin;
	d.font = xfont;
	d.gc = c->gc;

	if(c == sel_client())
		d.color = def.sel;
	else
		d.color = def.norm;

	/* draw border */
	if(def.border) {
		d.rect = c->frame[c->sel]->rect;
		d.rect.x = d.rect.y = 0;
		d.notch = &c->rect;
		blitz_drawlabel(dpy, &d);
		blitz_drawborder(dpy, &d);
	}
	d.rect.x = 0;
	d.rect.y = 0;
	d.rect.width = c->frame[c->sel]->rect.width;
	d.rect.height = bar_height();
	d.notch = nil;

	tags2str(buf, sizeof(buf), c->tag, c->ntag);
	cext_strlcat(buf, " | ", sizeof(buf));
	cext_strlcat(buf, c->name, sizeof(buf) - strlen(buf));
	d.data = buf;
	blitz_drawlabel(dpy, &d);
	blitz_drawborder(dpy, &d);
	XSync(dpy, False);
}

void
gravitate(Client *c, Bool invert)
{
	int dx = 0, dy = 0;
	int gravity = NorthWestGravity;

	if(c->size.flags & PWinGravity) {
		gravity = c->size.win_gravity;
	}

	/* y */
	switch (gravity) {
	case StaticGravity:
	case NorthWestGravity:
	case NorthGravity:
	case NorthEastGravity:
		dy = bar_height();
		break;
	case EastGravity:
	case CenterGravity:
	case WestGravity:
		dy = -(c->rect.height / 2) + bar_height();
		break;
	case SouthEastGravity:
	case SouthGravity:
	case SouthWestGravity:
		dy = -c->rect.height;
		break;
	default:
		break;
	}

	/* x */
	switch (gravity) {
	case StaticGravity:
	case NorthWestGravity:
	case WestGravity:
	case SouthWestGravity:
		dx = def.border;
		break;
	case NorthGravity:
	case CenterGravity:
	case SouthGravity:
		dx = -(c->rect.width / 2) + def.border;
		break;
	case NorthEastGravity:
	case EastGravity:
	case SouthEastGravity:
		dx = -(c->rect.width + def.border);
		break;
	default:
		break;
	}

	if(invert) {
		dx = -dx;
		dy = -dy;
	}
	c->rect.x += dx;
	c->rect.y += dy;
}

void
manage_client(Client *c)
{
	View *v;
	Client *trans;
	unsigned int i;

	if(c->trans && (trans = win2client(c->trans))) {
		for(i = 0; i < trans->ntag; i++)
			cext_strlcpy(c->tag[i], trans->tag[i], sizeof(c->tag[i]));
		c->ntag = trans->ntag;
	}
	else
		match_tags(c);

	reparent_client(c, c->framewin, c->rect.x, c->rect.y);

	v = nview ? view[sel] : alloc_view(def.tag);
	if(!c->ntag) {
		for(i = 0; i < v->ntag; i++) {
			cext_strlcpy(c->tag[i], v->tag[i], sizeof(c->tag[i]));
			c->ntag++;
		}
	}
	else if((c->ntag == 1) && !strncmp(c->tag[0], "~", 2)) {
		for(i = 0; i < v->ntag && i + 1 < MAX_TAGS; i++) {
			cext_strlcpy(c->tag[i + 1], v->tag[i], sizeof(c->tag[i + 1]));
			c->ntag++;
		}
	}

	update_tags();
}

static int
dummy_error_handler(Display *dpy, XErrorEvent *error)
{
	return 0;
}

void
destroy_client(Client *c)
{
	int i;
	Client *cl;

	XGrabServer(dpy);
	XSetErrorHandler(dummy_error_handler);

	for(i = 0; i < nview; i++)
		detach_fromview(view[i], c);

	unmap_client(c);

	if(c->nframe) {
		c->rect.x = c->frame[c->sel]->rect.x;
		c->rect.y = c->frame[c->sel]->rect.y;
	}

	reparent_client(c, root, c->rect.x, c->rect.y);
	XFreeGC(dpy, c->gc);
	XDestroyWindow(dpy, c->framewin);
	cext_array_detach((void **)client, c, &clientsz);
	nclient--;
	update_tags();
	free(c);

	if((cl = sel_client_of_view(view[sel])))
		focus_client(cl);

	XSync(dpy, False);
	XSetErrorHandler(wmii_error_handler);
	XUngrabServer(dpy);
}

Client *
sel_client()
{
	return nview ? sel_client_of_view(view[sel]) : nil;
}

static void
match_sizehints(Client *c)
{
	XSizeHints *s = &c->size;

	if(s->flags & PMinSize) {
		if(c->rect.width < c->size.min_width)
			c->rect.width = c->size.min_width;
		if(c->rect.height < c->size.min_height)
			c->rect.height = c->size.min_height;
	}
	if(s->flags & PMaxSize) {
		if(c->rect.width > c->size.max_width)
			c->rect.width = c->size.max_width;
		if(c->rect.height > c->size.max_height)
			c->rect.height = c->size.max_height;
	}

	if(s->flags & PResizeInc) {
		int w = 0, h = 0;

		if(c->size.flags & PBaseSize) {
			w = c->size.base_width;
			h = c->size.base_height;
		} else if(c->size.flags & PMinSize) {
			/* base_{width,height} default to min_{width,height} */
			w = c->size.min_width;
			h = c->size.min_height;
		}
		/* client_width = base_width + i * c->size.width_inc for an integer i */
		w = c->frame[c->sel]->rect.width - 2 * def.border - w;
		if(s->width_inc > 0)
			c->frame[c->sel]->rect.width -= w % s->width_inc;

		h = c->frame[c->sel]->rect.height - def.border - bar_height() - h;
		if(s->height_inc > 0)
			c->frame[c->sel]->rect.height -= h % s->height_inc;
	}
}

void
resize_client(Client *c, XRectangle *r, Bool ignore_xcall)
{
	Frame *f = c->frame[c->sel];
	f->rect = *r;

	if((f->area->mode != Colstack) || (f->area->sel == frame2index(f)))
		match_sizehints(c);

	if(!ignore_xcall) {
		if(f->area->view == view[sel])
			XMoveResizeWindow(dpy, c->framewin, f->rect.x,
					f->rect.y, f->rect.width, f->rect.height);
		else
			XMoveResizeWindow(dpy, c->framewin, 2 * rect.width + f->rect.x,
					f->rect.y, f->rect.width, f->rect.height);
	}

	if((f->area->mode != Colstack) || (f->area->sel == frame2index(f))) {
		c->rect.x = def.border;
		c->rect.y = bar_height();
		c->rect.width = f->rect.width - 2 * def.border;
		c->rect.height = f->rect.height - def.border - bar_height();
		XMoveResizeWindow(dpy, c->win, c->rect.x, c->rect.y,
				c->rect.width, c->rect.height);
		configure_client(c);
	}
}

void
select_client(Client *c, char *arg)
{
	Frame *f = c->frame[c->sel];
	Area *a = f->area;
	int i = frame2index(f);
	if(i == -1)
		return;
	if(!strncmp(arg, "prev", 5)) {
		if(!i)
			i = a->nframe - 1;
		else
			i--;
	} else if(!strncmp(arg, "next", 5)) {
		if(i + 1 < a->nframe)
			i++;
		else
			i = 0;
	}
	else {
		const char *errstr;
		i = cext_strtonum(arg, 0, a->nframe - 1, &errstr);
		if(errstr)
			return;
	}
	focus_client(a->frame[i]->client);
}

void
send2area_client(Client *c, char *arg)
{
	const char *errstr;
	Frame *f = c->frame[c->sel];
	Area *to, *a = f->area;
	View *v = a->view;
	int i = area2index(a);

	if(i == -1)
		return;
	if(!strncmp(arg, "new", 4) && i) {
		if(a->nframe == 1)
			return;
		to = alloc_area(v);
		arrange_tag(v, True);
	}
	else if(!strncmp(arg, "prev", 5) && i) {
		if(i == 1)
			to = v->area[v->narea - 1];
		else
			to = v->area[i - 1];
	}
	else if(!strncmp(arg, "next", 5) && i) {
		if(i < v->narea - 1)
			to = v->area[i + 1];
		else
			to = v->area[1];
	}
	else if(!strncmp(arg, "toggle", 7)) {
		if(i)
			to = v->area[0];
		else if(c->revert && c->revert != v->area[0])
			to = c->revert;
		else
			to = v->area[1];
	}
	else {
		i = cext_strtonum(arg, 0, v->narea - 1, &errstr);
		if(errstr)
			return;
		to = v->area[i];
	}
	send2area(to, a, c);
}

void
resize_all_clients()
{
	unsigned int i;
	for(i = 0; i < nclient; i++) {
		Client *c = client[i];
		if(c->nframe && c->frame[c->sel]->area) {
			if(area2index(c->frame[c->sel]->area))
				resize_area(c, &c->frame[c->sel]->rect, nil);
			else
				resize_client(c, &c->frame[c->sel]->rect, False);
		}
	}
}

/* convenience function */
void
focus(Client *c)
{
	Frame *f = c->nframe ? c->frame[c->sel] : nil;
	View *v;

	if(!f)
		return;

	v = f->area->view;
	if(view[sel] != v)
		focus_view(v);
	focus_client(c);
}

int
cid2index(unsigned short id)
{
	int i;
	for(i = 0; i < nclient; i++)
		if(client[i]->id == id)
			return i;
	return -1;
}

Bool
clienthastag(Client *c, const char *t)
{
	unsigned int i;
	for(i = 0; i < c->ntag; i++)
		if(!strncmp(c->tag[i], t, sizeof(c->tag[i])))
			return True;
	return False;
}
