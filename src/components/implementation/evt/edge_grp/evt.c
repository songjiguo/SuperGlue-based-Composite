/**
 * Copyright 2008 by Boston University.  All rights reserved.
 * Copyright 2011 by The George Washington University.  All rights reserved.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Author: Gabriel Parmer, gabep1@cs.bu.edu, 2008.
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2011.
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2012: add real groups.
 */

#define COS_FMT_PRINT

#include <cos_synchronization.h>
#include <cos_component.h>
#include <cos_alloc.h>
#include <cos_debug.h>
#include <cos_list.h>
#include <print.h>
#include <cmap.h>
#include <errno.h>
#include <evt.h>
#include <sched.h>

#include <name_server.h>

static int test_num = 0;

typedef enum {
	EVT_GROUP, 
	EVT_NORMAL
} evt_t;

typedef enum {
	EVT_BLOCKED   = 0x1,    /* a thread is blocked on this event */
	EVT_TRIGGERED = 0x2 	/* has the event been triggered */
} evt_status_t;

struct evt {
	evt_t type;
	evt_status_t status;
	long eid;
	struct evt *grp;
	struct evt *iachildren, *tchildren; /* inactive and triggered children */
	struct evt *next, *prev;
	u16_t bthd;	                    /* blocked thread */
	spdid_t creator;
};

#define CSLAB_ALLOC(sz)   alloc_page()
#define CSLAB_FREE(x, sz) free_page(x)
#include <cslab.h>
CSLAB_CREATE(evt, sizeof(struct evt));
/* A mapping between event ids and actual events */
/* CMAP_CREATE_STATIC(evt_map); */
cos_lock_t evt_lock;

CVECT_CREATE_STATIC(evt_map);  // Jiguo: use name server to get id

static struct evt *
__evt_alloc(evt_t t, long parent, spdid_t spdid)
{
	struct evt *e = NULL, *p = NULL;

	if (parent) {
		p = cvect_lookup(&evt_map, parent);
		/* p = cmap_lookup(&evt_map, parent); */  // Jiguo
		if (!p)                   goto done;
		if (p->creator != spdid)  goto done;
		if (p->type != EVT_GROUP) goto done;
	}
	e = cslab_alloc_evt();
	if (!e) goto done;
	memset(e, 0, sizeof(struct evt));

	e->type = t;
	INIT_LIST(e, next, prev);
	e->grp  = p;
	e->creator = spdid;
	if (p) {
		if (p->iachildren) ADD_LIST(p->iachildren, e, next, prev);
		else               p->iachildren = e;
	}
	/* e->eid = cmap_add(&evt_map, e); */   //Jiguo
	e->eid = ns_alloc(spdid);
	assert(e->eid > 0);
	cvect_add(&evt_map, e, e->eid);
done:
	return e;
}

/* 
 * This does _not_ free all children of a freed group.  They have to
 * be freed individually.
 */
static int
__evt_free(spdid_t spdid, long eid)
{
	struct evt *e, *c, *f;

	/* e = cmap_lookup(&evt_map, eid); */  // Jiguo
	e = cvect_lookup(&evt_map, eid);

	if (!e)                  return -EINVAL;
	if (e->creator != spdid) return -EACCES;
	if (e->bthd)             return -EAGAIN;

	if (e->iachildren) {
		f = c = FIRST_LIST(e->iachildren, next, prev);
		do {
			c->grp = NULL;
			REM_LIST(c, next, prev);
		} while (f != (c = FIRST_LIST(e->iachildren, next, prev)));
		e->iachildren = NULL;
	}
	if (e->tchildren) {
		f = c = FIRST_LIST(e->tchildren, next, prev);
		do {
			c->grp = NULL;
			REM_LIST(c, next, prev);
		} while (f != (c = FIRST_LIST(e->tchildren, next, prev)));
		e->tchildren = NULL;
	}
	
	REM_LIST(e, next, prev);
	/* cmap_del(&evt_map, eid); */  // Jiguo
	cvect_del(&evt_map, eid);

	cslab_free_evt(e);

	return 0;
}

/* 
 * Trigger the most specific group with a thread blocked waiting, or
 * the most generic otherwise.
 *
 * Return > 0 for the thread to wake up, 0 for no wakeup, and < 0 for
 * error.
 */
static inline long
__evt_trigger(spdid_t spdid, long eid)
{
	struct evt *e, *g, *t = NULL;

	/* e = cmap_lookup(&evt_map, eid); */  // Jiguo
	e = cvect_lookup(&evt_map, eid);

	/* can't trigger groups */
	if (!e || e->type != EVT_NORMAL) return -EINVAL;
	/* go up the tree toward the root... */
	for (g = e ; g ; g = g->grp) {
		g->status |= EVT_TRIGGERED;
		/* add ourselves to the triggered list of our parent,
		 * and propagate the event up. */
		if (g->grp) {
			REM_LIST(g, next, prev);
			/* FIFO event delivery */
			if (!g->grp->tchildren) g->grp->tchildren = g;
			else ADD_END_LIST(g->grp->tchildren, g, next, prev); 
		}
		if (!t && (g->status & EVT_BLOCKED || !g->grp)) t = g;
	}
	assert(t);
	t->status |= EVT_TRIGGERED;
	if (t->status & EVT_BLOCKED) {
		u16_t tid = t->bthd;
		assert(tid);

		t->status &= ~EVT_BLOCKED;
		t->bthd    = 0;
		return tid;
	}
	assert(!t->bthd);
	return 0;
}

/* 
 * Return the event id if one has been triggered, otherwise 0 and the
 * thread should be blocked, only to retry this operation when woken.
 * Negative values denote error values (errno).
 *
 * Note: only one thread can block waiting for a specific event at any
 * time.
 */
static inline long
__evt_wait(spdid_t spdid, long eid)
{
	struct evt *e, *g, *c, *t;

	/* e = cmap_lookup(&evt_map, eid); */  // Jiguo
	e = cvect_lookup(&evt_map, eid);

	if (!e)                  return -EINVAL;
	if (e->bthd)             return -EAGAIN; /* another thread already blocked? */
	if (e->creator != spdid) return -EINVAL;   // whey do we need this?  Jiguo
	assert(!(e->status & EVT_BLOCKED));
	assert(e->eid);

	if (!(e->status & EVT_TRIGGERED)) {
		e->status |= EVT_BLOCKED;
		e->bthd = cos_get_thd_id();
		return 0;
	}
	
	if (!e->tchildren) {
		t = e;
	} else { /* find the "bottom" triggered child */
		for (c = e->tchildren ; c->tchildren ; c = c->tchildren) ;
		t = c;
	}
	t->status &= ~EVT_TRIGGERED;
	/* go up from the child, removing "triggered" where appropriate */
	for (g = t->grp ; g ; g = g->grp) {
		struct evt *r, *f;
		int more = 1;
			
		assert(g->tchildren);
		r = g->tchildren;
		if (EMPTY_LIST(r, next, prev)) {
			REM_LIST(r, next, prev);
			g->tchildren = NULL;
			g->status &= ~EVT_TRIGGERED;
		} else {
			f = FIRST_LIST(r, next, prev);
			REM_LIST(r, next, prev);
			g->tchildren = f;
			more = 0;
		}
		if (g->iachildren) ADD_LIST(g->iachildren, r, next, prev);
		else               g->iachildren = r;
		
		if (!more) break;
	}
	return t->eid;
} 

static long 
__evt_split(spdid_t spdid, long parent, int group, int old_evtid)
{
	struct evt *e;
	long ret = -ENOMEM;

	lock_take(&evt_lock);

	e = __evt_alloc(group ? EVT_GROUP : EVT_NORMAL, parent, spdid);
	if (!e) goto done;
	ret = e->eid;
	assert(ret > 0);

#if (RECOVERY_ENABLE == 1) && defined(TEST_EVT_SPLIT)
	if (spdid == 11 && cos_get_thd_id() == 11 && test_num++ > 2) {
		printc("trigger fault in evt_split: thd %d spd %ld passed spd %d\n", cos_get_thd_id(), cos_spd_id(), spdid);
		assert(0);
	}
#endif

	if (unlikely(old_evtid)) {
		assert(old_evtid != e->eid);
		if (ns_update(cos_spd_id(), old_evtid, e->eid, 0)) assert(0);
	}
	
	lock_release(&evt_lock);
done:
	return ret;
}

long evt_split(spdid_t spdid, long parent, int group)
{
	return __evt_split(spdid, parent, group, 0);
}

long c3_evt_split(spdid_t spdid, long parent, int group, int old_evtid)
{
	return __evt_split(spdid, parent, group, old_evtid);
}

void evt_free(spdid_t spdid, long evt_id)
{
	int ret;

	lock_take(&evt_lock);

#if (RECOVERY_ENABLE == 1) && defined(TEST_EVT_FREE)
	if (spdid == 11 && cos_get_thd_id() == 11 && test_num++ > 2) {
		printc("trigger fault in evt_free: thd %d spd %ld passed spd %d\n", cos_get_thd_id(), cos_spd_id(), spdid);
		assert(0);
	}
#endif

	ret = __evt_free(spdid, evt_id);
	
	ns_free(cos_spd_id(), evt_id);   // Jiguo: free must be in the same spd as split
	
	lock_release(&evt_lock);
	return;
}

long evt_wait_n(spdid_t spdid, long evt_id, int n) {
	assert(0);
	return -1;
}

long evt_wait(spdid_t spdid, long evt_id)
{
	long ret;

	do {
		lock_take(&evt_lock);

#if (RECOVERY_ENABLE == 1) && defined(TEST_EVT_WAIT_BEFORE)
		if (spdid == 11 && cos_get_thd_id() == 11 && test_num++ > 3) {
			printc("trigger fault in evt_wait (test_num %d): thd %d spd %ld passed spd %d\n", test_num, cos_get_thd_id(), cos_spd_id(), spdid);
			assert(0);
		}
#endif


		ret = __evt_wait(spdid, evt_id);
		lock_release(&evt_lock);
		if (!ret && 0 > sched_block(cos_spd_id(), 0)) BUG();

#if (RECOVERY_ENABLE == 1) && defined(TEST_EVT_WAIT_AFTER)
			if (spdid == 11 && cos_get_thd_id() == 11 && test_num++ > 3) {
				printc("trigger fault in evt_wait (test_num %d): thd %d spd %ld passed spd %d\n", test_num, cos_get_thd_id(), cos_spd_id(), spdid);
				assert(0);
			}
#endif

	} while (!ret);
	return ret; 
}

int
evt_trigger(spdid_t spdid, long evt_id)
{
	int ret;
	lock_take(&evt_lock);

#if (RECOVERY_ENABLE == 1) && defined(TEST_EVT_TRIGGER_BEFORE)
	if (spdid == 13 && cos_get_thd_id() == 12 && test_num++ > 2) {
	printc("trigger fault in evt_trigger: thd %d passed spd %d (evt %d)\n",
	       cos_get_thd_id(), spdid, evt_id);
		assert(0);
	}
#endif

	/* Jiguo: evt_trigger can be called from a different spd where
	 * the id mapping is not tracked. We add this additional
	 * invocation to the name server here to get the actual server
	 * side id --- TODO: use cache to speed up this process at the
	 * price of more data bookkeeping here */
	int actual_evtid = ns_lookup(cos_spd_id(), evt_id);
	/* printc("ns_lookup return id %d\n", actual_evtid); */
	assert(actual_evtid >= 1);  // the event has been freed by the woken thread ?
	ret = __evt_trigger(spdid, actual_evtid);

	/* ret = __evt_trigger(spdid, evt_id); */  // Jiguo
	lock_release(&evt_lock);
	if (ret && sched_wakeup(cos_spd_id(), ret)) BUG();

#if (RECOVERY_ENABLE == 1) && defined(TEST_EVT_TRIGGER_AFTER)
	if (spdid == 13 && cos_get_thd_id() == 12 && test_num++ > 2) {
		printc("trigger fault in evt_trigger: thd %d passed spd %d (evt %d)\n",
		       cos_get_thd_id(), spdid, evt_id);
		assert(0);
	}
#endif

	return 0;
}

static void rd_reflection();

void 
cos_init(void)
{
	lock_static_init(&evt_lock);

	/* cmap_init_static(&evt_map); */  // Jiguo
	/* assert(0 == cmap_add(&evt_map, (void*)1)); */  // Jiguo

	cvect_init_static(&evt_map);

	// Jiguo: rdreflection
	if (unlikely(cos_fault_cntl(COS_CAP_REFLECT_UPDATE, cos_spd_id(), 0))) {
		printc("\n\nneed do reflection now!!!!\n");
		rd_reflection();
		printc("\nreflection done!!!\n");
	}
}

long
evt_create(spdid_t spdid) { return -1; }

long 
evt_grp_create(spdid_t spdid, long parent) { return -1; }

int 
evt_set_prio(spdid_t spdid, long extern_evt, int prio) { return -1; }

/* Wait on a group of events (like epoll) */
long evt_grp_wait(spdid_t spdid) { return -1; }

/* As above, but return more than one event notifications */
int 
evt_grp_mult_wait(spdid_t spdid, struct cos_array *data) { return -1; }

unsigned long *evt_stats(spdid_t spdid, unsigned long *stats) { return NULL; }
int evt_stats_len(spdid_t spdid) { return 0; }

long evt_reflection(spdid_t spdid, long extern_evt) { return 0;}
int evt_trigger_all(spdid_t spdid) {return 0;}


extern int sched_reflect(spdid_t spdid, int src_spd, int cnt);
extern int sched_wakeup(spdid_t spdid, unsigned short int thd_id);
extern vaddr_t mman_reflect(spdid_t spd, int src_spd, int cnt);
extern int mman_release_page(spdid_t spd, vaddr_t addr, int flags); 
extern int lock_trigger_all(spdid_t spdid, int dest);  

static void
rd_reflection()
{
	int count_obj = 0; // reflected objects
	int dest_spd = cos_spd_id();
	
	// remove the mapped page for evt spd
	vaddr_t addr;
	count_obj = mman_reflect(cos_spd_id(), dest_spd, 1);
	/* printc("evt relfects on mmgr: %d objs\n", count_obj); */
	while (count_obj--) {
		addr = mman_reflect(cos_spd_id(), dest_spd, 0);
		/* printc("evt mman_release: %p addr\n", (void *)addr); */
		mman_release_page(cos_spd_id(), addr, dest_spd);
	}

	// to reflect all threads blocked from evt component, such as evt_wait
	int wake_thd;
	count_obj = sched_reflect(cos_spd_id(), dest_spd, 1);
	/* printc("evt relfects on sched: %d objs\n", count_obj); */
	while (count_obj--) {
		wake_thd = sched_reflect(cos_spd_id(), dest_spd, 0);
		/* printc("wake_thd %d\n", wake_thd); */
		/* evt_trigger(cos_spd_id(), evt_id); */  // pointless to call evt mgr
		sched_wakeup(cos_spd_id(), wake_thd);
	}

        //TODO: reflection on lock manager as well
	lock_trigger_all(cos_spd_id(), cos_spd_id());

	return;
}
