/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  linux/mds/mds_lov.c
 *  Lustre Metadata Server (mds) handling of striped file data
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_idl.h>
#include <linux/obd_class.h>
#include <linux/obd_lov.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_fsfilt.h>

#include "mds_internal.h"


/*
 * TODO:
 *   - magic in mea struct
 *   - error handling is totally missed
 */

int mds_lmv_connect(struct obd_device *obd, char * lmv_name)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lustre_handle conn = {0,};
        int valsize, mdsize;
        int rc;
        ENTRY;

        if (IS_ERR(mds->mds_lmv_obd))
                RETURN(PTR_ERR(mds->mds_lmv_obd));

        if (mds->mds_lmv_obd)
                RETURN(0);

        mds->mds_lmv_obd = class_name2obd(lmv_name);
        if (!mds->mds_lmv_obd) {
                CERROR("MDS cannot locate LMV %s\n",
                       lmv_name);
                mds->mds_lmv_obd = ERR_PTR(-ENOTCONN);
                RETURN(-ENOTCONN);
        }

        rc = obd_connect(&conn, mds->mds_lmv_obd, &obd->obd_uuid);
        if (rc) {
                CERROR("MDS cannot connect to LMV %s (%d)\n",
                       lmv_name, rc);
                mds->mds_lmv_obd = ERR_PTR(rc);
                RETURN(rc);
        }
        mds->mds_lmv_exp = class_conn2export(&conn);
        if (mds->mds_lmv_exp == NULL)
                CERROR("can't get export!\n");

        rc = obd_register_observer(mds->mds_lmv_obd, obd);
        if (rc) {
                CERROR("MDS cannot register as observer of LMV %s (%d)\n",
                       lmv_name, rc);
                GOTO(err_discon, rc);
        }

        /* retrieve size of EA */
        rc = obd_get_info(mds->mds_lmv_exp, strlen("mdsize"), "mdsize", 
                          &valsize, &mdsize);
        if (rc) 
                GOTO(err_reg, rc);
        if (mdsize > mds->mds_max_mdsize)
                mds->mds_max_mdsize = mdsize;

        /* find our number in LMV cluster */
        rc = obd_get_info(mds->mds_lmv_exp, strlen("mdsnum"), "mdsnum", 
                          &valsize, &mdsize);
        if (rc) 
                GOTO(err_reg, rc);
        mds->mds_num = mdsize;

        rc = obd_set_info(mds->mds_lmv_exp, strlen("inter_mds"),
                                "inter_mds", 0, NULL);
        if (rc)
                GOTO(err_reg, rc);
	RETURN(0);

err_reg:
        RETURN(rc);

err_discon:
        /* FIXME: cleanups here! */
        obd_disconnect(mds->mds_lmv_exp, 0);
        mds->mds_lmv_exp = NULL;
        mds->mds_lmv_obd = ERR_PTR(rc);
        RETURN(rc);
}

int mds_lmv_postsetup(struct obd_device *obd)
{
        struct mds_obd *mds = &obd->u.mds;
        ENTRY;
        if (mds->mds_lmv_exp)
                obd_init_ea_size(mds->mds_lmv_exp, mds->mds_max_mdsize,
                                 mds->mds_max_cookiesize);
        RETURN(0);
}

int mds_lmv_disconnect(struct obd_device *obd, int flags)
{
        struct mds_obd *mds = &obd->u.mds;
        int rc = 0;
        ENTRY;

        if (!IS_ERR(mds->mds_lmv_obd) && mds->mds_lmv_exp != NULL) {

                obd_register_observer(mds->mds_lmv_obd, NULL);

                rc = obd_disconnect(mds->mds_lmv_exp, flags);
                /* if obd_disconnect fails (probably because the
                 * export was disconnected by class_disconnect_exports)
                 * then we just need to drop our ref. */
                if (rc != 0)
                        class_export_put(mds->mds_lmv_exp);
                mds->mds_lmv_exp = NULL;
                mds->mds_lmv_obd = NULL;
        }

        RETURN(rc);
}


int mds_get_lmv_attr(struct obd_device *obd, struct inode *inode,
			struct mea **mea, int *mea_size)
{
        struct mds_obd *mds = &obd->u.mds;
	int rc;
        ENTRY;

	if (!mds->mds_lmv_obd)
		RETURN(0);

	/* first calculate mea size */
        *mea_size = obd_alloc_diskmd(mds->mds_lmv_exp,
                                     (struct lov_mds_md **)mea);
	/* FIXME: error handling here */
	LASSERT(*mea != NULL);

	down(&inode->i_sem);
	rc = fsfilt_get_md(obd, inode, *mea, *mea_size);
	up(&inode->i_sem);
	/* FIXME: error handling here */
	if (rc <= 0) {
		OBD_FREE(*mea, *mea_size);
		*mea = NULL;
	}
        if (rc > 0)
                rc = 0;
	RETURN(rc);
}

struct dir_entry {
        __u16   namelen;
        __u16   mds;
        __u32   ino;
        __u32   generation;
        char    name[0];
};

#define DIR_PAD			4
#define DIR_ROUND		(DIR_PAD - 1)
#define DIR_REC_LEN(name_len)	(((name_len) + 12 + DIR_ROUND) & ~DIR_ROUND)

/* this struct holds dir entries for particular MDS to be flushed */
struct dir_cache {
        struct list_head list;
        void *cur;
        int free;
        int cached;
        struct obdo oa;
        struct brw_page brwc;
};

struct dirsplit_control {
        struct obd_device *obd;
        struct inode *dir;
        struct dentry *dentry;
        struct mea *mea;
        struct dir_cache *cache;
};

static int dc_new_page_to_cache(struct dir_cache * dirc)
{
        struct page *page;

        if (!list_empty(&dirc->list) && dirc->free > sizeof(__u16)) {
                /* current page became full, mark the end */
                struct dir_entry *de = dirc->cur;
                de->namelen = 0;
        }

        page = alloc_page(GFP_KERNEL);
        if (page == NULL)
                return -ENOMEM;
        list_add_tail(&page->list, &dirc->list);
        dirc->cur = page_address(page);
        dirc->free = PAGE_SIZE;
        return 0;
}

static int retrieve_generation_numbers(struct dirsplit_control *dc, void *buf)
{
        struct mds_obd *mds = &dc->obd->u.mds;
        struct dir_entry *de;
        struct dentry *dentry;
        char * end;
        
        end = buf + PAGE_SIZE;
        de = (struct dir_entry *) buf;
        while ((char *) de < end && de->namelen) {
                /* lookup an inode */
                LASSERT(de->namelen <= 255);
                dentry = ll_lookup_one_len(de->name, dc->dentry, de->namelen);
                if (IS_ERR(dentry)) {
                        CERROR("can't lookup %*s: %d\n", de->namelen,
                               de->name, (int) PTR_ERR(dentry));
                        goto next;
                }
                if (dentry->d_inode != NULL) {
                        de->mds = mds->mds_num;
                        de->ino = dentry->d_inode->i_ino;
                        de->generation = dentry->d_inode->i_generation;
                } else if (dentry->d_flags & DCACHE_CROSS_REF) {
                        de->mds = dentry->d_mdsnum;
                        de->ino = dentry->d_inum;
                        de->generation = dentry->d_generation;
                } else {
                        CERROR("can't lookup %*s\n", de->namelen, de->name);
                        goto next;
                }
                l_dput(dentry);

next:
                de = (struct dir_entry *)
                        ((char *) de + DIR_REC_LEN(de->namelen));
        }
        return 0;
}

static int flush_buffer_onto_mds(struct dirsplit_control *dc, int mdsnum)
{
        struct mds_obd *mds = &dc->obd->u.mds;
        struct dir_cache *ca;
        struct list_head *cur, *tmp;
        ENTRY; 
        ca = dc->cache + mdsnum;

        if (ca->free > sizeof(__u16)) {
                /* current page became full, mark the end */
                struct dir_entry *de = ca->cur;
                de->namelen = 0;
        }

        list_for_each_safe(cur, tmp, &ca->list) {
                struct page *page;

                page = list_entry(cur, struct page, list);
                LASSERT(page != NULL);

                retrieve_generation_numbers(dc, page_address(page));

                ca->brwc.pg = page;
                ca->brwc.off = 0;
                ca->brwc.count = PAGE_SIZE;
                ca->brwc.flag = 0;
                ca->oa.o_mds = mdsnum;
                obd_brw(OBD_BRW_WRITE, mds->mds_lmv_exp, &ca->oa,
                                (struct lov_stripe_md *) dc->mea,
                                1, &ca->brwc, NULL);

                list_del(&page->list);
                __free_page(page);
        }
        RETURN(0);
}

static int filldir(void * __buf, const char * name, int namlen, loff_t offset,
		   ino_t ino, unsigned int d_type)
{
        struct dirsplit_control *dc = __buf;
        struct mds_obd *mds = &dc->obd->u.mds;
        struct dir_cache *ca;
        struct dir_entry *de;
        int newmds;
        char *n;
        ENTRY;

        if (name[0] == '.' && (namlen == 1 ||
                                (namlen == 2 && name[1] == '.'))) {
                /* skip special entries */
                RETURN(0);
        }

        LASSERT(dc != NULL);
        newmds = mea_name2idx(dc->mea, (char *) name, namlen);

        if (newmds == mds->mds_num) {
                /* this entry remains on the current MDS, skip moving */
                RETURN(0);
        }
        
        OBD_ALLOC(n, namlen + 1);
        memcpy(n, name, namlen);
        n[namlen] = (char) 0;
        
        OBD_FREE(n, namlen + 1);

        /* check for space in buffer for new entry */
        ca = dc->cache + newmds;
        if (DIR_REC_LEN(namlen) > ca->free) {
                int err = dc_new_page_to_cache(ca);
                LASSERT(err == 0);
        }
        
        /* insert found entry into buffer to be flushed later */
        /* NOTE: we'll fill generations number later, because we
         * it's stored in inode, thus we need to lookup an entry,
         * but directory is locked for readdir(), so we delay this */
        de = ca->cur;
        de->ino = ino;
        de->mds = d_type;
        de->namelen = namlen;
        memcpy(de->name, name, namlen);
        ca->cur += DIR_REC_LEN(namlen);
        ca->free -= DIR_REC_LEN(namlen);
        ca->cached++;

        RETURN(0);
}

int scan_and_distribute(struct obd_device *obd, struct dentry *dentry,
                                struct mea *mea)
{
        struct inode *dir = dentry->d_inode;
        struct dirsplit_control dc;
        struct file * file;
        int err, i, nlen;
        char *file_name;

        nlen = strlen("__iopen__/") + 10 + 1;
        OBD_ALLOC(file_name, nlen);
        if (!file_name)
                RETURN(-ENOMEM);
        i = sprintf(file_name, "__iopen__/0x%lx", dentry->d_inode->i_ino);

        file = filp_open(file_name, O_RDONLY, 0);
        if (IS_ERR(file)) {
                CERROR("can't open directory %s: %d\n",
                                file_name, (int) PTR_ERR(file));
                OBD_FREE(file_name, nlen);
                RETURN(PTR_ERR(file));
        }

        memset(&dc, 0, sizeof(dc));
        dc.obd = obd;
        dc.dir = dir;
        dc.dentry = dentry;
        dc.mea = mea;
        OBD_ALLOC(dc.cache, sizeof(struct dir_cache) * mea->mea_count);
        LASSERT(dc.cache != NULL);
        for (i = 0; i < mea->mea_count; i++) {
                INIT_LIST_HEAD(&dc.cache[i].list);
                dc.cache[i].free = 0;
                dc.cache[i].cached = 0;
        }

        err = vfs_readdir(file, filldir, &dc);
        
        filp_close(file, 0);

        for (i = 0; i < mea->mea_count; i++) {
                if (dc.cache[i].cached)
                        flush_buffer_onto_mds(&dc, i);
        }

        OBD_FREE(dc.cache, sizeof(struct dir_cache) * mea->mea_count);
        OBD_FREE(file_name, nlen);

        return 0;
}

#define MAX_DIR_SIZE    (64 * 1024)

int mds_splitting_expected(struct obd_device *obd, struct dentry *dentry)
{
        struct mds_obd *mds = &obd->u.mds;
        struct mea *mea = NULL;
        int rc, size;

	/* clustered MD ? */
	if (!mds->mds_lmv_obd)
		RETURN(0);

        /* inode exist? */
        if (dentry->d_inode == NULL)
                return 0;

        /* a dir can be splitted only */
        if (!S_ISDIR(dentry->d_inode->i_mode))
                return 0;

        /* large enough to be splitted? */
        if (dentry->d_inode->i_size < MAX_DIR_SIZE)
                return 0;

        /* don't split root directory */
        if (dentry->d_inode->i_ino == mds->mds_rootfid.id)
                return 0;

        mds_get_lmv_attr(obd, dentry->d_inode, &mea, &size);
        if (mea) {
                /* already splitted or slave object: shouldn't be splitted */
                rc = 0;
        } else {
                /* may be splitted */
                rc = 1;
        }

        if (mea)
                OBD_FREE(mea, size);
        RETURN(rc);
}

/*
 * must not be called on already splitted directories
 */
int mds_try_to_split_dir(struct obd_device *obd,
                         struct dentry *dentry, struct mea **mea, int nstripes)
{
        struct inode *dir = dentry->d_inode;
        struct mds_obd *mds = &obd->u.mds;
        struct mea *tmea = NULL;
        struct obdo *oa = NULL;
	int rc, mea_size = 0;
	void *handle;
	ENTRY;

        /* TODO: optimization possible - we already may have mea here */
        if (!mds_splitting_expected(obd, dentry))
                RETURN(0);
        LASSERT(mea == NULL || *mea == NULL);

        CDEBUG(D_OTHER, "%s: split directory %u/%lu/%lu\n",
               obd->obd_name, mds->mds_num, dir->i_ino,
               (unsigned long) dir->i_generation);

        if (mea == NULL)
                mea = &tmea;
        mea_size = obd_size_diskmd(mds->mds_lmv_exp, NULL);

        /* FIXME: Actually we may only want to allocate enough space for
         * necessary amount of stripes, but on the other hand with this
         * approach of allocating maximal possible amount of MDS slots,
         * it would be easier to split the dir over more MDSes */
        rc = obd_alloc_diskmd(mds->mds_lmv_exp, (void *) mea);
        if (!(*mea))
                RETURN(-ENOMEM);
        (*mea)->mea_count = nstripes;
       
	/* 1) create directory objects on slave MDS'es */
	/* FIXME: should this be OBD method? */
        oa = obdo_alloc();
        /* FIXME: error handling here */
        LASSERT(oa != NULL);
	oa->o_id = dir->i_ino;
	oa->o_generation = dir->i_generation;
	obdo_from_inode(oa, dir, OBD_MD_FLTYPE | OBD_MD_FLATIME |
			OBD_MD_FLMTIME | OBD_MD_FLCTIME |
                        OBD_MD_FLUID | OBD_MD_FLGID);
        oa->o_gr = FILTER_GROUP_FIRST_MDS + mds->mds_num;
        oa->o_valid |= OBD_MD_FLID | OBD_MD_FLFLAGS | OBD_MD_FLGROUP;
        oa->o_mode = dir->i_mode;
        CDEBUG(D_OTHER, "%s: create subdirs with mode %o, uid %u, gid %u\n",
                        obd->obd_name, dir->i_mode, dir->i_uid, dir->i_gid);
                        
        rc = obd_create(mds->mds_lmv_exp, oa,
                        (struct lov_stripe_md **) mea, NULL);
        /* FIXME: error handling here */
	LASSERT(rc == 0);
        CDEBUG(D_OTHER, "%d dirobjects created\n",
               (int) (*mea)->mea_count);

	/* 2) update dir attribute */
        down(&dir->i_sem);
        handle = fsfilt_start(obd, dir, FSFILT_OP_SETATTR, NULL);
        LASSERT(!IS_ERR(handle));
	rc = fsfilt_set_md(obd, dir, handle, *mea, mea_size);
        LASSERT(rc == 0);
        fsfilt_commit(obd, dir, handle, 0);
        LASSERT(rc == 0);
	up(&dir->i_sem);
	obdo_free(oa);

	/* 3) read through the dir and distribute it over objects */
        scan_and_distribute(obd, dentry, *mea);

	if (mea == &tmea)
                obd_free_diskmd(mds->mds_lmv_exp,
                                (struct lov_mds_md **) mea);
	RETURN(1);
}

static int filter_start_page_write(struct inode *inode,
                                   struct niobuf_local *lnb)
{
        struct page *page = alloc_pages(GFP_HIGHUSER, 0);
        if (page == NULL) {
                CERROR("no memory for a temp page\n");
                RETURN(lnb->rc = -ENOMEM);
        }
        POISON_PAGE(page, 0xf1);
        page->index = lnb->offset >> PAGE_SHIFT;
        lnb->page = page;

        return 0;
}

struct dentry *filter_fid2dentry(struct obd_device *obd,
                                 struct dentry *dir_dentry,
                                 obd_gr group, obd_id id);

int mds_preprw(int cmd, struct obd_export *exp, struct obdo *oa,
                int objcount, struct obd_ioobj *obj,
                int niocount, struct niobuf_remote *nb,
                struct niobuf_local *res,
                struct obd_trans_info *oti)
{
        struct mds_obd *mds = &exp->exp_obd->u.mds;
        struct niobuf_remote *rnb;
        struct niobuf_local *lnb = NULL;
        int rc = 0, i, tot_bytes = 0;
        unsigned long now = jiffies;
        struct dentry *dentry;
        struct ll_fid fid;
        ENTRY;
        LASSERT(objcount == 1);
        LASSERT(obj->ioo_bufcnt > 0);

        memset(res, 0, niocount * sizeof(*res));

        fid.id = obj->ioo_id;
        fid.generation = obj->ioo_gr;
        dentry = mds_fid2dentry(mds, &fid, NULL);
        LASSERT(!IS_ERR(dentry));

        if (dentry->d_inode == NULL) {
                CERROR("trying to BRW to non-existent file "LPU64"\n",
                       obj->ioo_id);
                l_dput(dentry);
                GOTO(cleanup, rc = -ENOENT);
        }

        if (time_after(jiffies, now + 15 * HZ))
                CERROR("slow preprw_write setup %lus\n", (jiffies - now) / HZ);
        else
                CDEBUG(D_INFO, "preprw_write setup: %lu jiffies\n",
                       (jiffies - now));

        for (i = 0, rnb = nb, lnb = res; i < obj->ioo_bufcnt;
             i++, lnb++, rnb++) {
                lnb->dentry = dentry;
                lnb->offset = rnb->offset;
                lnb->len    = rnb->len;
                lnb->flags  = rnb->flags;

                rc = filter_start_page_write(dentry->d_inode, lnb);
                if (rc) {
                        CDEBUG(rc == -ENOSPC ? D_INODE : D_ERROR, "page err %u@"
                               LPU64" %u/%u %p: rc %d\n", lnb->len, lnb->offset,
                               i, obj->ioo_bufcnt, dentry, rc);
                        while (lnb-- > res)
                                __free_pages(lnb->page, 0);
                        l_dput(dentry);
                        GOTO(cleanup, rc);
                }
                tot_bytes += lnb->len;
        }

        if (time_after(jiffies, now + 15 * HZ))
                CERROR("slow start_page_write %lus\n", (jiffies - now) / HZ);
        else
                CDEBUG(D_INFO, "start_page_write: %lu jiffies\n",
                       (jiffies - now));

        EXIT;
cleanup:
        return rc;
}

int mds_commitrw(int cmd, struct obd_export *exp, struct obdo *oa,
                 int objcount, struct obd_ioobj *obj, int niocount,
                 struct niobuf_local *res, struct obd_trans_info *oti,
                 int retcode)
{
        struct obd_device *obd = exp->exp_obd;
        struct niobuf_local *lnb;
        struct inode *inode = NULL;
        int rc = 0, i, cleanup_phase = 0, err, entries = 0;
        ENTRY;

        LASSERT(objcount == 1);
        LASSERT(current->journal_info == NULL);

        cleanup_phase = 1;
        inode = res->dentry->d_inode;

        for (i = 0, lnb = res; i < obj->ioo_bufcnt; i++, lnb++) {
                char *end, *buf;
                struct dir_entry *de;

                buf = kmap(lnb->page);
                LASSERT(buf != NULL);
                end = buf + lnb->len;
                de = (struct dir_entry *) buf;
                while ((char *) de < end && de->namelen) {
                        err = fsfilt_add_dir_entry(obd, res->dentry, de->name,
                                                   de->namelen, de->ino,
                                                   de->generation, de->mds);
                        /* FIXME: remove entries from the original dir */
#warning "removing entries from the original dir"
                        LASSERT(err == 0);
                        de = (struct dir_entry *)
                                ((char *) de + DIR_REC_LEN(de->namelen));
                        entries++;
                }
                kunmap(buf);
        }

        for (i = 0, lnb = res; i < obj->ioo_bufcnt; i++, lnb++)
                __free_page(lnb->page);
        l_dput(res->dentry);

        RETURN(rc);
}

int mds_choose_mdsnum(struct obd_device *obd, const char *name, int len, int flags)
{
        struct lmv_obd *lmv;
        struct mds_obd *mds = &obd->u.mds;
        int i = mds->mds_num;

        if (flags & REC_REINT_CREATE) { 
                i = mds->mds_num;
        } else if (mds->mds_lmv_exp) {
                lmv = &mds->mds_lmv_exp->exp_obd->u.lmv;
                i = raw_name2idx(lmv->count, name, len);
        }
        RETURN(i);
}

