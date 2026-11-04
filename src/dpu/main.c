#include <common.h>
#include <stdint.h>
#include <assert.h>
#include <mram.h>
#include <defs.h>
#include <alloc.h>
#include <barrier.h>
#include <perfcounter.h>

extern void KERNEL_FUNC(sysname_t tasklet_id);

__host volatile uint8_t OFIS_dpu_state;

BARRIER_INIT(my_barrier, NR_TASKLETS);
BARRIER_INIT(exec_barrier, NR_TASKLETS);
BARRIER_INIT(flag_barrier, NR_TASKLETS);

int main() {
    sysname_t tid = me();
	OFIS_dpu_state = 2;
#ifdef OFIS_MODE
	while(OFIS_dpu_state != 0){
		while(OFIS_dpu_state != 2){
			if(OFIS_dpu_state == 0) return 0;
		} 	// wait for host signal
	  		if (tid == 0) {
	       		mem_reset();
#ifdef PERF
	        	perfcounter_config(COUNT_CYCLES, true);
#endif
	    	}
	    	barrier_wait(&my_barrier);
	    	KERNEL_FUNC(tid);
	    	barrier_wait(&exec_barrier);
	    	if (tid == 0) {
                OFIS_dpu_state = 1;
            }
	    	barrier_wait(&flag_barrier);
		}
#endif
#ifndef OFIS_MODE
	if (tid == 0) {
		mem_reset();
#ifdef DPU_LOG
		printf("just for log\n");
#endif
#ifdef PERF
		perfcounter_config(COUNT_CYCLES, true);
#endif
	}
	barrier_wait(&my_barrier);

	KERNEL_FUNC(tid);
#endif
    return 0;
}
