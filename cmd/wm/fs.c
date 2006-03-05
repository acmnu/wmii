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
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <sys/socket.h>

#include "wm.h"

static char E9pversion[] = "9P version not supported";
static char Enoperm[] = "permission denied";
static char Enofid[] = "fid not found";
static char Enofile[] = "file not found";
static char Enomode[] = "mode not supported";
static char Enofunc[] = "function not supported";
static char Enocommand[] = "command not supported";

#define WMII_IOUNIT 	2048

/*
 * filesystem specification
 * / 					FsDroot
 * /def/				FsDdef
 * /def/border			FsFborder		0..n
 * /def/snap 			FsFsnap  		0..n
 * /def/font			FsFfont  		xlib font name
 * /def/selcolors		FsFselcolors	sel color
 * /def/normcolors		FsFnormcolors 	normal colors
 * /keys/				FsDkeys
 * /keys/foo			FsFkey
 * /tags/				FsDtags
 * /tags/foo			FsFtag
 * /bar/				FsDbar
 * /bar/expand			FsFexpand 		id of expandable label
 * /bar/new/			FsDlabel
 * /bar/1/				FsDlabel
 * /bar/1/data 			FsFdata			<arbitrary data which gets displayed>
 * /bar/1/colors		FsFcolors		<#RRGGBB> <#RRGGBB> <#RRGGBB>
 * /event				FsFevent
 * /ctl					FsFctl 			command interface (root)
 * /ws/				    FsDws			returns new tag
 * /ws/					FsDws			tag
 * /ws/ctl				FsFctl			command interface (tag)
 * /ws/sel/				FsDarea
 * /ws/float/			FsDarea			floating clients in area 0
 * /ws/float/ctl 		FsFctl			command interface (area)
 * /ws/float/mode		FsFmode			col mode
 * /ws/float/sel/		FsDclient
 * /ws/float/1/			FsDclient
 * /ws/float/1/name		FsFname			name of client
 * /ws/float/1/tags		FsFtags			tags of client
 * /ws/float/1/geom		FsFgeom			geometry of client
 * /ws/float/1/ctl 		FsFctl 			command interface (client)
 * /ws/1/				FsDarea
 * /ws/1/ctl 			FsFctl 			command interface (area)
 * /ws/1/mode			FsFmode			col mode
 * /ws/1/1/sel/			FsDclient
 * /ws/1/1/1/			FsDclient
 * /ws/1/1/name			FsFname			name of client
 * /ws/1/1/tags			FsFtags			tag of client
 * /ws/1/1/geom			FsFgeom			geometry of client
 * /ws/1/1/ctl 			FsFctl 			command interface (client)
 */

Qid root_qid;
const char *err;

/* IXP stuff */

/**
 * Qid->path is calculated related to the index of the associated structure.
 * i1 is associated to tag, key or label
 * i2 is associated to area
 * i3 is associated to client
 * ie /sel/sel/ctl is i1id = sel tag id, i2id = sel area id , i3id = 0 (no id)
 */
unsigned long long
mkqpath(unsigned char type, unsigned short i1id, unsigned short i2id, unsigned short i3id)
{
    return ((unsigned long long) type << 48) | ((unsigned long long) i1id << 32)
		| ((unsigned long long) i2id << 16) | (unsigned long long) i3id;
}

static unsigned char
qpath_type(unsigned long long path)
{
    return (path >> 48) & 0xff;
}

static unsigned short
qpath_i1id(unsigned long long path)
{
    return (path >> 32) & 0xffff;
}

static unsigned short
qpath_i2id(unsigned long long path)
{
    return (path >> 16) & 0xffff;
}

static unsigned short
qpath_i3id(unsigned long long path)
{
    return path & 0xffff;
}

static void
decode_qpath(Qid *qid, unsigned char *type, int *i1, int *i2, int *i3)
{
	unsigned short i1id = qpath_i1id(qid->path);
	unsigned short i2id = qpath_i2id(qid->path);
	unsigned short i3id = qpath_i3id(qid->path);
	*type = qpath_type(qid->path);

	if(i1id) {
		switch(*type) {
			case FsFkey: *i1 = kid2index(i1id); break;
			case FsFdata:
			case FsFcolors:
			case FsDlabel: *i1 = lid2index(i1id); break;
			default: *i1 = tid2index(i1id); break;
		}
		if(i2id && (*i1 != -1)) {
			*i2 = aid2index(tag[*i1], i2id);
			if(i3id && (*i2 != -1))
				*i3 = frid2index(tag[*i1]->area[*i2], i3id);
		}
	}
}

static char *
qid2name(Qid *qid)
{
	unsigned char type;
	int i1 = -1, i2 = -1, i3 = -1;
	static char buf[32];

	decode_qpath(qid, &type, &i1, &i2, &i3);

	switch(type) {
		case FsDroot: return "/"; break;
		case FsDdef: return "def"; break;
		case FsDkeys: return "keys"; break;
		case FsDtags: return "tags"; break;
		case FsDbar: return "bar"; break;
		case FsDws:
			if(i1 == -1)
				return nil;
			if(i1 == sel)
				return "sel";
			snprintf(buf, sizeof(buf), "%u", i1);
			return buf;
			break;
		case FsDlabel:
			if(i1 == -1)
				return nil;
			snprintf(buf, sizeof(buf), "%u", i1);
			return buf;
			break;
		case FsDarea:
			if(i1 == -1 || i2 == -1)
				return nil;
			if(!i2) {
				if(i2 == tag[i1]->sel)
					return "sel";
				else
					return "float";
			}
			if(tag[i1]->sel == i2)
				return "sel";
			snprintf(buf, sizeof(buf), "%u", i2);
			return buf;
			break;
		case FsDclient:
			if(i1 == -1 || i2 == -1 || i3 == -1)
				return nil;
			if(tag[i1]->area[i2]->sel == i3)
				return "sel";
			snprintf(buf, sizeof(buf), "%u", i3);
			return buf;
			break;
		case FsFselcolors: return "selcolors"; break;
		case FsFnormcolors: return "normcolors"; break;
		case FsFfont: return "font"; break;
		case FsFcolors: return "colors"; break;
		case FsFdata:
			if(i1 == -1)
				return nil;
			return "data";
			break;
		case FsFexpand:
			if(i1 == -1)
				return nil;
			return "expand";
			break;
		case FsFctl: return "ctl"; break;
		case FsFborder: return "border"; break;
		case FsFsnap: return "snap"; break;
		case FsFgeom:
			if(i1 == -1 || i2 == -1 || i3 == -1)
				return nil;
			return "geom";
			break;
		case FsFname:
			if(i1 == -1 || i2 == -1 || i3 == -1)
				return nil;
			return "name";
			break;
		case FsFtags:
			if(i1 == -1 || i2 == -1 || i3 == -1)
				return nil;
		 	return "tags";
			break;
		case FsFmode:
			if(i1 == -1 || i2 == -1)
				return nil;
			return "mode";
			break;
		case FsFevent: return "event"; break;
		case FsFkey:
			if(i1 == -1)
				return nil;
		 	return key[i1]->name;
			break; 
		default: return nil; break;
	}
}

static int
name2type(char *name, unsigned char dir_type)
{
    unsigned int i;
	if(!name || !name[0] || !strncmp(name, "/", 2) || !strncmp(name, "..", 3))
		return FsDroot;
	if(!strncmp(name, "new", 4)) {
		switch(dir_type) {
		case FsDroot: return FsDws; break;
		case FsDbar: 	return FsDlabel; break;
		}
	}
	if(!strncmp(name, "tags", 5)) {
		switch(dir_type) {	
		case FsDroot: return FsDtags; break;
		case FsDclient: return FsFtags; break;
		}
	}
	if(!strncmp(name, "bar", 4))
		return FsDbar;
	if(!strncmp(name, "def", 4))
		return FsDdef;
	if(!strncmp(name, "keys", 5))
		return FsDkeys;
	if(!strncmp(name, "ctl", 4))
		return FsFctl;
	if(!strncmp(name, "event", 6))
		return FsFevent;
	if(!strncmp(name, "snap", 5))
		return FsFsnap;
	if(!strncmp(name, "name", 5))
		return FsFname;
	if(!strncmp(name, "border", 7))
		return FsFborder;
	if(!strncmp(name, "geom", 5))
		return FsFgeom;
	if(!strncmp(name, "expand", 7))
		return FsFexpand;
	if(!strncmp(name, "colors", 7))
		return FsFcolors;
	if(!strncmp(name, "selcolors", 10))
		return FsFselcolors;
	if(!strncmp(name, "normcolors", 11))
		return FsFnormcolors;
	if(!strncmp(name, "font", 5))
		return FsFfont;
	if(!strncmp(name, "data", 5))
		return FsFdata;
	if(!strncmp(name, "mode", 5))
		return FsFmode;
	if(name2key(name))
		return FsFkey;
	if(has_ctag(name))
		return FsFtags;
	if(!strncmp(name, "sel", 4))
		goto dyndir;
   	i = (unsigned short) cext_strtonum(name, 0, 0xffff, &err);
    if(err)
		return -1;
dyndir:
	/*fprintf(stderr, "nametotype: dir_type = %d\n", dir_type);*/
	switch(dir_type) {
	case FsDroot: return FsDws; break;
	case FsDbar: return FsDlabel; break;
	case FsDws: return FsDarea; break;
	case FsDarea: return FsDclient; break;
	}
	return -1;
}

static int
mkqid(Qid *dir, char *wname, Qid *new, Bool iswalk)
{
	unsigned char dir_type;
	int dir_i1 = -1, dir_i2 = -1, dir_i3 = -1;
	int type, i;

	decode_qpath(dir, &dir_type, &dir_i1, &dir_i2, &dir_i3);
	type = name2type(wname, dir_type);

	new->dir_type = dir_type;
    new->version = 0;
	switch(type) {
	case FsDroot:
		*new = root_qid;
		break;
	case FsDdef:
	case FsDkeys:
	case FsDtags:
	case FsDbar:
		new->type = IXP_QTDIR;
		new->path = mkqpath(type, 0, 0, 0);
		break;
	case FsDlabel:
		new->type = IXP_QTDIR;
		if(!strncmp(wname, "new", 4)) {
			if(iswalk)
				new->path = mkqpath(FsDlabel, new_label()->id, 0, 0);
			else
				new->path = mkqpath(FsDlabel, 0, 0 ,0);
		}
		else {
			i = cext_strtonum(wname, 0, 0xffff, &err);
			if(err || (i >= nlabel))
				return -1;
			new->path = mkqpath(FsDlabel, label[i]->id, 0, 0);
		}
		break;
	case FsDws:
		new->type = IXP_QTDIR;
		if(!strncmp(wname, "new", 4)) {
			if(iswalk) {
				Tag *p = alloc_tag(wname);
				new->path = mkqpath(FsDws, p->id, 0, 0);
			}
			else
				new->path = mkqpath(FsDws, 0, 0, 0);
		}
		else if(!strncmp(wname, "sel", 4)) {
			if(!ntag)
				return -1;
			new->path = mkqpath(FsDws, tag[sel]->id, 0, 0);
		}
		else {
			i = cext_strtonum(wname, 0, 0xffff, &err);
			if(err || (i >= ntag))
				return -1;
			new->path = mkqpath(FsDws, tag[i]->id, 0, 0);
		}
		break;
	case FsDarea:
		if(dir_i1 == -1)
			return -1;
		{
			Tag *p = tag[dir_i1];
			new->type = IXP_QTDIR;
			if(!strncmp(wname, "sel", 4)) {
				new->path = mkqpath(FsDarea, p->id, p->area[p->sel]->id, 0);
			}
			else {
				i = cext_strtonum(wname, 0, 0xffff, &err);
				if(err || (i >= p->narea))
					return -1;
				new->path = mkqpath(FsDarea, p->id, p->area[i]->id, 0);
			}
		}
		break;
	case FsDclient:
		if(dir_i1 == -1 || dir_i2 == -1)
			return -1;
		{
			Tag *p = tag[dir_i1];
			Area *a = p->area[dir_i2];
			new->type = IXP_QTDIR;
			if(!strncmp(wname, "sel", 4)) {
				if(!a->nframe)
					return -1;
				new->path = mkqpath(FsDclient, p->id, a->id, a->frame[a->sel]->id);
			}
			else {
				i = cext_strtonum(wname, 0, 0xffff, &err);
				if(err || (i >= a->nframe))
					return -1;
				new->path = mkqpath(FsDclient, p->id, a->id, a->frame[i]->id);
			}
		}
		break;
	case FsFkey:
		{
			Key *k;
			if(!(k = name2key(wname)))
				return -1;
			new->type = IXP_QTFILE;
			new->path = mkqpath(FsFkey, k->id, 0, 0);
		}
		break;
	case FsFdata:
	case FsFcolors:
		if((dir_type == FsDlabel) && (dir_i1 == -1))
			return -1;
		new->type = IXP_QTFILE;
		new->path = mkqpath(type, qpath_i1id(dir->path), qpath_i2id(dir->path), qpath_i3id(dir->path));
		break;
	case FsFmode:
		if(dir_i1 == -1 || dir_i2 == -1)
			return -1;
		new->type = IXP_QTFILE;
		new->path = mkqpath(type, qpath_i1id(dir->path), qpath_i2id(dir->path), qpath_i3id(dir->path));
		break;
	case FsFgeom:
	case FsFname:
	case FsFtags:
		if(dir_i1 == -1 || dir_i2 == -1 || dir_i3 == -1)
			return -1;
		new->type = IXP_QTFILE;
		new->path = mkqpath(type, qpath_i1id(dir->path), qpath_i2id(dir->path), qpath_i3id(dir->path));
		break;
	default:
		new->type = IXP_QTFILE;
		new->path = mkqpath(type, qpath_i1id(dir->path), qpath_i2id(dir->path), qpath_i3id(dir->path));
		break;
	}
    return 0;
}

static char *
xversion(IXPConn *c, Fcall *fcall)
{
    if(strncmp(fcall->version, IXP_VERSION, strlen(IXP_VERSION)))
        return E9pversion;
    else if(fcall->maxmsg > IXP_MAX_MSG)
        fcall->maxmsg = IXP_MAX_MSG;
    fcall->id = RVERSION;
	ixp_server_respond_fcall(c, fcall);
    return nil;
}

static char *
xattach(IXPConn *c, Fcall *fcall)
{
    IXPMap *new = cext_emallocz(sizeof(IXPMap));
    new->qid = root_qid;
    new->fid = fcall->fid;
	c->map = (IXPMap **)cext_array_attach((void **)c->map, new,
					sizeof(IXPMap *), &c->mapsz);
    fcall->id = RATTACH;
    fcall->qid = root_qid;
	ixp_server_respond_fcall(c, fcall);
    return nil;
}

static char *
xwalk(IXPConn *c, Fcall *fcall)
{
    unsigned short nwqid = 0;
    Qid dir = root_qid;
    IXPMap *m;

	/*fprintf(stderr, "wm: xwalk: fid=%d\n", fcall->fid);*/
    if(!(m = ixp_server_fid2map(c, fcall->fid)))
        return Enofid;
	/*fprintf(stderr, "wm: xwalk1: fid=%d\n", fcall->fid);*/
    if(fcall->fid != fcall->newfid && (ixp_server_fid2map(c, fcall->newfid)))
        return Enofid;
    if(fcall->nwname) {
        dir = m->qid;
        for(nwqid = 0; (nwqid < fcall->nwname)
            && !mkqid(&dir, fcall->wname[nwqid], &fcall->wqid[nwqid], True); nwqid++)
		{
            dir = fcall->wqid[nwqid];
		}
        if(!nwqid) {
			fprintf(stderr, "%s", "xwalk: no such file\n");
			return Enofile;
		}
    }
    /* a fid will only be valid, if the walk was complete */
    if(nwqid == fcall->nwname) {
		/*fprintf(stderr, "wm: xwalk4: newfid=%d\n", fcall->newfid);*/
        if(fcall->fid != fcall->newfid) {
			m = cext_emallocz(sizeof(IXPMap));
			c->map = (IXPMap **)cext_array_attach((void **)c->map, m,
							sizeof(IXPMap *), &c->mapsz);
        }
        m->qid = dir;
        m->fid = fcall->newfid;
    }
    fcall->id = RWALK;
    fcall->nwqid = nwqid;
	ixp_server_respond_fcall(c, fcall);
    return nil;
}

static char *
xcreate(IXPConn *c, Fcall *fcall)
{
    IXPMap *m = ixp_server_fid2map(c, fcall->fid);

    if(!(fcall->mode | IXP_OWRITE))
        return Enomode;
    if(!m)
        return Enofid;
	if(!strncmp(fcall->name, ".", 2) || !strncmp(fcall->name, "..", 3))
		return "illegal file name";
	if(qpath_type(m->qid.path) != FsDkeys)
		return Enoperm;
	grab_key(create_key(fcall->name));
	mkqid(&m->qid, fcall->name, &m->qid, False);
    fcall->id = RCREATE;
    fcall->qid = m->qid;
    fcall->iounit = WMII_IOUNIT;
	ixp_server_respond_fcall(c, fcall);
    return nil;
}

static char *
xopen(IXPConn *c, Fcall *fcall)
{
    IXPMap *m = ixp_server_fid2map(c, fcall->fid);

    if(!m)
        return Enofid;
    if(!(fcall->mode | IXP_OREAD) && !(fcall->mode | IXP_OWRITE))
        return Enomode;
    fcall->id = ROPEN;
    fcall->qid = m->qid;
    fcall->iounit = WMII_IOUNIT;
	ixp_server_respond_fcall(c, fcall);
    return nil;
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
    mkqid(dir, name, &stat->qid, False);
	return ixp_sizeof_stat(stat);
}

static unsigned int
type2stat(Stat *stat, char *wname, Qid *dir)
{
	unsigned char dir_type;
	int dir_i1 = 0, dir_i2 = 0, dir_i3 = 0;
	int type;
	char buf[32];
	Frame *f;

	decode_qpath(dir, &dir_type, &dir_i1, &dir_i2, &dir_i3);
	if((dir_i1 == -1) || (dir_i2 == -1) || (dir_i3 == -1))
		return -1;
	type = name2type(wname, dir_type);

    switch (type) {
    case FsDclient:
    case FsDarea:
    case FsDws:
    case FsDdef:
	case FsDkeys:
	case FsDtags:
	case FsDbar:
	case FsDlabel:
    case FsDroot:
		return mkstat(stat, dir, wname, 0, DMDIR | DMREAD | DMEXEC);
        break;
	case FsFctl:
    case FsFevent:
		return mkstat(stat, dir, wname, 0, DMREAD);
		break;
    case FsFborder:
		snprintf(buf, sizeof(buf), "%d", def.border);
		return mkstat(stat, dir, wname, strlen(buf), DMREAD | DMWRITE);
        break;
    case FsFgeom:
		f = tag[dir_i1]->area[dir_i2]->frame[dir_i3];
		snprintf(buf, sizeof(buf), "%d %d %d %d", f->rect.x, f->rect.y,
				f->rect.width, f->rect.height);
		return mkstat(stat, dir, wname, strlen(buf), DMREAD | DMWRITE);
        break;
    case FsFsnap:
		snprintf(buf, sizeof(buf), "%d", def.snap);
		return mkstat(stat, dir, wname, strlen(buf), DMREAD | DMWRITE);
		break;
    case FsFname:
		f = tag[dir_i1]->area[dir_i2]->frame[dir_i3];
		return mkstat(stat, dir, wname, strlen(f->client->name), DMREAD);
        break;
    case FsFtags:
		switch(dir_type) {
		case FsDclient:
			f = tag[dir_i1]->area[dir_i2]->frame[dir_i3];
			return mkstat(stat, dir, wname, strlen(f->client->tags), DMREAD | DMWRITE);
			break;
		case FsDtags:
			return mkstat(stat, dir, wname, 0, 0);
			break;
		}
        break;
    case FsFkey:
		return mkstat(stat, dir, wname, 0, DMWRITE);
		break;
    case FsFexpand:
		snprintf(buf, sizeof(buf), "%u", iexpand);
		return mkstat(stat, dir, wname, strlen(buf), DMREAD | DMWRITE);
		break;
    case FsFdata:
		return mkstat(stat, dir, wname, (dir_i1 == nlabel) ? 0 : strlen(label[dir_i1]->data), DMREAD | DMWRITE);
		break;	
    case FsFmode:
		return mkstat(stat, dir, wname, strlen(mode2str(tag[dir_i1]->area[dir_i2]->mode)), DMREAD | DMWRITE);
		break;	
    case FsFcolors:
    case FsFselcolors:
    case FsFnormcolors:
		return mkstat(stat, dir, wname, 23, DMREAD | DMWRITE);
    case FsFfont:
		return mkstat(stat, dir, wname, strlen(def.font), DMREAD | DMWRITE);
		break;
    }
	return 0;
}

static char *
xremove(IXPConn *c, Fcall *fcall)
{
    IXPMap *m = ixp_server_fid2map(c, fcall->fid);
	unsigned char type;
	char *ret;
	int i1 = 0, i2 = 0, i3 = 0;

    if(!m)
        return Enofid;
	decode_qpath(&m->qid, &type, &i1, &i2, &i3);
	if((i1 == -1) || (i2 == -1) || (i3 == -1))
		return Enofile;
	switch(type) {
	case FsDws:
		if((ret = destroy_tag(tag[i1])))
			return ret;
		break;
	case FsDlabel:
		{
			Label *l = label[i1];
			/* clunk */
			cext_array_detach((void **)c->map, m, &c->mapsz);
			free(m);
			/* now detach the label */
			detach_label(l);
			free(l);
			if(iexpand >= nlabel)
				iexpand = 0;
			draw_bar();
		}
		break;
	case FsFkey:
		{
			Key *k = key[i1];
			ungrab_key(k);
			destroy_key(k);
		}
		break;
	default:
		return Enoperm;
		break;
	}
    fcall->id = RREMOVE;
	ixp_server_respond_fcall(c, fcall);
	return nil;
}

static char *
xread(IXPConn *c, Fcall *fcall)
{
	Stat stat;
    IXPMap *m = ixp_server_fid2map(c, fcall->fid);
    unsigned char *p = fcall->data;
	unsigned int i, len;
	char buf[32];
	unsigned char type;
	int i1 = 0, i2 = 0, i3 = 0;
	Frame *f;

    if(!m)
        return Enofid;
	decode_qpath(&m->qid, &type, &i1, &i2, &i3);
	if((i1 == -1) || (i2 == -1) || (i3 == -1))
		return Enofile;

	fcall->count = 0;
	if(fcall->offset) {
		switch (type) {
		case FsDroot:
			/* jump to offset */
			len = type2stat(&stat, "ctl", &m->qid);
			len += type2stat(&stat, "event", &m->qid);
			len += type2stat(&stat, "def", &m->qid);
			len += type2stat(&stat, "keys", &m->qid);
			len += type2stat(&stat, "bar", &m->qid);
			len += type2stat(&stat, "tags", &m->qid);
			len += type2stat(&stat, "new", &m->qid);
			for(i = 0; i < ntag; i++) {
				if(i == sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len += type2stat(&stat, buf, &m->qid);
				if(len <= fcall->offset)
					continue;
				break;
			}
			/* offset found, proceeding */
			for(; i < ntag; i++) {
				if(i == sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDkeys:
			/* jump to offset */
			len = 0;
			for(i = 0; i < nkey; i++) {
				len += type2stat(&stat, key[i]->name, &m->qid);
				if(len <= fcall->offset)
					continue;
				break;
			}
			/* offset found, proceeding */
			for(; i < nkey; i++) {
				len = type2stat(&stat, key[i]->name, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDtags:
			/* jump to offset */
			len = 0;
			for(i = 0; i < nctag; i++) {
				len += type2stat(&stat, ctag[i], &m->qid);
				if(len <= fcall->offset)
					continue;
				break;
			}
			/* offset found, proceeding */
			for(; i < nctag; i++) {
				len = type2stat(&stat, ctag[i], &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDbar:
			/* jump to offset */
			len = type2stat(&stat, "expand", &m->qid);
			len += type2stat(&stat, "new", &m->qid);
			for(i = 0; i < nlabel; i++) {
				snprintf(buf, sizeof(buf), "%u", i);
				len += type2stat(&stat, buf, &m->qid);
				if(len <= fcall->offset)
					continue;
				break;
			}
			/* offset found, proceeding */
			for(; i < nlabel; i++) {
				snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDws:
			/* jump to offset */
			len = type2stat(&stat, "ctl", &m->qid);
			for(i = 0; i < tag[i1]->narea; i++) {
				if(i == tag[i1]->sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len += type2stat(&stat, buf, &m->qid);
				if(len <= fcall->offset)
					continue;
				break;
			}
			/* offset found, proceeding */
			for(; i < tag[i1]->narea; i++) {
				if(i == tag[i1]->sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDarea:
			/* jump to offset */
			len = type2stat(&stat, "ctl", &m->qid);
			if(i2)
				len += type2stat(&stat, "mode", &m->qid);
			for(i = 0; i < tag[i1]->area[i2]->nframe; i++) {
				if(i == tag[i1]->area[i2]->sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len += type2stat(&stat, buf, &m->qid);
				if(len <= fcall->offset)
					continue;
				break;
			}
			/* offset found, proceeding */
			for(; i < tag[i1]->area[i2]->nframe; i++) {
				if(i == tag[i1]->area[i2]->sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsFevent:
			memcpy(&c->pending, fcall, sizeof(Fcall));
			c->is_pending = 1;
			return nil;
			break;
		default:
			break;
		}
	}
	else {
		switch (type) {
		case FsDroot:
			fcall->count = type2stat(&stat, "ctl", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "event", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "def", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "keys", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "bar", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "tags", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "new", &m->qid);
			p = ixp_enc_stat(p, &stat);
			for(i = 0; i < ntag; i++) {
				if(i == sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDkeys:
			for(i = 0; i < nkey; i++) {
				len = type2stat(&stat, key[i]->name, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDtags:
			for(i = 0; i < nctag; i++) {
				len = type2stat(&stat, ctag[i], &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDbar:
			fcall->count = type2stat(&stat, "expand", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "new", &m->qid);
			p = ixp_enc_stat(p, &stat);
			for(i = 0; i < nlabel; i++) {
				snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDlabel:
			if(i1 >= nlabel)
				return Enofile;
			fcall->count = type2stat(&stat, "colors", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "data", &m->qid);
			p = ixp_enc_stat(p, &stat);
			break;
		case FsDdef:
			fcall->count += type2stat(&stat, "border", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "snap", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "selcolors", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "normcolors", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "font", &m->qid);
			p = ixp_enc_stat(p, &stat);
			break;
		case FsDws:
			fcall->count = type2stat(&stat, "ctl", &m->qid);
			p = ixp_enc_stat(p, &stat);
			for(i = 0; i < tag[i1]->narea; i++) {
				if(i == tag[i1]->sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDarea:
			fcall->count = type2stat(&stat, "ctl", &m->qid);
			p = ixp_enc_stat(p, &stat);
			if(i2)
				fcall->count += type2stat(&stat, "mode", &m->qid);
				p = ixp_enc_stat(p, &stat);
			for(i = 0; i < tag[i1]->area[i2]->nframe; i++) {
				if(i == tag[i1]->area[i2]->sel)
					snprintf(buf, sizeof(buf), "%s", "sel");
				else
					snprintf(buf, sizeof(buf), "%u", i);
				len = type2stat(&stat, buf, &m->qid);
				if(fcall->count + len > fcall->iounit)
					break;
				fcall->count += len;
				p = ixp_enc_stat(p, &stat);
			}
			break;
		case FsDclient:
			fcall->count += type2stat(&stat, "name", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "tags", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "geom", &m->qid);
			p = ixp_enc_stat(p, &stat);
			fcall->count += type2stat(&stat, "ctl", &m->qid);
			p = ixp_enc_stat(p, &stat);
			break;
		case FsFctl:
			return Enoperm;
			break;
		case FsFevent:
			memcpy(&c->pending, fcall, sizeof(Fcall));
			c->is_pending = 1;
			return nil;
			break;
		case FsFborder:
			snprintf(buf, sizeof(buf), "%u", def.border);
			fcall->count = strlen(buf);
			memcpy(p, buf, fcall->count);
			break;
		case FsFgeom:
			f = tag[i1]->area[i2]->frame[i3];
			snprintf(buf, sizeof(buf), "%d %d %d %d", f->rect.x, f->rect.y,
					f->rect.width, f->rect.height);
			fcall->count = strlen(buf);
			memcpy(p, buf, fcall->count);
			break;
		case FsFsnap:
			snprintf(buf, sizeof(buf), "%u", def.snap);
			fcall->count = strlen(buf);
			memcpy(p, buf, fcall->count);
			break;
		case FsFname:
			if((fcall->count = strlen(tag[i1]->area[i2]->frame[i3]->client->name)))
				memcpy(p, tag[i1]->area[i2]->frame[i3]->client->name, fcall->count);
			break;
		case FsFtags:
			if((fcall->count = strlen(tag[i1]->area[i2]->frame[i3]->client->tags)))
				memcpy(p, tag[i1]->area[i2]->frame[i3]->client->tags, fcall->count);
			break;
		case FsFexpand:
			snprintf(buf, sizeof(buf), "%u", iexpand);
			fcall->count = strlen(buf);
			memcpy(p, buf, fcall->count);
			break;
		case FsFdata:
			if(i1 >= nlabel)
				return Enofile;
			if((fcall->count = strlen(label[i1]->data)))
				memcpy(p, label[i1]->data, fcall->count);
			break;
		case FsFcolors:
			if(i1 >= nlabel)
				return Enofile;
			if((fcall->count = strlen(label[i1]->colstr)))
				memcpy(p, label[i1]->colstr, fcall->count);
			break;
		case FsFselcolors:
			if((fcall->count = strlen(def.selcolor)))
				memcpy(p, def.selcolor, fcall->count);
			break;
		case FsFnormcolors:
			if((fcall->count = strlen(def.normcolor)))
				memcpy(p, def.normcolor, fcall->count);
			break;
		case FsFfont:
			if((fcall->count = strlen(def.font)))
				memcpy(p, def.font, fcall->count);
			break;
		case FsFmode:
			if(!i2)
				return Enofile;
			snprintf(buf, sizeof(buf), "%s", mode2str(tag[i1]->area[i2]->mode));
			fcall->count = strlen(buf);
			memcpy(p, buf, fcall->count);
			break;	
		default:
			return "invalid read";
			break;
		}
	}
	fcall->id = RREAD;
	ixp_server_respond_fcall(c, fcall);
    return nil;
}

static char *
xstat(IXPConn *c, Fcall *fcall)
{
    IXPMap *m = ixp_server_fid2map(c, fcall->fid);
	char *name;

    if(!m)
        return Enofid;
	name = qid2name(&m->qid);
	if(!type2stat(&fcall->stat, name, &m->qid))
		return Enofile;
    fcall->id = RSTAT;
	ixp_server_respond_fcall(c, fcall);
    return nil;
}

static char *
xwrite(IXPConn *c, Fcall *fcall)
{
	char buf[256];
    IXPMap *m = ixp_server_fid2map(c, fcall->fid);
	unsigned char type;
	int i, i1 = 0, i2 = 0, i3 = 0;
	Frame *f;

    if(!m)
        return Enofid;
	decode_qpath(&m->qid, &type, &i1, &i2, &i3);
	if((i1 == -1) || (i2 == -1) || (i3 == -1))
		return Enofile;

	switch (qpath_type(m->qid.path)) {
	case FsFctl:
		if(fcall->count > sizeof(buf) - 1)
			return Enocommand;
		memcpy(buf, fcall->data, fcall->count);
		buf[fcall->count] = 0;
		switch(m->qid.dir_type) {
		case FsDroot:
			if(!strncmp(buf, "quit", 5))
				srv.running = 0;
			else if(!strncmp(buf, "select", 6))
				select_tag(&buf[7]);
			else if(!strncmp(buf, "warp ", 5)) {
				char *err;
				if((err = warp_mouse(&buf[5])))
					return err;
			}
			else
				return Enocommand;
			break;
		case FsDws:
			if(!strncmp(buf, "select ", 7))
				select_area(tag[i1]->area[tag[i1]->sel], &buf[7]);
			break;
		case FsDarea:
			if(!strncmp(buf, "select ", 7)) {
			   Area *a = tag[i1]->area[i2];
			   if(a->nframe)
				   select_client(a->frame[a->sel]->client, &buf[7]);
			}
			break;
		case FsDclient:
			f = tag[i1]->area[i2]->frame[i3];
			if(!strncmp(buf, "kill", 5))
				kill_client(f->client);
			else if(!strncmp(buf, "sendtotag ", 11))
				sendtotag_client(f->client, &buf[11]);
			else if(!strncmp(buf, "sendtoarea ", 11))
				sendtoarea_client(f->client, &buf[11]);
			break;
		default:
			break;
		}
		break;
	case FsFsnap:
		if(fcall->count > sizeof(buf))
			return "snap value out of range 0x0000,..,0xffff";
		memcpy(buf, fcall->data, fcall->count);
		buf[fcall->count] = 0;
		i = cext_strtonum(buf, 0, 0xffff, &err);
		if(err)
			return "snap value out of range 0x0000,..,0xffff";
		def.snap = i;
		break;
	case FsFborder:
		if(fcall->count > sizeof(buf))
			return "border value out of range 0x0000,..,0xffff";
		memcpy(buf, fcall->data, fcall->count);
		buf[fcall->count] = 0;
		i = cext_strtonum(buf, 0, 0xffff, &err);
		if(err)
			return "border value out of range 0x0000,..,0xffff";
		def.border = i;
		resize_all_clients();
		break;
	case FsFtags:
		f = tag[i1]->area[i2]->frame[i3];
		if(fcall->count > sizeof(f->client->tags))
			return "tags value too long";
		memcpy(f->client->tags, fcall->data, fcall->count);
		update_ctags();
		break;
	case FsFgeom:
		f = tag[i1]->area[i2]->frame[i3];
		if(fcall->count > sizeof(buf))
			return "geometry values out of range";
		memcpy(buf, fcall->data, fcall->count);
		buf[fcall->count] = 0;
		blitz_strtorect(&rect, &f->rect, buf);
		resize_client(f->client, &f->rect, 0, False);
		break;
    case FsFexpand:
		{
			const char *err;
			if(fcall->count && fcall->count < 16) {
				memcpy(buf, fcall->data, fcall->count);
				buf[fcall->count] = 0;
				i = (unsigned short) cext_strtonum(buf, 0, 0xffff, &err);
				if(!err && (i < nlabel)) {
					iexpand = i;
					draw_bar();
					break;
				}
			}
		}
		return Enofile;
		break;
	case FsFdata:
		{
			unsigned int len = fcall->count;
			if(len >= sizeof(label[i1]->data))
				len = sizeof(label[i1]->data) - 1;
			memcpy(label[i1]->data, fcall->data, len);
			label[i1]->data[len] = 0;
			draw_bar();
		}
		break;
	case FsFcolors:
		if((i1 >= nlabel) || (fcall->count != 23)
			|| (fcall->data[0] != '#') || (fcall->data[8] != '#')
		    || (fcall->data[16] != '#')
		  )
			return "wrong color format";
		memcpy(label[i1]->colstr, fcall->data, fcall->count);
		label[i1]->colstr[fcall->count] = 0;
		blitz_loadcolor(dpy, screen, label[i1]->colstr, &label[i1]->color);
		draw_bar();
		break;
	case FsFselcolors:
		if((fcall->count != 23)
			|| (fcall->data[0] != '#') || (fcall->data[8] != '#')
		    || (fcall->data[16] != '#')
		  )
			return "wrong color format";
		memcpy(def.selcolor, fcall->data, fcall->count);
		def.selcolor[fcall->count] = 0;
		blitz_loadcolor(dpy, screen, def.selcolor, &def.sel);
		for(i = 0; i < nclient; i++)
			if(client[i]->frame->area->tag == tag[sel])
				draw_client(client[i]);
		break;
	case FsFnormcolors:
		if((fcall->count != 23)
			|| (fcall->data[0] != '#') || (fcall->data[8] != '#')
		    || (fcall->data[16] != '#')
		  )
			return "wrong color format";
		memcpy(def.normcolor, fcall->data, fcall->count);
		def.normcolor[fcall->count] = 0;
		blitz_loadcolor(dpy, screen, def.normcolor, &def.norm);
		for(i = 0; i < nclient; i++)
			if(client[i]->frame->area->tag == tag[sel])
				draw_client(client[i]);
		break;
	case FsFfont:
		if(def.font)
			free(def.font);
		def.font = cext_emallocz(fcall->count + 1);
		memcpy(def.font, fcall->data, fcall->count);
		XFreeFont(dpy, xfont);
    	xfont = blitz_getfont(dpy, def.font);
		update_bar_geometry();
		resize_all_clients();
		break;
	case FsFmode:
		if(!i2)
			return Enofile;
		memcpy(buf, fcall->data, fcall->count);
		buf[fcall->count] = 0;
		if((i = str2mode(buf)) == -1)
			return "invalid area mode";
		tag[i1]->area[i2]->mode = i;
		arrange_area(tag[i1]->area[i2]);
		break;	
	case FsFkey:
		break;
	default:
		return "invalid write";
		break;
	}
    fcall->id = RWRITE;
	ixp_server_respond_fcall(c, fcall);
	return nil;
}

static char *
xclunk(IXPConn *c, Fcall *fcall)
{
    IXPMap *m = ixp_server_fid2map(c, fcall->fid);

    if(!m)
        return Enofid;
	cext_array_detach((void **)c->map, m, &c->mapsz);
    free(m);
    fcall->id = RCLUNK;
	ixp_server_respond_fcall(c, fcall);
    return nil;
}

static void
do_fcall(IXPConn *c)
{
	static Fcall fcall;
    unsigned int msize;
	char *errstr;

	if((msize = ixp_server_receive_fcall(c, &fcall))) {
		/*fprintf(stderr, "do_fcall=%d\n", fcall.id);*/
		switch(fcall.id) {
		case TVERSION: errstr = xversion(c, &fcall); break;
		case TATTACH: errstr = xattach(c, &fcall); break;
		case TWALK: errstr = xwalk(c, &fcall); break;
		case TCREATE: errstr = xcreate(c, &fcall); break;
		case TOPEN: errstr = xopen(c, &fcall); break;
		case TREMOVE: errstr = xremove(c, &fcall); break;
		case TREAD: errstr = xread(c, &fcall); break;
		case TWRITE: errstr = xwrite(c, &fcall); break;
		case TCLUNK: errstr = xclunk(c, &fcall); break;
		case TSTAT: errstr = xstat(c, &fcall); break;
		default: errstr = Enofunc; break;
		}
		if(errstr)
			ixp_server_respond_error(c, &fcall, errstr);
	}
	check_x_event(nil);
}

void
write_event(char *event)
{
	unsigned int i;
	for(i = 0; (i < srv.connsz) && srv.conn[i]; i++) {
		IXPConn *c = srv.conn[i];
		if(c->is_pending) {
			/* pending reads on /event only, no qid checking */
			IXPMap *m = ixp_server_fid2map(c, c->pending.fid);
			unsigned char *p = c->pending.data;
			if(!m) {
				if(ixp_server_respond_error(c, &c->pending, Enofid))
					break;
			}
			else if(qpath_type(m->qid.path) == FsFevent) {
				c->pending.count = strlen(event);
				memcpy(p, event, c->pending.count);
				c->pending.id = RREAD;
				if(ixp_server_respond_fcall(c, &c->pending))
					break;
			}
		}
	}
}

void
new_ixp_conn(IXPConn *c)
{
	int fd = ixp_accept_sock(c->fd);
	
	if(fd >= 0)
		ixp_server_open_conn(c->srv, fd, do_fcall, ixp_server_close_conn);
}
