#include <cos_component.h>
#include <print.h>
#include <cos_component.h>
#include <res_spec.h>
#include <sched.h>

#include <periodic_wake.h>
#include <timed_blk.h>

#include <swifi.h>

int high;
unsigned long counter = 0;

//#define TARGET_COMPONENT 15   /* ramfs (turn off some_delay in mm!!!), also have the client just looping */
#define TARGET_COMPONENT  3   /* mm (turn on some_delay in mm, should not turn on when normal operation) */
//#define TARGET_COMPONENT  2   /* sched */

#ifndef TARGET_COMPONENT   
#define TARGET_COMPONENT 0    	/* no fault injection */
#endif

int fault_inject()
{
	int ret = 0;
	int tid, spdid;

	printc("\nthread %d in fault injector %ld\n\n", cos_get_thd_id(), cos_spd_id());

	if (TARGET_COMPONENT == 0) return 0;
	
	struct cos_regs r;
	for (tid = 1; tid <= MAX_NUM_THREADS; tid++) {
		spdid = cos_thd_cntl(COS_THD_INV_SPD, tid, TARGET_COMPONENT, 0);
		if (spdid == -1) continue;

		/* printc("found thd %d and spd %d\n", tid, spdid); */
		counter++;
		printc("<<%lu>> flip the register in component %d!!!\n", counter, TARGET_COMPONENT);
		cos_regs_read(tid, spdid, &r);
		/* cos_regs_print(&r); */
		flip_all_regs(&r);
		/* cos_regs_print(&r); */
	}
	return 0;
}


void cos_init(void)
{
	static int first = 0;
	union sched_param sp;
	int rand;

	if(first == 0){
		first = 1;

		sp.c.type = SCHEDP_PRIO;
		sp.c.value = 4;
		high = sched_create_thd(cos_spd_id(), sp.v, 0, 0);
	} else {
#ifdef SWIFI_ENABLE
		if (cos_get_thd_id() == high) {
			periodic_wake_create(cos_spd_id(), 5);
			while(1) {
				fault_inject();
				periodic_wake_wait(cos_spd_id());
			}
		}
#endif
	}
}
