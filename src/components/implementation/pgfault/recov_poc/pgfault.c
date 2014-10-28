/**
 * Copyright 2011 by The George Washington University.  All rights reserved.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2011
 */

#include <cos_component.h>
#include <pgfault.h>
#include <sched.h>
#include <print.h>
#include <fault_regs.h>

#include <failure_notif.h>

/* FIXME: should have a set of saved fault regs per thread. */
// when test mailbox, why spdid is 14, but reg->bx and cx are 7?
int fault_flt_notif_handler(spdid_t spdid, void *fault_addr, int flags, void *ip)
{
	unsigned long r_ip; 	/* the ip to return to */
	printc("pgfault notifier: spdid %d fault_addr %p flags %d ip %p (thd %d)\n", spdid, fault_addr, flags, ip, cos_get_thd_id());

	int tid = cos_get_thd_id();

	/* if (spdid != cos_thd_cntl(COS_THD_HOME_SPD, tid, 0, 0)) { */
	assert(!cos_thd_cntl(COS_THD_INV_FRAME_REM, tid, 1, 0));
	assert(r_ip = cos_thd_cntl(COS_THD_INVFRM_IP, tid, 1, 0));
	assert(!cos_thd_cntl(COS_THD_INVFRM_SET_IP, tid, 1, r_ip-8));
	/* } else { */


	// if this is timer thread, just block!!!!

	/* if (spdid == cos_thd_cntl(COS_THD_HOME_SPD, tid, 0, 0)) { */
	/* 	printc("set thd %d 's eip (find next)\n", tid); */
	/* 	/\* a temporary solution for now, when the thread */
	/* 	 * faults in its home component. Block the current thd */
	/* 	 * and run the regenerated thread in the same */
	/* 	 * spd. TODO: kill this thread instead *\/ */
	/* 	sched_block(cos_spd_id(), 0); */
	/* } */

	return 0;
}


static int test_num = 0;

int fault_page_fault_handler(spdid_t spdid, void *fault_addr, int flags, void *ip)
{
	unsigned long r_ip; 	/* the ip to return to */
	int tid = cos_get_thd_id();
	//int i;

	printc("pgfault parameters: spdid %d fault_addr %p flags %d ip %p (thd %d)\n", spdid, fault_addr, flags, ip, cos_get_thd_id());

	if (test_num++ > 5) {
		printc("has failed %d times\n", test_num);
		assert(0);
	}

	printc("in the fault_page_fault_handler 1\n");
	/* printc("Thread %d faults in spd %d @ %p\n", tid, spdid, fault_addr); */
	if (spdid != cos_thd_cntl(COS_THD_HOME_SPD, tid, 0, 0)) {
		/* remove from the invocation stack the faulting component! */
		assert(!cos_thd_cntl(COS_THD_INV_FRAME_REM, tid, 1, 0));

		printc("in the fault_page_fault_handler 2\n");
		/* Manipulate the return address of the component that called
		 * the faulting component... */
		assert(r_ip = cos_thd_cntl(COS_THD_INVFRM_IP, tid, 1, 0));
		/* ...and set it to its value -8, which is the fault handler
		 * of the stub. */
		assert(!cos_thd_cntl(COS_THD_INVFRM_SET_IP, tid, 1, r_ip-8));
		
		/* increase fault counter in component data structure */
		assert(!cos_fault_cntl(COS_SPD_FAULT_TRIGGER, spdid, 0));
	} else {  // home component
                /* increase fault counter in component data structure */
		assert(!cos_fault_cntl(COS_SPD_FAULT_TRIGGER, spdid, 0));
	}
	
	/* 
	 * Look at the booter: when recover is happening, the sstub is
	 * set to 0x1, thus we should just wait till recovery is done.
	 */
	if ((int)ip == 1) failure_notif_wait(cos_spd_id(), spdid);
	else         failure_notif_fail(cos_spd_id(), spdid);
	
	return 0;
}
