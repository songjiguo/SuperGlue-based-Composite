/**
 * Copyright 2010 by The George Washington University.  All rights reserved.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2010
 */

#include <cbuf.h>
#include <cos_vect.h>

/* 
 * All of the structures that must be compiled (only once) into each
 * component that uses cbufs.
 *
 * I'd like to avoid having to make components initialize this data,
 * so try and use structures that by default are initialized as all
 * zero. 
 */

COS_VECT_CREATE_STATIC(meta_cbuf);
COS_VECT_CREATE_STATIC(slab_descs);
struct cbuf_slab_freelist slab_freelists[N_CBUF_SLABS];

/* 
 * A component has tried to map a cbuf_t to a buffer, but that cbuf
 * isn't mapping into the component.  The component's cache of cbufs
 * had a miss.
 */
int 
cbuf_cache_miss(int cbid, int idx, int len)
{
	union cbuf_meta mc;
	char *h;

	/* FIXME: race race race */
	h = cos_get_heap_ptr();
	cos_set_heap_ptr(h + PAGE_SIZE);
	mc.c.ptr    = (long)h >> PAGE_ORDER;
	mc.c.obj_sz = len;
	if (cbuf_c_retrieve(cos_spd_id(), cbid, len, h)) {
		/* do dumb test for race... */
		assert(cos_get_heap_ptr() == h + PAGE_SIZE);
		cos_set_heap_ptr(h);
		/* Illegal cbid or length!  Bomb out. */
		return -1;
	}
	/* This is the commit point */
	cos_vect_add_id(&meta_cbuf, (void*)mc.v, cbid);

	return 0;
}

void
cbuf_slab_cons(struct cbuf_slab *s, int cbid, void *page, 
	       int obj_sz, struct cbuf_slab_freelist *freelist)
{
	s->cbid = cbid;
	s->mem = page;
	s->obj_sz = obj_sz;
	memset(&s->bitmap[0], 0, sizeof(u32_t)*SLAB_BITMAP_SIZE);
	s->nfree = s->max_objs = PAGE_SIZE/obj_sz; /* not a perf sensitive path */
	/* FIXME: race race race */
	s->next = freelist->list;
	s->prev = NULL;
	s->flh  = freelist;
	freelist->list = s;
	freelist->npages++;

	return;
}

struct cbuf_slab *
cbuf_slab_alloc(int size, struct cbuf_slab_freelist *freelist)
{
	struct cbuf_slab *s = malloc(sizeof(struct cbuf_slab)), *ret = NULL;
	void *h;
	int cbid;

	if (!s) return NULL;
	/* FIXME: race race race */
	h = cos_get_heap_ptr();
	cos_set_heap_ptr(h + PAGE_SIZE);
	cbid = cbuf_c_create(cos_spd_id(), size, h);
	if (cbid < 0) goto err;
	cos_vect_add_id(&slab_descs, s, cbid);
	cbuf_slab_cons(s, cbid, h, size, freelist);
	freelist->velocity = 0;
	ret = s;
done:   
	return ret;
err:    
	/* FIXME: race race race */
	cos_set_heap_ptr(h);
	free(s);
	goto done;
}

void
cbuf_slab_free(struct cbuf_slab *s)
{
	struct cbuf_slab_freelist *freelist;

	/* FIXME: soooo many races */
	freelist = s->flh;
	assert(freelist);
	freelist->velocity--;
	if (freelist->velocity < SLAB_VELOCITY_THRESH) return;

	/* Have we freed the configured # in a row? Return the page. */
	if (freelist->list == s) {
		freelist->list = s->next;
	} else {
		assert(s->prev);
		s->prev->next = s->next;
	}
	if (s->next) s->next->prev = NULL;
	assert(s->nfree = (PAGE_SIZE/s->obj_sz));
	
	/* FIXME: reclaim heap VAS! */
	cos_vect_del(&slab_descs, s->cbid);
	cbuf_c_delete(cos_spd_id(), s->cbid);
	free(s);

	return;
}
