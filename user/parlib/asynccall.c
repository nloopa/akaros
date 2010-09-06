#include <stdlib.h>

#include <ros/common.h>
#include <ros/syscall.h>
#include <ros/ring_syscall.h>
#include <ros/sysevent.h>
#include <arc.h>
#include <errno.h>
#include <arch/arch.h>
#include <sys/param.h>

syscall_front_ring_t syscallfrontring;
sysevent_back_ring_t syseventbackring;
syscall_desc_pool_t syscall_desc_pool;
async_desc_pool_t async_desc_pool;
async_desc_t* current_async_desc;

// use globals for now
void init_arc()
{
	// Set up the front ring for the general syscall ring
	// and the back ring for the general sysevent ring
	// TODO: Reorganize these global variables
	FRONT_RING_INIT(&syscallfrontring, &(__procdata.syscallring), SYSCALLRINGSIZE);
	BACK_RING_INIT(&syseventbackring, &(__procdata.syseventring), SYSEVENTRINGSIZE);
	POOL_INIT(&syscall_desc_pool, MAX_SYSCALLS);
	POOL_INIT(&async_desc_pool, MAX_ASYNCCALLS);
	sys_init_arsc();


}
// Wait on all syscalls within this async call.  TODO - timeout or something?
int waiton_async_call(async_desc_t* desc, async_rsp_t* rsp)
{
	syscall_rsp_t syscall_rsp;
	syscall_desc_t* d;
	int err = 0;
	if (!desc) {
		errno = EINVAL;
		return -1;
	}

	while (!(TAILQ_EMPTY(&desc->syslist))) {
		d = TAILQ_FIRST(&desc->syslist);
		err = waiton_syscall(d, &syscall_rsp);
		// TODO: processing the retval out of rsp here.  might be specific to
		// the async call.  do we want to accumulate?  return any negative
		// values?  depends what we want from the return value, so we might
		// have to pass in a function that is used to do the processing and
		// pass the answer back out in rsp.
		//rsp->retval += syscall_rsp.retval; // For example
		rsp->retval = MIN(rsp->retval, syscall_rsp.retval);
		// remove from the list and free the syscall desc
		TAILQ_REMOVE(&desc->syslist, d, next);
		POOL_PUT(&syscall_desc_pool, d);
	}
	// run a cleanup function for this desc, if available
	if (desc->cleanup)
		desc->cleanup(desc->data);
	// free the asynccall desc
	POOL_PUT(&async_desc_pool, desc);
	return err;
}

// Finds a free async_desc_t, on which you can wait for a series of syscalls
async_desc_t* get_async_desc(void)
{
	async_desc_t* desc = POOL_GET(&async_desc_pool);
	if (desc) {
		// Clear out any data that was in the old desc
		memset(desc, 0, sizeof(*desc));
		TAILQ_INIT(&desc->syslist);
	}
	return desc;
}

// Finds a free sys_desc_t, on which you can wait for a specific syscall, and
// binds it to the group desc.
syscall_desc_t* get_sys_desc(async_desc_t* desc)
{
	syscall_desc_t* d = POOL_GET(&syscall_desc_pool);
	if (d) {
		// Clear out any data that was in the old desc
		memset(d, 0, sizeof(*d));
    	TAILQ_INSERT_TAIL(&desc->syslist, d, next);
	}
	return d;
}

// Gets an async and a sys desc, with the sys bound to async.  Also sets
// current_async_desc.  This is meant as an easy wrapper when there is only one
// syscall for an async call.
int get_all_desc(async_desc_t** a_desc, syscall_desc_t** s_desc)
{
	assert(a_desc && s_desc);
	if ((current_async_desc = get_async_desc()) == NULL){
		errno = EBUSY;
		return -1;
	}
	*a_desc = current_async_desc;
	if ((*s_desc = get_sys_desc(current_async_desc)))
		return 0;
	// in case we could get an async, but not a syscall desc, then clean up.
	POOL_PUT(&async_desc_pool, current_async_desc);
	current_async_desc = NULL;
	errno = EBUSY;
	return -1;
}

// This runs one syscall instead of a group. 
int async_syscall(syscall_req_t* req, syscall_desc_t* desc)
{
	// Note that this assumes one global frontring (TODO)
	// abort if there is no room for our request.  ring size is currently 64.
	// we could spin til it's free, but that could deadlock if this same thread
	// is supposed to consume the requests it is waiting on later.
	if (RING_FULL(&syscallfrontring)) {
		errno = EBUSY;
		return -1;
	}
	// req_prod_pvt comes in as the previously produced item.  need to
	// increment to the next available spot, which is the one we'll work on.
	// at some point, we need to listen for the responses.
	desc->idx = ++(syscallfrontring.req_prod_pvt);
	desc->sysfr = &syscallfrontring;
	syscall_req_t* r = RING_GET_REQUEST(&syscallfrontring, desc->idx);
	memcpy(r, req, sizeof(syscall_req_t));
	// push our updates to syscallfrontring.req_prod_pvt
	RING_PUSH_REQUESTS(&syscallfrontring);
	//cprintf("DEBUG: sring->req_prod: %d, sring->rsp_prod: %d\n", 
	//   syscallfrontring.sring->req_prod, syscallfrontring.sring->rsp_prod);
	return 0;
}

// consider a timeout too
int waiton_syscall(syscall_desc_t* desc, syscall_rsp_t* rsp)
{
	// Make sure we were given a desc with a non-NULL frontring.  This could
	// happen if someone forgot to check the error code on the paired syscall.
	if (!desc->sysfr){
		errno = EFAIL;
		return -1;
	}
	// this forces us to call wait in the order in which the syscalls are made.
	if (desc->idx != desc->sysfr->rsp_cons + 1){
		errno = EDEADLOCK;
		return -1;
	}
	while (!(RING_HAS_UNCONSUMED_RESPONSES(desc->sysfr)))
		cpu_relax();
	memcpy(rsp, RING_GET_RESPONSE(desc->sysfr, desc->idx), sizeof(*rsp));
	desc->sysfr->rsp_cons++;
    // run a cleanup function for this desc, if available
    if (desc->cleanup)
    	desc->cleanup(desc->data);
	if (rsp->syserr){
		errno = rsp->syserr;
		return -1;
	} else 
		return 0;
}

