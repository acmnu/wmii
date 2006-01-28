/*
 * (C)opyright MMIV-MMVI Anselm R. Garbe <garbeam at gmail dot com>
 * See LICENSE file for license details.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <sys/socket.h>

#include "../libixp2/ixp.h"
#include "blitz.h"

/*
 * filesystem specification
 * / 					Droot
 * /display				Fdisplay	'north', 'south', 'none'
 * /font				Ffont		<xlib font name>
 * /new					Fnew 		returns id of new item
 * /event				Fevent
 * /default/ 			Ditem
 * /default/color		Fcolor		<#RRGGBB> <#RRGGBB> <#RRGGBB>
 * /1/					Ditem
 * /1/data 				Fdata		<arbitrary data which gets displayed>
 * /1/color				Fcolor		<#RRGGBB> <#RRGGBB> <#RRGGBB>
 */

/* 8-bit qid.path.type */
enum {                          
    Droot,
    Ditem,
    Fdisplay,
    Ffont,
    Fevent,
    Fdata,                      /* data to display */
    Fcolor
};

typedef struct {
    char data[256];
	char color[24];
    unsigned long bg;
    unsigned long fg;
    unsigned long border;
} Item;

static unsigned char *msg[IXP_MAX_MSG];
char *errstr = 0;
static size_t nitem = 0;
static size_t itemsz = 0;
static Item **item = 0;
static char *address = nil;
static pid_t mypid = 0;
static IXPServer srv = { 0 };
static Qid root_qid;
static Display *dpy;
static int screen_num;
static char *font = nil;
static Align align = SOUTH;
/*
static XFontStruct *xfont;
static GC gc;
static Window win;
static XRectangle geom;
static Pixm pm;

static Draw zero_draw = { 0 };
*/

static char *version[] = {
    "wmiibar - window manager improved bar - " VERSION "\n"
        "  (C)opyright MMIV-MMVI Anselm R. Garbe\n", 0
};

static void
usage()
{
    fprintf(stderr, "%s %d",
            "usage: wmiibar -a <server address> [-v]\n"
            "      -a    server address \n"
            "      -v    version info\n", 0);
    exit(1);
}

static int
dummy_error_handler(Display * dpy, XErrorEvent * err)
{
    return 0;
}

static void
exit_cleanup()
{
    if(mypid == getpid())
        unlink(address);
}

static void
new_item()
{
	size_t i;
	if(!item) {
		itemsz = 2;
		item = cext_emallocz(sizeof(Item *) * itemsz);
	}
	if(nitem + 1 >= itemsz) {
		Item **tmp = item;
		itemsz *= 2;
		item = cext_emallocz(sizeof(Item *) * itemsz);
		for(i = 0; tmp[i]; i++)
			item[i] = tmp[i];
		free(tmp);
	}
	item[nitem++] = cext_emallocz(sizeof(Item));
	if(nitem > 1)
		cext_strlcpy(item[nitem - 1]->color, item[0]->color,
					 sizeof(item[nitem - 1]->color));
}

void
detach_item(Item *it)
{
	size_t i;
	for(i = 0; item[i] != it; i++);
	for(; item[i + 1]; i++)
		item[i] = item[i + 1];
	item[i] = nil;
	nitem--;
}

static unsigned long long
mkqpath(unsigned char type, unsigned short item)
{
    return ((unsigned long long) item << 8) | (unsigned long long) type;
}

static unsigned char
qpath_type(unsigned long long path)
{
    return path & 0xff;
}

static unsigned short
qpath_item(unsigned long long path)
{
    return (path >> 8) & 0xffff;
}

static char *
qid_to_name(Qid *qid)
{
	unsigned char type = qpath_type(qid->path);
	unsigned short i = qpath_item(qid->path);
	static char buf[32];

	switch(type) {
		case Droot: return "/"; break;
		case Ditem:
			if(!i) 
				return "default";
			else if(i == nitem)
				return "new";
			snprintf(buf, sizeof(buf), "%u", i);
			return buf;
			break;
		case Fdisplay: return "display"; break;
		case Ffont: return "font"; break;
		case Fdata: return "data"; break;
		case Fevent: return "event"; break;
		case Fcolor: return "color"; break;
		default: return nil; break;
	}
}

static int
name_to_type(char *name)
{
	const char *err;
    unsigned int i;
	if(!name || !name[0] || !strncmp(name, "..", 3))
		return Droot;
	if(!strncmp(name, "default", 8) || !strncmp(name, "new", 4))
		return Ditem;
	if(!strncmp(name, "display", 8))
		return Fdisplay;
	if(!strncmp(name, "font", 5))
		return Ffont;
	if(!strncmp(name, "data", 5))
		return Fdata;
	if(!strncmp(name, "event", 6))
		return Fevent;
	if(!strncmp(name, "color", 6))
		return Fcolor;
   	i = (unsigned short) cext_strtonum(name, 1, 0xffff, &err);
    if(!err && (i <= nitem))
		return Ditem;
	return -1;
}

static int
mkqid(Qid *dir, char *wname, Qid *new)
{
	const char *err;
	int type = name_to_type(wname);
	unsigned short i = qpath_item(dir->path);

    if((dir->type != IXP_QTDIR) || (type == -1))
        return -1;
	
    new->version = 0;
	switch(type) {
	case Droot:
		new->type = IXP_QTDIR;
		*new = root_qid;
		break;
	case Ditem:
		new->type = IXP_QTDIR;
		if(!strncmp(wname, "default", 8))
			new->path = mkqpath(Ditem, 0);
		else if(!strncmp(wname, "new", 4))
			new->path = mkqpath(Ditem, nitem);
		else {
			unsigned short i = cext_strtonum(wname, 1, 0xffff, &err);
			if(err || (i >= nitem))
				return -1;
			new->path = mkqpath(Ditem, i);
		}
		break;
	case Fdata: /* note, Fdata needs to be before Fcolor, fallthrough */
		if(!i)
			return -1;
	case Fcolor:
		if(i > nitem)
			return -1;
	default:
		new->type = IXP_QTFILE;
    	new->path = mkqpath(type, i);
		break;
	}
    return 0;
}

static int
xversion(IXPReq *r)
{
    if(strncmp(r->fcall->version, IXP_VERSION, strlen(IXP_VERSION))) {
        errstr = "9P versions differ";
        return -1;
    } else if(r->fcall->maxmsg > IXP_MAX_MSG)
        r->fcall->maxmsg = IXP_MAX_MSG;
    r->fcall->id = RVERSION;
    return 0;
}

static int
xattach(IXPReq *r)
{
    IXPMap *new = cext_emallocz(sizeof(IXPMap));
    new->qid = root_qid;
    new->fid = r->fcall->fid;
	r->map = ixp_server_attach_map(new, r->map, &r->mapsz);
    r->fcall->id = RATTACH;
    r->fcall->qid = root_qid;
    return 0;
}

static int
xwalk(IXPReq *r)
{
    unsigned short nwqid = 0;
    Qid dir = root_qid;
    IXPMap *m;

    if(!(m = ixp_server_fid2map(r, r->fcall->fid))) {
        errstr = "no dir associated with fid";
        return -1;
    }
    if(r->fcall->fid != r->fcall->newfid
       && (ixp_server_fid2map(r, r->fcall->newfid))) {
        errstr = "fid alreay in use";
        return -1;
    }
    if(r->fcall->nwname) {
        dir = m->qid;
        for(nwqid = 0; (nwqid < r->fcall->nwname)
            && !mkqid(&dir, r->fcall->wname[nwqid], &r->fcall->wqid[nwqid]); nwqid++)
            dir = r->fcall->wqid[nwqid];
        if(!nwqid) {
            errstr = "file not found";
            return -1;
        }
    }
    /* a fid will only be valid, if the walk was complete */
    if(nwqid == r->fcall->nwname) {
        if(r->fcall->fid != r->fcall->newfid) {
			m = cext_emallocz(sizeof(IXPMap));
			r->map = ixp_server_attach_map(m, r->map, &r->mapsz);
        }
        m->qid = dir;
        m->fid = r->fcall->newfid;
    }
    r->fcall->id = RWALK;
    r->fcall->nwqid = nwqid;
    return 0;
}

static int
xopen(IXPReq *r)
{
    IXPMap *m = ixp_server_fid2map(r, r->fcall->fid);

    if(!m) {
        errstr = "invalid fid";
        return -1;
    }
    if(!(r->fcall->mode | IXP_OREAD) && !(r->fcall->mode | IXP_OWRITE)) {
		fprintf(stderr, "got mode 0x%x\n", r->fcall->mode);
        errstr = "mode not supported";
        return -1;
    }
    r->fcall->id = ROPEN;
    r->fcall->qid = m->qid;
    r->fcall->iounit =
        r->fcall->maxmsg - (sizeof(unsigned char) + sizeof(unsigned short) + 2 * sizeof(unsigned int));
    return 0;
}

static unsigned int
mkstat(Stat *stat, Qid *dir, char *name, unsigned long long length, unsigned int mode)
{
    stat->mode = mode;
    stat->atime = stat->mtime = time(0);
    cext_strlcpy(stat->uid, getenv("USER"), sizeof(stat->uid));
    cext_strlcpy(stat->gid, getenv("USER"), sizeof(stat->gid));
    cext_strlcpy(stat->muid, getenv("USER"), sizeof(stat->muid));

    cext_strlcpy(stat->name, name, sizeof(stat->name));
    stat->length = length;
    mkqid(dir, name, &stat->qid);
	return ixp_sizeof_stat(stat);
}

static int
xremove(IXPReq *r)
{
    IXPMap *m = ixp_server_fid2map(r, r->fcall->fid);
	unsigned short i;

    if(!m) {
        errstr = "invalid fid";
        return -1;
    }
	i = qpath_item(m->qid.path);
	if((qpath_type(m->qid.path) == Ditem) && i && (i < nitem)) {
		Item *it = item[i];
		detach_item(it);
		free(it);
    	r->fcall->id = RREMOVE;
		return 0;
	}
	errstr = "permission denied";
	return -1;
}

static int
xread(IXPReq *r)
{
	Stat stat;
    IXPMap *m = ixp_server_fid2map(r, r->fcall->fid);
    unsigned char *p = r->fcall->data;
	unsigned short i;
	char buf[32];

    if(!m) {
        errstr = "invalid fid";
        return -1;
    }
	i = qpath_item(m->qid.path);
	r->fcall->count = 0; /* EOF by default */
	if(!r->fcall->offset) {
		switch (qpath_type(m->qid.path)) {
		case Droot:
			if(align == SOUTH || align == NORTH)
				r->fcall->count = mkstat(&stat, &root_qid, "display", 6, DMREAD | DMWRITE);
			else
				r->fcall->count = mkstat(&stat, &root_qid, "display", 5, DMREAD | DMWRITE); /* none */
			p = ixp_enc_stat(p, &stat);
			r->fcall->count += mkstat(&stat, &root_qid, "font", strlen(font), DMREAD | DMWRITE);
			p = ixp_enc_stat(p, &stat);
			r->fcall->count += mkstat(&stat, &root_qid, "new", 0, DMDIR | DMREAD | DMEXEC);
			p = ixp_enc_stat(p, &stat);
			r->fcall->count += mkstat(&stat, &root_qid, "event", 0, DMREAD);
			p = ixp_enc_stat(p, &stat);
			r->fcall->count += mkstat(&stat, &root_qid, "default", 0, DMDIR | DMREAD | DMEXEC);
			p = ixp_enc_stat(p, &stat);
			for(i = 1; i < nitem; i++) {
				snprintf(buf, sizeof(buf), "%u", i);
				r->fcall->count += mkstat(&stat, &root_qid, buf, 0, DMDIR | DMREAD | DMEXEC);
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case Ditem:
			if(i > nitem)
				goto error_xread;
			if(!i) {
				r->fcall->count = mkstat(&stat, &root_qid, "color", strlen(item[0]->color), DMREAD | DMWRITE);
				p = ixp_enc_stat(p, &stat);
				break;
			}
			{
				if(i == nitem)
					new_item();
				Qid dir = {IXP_QTDIR, 0, mkqpath(Ditem, i)};
				r->fcall->count = mkstat(&stat, &dir, "color", strlen(item[i]->color), DMREAD | DMWRITE);
				p = ixp_enc_stat(p, &stat);
				r->fcall->count += mkstat(&stat, &dir, "data", strlen(item[i]->data), DMREAD | DMWRITE);
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case Fdisplay:
			switch(align) {
			case SOUTH:
				memcpy(p, "south", 5);
				r->fcall->count = 5;
				break;
			case NORTH:
				memcpy(p, "north", 5);
				r->fcall->count = 5;
				break;
			default:
				memcpy(p, "none", 4);
				r->fcall->count = 4;
				break;
			}
			break;
		case Ffont:
			if((r->fcall->count = strlen(font)))
				memcpy(p, font, r->fcall->count);
			break;
		case Fevent:
			/* has to be processed asynchroneous, will be enqueued */
			return 1;
			break;
		case Fdata:
			if(i == nitem)
				new_item();
			if(i >= nitem)
				goto error_xread;
			if((r->fcall->count = strlen(item[i]->data)))
				memcpy(p, item[i]->data, r->fcall->count);
			break;
		case Fcolor:
			if(i == nitem)
				new_item();
			if(i >= nitem)
				goto error_xread;
			if((r->fcall->count = strlen(item[i]->color)))
				memcpy(p, item[i]->color, r->fcall->count);
			break;
		default:
error_xread:
            errstr = "invalid read request";
			return -1;
			break;
		}
	}
    r->fcall->id = RREAD;
    return 0;
}

static int
xstat(IXPReq *r)
{
    IXPMap *m = ixp_server_fid2map(r, r->fcall->fid);
	unsigned short i;
	Qid dir;

    if(!m) {
        errstr = "invalid fid";
        return -1;
    }
   	i = qpath_item(m->qid.path);
   	dir.version = 0;
	dir.type = IXP_QTDIR;
	dir.path = mkqpath(Ditem, i);

    switch (qpath_type(m->qid.path)) {
    case Droot:
    case Ditem:
		mkstat(&r->fcall->stat, &root_qid, qid_to_name(&m->qid), 0, DMDIR | DMREAD | DMEXEC);
        break;
	case Fdisplay:
		if(align == SOUTH || align == NORTH)
			mkstat(&r->fcall->stat, &root_qid, qid_to_name(&m->qid), 6, DMREAD | DMWRITE);
		else
			mkstat(&r->fcall->stat, &root_qid, qid_to_name(&m->qid), 5, DMREAD | DMWRITE);
		break;
    case Fevent:
		mkstat(&r->fcall->stat, &root_qid, qid_to_name(&m->qid), 0, DMREAD);
		break;
    case Ffont:
		mkstat(&r->fcall->stat, &root_qid, qid_to_name(&m->qid), strlen(font), DMREAD | DMWRITE);
        break;
    case Fdata:
		if(i == nitem)
			i = 0;
		mkstat(&r->fcall->stat, &dir, qid_to_name(&m->qid), strlen(item[i]->data), DMREAD | DMWRITE);
		break;	
    case Fcolor:
		if(i == nitem)
			i = 0;
		mkstat(&r->fcall->stat, &dir, qid_to_name(&m->qid), strlen(item[i]->color), DMREAD | DMWRITE);
		break;
    default:
		errstr = "invalid stat request";
		return -1;
		break;
    }
    r->fcall->id = RSTAT;
    return 0;
}

static int
xwrite(IXPReq *r)
{
	char buf[256];
    IXPMap *m = ixp_server_fid2map(r, r->fcall->fid);
	unsigned short i;

    if(!m) {
        errstr = "invalid fid";
        return -1;
    }

	i = qpath_item(m->qid.path);
	switch (qpath_type(m->qid.path)) {
	case Fdisplay:
		if(r->fcall->count > 5)
			goto error_xwrite;
		memcpy(buf, r->fcall->data, r->fcall->count);
		buf[r->fcall->count] = 0;
		if(!blitz_strtoalign(&align, buf))
			goto error_xwrite;
		/* TODO: resize/hide */
		break;
	case Ffont:
		if(font)
			free(font);
		font = cext_emallocz(r->fcall->count + 1);
		memcpy(font, r->fcall->data, r->fcall->count);
		/* TODO: XQueryFont */
		break;
	case Fdata:
		{
			unsigned int len = r->fcall->count;
			if(i == nitem)
				new_item();
			if(!i || (i >= nitem))
				goto error_xwrite;
			if(len >= sizeof(item[i]->data))
				len = sizeof(item[i]->data) - 1;
			memcpy(item[i]->data, r->fcall->data, len);
			item[i]->data[len] = 0;
			/* TODO: redraw */
		}
		break;
	case Fcolor:
		if(i == nitem)
			new_item();
		if((i >= nitem) || (r->fcall->count != 24))
			goto error_xwrite;
		if(r->fcall->data[0] != '#' || r->fcall->data[8] != '#' || r->fcall->data[16] != '#')
			goto error_xwrite;
		memcpy(item[i]->color, r->fcall->data, r->fcall->count);
		item[i]->color[r->fcall->count] = 0;
		/* TODO: update color */
		break;
	default:
error_xwrite:
		errstr = "invalid write request";
		return -1;
		break;
	}
    r->fcall->id = RWRITE;
	return 0;
}

static int
xclunk(IXPReq *r)
{
    IXPMap *m = ixp_server_fid2map(r, r->fcall->fid);

    if(!m) {
        errstr = "invalid fid";
        return -1;
    }
	ixp_server_detach_map(m, r->map);
    free(m);
    r->fcall->id = RCLUNK;
    return 0;
}

static int 
doixp(IXPReq *r)
{
	switch(r->fcall->id) {
	case TVERSION: return xversion(r); break;
	case TATTACH: return xattach(r); break;
	case TWALK: return xwalk(r); break;
	case TREMOVE: return xremove(r); break;
	case TOPEN: return xopen(r); break;
	case TREAD: return xread(r); break;
	case TWRITE: return xwrite(r); break;
	case TCLUNK: return xclunk(r); break;
	case TSTAT: return xstat(r); break;
	default:
		break;
	}
	return -1;
}

static void
handle_ixp_req(IXPServer *s, IXPConn *c)
{
	IXPReq *r = c->aux;
    unsigned int msize;
	int ret = -1;
    errstr = 0;
	if(!(msize = ixp_recv_message(c->fd, msg, IXP_MAX_MSG, &errstr))) {
		ixp_server_free_conn(s, c);
		return;
	}
	if(!r->fcall)
		r->fcall = cext_emallocz(sizeof(Fcall));
    if((msize = ixp_msg_to_fcall(msg, IXP_MAX_MSG, r->fcall)))
		ret = doixp(r);
	if(ret == -1) {
		if(!errstr)
			errstr = "function not supported";
		r->fcall->id = RERROR;
		cext_strlcpy(r->fcall->errstr, errstr, sizeof(r->fcall->errstr));
	}
	else if(ret == 1) {
		r->async = ixp_server_attach_fcall(r->fcall, r->async, &r->asyncsz);
		r->fcall = nil;
		return;	 /* response asynchroneously */
	}
	msize = ixp_fcall_to_msg(r->fcall, msg, IXP_MAX_MSG);
	if(ixp_send_message(c->fd, msg, msize, &errstr) != msize)
		ixp_server_free_conn(s, c);
}

static void
new_ixp_conn(IXPServer *s, IXPConn *c)
{
    IXPConn *new = ixp_server_alloc_conn(s);
    if(new && ((new->fd = ixp_accept_sock(c->fd)) >= 0)) {
		new->read = handle_ixp_req;
		new->close = ixp_server_close_conn;
		new->aux = cext_emallocz(sizeof(IXPReq));
	}
}

int
main(int argc, char *argv[])
{
    int i;
	IXPConn *c;

    /* command line args */
    for(i = 1; (i < argc) && (argv[i][0] == '-'); i++) {
        switch (argv[i][1]) {
        case 'v':
            fprintf(stdout, "%s", version[0]);
            exit(0);
            break;
        case 'a':
            if(i + 1 < argc)
                address = argv[++i];
            else
                usage();
            break;
        default:
            usage();
            break;
        }
    }

    dpy = XOpenDisplay(0);
    if(!dpy) {
        fprintf(stderr, "%s", "wmiibar: cannot open display\n");
        exit(1);
    }
    XSetErrorHandler(dummy_error_handler);
    screen_num = DefaultScreen(dpy);

    if(!address) {
		fprintf(stderr, "%s\n", "wmiibar: no socket address provided");
		exit(1);
	}
    ixp_server_init(&srv);
	c = ixp_server_alloc_conn(&srv);
	if((c->fd = ixp_create_sock(address, &errstr)) < 0) {
        fprintf(stderr, "wmiibar: fatal: %s\n", errstr);
		exit(1);
	}
	c->read = new_ixp_conn;
	c->close = ixp_server_close_conn;
	/* TODO: add X conn */
	
    root_qid.type = IXP_QTDIR;
    root_qid.version = 0;
    root_qid.path = mkqpath(Droot, 0);

    mypid = getpid();
    atexit(exit_cleanup);

    /* default item settings */
	new_item();
	cext_strlcpy(item[0]->color, "#000000 #ffffff #ffffff", sizeof(item[0]->color));

    font = strdup("fixed");

    if((errstr = ixp_server_loop(&srv))) {
        fprintf(stderr, "wmiibar: fatal: %s\n", errstr);
        ixp_server_deinit(&srv);
        exit(1);
    }
    ixp_server_deinit(&srv);
    return 0;
}
