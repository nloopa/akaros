#include <ros/arch/trapframe.h>
#include <pthread.h>
#include <vcore.h>
#include <mcs.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <rstdio.h>
#include <errno.h>
#include <parlib.h>
#include <ros/event.h>
#include <arch/atomic.h>
#include <arch/arch.h>
#include <sys/queue.h>
#include <sys/mman.h>
#include <assert.h>
#include <event.h>

struct pthread_queue ready_queue = TAILQ_HEAD_INITIALIZER(ready_queue);
struct pthread_queue active_queue = TAILQ_HEAD_INITIALIZER(active_queue);
mcs_lock_t queue_lock = MCS_LOCK_INIT;
pthread_once_t init_once = PTHREAD_ONCE_INIT;
int threads_ready = 0;
int threads_active = 0;

/* Helper / local functions */
static int get_next_pid(void);
static inline void spin_to_sleep(unsigned int spins, unsigned int *spun);

/* Pthread 2LS operations */
struct uthread *pth_init(void);
void pth_sched_entry(void);
struct uthread *pth_thread_create(void (*func)(void), void *udata);
void pth_thread_runnable(struct uthread *uthread);
void pth_thread_yield(struct uthread *uthread);
void pth_thread_exit(struct uthread *uthread);
void pth_preempt_pending(void);
void pth_spawn_thread(uintptr_t pc_start, void *data);

struct schedule_ops pthread_sched_ops = {
	pth_init,
	pth_sched_entry,
	pth_thread_create,
	pth_thread_runnable,
	pth_thread_yield,
	pth_thread_exit,
	0, /* pth_preempt_pending, */
	0, /* pth_spawn_thread, */
};

/* Publish our sched_ops, overriding the weak defaults */
struct schedule_ops *sched_ops = &pthread_sched_ops;

/* Static helpers */
static void __pthread_free_stack(struct pthread_tcb *pt);
static int __pthread_allocate_stack(struct pthread_tcb *pt);

/* Do whatever init you want.  Return a uthread representing thread0 (int
 * main()) */
struct uthread *pth_init(void)
{
	struct mcs_lock_qnode local_qn = {0};
	/* Tell the kernel where and how we want to receive events.  This is just an
	 * example of what to do to have a notification turned on.  We're turning on
	 * USER_IPIs, posting events to vcore 0's vcpd, and telling the kernel to
	 * send to vcore 0.  Note sys_self_notify will ignore the vcoreid pref.
	 * Also note that enable_kevent() is just an example, and you probably want
	 * to use parts of event.c to do what you want. */
	enable_kevent(EV_USER_IPI, 0, EVENT_IPI);

	/* Create a pthread_tcb for the main thread */
	pthread_t t = (pthread_t)calloc(1, sizeof(struct pthread_tcb));
	t->id = get_next_pid();
	assert(t->id == 0);

	/* Put the new pthread on the active queue */
	mcs_lock_lock(&queue_lock, &local_qn);
	threads_active++;
	TAILQ_INSERT_TAIL(&active_queue, t, next);
	mcs_lock_unlock(&queue_lock, &local_qn);
	return (struct uthread*)t;
}

/* Called from vcore entry.  Options usually include restarting whoever was
 * running there before or running a new thread.  Events are handled out of
 * event.c (table of function pointers, stuff like that). */
void pth_sched_entry(void)
{
	if (current_uthread) {
		run_current_uthread();
		assert(0);
	}
	/* no one currently running, so lets get someone from the ready queue */
	struct pthread_tcb *new_thread = NULL;
	struct mcs_lock_qnode local_qn = {0};
	mcs_lock_lock(&queue_lock, &local_qn);
	new_thread = TAILQ_FIRST(&ready_queue);
	if (new_thread) {
		TAILQ_REMOVE(&ready_queue, new_thread, next);
		TAILQ_INSERT_TAIL(&active_queue, new_thread, next);
		threads_active++;
		threads_ready--;
	}
	mcs_lock_unlock(&queue_lock, &local_qn);
	/* For now, this dumb logic is done here */
	if (!new_thread) {
		/* TODO: consider doing something more intelligent here */
		printd("[P] No threads, vcore %d is yielding\n", vcore_id());
		sys_yield(0);
		assert(0);
	}
	run_uthread((struct uthread*)new_thread);
	assert(0);
}

/* Could move this, along with start_routine and arg, into the 2LSs */
static void __pthread_run(void)
{
	struct pthread_tcb *me = pthread_self();
	pthread_exit(me->start_routine(me->arg));
}

/* Responible for creating the uthread and initializing its user trap frame */
struct uthread *pth_thread_create(void (*func)(void), void *udata)
{
	struct pthread_tcb *pthread;
	pthread_attr_t *attr = (pthread_attr_t*)udata;
	pthread = (pthread_t)calloc(1, sizeof(struct pthread_tcb));
	pthread->stacksize = PTHREAD_STACK_SIZE;	/* default */
	pthread->id = get_next_pid();
	pthread->detached = FALSE;				/* default */
	/* Respect the attributes */
	if (attr) {
		if (attr->stacksize)					/* don't set a 0 stacksize */
			pthread->stacksize = attr->stacksize;
		if (attr->detachstate == PTHREAD_CREATE_DETACHED)
			pthread->detached = TRUE;
	}
	/* allocate a stack */
	if (__pthread_allocate_stack(pthread))
		printf("We're fucked\n");
	/* Set the u_tf to start up in __pthread_run, which will call the real
	 * start_routine and pass it the arg.  Note those aren't set until later in
	 * pthread_create(). */
	init_user_tf(&pthread->uthread.utf, (uint32_t)__pthread_run, 
                 (uint32_t)(pthread->stacktop));
	return (struct uthread*)pthread;
}

void pth_thread_runnable(struct uthread *uthread)
{
	struct pthread_tcb *pthread = (struct pthread_tcb*)uthread;
	struct mcs_lock_qnode local_qn = {0};
	/* Insert the newly created thread into the ready queue of threads.
	 * It will be removed from this queue later when vcore_entry() comes up */
	mcs_lock_lock(&queue_lock, &local_qn);
	TAILQ_INSERT_TAIL(&ready_queue, pthread, next);
	threads_ready++;
	mcs_lock_unlock(&queue_lock, &local_qn);
}

/* The calling thread is yielding.  Do what you need to do to restart (like put
 * yourself on a runqueue), or do some accounting.  Eventually, this might be a
 * little more generic than just yield. */
void pth_thread_yield(struct uthread *uthread)
{
	struct pthread_tcb *pthread = (struct pthread_tcb*)uthread;
	struct mcs_lock_qnode local_qn = {0};
	/* Take from the active list, and put on the ready list (tail).  Don't do
	 * this until we are done completely with the thread, since it can be
	 * restarted somewhere else. */
	mcs_lock_lock(&queue_lock, &local_qn);
	threads_active--;
	TAILQ_REMOVE(&active_queue, pthread, next);
	threads_ready++;
	TAILQ_INSERT_TAIL(&ready_queue, pthread, next);
	mcs_lock_unlock(&queue_lock, &local_qn);
}

/* Thread is exiting, do your 2LS specific stuff.  You're in vcore context.
 * Don't use the thread's TLS or stack or anything. */
void pth_thread_exit(struct uthread *uthread)
{
	struct pthread_tcb *pthread = (struct pthread_tcb*)uthread;
	struct mcs_lock_qnode local_qn = {0};
	/* Remove from the active runqueue */
	mcs_lock_lock(&queue_lock, &local_qn);
	threads_active--;
	TAILQ_REMOVE(&active_queue, pthread, next);
	mcs_lock_unlock(&queue_lock, &local_qn);
	/* Cleanup, mirroring pth_thread_create() */
	__pthread_free_stack(pthread);
	/* TODO: race on detach state */
	if (pthread->detached)
		free(pthread);
	/* Once we do this, our joiner can free us.  He won't free us if we're
	 * detached, but there is still a potential race there (since he's accessing
	 * someone who is freed. */
	pthread->finished = 1;
}

void pth_preempt_pending(void)
{
}

void pth_spawn_thread(uintptr_t pc_start, void *data)
{
}

/* Pthread interface stuff and helpers */

int pthread_attr_init(pthread_attr_t *a)
{
 	a->stacksize = PTHREAD_STACK_SIZE;
	a->detachstate = PTHREAD_CREATE_JOINABLE;
  	return 0;
}

int pthread_attr_destroy(pthread_attr_t *a)
{
	return 0;
}

static void __pthread_free_stack(struct pthread_tcb *pt)
{
	assert(!munmap(pt->stacktop - PTHREAD_STACK_SIZE, PTHREAD_STACK_SIZE));
}

static int __pthread_allocate_stack(struct pthread_tcb *pt)
{
	assert(pt->stacksize);
	void* stackbot = mmap(0, pt->stacksize,
	                      PROT_READ|PROT_WRITE|PROT_EXEC,
	                      MAP_POPULATE|MAP_ANONYMOUS, -1, 0);
	if (stackbot == MAP_FAILED)
		return -1; // errno set by mmap
	pt->stacktop = stackbot + pt->stacksize;
	return 0;
}

// Warning, this will reuse numbers eventually
static int get_next_pid(void)
{
	static uint32_t next_pid = 0;
	return next_pid++;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
	attr->stacksize = stacksize;
	return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
	*stacksize = attr->stacksize;
	return 0;
}

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void *(*start_routine)(void *), void* arg)
{
	struct pthread_tcb *pthread =
	       (struct pthread_tcb*)uthread_create(__pthread_run, (void*)attr);
	if (!pthread)
		return -1;
	pthread->start_routine = start_routine;
	pthread->arg = arg;
	uthread_runnable((struct uthread*)pthread);
	*thread = pthread;
	return 0;
}

int pthread_join(pthread_t thread, void** retval)
{
	/* Not sure if this is the right semantics.  There is a race if we deref
	 * thread and he is already freed (which would have happened if he was
	 * detached. */
	if (thread->detached) {
		printf("[pthread] trying to join on a detached pthread");
		return -1;
	}
	while (!thread->finished)
		pthread_yield();
	if (retval)
		*retval = thread->retval;
	free(thread);
	return 0;
}

int pthread_yield(void)
{
	uthread_yield();
	return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t* attr)
{
  attr->type = PTHREAD_MUTEX_DEFAULT;
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t* attr)
{
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *__attr, int __detachstate)
{
	__attr->detachstate = __detachstate;
	return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type)
{
  *type = attr ? attr->type : PTHREAD_MUTEX_DEFAULT;
  return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type)
{
  if(type != PTHREAD_MUTEX_NORMAL)
    return EINVAL;
  attr->type = type;
  return 0;
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr)
{
  m->attr = attr;
  m->lock = 0;
  return 0;
}

/* Set *spun to 0 when calling this the first time.  It will yield after 'spins'
 * calls.  Use this for adaptive mutexes and such. */
static inline void spin_to_sleep(unsigned int spins, unsigned int *spun)
{
	if ((*spun)++ == spins) {
		pthread_yield();
		*spun = 0;
	}
}

int pthread_mutex_lock(pthread_mutex_t* m)
{
	unsigned int spinner = 0;
	while(pthread_mutex_trylock(m))
		while(*(volatile size_t*)&m->lock) {
			cpu_relax();
			spin_to_sleep(PTHREAD_MUTEX_SPINS, &spinner);
		}
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* m)
{
  return atomic_swap(&m->lock,1) == 0 ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t* m)
{
  /* Need to prevent the compiler (and some arches) from reordering older
   * stores */
  wmb();
  m->lock = 0;
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m)
{
  return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
  c->attr = a;
  memset(c->waiters,0,sizeof(c->waiters));
  memset(c->in_use,0,sizeof(c->in_use));
  c->next_waiter = 0;
  return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
  return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
  memset(c->waiters,0,sizeof(c->waiters));
  return 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
  int i;
  for(i = 0; i < MAX_PTHREADS; i++)
  {
    if(c->waiters[i])
    {
      c->waiters[i] = 0;
      break;
    }
  }
  return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
  int old_waiter = c->next_waiter;
  int my_waiter = c->next_waiter;
  
  //allocate a slot
  while (atomic_swap (& (c->in_use[my_waiter]), SLOT_IN_USE) == SLOT_IN_USE)
  {
    my_waiter = (my_waiter + 1) % MAX_PTHREADS;
    assert (old_waiter != my_waiter);  // do not want to wrap around
  }
  c->waiters[my_waiter] = WAITER_WAITING;
  c->next_waiter = (my_waiter+1) % MAX_PTHREADS;  // race on next_waiter but ok, because it is advisary

  pthread_mutex_unlock(m);

  volatile int* poll = &c->waiters[my_waiter];
  while(*poll);
  c->in_use[my_waiter] = SLOT_FREE;
  pthread_mutex_lock(m);

  return 0;
}

int pthread_condattr_init(pthread_condattr_t *a)
{
  a = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *a)
{
  return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *a, int s)
{
  a->pshared = s;
  return 0;
}

int pthread_condattr_getpshared(pthread_condattr_t *a, int *s)
{
  *s = a->pshared;
  return 0;
}

pthread_t pthread_self()
{
  return (struct pthread_tcb*)current_uthread;
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
  return t1 == t2;
}

/* This function cannot be migrated to a different vcore by the userspace
 * scheduler.  Will need to sort that shit out. */
void pthread_exit(void *ret)
{
	struct pthread_tcb *pthread = pthread_self();
	pthread->retval = ret;
	uthread_exit();
}

int pthread_once(pthread_once_t* once_control, void (*init_routine)(void))
{
  if(atomic_swap(once_control,1) == 0)
    init_routine();
  return 0;
}

int pthread_barrier_init(pthread_barrier_t* b, const pthread_barrierattr_t* a, int count)
{
  b->nprocs = b->count = count;
  b->sense = 0;
  pthread_mutex_init(&b->pmutex, 0);
  return 0;
}

int pthread_barrier_wait(pthread_barrier_t* b)
{
  unsigned int spinner = 0;
  int ls = !b->sense;

  pthread_mutex_lock(&b->pmutex);
  int count = --b->count;
  pthread_mutex_unlock(&b->pmutex);

  if(count == 0)
  {
    printd("Thread %d is last to hit the barrier, resetting...\n", pthread_self()->id);
    b->count = b->nprocs;
	wmb();
    b->sense = ls;
    return PTHREAD_BARRIER_SERIAL_THREAD;
  }
  else
  {
    while(b->sense != ls) {
      cpu_relax();
      spin_to_sleep(PTHREAD_BARRIER_SPINS, &spinner);
    }
    return 0;
  }
}

int pthread_barrier_destroy(pthread_barrier_t* b)
{
  pthread_mutex_destroy(&b->pmutex);
  return 0;
}

int pthread_detach(pthread_t thread)
{
	thread->detached = TRUE;
	return 0;
}
