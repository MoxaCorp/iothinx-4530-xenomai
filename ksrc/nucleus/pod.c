/*!\file pod.c
 * \brief Real-time pod services.
 * \author Philippe Gerum
 *
 * Copyright (C) 2001-2008 Philippe Gerum <rpm@xenomai.org>.
 * Copyright (C) 2004 The RTAI project <http://www.rtai.org>
 * Copyright (C) 2004 The HYADES project <http://www.hyades-itea.org>
 * Copyright (C) 2005 The Xenomai project <http://www.Xenomai.org>
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * \ingroup pod
 */

/*!
 * \ingroup nucleus
 * \defgroup pod Real-time pod services.
 *
 * Real-time pod services.
 *@{*/

#include <stdarg.h>
#include <nucleus/pod.h>
#include <nucleus/timer.h>
#include <nucleus/synch.h>
#include <nucleus/heap.h>
#include <nucleus/intr.h>
#include <nucleus/registry.h>
#include <nucleus/module.h>
#include <nucleus/stat.h>
#include <nucleus/assert.h>
#include <asm/xenomai/bits/pod.h>

#ifndef CONFIG_XENO_OPT_DEBUG_NUCLEUS
#define CONFIG_XENO_OPT_DEBUG_NUCLEUS 0
#endif

/*
 * NOTE: We need to initialize the globals; remember that this code
 * also runs over the simulator in user-space.
 */
xnpod_t nkpod_struct;

DEFINE_XNLOCK(nklock);

u_long nklatency = 0;

/* Already accounted for in nklatency, kept separately for user information. */
u_long nktimerlat = 0;

char *nkmsgbuf = NULL;

xnarch_cpumask_t nkaffinity = XNPOD_ALL_CPUS;

#ifdef CONFIG_XENO_HW_FPU

static inline void __xnpod_init_fpu(struct xnsched *sched,
				    struct xnthread *thread)
{
	/*
	 * When switching to a newly created thread, it is necessary
	 * to switch FPU contexts, as a replacement for xnpod_schedule
	 * epilogue (a newly created was not switched out by calling
	 * xnpod_schedule, since it is new).
	 */
	if (xnthread_test_state(thread, XNFPU)) {
		if (sched->fpuholder != NULL &&
		    xnarch_fpu_ptr(xnthread_archtcb(sched->fpuholder)) !=
		    xnarch_fpu_ptr(xnthread_archtcb(thread)))
			xnarch_save_fpu(xnthread_archtcb(sched->fpuholder));

		xnarch_init_fpu(xnthread_archtcb(thread));

		sched->fpuholder = thread;
	}
}

static inline void __xnpod_giveup_fpu(struct xnsched *sched,
				      struct xnthread *thread)
{
	if (thread == sched->fpuholder)
		sched->fpuholder = NULL;
}

static inline void __xnpod_release_fpu(struct xnthread *thread)
{
	if (xnthread_test_state(thread, XNFPU)) {
		/*
		 * Force the FPU save, and nullify the
		 * sched->fpuholder pointer, to avoid leaving
		 * fpuholder pointing on the backup area of the
		 * migrated thread.
		 */
		xnarch_save_fpu(xnthread_archtcb(thread));
		thread->sched->fpuholder = NULL;
	}
}

static inline void __xnpod_switch_fpu(struct xnsched *sched)
{
	xnthread_t *curr = sched->curr;

	if (!xnthread_test_state(curr, XNFPU))
		return;

	if (sched->fpuholder != curr) {
		if (sched->fpuholder == NULL ||
		    xnarch_fpu_ptr(xnthread_archtcb(sched->fpuholder)) !=
		    xnarch_fpu_ptr(xnthread_archtcb(curr))) {
			if (sched->fpuholder)
				xnarch_save_fpu(xnthread_archtcb
						(sched->fpuholder));

			xnarch_restore_fpu(xnthread_archtcb(curr));
		} else
			xnarch_enable_fpu(xnthread_archtcb(curr));

		sched->fpuholder = curr;
	} else
		xnarch_enable_fpu(xnthread_archtcb(curr));
}

/* xnpod_switch_fpu() -- Switches to the current thread's FPU context,
   saving the previous one as needed. */

void xnpod_switch_fpu(xnsched_t *sched)
{
	__xnpod_switch_fpu(sched);
}

static inline int __xnpod_fault_init_fpu(struct xnthread *thread)
{
#ifdef CONFIG_XENO_OPT_PERVASIVE
	xnarchtcb_t *tcb = xnthread_archtcb(thread);

	if (xnpod_shadow_p() && !xnarch_fpu_init_p(tcb->user_task)) {
		/*
		 * The faulting task is a shadow using the FPU for the
		 * first time, initialize its FPU. Of course if
		 * Xenomai is not compiled with support for FPU, such
		 * use of the FPU is an error.
		 */
		xnarch_init_fpu(tcb);
		return 1;
	}
#endif /* CONFIG_XENO_OPT_PERVASIVE */

	return 0;
}

#else /* !CONFIG_XENO_HW_FPU */

static inline void __xnpod_init_fpu(struct xnsched *sched,
				    struct xnthread *thread)
{
}

static inline void __xnpod_giveup_fpu(struct xnsched *sched,
				      struct xnthread *thread)
{
}

static inline void __xnpod_release_fpu(struct xnthread *thread)
{
}

static inline void __xnpod_switch_fpu(struct xnsched *sched)
{
}

static inline int __xnpod_fault_init_fpu(struct xnthread *thread)
{
	return 0;
}

#endif /* !CONFIG_XENO_HW_FPU */

const char *xnpod_fatal_helper(const char *format, ...)
{
	const unsigned nr_cpus = xnarch_num_online_cpus();
	xnholder_t *holder;
	char *p = nkmsgbuf;
	xnticks_t now;
	unsigned cpu;
	va_list ap;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	va_start(ap, format);
	p += vsnprintf(p, XNPOD_FATAL_BUFSZ, format, ap);
	va_end(ap);

	if (!xnpod_active_p() || xnpod_fatal_p())
		goto out;

	__setbits(nkpod->status, XNFATAL);
	now = xntbase_get_jiffies(&nktbase);

	p += snprintf(p, XNPOD_FATAL_BUFSZ - (p - nkmsgbuf),
		      "\n %-3s  %-6s %-8s %-8s %-8s  %s\n",
		      "CPU", "PID", "PRI", "TIMEOUT", "STAT", "NAME");

	for (cpu = 0; cpu < nr_cpus; ++cpu) {
		xnsched_t *sched = xnpod_sched_slot(cpu);
		char pbuf[16];

		holder = getheadq(&nkpod->threadq);

		while (holder) {
			xnthread_t *thread = link2thread(holder, glink);
			int cprio, dnprio;

			holder = nextq(&nkpod->threadq, holder);

			if (thread->sched != sched)
				continue;

			cprio = xnthread_current_priority(thread);
			dnprio = xnthread_get_denormalized_prio(thread, cprio);

			if (dnprio != cprio)
				snprintf(pbuf, sizeof(pbuf), "%3d(%d)",
					 cprio, dnprio);
			else
				snprintf(pbuf, sizeof(pbuf), "%3d", dnprio);

			p += snprintf(p, XNPOD_FATAL_BUFSZ - (p - nkmsgbuf),
				      "%c%3u  %-6d %-8s %-8Lu %.8lx  %s\n",
				      thread == sched->curr ? '>' : ' ',
				      cpu,
				      xnthread_user_pid(thread),
				      pbuf,
				      xnthread_get_timeout(thread, now),
				      xnthread_state_flags(thread),
				      xnthread_name(thread));
		}
	}

	if (xntbase_enabled_p(&nktbase))
		p += snprintf(p, XNPOD_FATAL_BUFSZ - (p - nkmsgbuf),
			      "Master time base: clock=%Lu\n",
			      xntbase_get_rawclock(&nktbase));
	else
		p += snprintf(p, XNPOD_FATAL_BUFSZ - (p - nkmsgbuf),
			      "Master time base: disabled\n");
#ifdef CONFIG_SMP
		p += snprintf(p, XNPOD_FATAL_BUFSZ - (p - nkmsgbuf),
			      "Current CPU: #%d\n", xnarch_current_cpu());
#endif
      out:

	xnlock_put_irqrestore(&nklock, s);

	return nkmsgbuf;
}

void xnpod_schedule_handler(void) /* Called with hw interrupts off. */
{
	xnsched_t *sched = xnpod_current_sched();

	trace_mark(xn_nucleus_sched_remote, MARK_NOARGS);
#if defined(CONFIG_SMP) && defined(CONFIG_XENO_OPT_PRIOCPL)
	if (testbits(sched->status, XNRPICK)) {
		clrbits(sched->status, XNRPICK);
		xnshadow_rpi_check();
	}
#endif /* CONFIG_SMP && CONFIG_XENO_OPT_PRIOCPL */
	xnsched_set_self_resched(sched);
	xnpod_schedule();
}

void xnpod_schedule_deferred(void)
{
	if (xnpod_active_p() && xnsched_resched_p(xnpod_current_sched()))
		xnpod_schedule();
}

static void xnpod_flush_heap(xnheap_t *heap,
			     void *extaddr, u_long extsize, void *cookie)
{
	xnarch_free_host_mem(extaddr, extsize);
}

#if CONFIG_XENO_OPT_SYS_STACKPOOLSZ > 0
static void xnpod_flush_stackpool(xnheap_t *heap,
				  void *extaddr, u_long extsize, void *cookie)
{
	xnarch_free_stack_mem(extaddr, extsize);
}
#endif

/*! 
 * \fn int xnpod_init(void)
 * \brief Initialize the core pod.
 *
 * Initializes the core interface pod which can subsequently be used
 * to start real-time activities. Once the core pod is active,
 * real-time skins can be stacked over. There can only be a single
 * core pod active in the host environment. Such environment can be
 * confined to a process (e.g. simulator), or expand machine-wide
 * (e.g. I-pipe).
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -ENOMEM is returned if the memory manager fails to initialize.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 */

int xnpod_init(void)
{
	extern int xeno_nucleus_status;
	int cpu, nr_cpus = xnarch_num_online_cpus();
	struct xnsched *sched;
	struct xnpod *pod;
	void *heapaddr;
	int ret;
	spl_t s;

	if (xeno_nucleus_status < 0)
		/* xeno_nucleus module failed to load properly, bail out. */
		return xeno_nucleus_status;

	xnlock_get_irqsave(&nklock, s);

	if (xnpod_active_p()) {
		/*
		 * Another skin has initialized the global pod
		 * already; just increment the reference count.
		 */
		++nkpod->refcnt;
		xnlock_put_irqrestore(&nklock, s);
		return 0;
	}

	pod = &nkpod_struct;
	pod->status = 0;
	pod->refcnt = 1;
	initq(&pod->threadq);
	initq(&pod->tstartq);
	initq(&pod->tswitchq);
	initq(&pod->tdeleteq);
	xnarch_atomic_set(&pod->timerlck, 0);
#ifdef __XENO_SIM__
	pod->schedhook = NULL;
#endif

	xnlock_put_irqrestore(&nklock, s);

	heapaddr = xnarch_alloc_host_mem(xnmod_sysheap_size);
	if (heapaddr == NULL ||
	    xnheap_init(&kheap, heapaddr, xnmod_sysheap_size,
			XNHEAP_PAGE_SIZE) != 0) {
		return -ENOMEM;
	}

#if CONFIG_XENO_OPT_SYS_STACKPOOLSZ > 0
	/*
	 * We have to differentiate the system heap memory from the
	 * pool the kernel thread stacks will be obtained from,
	 * because on some architectures, vmalloc memory may not be
	 * accessed while running in physical addressing mode
	 * (e.g. exception trampoline code on powerpc with standard
	 * MMU support - CONFIG_PPC_STD_MMU). Meanwhile, since we want
	 * to allow the system heap to be larger than 128Kb in
	 * contiguous memory, we can't restrict to using kmalloc()
	 * memory for it either.  Therefore, we manage a private stack
	 * pool for kernel-based threads which will be populated with
	 * the kind of memory the underlying arch requires, still
	 * allowing the system heap to rely on a vmalloc'ed segment.
	 */
	heapaddr = xnarch_alloc_stack_mem(CONFIG_XENO_OPT_SYS_STACKPOOLSZ * 1024);
	if (heapaddr == NULL ||
	    xnheap_init(&kstacks, heapaddr, CONFIG_XENO_OPT_SYS_STACKPOOLSZ * 1024,
			XNHEAP_PAGE_SIZE) != 0) {
		xnheap_destroy(&kheap, &xnpod_flush_heap, NULL);
		return -ENOMEM;
	}
#endif /* CONFIG_XENO_OPT_SYS_STACKPOOLSZ > 0 */

	for (cpu = 0; cpu < nr_cpus; ++cpu) {
		sched = &pod->sched[cpu];
		xnsched_init(sched);
		appendq(&pod->threadq, &sched->rootcb.glink);
	}

	xnarch_hook_ipi(&xnpod_schedule_handler);

#ifdef CONFIG_XENO_OPT_REGISTRY
	xnregistry_init();
#endif /* CONFIG_XENO_OPT_REGISTRY */

	__setbits(pod->status, XNPEXEC);
	xnarch_memory_barrier();
	xnarch_notify_ready();

	ret = xnpod_enable_timesource();
	if (ret) {
		xnpod_shutdown(XNPOD_FATAL_EXIT);
		return ret;
	}

	return 0;
}

/*! 
 * \fn void xnpod_shutdown(int xtype)
 * \brief Shutdown the current pod.
 *
 * Forcibly shutdowns the active pod. All existing nucleus threads
 * (but the root one) are terminated, and the system heap is freed.
 *
 * @param xtype An exit code passed to the host environment who
 * started the nucleus. Zero is always interpreted as a successful
 * return.
 *
 * The nucleus never calls this routine directly. Skins should provide
 * their own shutdown handlers which end up calling xnpod_shutdown()
 * after their own housekeeping chores have been carried out.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 *
 * Rescheduling: never.
 */

void xnpod_shutdown(int xtype)
{
	struct xnholder *h, *nh;
	struct xnthread *thread;
	struct xnsched *sched;
	int cpu;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (!xnpod_active_p() || --nkpod->refcnt != 0) {
		xnlock_put_irqrestore(&nklock, s);
		return;	/* No-op */
	}

	/*
	 * FIXME: We must release the lock before disabling the time
	 * source, so we accept a potential race due to another skin
	 * being pushed while we remove the current pod, which is
	 * clearly not a common situation anyway.
	 */
	xnlock_put_irqrestore(&nklock, s);

	xnpod_disable_timesource();
	xnarch_notify_shutdown();

	xnlock_get_irqsave(&nklock, s);

	nh = getheadq(&nkpod->threadq);
	while ((h = nh) != NULL) {
		nh = nextq(&nkpod->threadq, h);

		thread = link2thread(h, glink);

		if (!xnthread_test_state(thread, XNROOT))
			xnpod_delete_thread(thread);
	}

	xnpod_schedule();

	__clrbits(nkpod->status, XNPEXEC);

	for (cpu = 0; cpu < xnarch_num_online_cpus(); cpu++) {
		sched = xnpod_sched_slot(cpu);
		xnsched_destroy(sched);
	}

	xnlock_put_irqrestore(&nklock, s);

#ifdef CONFIG_XENO_OPT_REGISTRY
	xnregistry_cleanup();
#endif /* CONFIG_XENO_OPT_REGISTRY */
	xnarch_notify_halt();
	xnheap_destroy(&kheap, &xnpod_flush_heap, NULL);
#if CONFIG_XENO_OPT_SYS_STACKPOOLSZ > 0
	xnheap_destroy(&kstacks, &xnpod_flush_stackpool, NULL);
#endif
}

void xnpod_fire_callouts(xnqueue_t *hookq, xnthread_t *thread)
{
	/* Must be called with nklock locked, interrupts off. */
	xnsched_t *sched = xnpod_current_sched();
	xnholder_t *holder, *nholder;

	__setbits(sched->status, XNKCOUT);

	/* The callee is allowed to alter the hook queue when running */

	nholder = getheadq(hookq);

	while ((holder = nholder) != NULL) {
		xnhook_t *hook = link2hook(holder);
		nholder = nextq(hookq, holder);
		hook->routine(thread);
	}

	__clrbits(sched->status, XNKCOUT);
}

void __xnpod_finalize_zombie(xnsched_t *sched)
{
	xnthread_t *thread = sched->zombie;

	xnthread_cleanup_tcb(thread);

	xnarch_finalize_no_switch(xnthread_archtcb(thread));

	if (xnthread_test_state(sched->curr, XNROOT))
		xnfreesync();

	sched->zombie = NULL;
}

/*! 
 * \fn void xnpod_init_thread(xnthread_t *thread,xntbase_t *tbase,const char *name,int prio,xnflags_t flags,unsigned stacksize, xnthrops_t *ops)
 * \brief Initialize a new thread.
 *
 * Initializes a new thread attached to the active pod. The thread is
 * left in an innocuous state until it is actually started by
 * xnpod_start_thread().
 *
 * @param thread The address of a thread descriptor the nucleus will
 * use to store the thread-specific data.  This descriptor must always
 * be valid while the thread is active therefore it must be allocated
 * in permanent memory. @warning Some architectures may require the
 * descriptor to be properly aligned in memory; this is an additional
 * reason for descriptors not to be laid in the program stack where
 * alignement constraints might not always be satisfied.
 *
 * @param name An ASCII string standing for the symbolic name of the
 * thread. This name is copied to a safe place into the thread
 * descriptor. This name might be used in various situations by the
 * nucleus for issuing human-readable diagnostic messages, so it is
 * usually a good idea to provide a sensible value here. The simulator
 * even uses this name intensively to identify threads in the
 * debugging GUI it provides. However, passing NULL here is always
 * legal and means "anonymous".
 *
 * @param tbase The time base descriptor to refer to for all timed
 * operations issued by the new thread. See xntbase_alloc() for
 * detailed explanations about time bases.
 *
 * @param prio The base priority of the new thread. This value must
 * range from [loprio .. hiprio] (inclusive) as specified when calling
 * the xnpod_init() service.
 *
 * @param flags A set of creation flags affecting the operation. The
 * following flags can be part of this bitmask, each of them affecting
 * the nucleus behaviour regarding the created thread:
 *
 * - XNSUSP creates the thread in a suspended state. In such a case,
 * the thread will have to be explicitly resumed using the
 * xnpod_resume_thread() service for its execution to actually begin,
 * additionally to issuing xnpod_start_thread() for it. This flag can
 * also be specified when invoking xnpod_start_thread() as a starting
 * mode.

 * - XNFPU (enable FPU) tells the nucleus that the new thread will use
 * the floating-point unit. In such a case, the nucleus will handle
 * the FPU context save/restore ops upon thread switches at the
 * expense of a few additional cycles per context switch. By default,
 * a thread is not expected to use the FPU. This flag is simply
 * ignored when the nucleus runs on behalf of a userspace-based
 * real-time control layer since the FPU management is always active
 * if present.
 *
 * @param stacksize The size of the stack (in bytes) for the new
 * thread. If zero is passed, the nucleus will use a reasonable
 * pre-defined size depending on the underlying real-time control
 * layer.
 *
 * @param ops A pointer to a structure defining the class-level
 * operations available for this thread. Fields from this structure
 * must have been set appropriately by the caller.
 *
 * @return 0 is returned on success. Otherwise, one of the following
 * error codes indicates the cause of the failure:
 *
 *         - -EINVAL is returned if @a flags has invalid bits set.
 *
 *         - -ENOMEM is returned if not enough memory is available
 *         from the system heap to create the new thread's stack.
 *
 * Side-effect: This routine does not call the rescheduling procedure.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_init_thread(xnthread_t *thread,
		      xntbase_t *tbase,
		      const char *name,
		      int prio, xnflags_t flags, unsigned stacksize,
		      xnthrops_t *ops)
{
	spl_t s;
	int err;

	if (flags & ~(XNFPU | XNSHADOW | XNSHIELD | XNSUSP))
		return -EINVAL;

#ifndef CONFIG_XENO_OPT_ISHIELD
	flags &= ~XNSHIELD;
#endif /* !CONFIG_XENO_OPT_ISHIELD */

	if (stacksize == 0)
		stacksize = XNARCH_THREAD_STACKSZ;

	/* Exclude XNSUSP, so that xnpod_suspend_thread() will actually do
	   the suspension work for the thread. */
	err = xnthread_init(thread, tbase, name, prio, flags & ~XNSUSP, stacksize, ops);

	if (err)
		return err;

	trace_mark(xn_nucleus_thread_init,
		   "thread %p thread_name %s flags %lu priority %d",
		   thread, xnthread_name(thread), flags, prio);

	xnlock_get_irqsave(&nklock, s);
	thread->sched = xnpod_current_sched();
	appendq(&nkpod->threadq, &thread->glink);
	nkpod->threadq_rev++;
	xnpod_suspend_thread(thread, XNDORMANT | (flags & XNSUSP), XN_INFINITE,
			     XN_RELATIVE, NULL);
	xnlock_put_irqrestore(&nklock, s);

	return 0;
}

/*! 
 * \fn int xnpod_start_thread(xnthread_t *thread,xnflags_t mode,int imask,xnarch_cpumask_t affinity,void (*entry)(void *cookie),void *cookie)
 * \brief Initial start of a newly created thread.
 *
 * Starts a (newly) created thread, scheduling it for the first
 * time. This call releases the target thread from the XNDORMANT
 * state. This service also sets the initial mode and interrupt mask
 * for the new thread.
 *
 * @param thread The descriptor address of the affected thread which
 * must have been previously initialized by the xnpod_init_thread()
 * service.
 *
 * @param mode The initial thread mode. The following flags can be
 * part of this bitmask, each of them affecting the nucleus
 * behaviour regarding the started thread:
 *
 * - XNLOCK causes the thread to lock the scheduler when it starts.
 * The target thread will have to call the xnpod_unlock_sched()
 * service to unlock the scheduler. A non-preemptible thread may still
 * block, in which case, the lock is reasserted when the thread is
 * scheduled back in.
 *
 * - XNRRB causes the thread to be marked as undergoing the
 * round-robin scheduling policy at startup.  The contents of the
 * thread.rrperiod field determines the time quantum (in ticks)
 * allowed for its next slice.
 *
 * - XNASDI disables the asynchronous signal handling for this thread.
 * See xnpod_schedule() for more on this.
 *
 * - XNSUSP makes the thread start in a suspended state. In such a
 * case, the thread will have to be explicitly resumed using the
 * xnpod_resume_thread() service for its execution to actually begin.
 *
 * @param imask The interrupt mask that should be asserted when the
 * thread starts. The processor interrupt state will be set to the
 * given value when the thread starts running. The interpretation of
 * this value might be different across real-time layers, but a
 * non-zero value should always mark an interrupt masking in effect
 * (e.g. local_irq_disable()). Conversely, a zero value should always
 * mark a fully preemptible state regarding interrupts
 * (e.g. local_irq_enable()).
 *
 * @param affinity The processor affinity of this thread. Passing
 * XNPOD_ALL_CPUS or an empty affinity set means "any cpu".
 *
 * @param entry The address of the thread's body routine. In other
 * words, it is the thread entry point.
 *
 * @param cookie A user-defined opaque cookie the nucleus will pass
 * to the emerging thread as the sole argument of its entry point.
 *
 * The START hooks are called on behalf of the calling context (if
 * any).
 *
 * @retval 0 if @a thread could be started ;
 *
 * @retval -EBUSY if @a thread was already started ;
 *
 * @retval -EINVAL if the value of @a affinity is invalid.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible.
 */

int xnpod_start_thread(xnthread_t *thread,
		       xnflags_t mode,
		       int imask,
		       xnarch_cpumask_t affinity,
		       void (*entry) (void *cookie), void *cookie)
{
	spl_t s;
	int err;

	if (!xnthread_test_state(thread, XNDORMANT))
		return -EBUSY;

	xnarch_cpus_and(affinity, affinity, nkaffinity);

	xnlock_get_irqsave(&nklock, s);

	thread->affinity = xnarch_cpu_online_map;
	xnarch_cpus_and(thread->affinity, affinity, thread->affinity);

	if (xnarch_cpus_empty(thread->affinity)) {
		err = -EINVAL;
		goto unlock_and_exit;
	}
#ifdef CONFIG_SMP
	if (!xnarch_cpu_isset(xnsched_cpu(thread->sched), thread->affinity))
		thread->sched =
		    xnpod_sched_slot(xnarch_first_cpu(thread->affinity));
#endif /* CONFIG_SMP */

	if (xnthread_test_state(thread, XNSTARTED)) {
		err = -EBUSY;
		goto unlock_and_exit;
	}
#ifndef CONFIG_XENO_OPT_ISHIELD
	mode &= ~XNSHIELD;
#endif /* !CONFIG_XENO_OPT_ISHIELD */

	xnthread_set_state(thread, (mode & (XNTHREAD_MODE_BITS | XNSUSP)) | XNSTARTED);
	thread->imask = imask;
	thread->imode = (mode & XNTHREAD_MODE_BITS);
	thread->entry = entry;
	thread->cookie = cookie;

	if (xnthread_test_state(thread, XNRRB))
		thread->rrcredit = thread->rrperiod;

	trace_mark(xn_nucleus_thread_start, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

#ifdef CONFIG_XENO_OPT_PERVASIVE
	if (xnthread_test_state(thread, XNSHADOW)) {
		xnlock_put_irqrestore(&nklock, s);
		xnshadow_start(thread);
		xnpod_schedule();
		return 0;
	}
#endif /* CONFIG_XENO_OPT_PERVASIVE */

	/* Setup the initial stack frame. */

	xnarch_init_thread(xnthread_archtcb(thread),
			   entry, cookie, imask, thread, thread->name);

	xnpod_resume_thread(thread, XNDORMANT);

#ifdef __XENO_SIM__
	if (!(mode & XNSUSP) && nkpod->schedhook)
		nkpod->schedhook(thread, XNREADY);
#endif /* __XENO_SIM__ */

	if (!emptyq_p(&nkpod->tstartq) && !xnthread_test_state(thread, XNROOT)) {
		trace_mark(xn_nucleus_thread_callout,
			   "thread %p thread_name %s hook %s",
			   thread, xnthread_name(thread), "START");
		xnpod_fire_callouts(&nkpod->tstartq, thread);
	}

	xnpod_schedule();

	err = 0;

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}

/*! 
 * \fn void xnpod_restart_thread(xnthread_t *thread)
 *
 * \brief Restart a thread.
 *
 * Restarts a previously started thread.  The thread is first
 * terminated then respawned using the same information that prevailed
 * when it was first started, including the mode bits and interrupt
 * mask initially passed to the xnpod_start_thread() service. As a
 * consequence of this call, the thread entry point is rerun.
 *
 * @param thread The descriptor address of the affected thread which
 * must have been previously started by the xnpod_start_thread()
 * service.
 *
 * Self-restarting a thread is allowed. However, restarting the root
 * thread is not.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible.
 */

void xnpod_restart_thread(xnthread_t *thread)
{
	spl_t s;

	if (!xnthread_test_state(thread, XNSTARTED))
		return;		/* Not started yet or not restartable. */

#if XENO_DEBUG(NUCLEUS) || defined(__XENO_SIM__)
	if (xnthread_test_state(thread, XNROOT | XNSHADOW))
		xnpod_fatal("attempt to restart a user-space thread");
#endif /* XENO_DEBUG(NUCLEUS) || __XENO_SIM__ */

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_thread_restart, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	/* Break the thread out of any wait it is currently in. */
	xnpod_unblock_thread(thread);

	/* Release all ownerships held by the thread on synch. objects */
	xnsynch_release_all_ownerships(thread);

	/* If the task has been explicitly suspended, resume it. */
	if (xnthread_test_state(thread, XNSUSP))
		xnpod_resume_thread(thread, XNSUSP);

	/* Reset modebits. */
	xnthread_clear_state(thread, XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, thread->imode);

	/* Reset scheduling class and priority to the initial ones. */
	thread->cprio = thread->iprio;
	thread->bprio = thread->iprio;
	thread->base_class = thread->init_class;
	thread->sched_class = thread->init_class;

	/* Clear pending signals. */
	thread->signals = 0;

	if (thread == xnpod_current_sched()->curr) {
		/*
		 * Clear all sched locks held by the restarted thread.
		 */
		if (xnthread_test_state(thread, XNLOCK)) {
			xnthread_clear_state(thread, XNLOCK);
			xnthread_lock_count(thread) = 0;
		}
		xnthread_set_state(thread, XNRESTART);
	}

	/* Reset the initial stack frame. */
	xnarch_init_thread(xnthread_archtcb(thread),
			   thread->entry,
			   thread->cookie, thread->imask, thread, thread->name);
	/*
	 * Running this code tells us that xnpod_restart_thread() was
	 * not self-directed, so we must reschedule now since our
	 * priority may be lower than the restarted thread's
	 * priority.
	 */
	xnpod_schedule();

	xnlock_put_irqrestore(&nklock, s);
}

/*! 
 * \fn void xnpod_set_thread_mode(xnthread_t *thread,xnflags_t clrmask,xnflags_t setmask)
 * \brief Change a thread's control mode.
 *
 * Change the control mode of a given thread. The control mode affects
 * the behaviour of the nucleus regarding the specified thread.
 *
 * @param thread The descriptor address of the affected thread.
 *
 * @param clrmask Clears the corresponding bits from the control field
 * before setmask is applied. The scheduler lock held by the current
 * thread can be forcibly released by passing the XNLOCK bit in this
 * mask. In this case, the lock nesting count is also reset to zero.
 *
 * @param setmask The new thread mode. The following flags can be part
 * of this bitmask, each of them affecting the nucleus behaviour
 * regarding the thread:
 *
 * - XNLOCK causes the thread to lock the scheduler.  The target
 * thread will have to call the xnpod_unlock_sched() service to unlock
 * the scheduler or clear the XNLOCK bit forcibly using this
 * service. A non-preemptible thread may still block, in which case,
 * the lock is reasserted when the thread is scheduled back in.
 *
 * - XNRRB causes the thread to be marked as undergoing the
 * round-robin scheduling policy.  The contents of the thread.rrperiod
 * field determines the time quantum (in ticks) allowed for its
 * next slice. If the thread is already undergoing the round-robin
 * scheduling policy at the time this service is called, the time
 * quantum remains unchanged.
 *
 * - XNASDI disables the asynchronous signal handling for this thread.
 * See xnpod_schedule() for more on this.
 *
 * - XNSHIELD enables the interrupt shield for the current user-space
 * task. When engaged, the interrupt shield protects the shadow task
 * running in secondary mode from any preemption by the regular Linux
 * interrupt handlers, without delaying in any way Xenomai's interrupt
 * handling. The shield is operated on a per-task basis at each
 * context switch, depending on the setting of this flag. This feature
 * is only available if the CONFIG_XENO_OPT_ISHIELD option has been
 * enabled at configuration time; otherwise, this flag is simply
 * ignored.
 *
 * - XNRPIOFF disables thread priority coupling between Xenomai and
 * Linux schedulers. This bit prevents the root Linux thread from
 * inheriting the priority of the running shadow Xenomai thread. Use
 * CONFIG_XENO_OPT_RPIOFF to globally disable priority coupling.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel-based task
 * - User-space task in primary mode.
 *
 * Rescheduling: never, therefore, the caller should reschedule if
 * XNLOCK has been passed into @a clrmask.
 */

xnflags_t xnpod_set_thread_mode(xnthread_t *thread,
				xnflags_t clrmask, xnflags_t setmask)
{
	xnthread_t *curr = xnpod_current_thread();
	xnflags_t oldmode;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_thread_setmode,
		   "thread %p thread_name %s clrmask %lu setmask %lu",
		   thread, xnthread_name(thread), clrmask, setmask);

#ifndef CONFIG_XENO_OPT_ISHIELD
	setmask &= ~XNSHIELD;
#endif /* !CONFIG_XENO_OPT_ISHIELD */
	oldmode = xnthread_state_flags(thread) & XNTHREAD_MODE_BITS;
	xnthread_clear_state(thread, clrmask & XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, setmask & XNTHREAD_MODE_BITS);

	if (curr == thread) {
		if (!(oldmode & XNLOCK)) {
			if (xnthread_test_state(thread, XNLOCK))
				/* Actually grab the scheduler lock. */
				xnpod_lock_sched();
		} else if (!xnthread_test_state(thread, XNLOCK))
			xnthread_lock_count(thread) = 0;
	}

	if (!(oldmode & XNRRB) && xnthread_test_state(thread, XNRRB))
		thread->rrcredit = thread->rrperiod;

	xnlock_put_irqrestore(&nklock, s);

#ifdef CONFIG_XENO_OPT_ISHIELD
	if (curr == thread &&
	    xnthread_test_state(thread, XNSHADOW) &&
	    ((clrmask | setmask) & XNSHIELD) != 0)
		xnshadow_reset_shield();
#endif /* CONFIG_XENO_OPT_ISHIELD */

	return oldmode;
}

/*! 
 * \fn void xnpod_delete_thread(xnthread_t *thread)
 *
 * \brief Delete a thread.
 *
 * Terminates a thread and releases all the nucleus resources it
 * currently holds. A thread exists in the system since
 * xnpod_init_thread() has been called to create it, so this service
 * must be called in order to destroy it afterwards.
 *
 * @param thread The descriptor address of the terminated thread.
 *
 * The target thread's resources may not be immediately removed if
 * this is an active shadow thread running in user-space. In such a
 * case, the mated Linux task is sent a termination signal instead,
 * and the actual deletion is deferred until the task exit event is
 * called.
 *
 * The DELETE hooks are called on behalf of the calling context (if
 * any). The information stored in the thread control block remains
 * valid until all hooks have been called.
 *
 * Self-terminating a thread is allowed. In such a case, this service
 * does not return to the caller.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible if the current thread self-deletes.
 */

void xnpod_delete_thread(xnthread_t *thread)
{
	xnsched_t *sched;
	spl_t s;

#if XENO_DEBUG(NUCLEUS) || defined(__XENO_SIM__)
	if (xnthread_test_state(thread, XNROOT))
		xnpod_fatal("attempt to delete the root thread");
#endif /* XENO_DEBUG(NUCLEUS) || __XENO_SIM__ */

#ifdef __XENO_SIM__
	if (nkpod->schedhook)
		nkpod->schedhook(thread, XNDELETED);
#endif /* __XENO_SIM__ */

	xnlock_get_irqsave(&nklock, s);

	if (xnthread_test_state(thread, XNZOMBIE))
		goto unlock_and_exit;	/* No double-deletion. */

	sched = thread->sched;

#ifdef CONFIG_XENO_OPT_PERVASIVE
	/*
	 * This block serves two purposes:
	 *
	 * 1) Make sure Linux counterparts of shadow threads do exit
	 * upon deletion request from the nucleus through a call to
	 * xnpod_delete_thread().
	 *
	 * 2) Make sure shadow threads are removed from the system on
	 * behalf of their own context, by sending them a lethal
	 * signal when it is not the case instead of wiping out their
	 * TCB. We only do that whenever the caller is a kernel-based
	 * Xenomai context. In such a case, the deletion is
	 * asynchronous, and killed thread will later enter
	 * xnpod_delete_thread() from the exit notification handler
	 * (I-pipe).
	 *
	 * Sidenote: xnpod_delete_thread() might be called for
	 * cleaning up a just created shadow task which has not been
	 * successfully mapped, so we need to make sure that we have
	 * an associated Linux mate before trying to send it a signal
	 * (i.e. user_task extension != NULL). This will also prevent
	 * any action on kernel-based Xenomai threads for which the
	 * user TCB extension is always NULL.  We don't send any
	 * signal to dormant threads because GDB (6.x) has some
	 * problems dealing with vanishing threads under some
	 * circumstances, likely when asynchronous cancellation is in
	 * effect. In most cases, this is a non-issue since
	 * pthread_cancel() is requested from the skin interface
	 * library in parallel on the target thread. In the rare case
	 * of calling xnpod_delete_thread() from kernel space against
	 * a created but unstarted user-space task, the Linux thread
	 * mated to the Xenomai shadow might linger unexpectedly on
	 * the startup barrier.
	 */

	if (xnthread_user_task(thread) != NULL &&
	    !xnthread_test_state(thread, XNDORMANT) &&
	    !xnpod_current_p(thread)) {
		if (!xnpod_userspace_p())
			xnshadow_send_sig(thread, SIGKILL, 0, 1);
		/*
		 * Otherwise, assume the interface library has issued
		 * pthread_cancel on the target thread, which should
		 * cause the current service to be called for
		 * self-deletion of that thread.
		 */
		goto unlock_and_exit;
	}
#endif /* CONFIG_XENO_OPT_PERVASIVE */

	trace_mark(xn_nucleus_thread_delete, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	removeq(&nkpod->threadq, &thread->glink);
	nkpod->threadq_rev++;

	if (xnthread_test_state(thread, XNREADY)) {
		XENO_BUGON(NUCLEUS, xnthread_test_state(thread, XNTHREAD_BLOCK_BITS));
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	xntimer_destroy(&thread->rtimer);
	xntimer_destroy(&thread->ptimer);

	if (xnthread_test_state(thread, XNPEND))
		xnsynch_forget_sleeper(thread);

	xnsynch_release_all_ownerships(thread);

	__xnpod_giveup_fpu(sched, thread);

	xnthread_set_state(thread, XNZOMBIE);

	if (sched->curr == thread) {
		/*
		 * We first need to pick a new curr before
		 * switching out the current one forever. Use the
		 * thread zombie state to go through the rescheduling
		 * procedure then actually destroy the thread object.
		 */
		xnsched_set_resched(sched);
		xnpod_schedule();
#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	} else if (!testbits(sched->status, XNSWLOCK) &&
		   !xnthread_test_state(thread, XNMIGRATE)) {
		/*
		 * When killing a thread in the course of a context
		 * switch or in flight to another CPU with nklock
		 * unlocked on a distant CPU, do nothing, this case
		 * will be caught in xnsched_finish_unlocked_switch.
		 */
#else /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */
	} else {
#endif /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */
		if (!emptyq_p(&nkpod->tdeleteq)
		    && !xnthread_test_state(thread, XNROOT)) {
			trace_mark(xn_nucleus_thread_callout,
				   "thread %p thread_name %s hook %s",
				   thread, xnthread_name(thread), "DELETE");
			xnpod_fire_callouts(&nkpod->tdeleteq, thread);
		}
		/*
		 * Note: the thread control block must remain
		 * available until the user hooks have been called.
		 */
		xnthread_cleanup_tcb(thread);

		xnarch_finalize_no_switch(xnthread_archtcb(thread));

		if (xnthread_test_state(sched->curr, XNROOT))
			xnfreesync();
	}

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);
}

/*! 
 * \fn void xnpod_abort_thread(xnthread_t *thread)
 *
 * \brief Abort a thread.
 *
 * Unconditionally terminates a thread and releases all the nucleus
 * resources it currently holds, regardless of whether the target
 * thread is currently active in kernel or user-space.
 * xnpod_abort_thread() should be reserved for use by skin cleanup
 * routines; xnpod_delete_thread() should be preferred as the common
 * method for removing threads from a running system.
 *
 * @param thread The descriptor address of the terminated thread.
 *
 * This service forces a call to xnpod_delete_thread() for the target
 * thread.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible if the current thread self-deletes.
 */
void xnpod_abort_thread(xnthread_t *thread)
{
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
	if (!xnpod_current_p(thread))
		xnpod_suspend_thread(thread, XNDORMANT, XN_INFINITE, XN_RELATIVE, NULL);
	xnpod_delete_thread(thread);
	xnlock_put_irqrestore(&nklock, s);
}

/*!
 * \fn void xnpod_suspend_thread(xnthread_t *thread, xnflags_t mask,
 *                               xnticks_t timeout, xntmode_t timeout_mode,
 *                               xnsynch_t *wchan)
 *
 * \brief Suspend a thread.
 *
 * Suspends the execution of a thread according to a given suspensive
 * condition. This thread will not be eligible for scheduling until it
 * all the pending suspensive conditions set by this service are
 * removed by one or more calls to xnpod_resume_thread().
 *
 * @param thread The descriptor address of the suspended thread.
 *
 * @param mask The suspension mask specifying the suspensive condition
 * to add to the thread's wait mask. Possible values usable by the
 * caller are:
 *
 * - XNSUSP. This flag forcibly suspends a thread, regardless of any
 * resource to wait for. A reverse call to xnpod_resume_thread()
 * specifying the XNSUSP bit must be issued to remove this condition,
 * which is cumulative with other suspension bits.@a wchan should be
 * NULL when using this suspending mode.
 *
 * - XNDELAY. This flags denotes a counted delay wait (in ticks) which
 * duration is defined by the value of the timeout parameter.
 *
 * - XNPEND. This flag denotes a wait for a synchronization object to
 * be signaled. The wchan argument must points to this object. A
 * timeout value can be passed to bound the wait. This suspending mode
 * should not be used directly by the client interface, but rather
 * through the xnsynch_sleep_on() call.
 *
 * @param timeout The timeout which may be used to limit the time the
 * thread pends on a resource. This value is a wait time given in
 * ticks (see note). It can either be relative, absolute monotonic, or
 * absolute adjustable depending on @a timeout_mode. Passing XN_INFINITE
 * @b and setting @a timeout_mode to XN_RELATIVE specifies an unbounded
 * wait. All other values are used to initialize a watchdog timer. If the
 * current operation mode of the system timer is oneshot and @a timeout
 * elapses before xnpod_suspend_thread() has completed, then the target
 * thread will not be suspended, and this routine leads to a null effect.
 *
 * @param timeout_mode The mode of the @a timeout parameter. It can
 * either be set to XN_RELATIVE, XN_ABSOLUTE, or XN_REALTIME (see also
 * xntimer_start()).
 *
 * @param wchan The address of a pended resource. This parameter is
 * used internally by the synchronization object implementation code
 * to specify on which object the suspended thread pends. NULL is a
 * legitimate value when this parameter does not apply to the current
 * suspending mode (e.g. XNSUSP).
 *
 * @note If the target thread is a shadow which has received a
 * Linux-originated signal, then this service immediately exits
 * without suspending the thread, but raises the XNBREAK condition in
 * its information mask.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible if the current thread suspends itself.
 *
 * @note The @a timeout value will be interpreted as jiffies if @a
 * thread is bound to a periodic time base (see xnpod_init_thread), or
 * nanoseconds otherwise.
 */

void xnpod_suspend_thread(xnthread_t *thread, xnflags_t mask,
			  xnticks_t timeout, xntmode_t timeout_mode,
			  xnsynch_t *wchan)
{
	xnsched_t *sched;
	spl_t s;

#if XENO_DEBUG(NUCLEUS) || defined(__XENO_SIM__)
	if (xnthread_test_state(thread, XNROOT))
		xnpod_fatal("attempt to suspend root thread %s", thread->name);

	if (thread->wchan && wchan)
		xnpod_fatal("thread %s attempts a conjunctive wait",
			    thread->name);
#endif /* XENO_DEBUG(NUCLEUS) || __XENO_SIM__ */

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_thread_suspend,
		   "thread %p thread_name %s mask %lu timeout %Lu "
		   "timeout_mode %d wchan %p",
		   thread, xnthread_name(thread), mask, timeout,
		   timeout_mode, wchan);

	sched = thread->sched;

	if (thread == sched->curr)
		xnsched_set_resched(sched);

	/* Is the thread ready to run? */
	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS)) {
#ifdef CONFIG_XENO_OPT_PERVASIVE
		/* If attempting to suspend a runnable (shadow) thread which
		   has received a Linux signal, just raise the break condition
		   and return immediately. Note: a relaxed shadow never has
		   the KICKED bit set, so that xnshadow_relax() is never
		   prevented from blocking the current thread. */
		if (xnthread_test_info(thread, XNKICKED)) {
			XENO_ASSERT(NUCLEUS, (mask & XNRELAX) == 0,
				    xnpod_fatal("Relaxing a kicked thread"
						"(thread=%s, mask=%lx)?!",
						thread->name, mask);
				);
			xnthread_clear_info(thread, XNRMID | XNTIMEO);
			xnthread_set_info(thread, XNBREAK);
			goto unlock_and_exit;
		}
#endif /* CONFIG_XENO_OPT_PERVASIVE */

		xnthread_clear_info(thread, XNRMID | XNTIMEO | XNBREAK | XNWAKEN | XNROBBED);
	}

	/* Don't start the timer for a thread indefinitely delayed by
	   a call to xnpod_suspend_thread(thread,XNDELAY,XN_INFINITE,XN_RELATIVE,NULL). */

	if (timeout != XN_INFINITE || timeout_mode != XN_RELATIVE) {
		xntimer_set_sched(&thread->rtimer, thread->sched);
		if (xntimer_start(&thread->rtimer, timeout, XN_INFINITE,
				  timeout_mode)) {
			/* (absolute) timeout value in the past, bail out. */
			if (wchan) {
				thread->wchan = wchan;
				xnsynch_forget_sleeper(thread);
			}
			xnthread_set_info(thread, XNTIMEO);
			goto unlock_and_exit;
		}
		xnthread_set_state(thread, XNDELAY);
	}

	if (xnthread_test_state(thread, XNREADY)) {
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	xnthread_set_state(thread, mask);

	/*
	 * We must make sure that we don't clear the wait channel if a
	 * thread is first blocked (wchan != NULL) then forcibly
	 * suspended (wchan == NULL), since these are conjunctive
	 * conditions.
	 */
	if (wchan)
		thread->wchan = wchan;

#ifdef __XENO_SIM__
	if (nkpod->schedhook)
		nkpod->schedhook(thread, mask);
#endif /* __XENO_SIM__ */

	if (thread == sched->curr)
		/*
		 * If the thread is runnning on another CPU,
		 * xnpod_schedule will just trigger the IPI.
		 */
		xnpod_schedule();
#ifdef CONFIG_XENO_OPT_PERVASIVE
	/*
	 * Ok, this one is an interesting corner case, which requires
	 * a bit of background first. Here, we handle the case of
	 * suspending a _relaxed_ shadow which is _not_ the current
	 * thread.  The net effect is that we are attempting to stop
	 * the shadow thread at the nucleus level, whilst this thread
	 * is actually running some code under the control of the
	 * Linux scheduler (i.e. it's relaxed).  To make this
	 * possible, we force the target Linux task to migrate back to
	 * the Xenomai domain by sending it a SIGSHADOW signal the
	 * skin interface libraries trap for this specific internal
	 * purpose, whose handler is expected to call back the
	 * nucleus's migration service. By forcing this migration, we
	 * make sure that the real-time nucleus controls, hence
	 * properly stops, the target thread according to the
	 * requested suspension condition. Otherwise, the shadow
	 * thread in secondary mode would just keep running into the
	 * Linux domain, thus breaking the most common assumptions
	 * regarding suspended threads. We only care for threads that
	 * are not current, and for XNSUSP and XNDELAY conditions,
	 * because:
	 *
	 * - skins are supposed to ask for primary mode switch when
	 * processing any syscall which may block the caller; IOW,
	 * __xn_exec_primary must be set in the mode flags for
	 * those. So there is no need to deal specifically with the
	 * relax+suspend issue when the about to be suspended thread
	 * is current, since it must not be relaxed anyway.
	 * 
	 * - among all blocking bits (XNTHREAD_BLOCK_BITS), only
	 * XNSUSP and XNDELAY may be applied by the current thread to
	 * a non-current thread. XNPEND is always added by the caller
	 * to its own state, XNDORMANT is a pre-runtime state, and
	 * XNRELAX has special semantics escaping this issue.
	 *
	 * Also note that we don't signal threads which are in a
	 * dormant state, since they are suspended by definition.
	 */

	else if (xnthread_test_state(thread, XNSHADOW | XNRELAX | XNDORMANT) ==
		 (XNSHADOW | XNRELAX) && (mask & (XNDELAY | XNSUSP)) != 0)
		xnshadow_suspend(thread);
#endif /* CONFIG_XENO_OPT_PERVASIVE */

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);
}

/*!
 * \fn void xnpod_resume_thread(xnthread_t *thread,xnflags_t mask)
 * \brief Resume a thread.
 *
 * Resumes the execution of a thread previously suspended by one or
 * more calls to xnpod_suspend_thread(). This call removes a
 * suspensive condition affecting the target thread. When all
 * suspensive conditions are gone, the thread is left in a READY state
 * at which point it becomes eligible anew for scheduling.
 *
 * @param thread The descriptor address of the resumed thread.
 *
 * @param mask The suspension mask specifying the suspensive condition
 * to remove from the thread's wait mask. Possible values usable by
 * the caller are:
 *
 * - XNSUSP. This flag removes the explicit suspension condition. This
 * condition might be additive to the XNPEND condition.
 *
 * - XNDELAY. This flag removes the counted delay wait condition.
 *
 * - XNPEND. This flag removes the resource wait condition. If a
 * watchdog is armed, it is automatically disarmed by this
 * call. Unlike the two previous conditions, only the current thread
 * can set this condition for itself, i.e. no thread can force another
 * one to pend on a resource.
 *
 * When the thread is eventually resumed by one or more calls to
 * xnpod_resume_thread(), the caller of xnpod_suspend_thread() in the
 * awakened thread that suspended itself should check for the
 * following bits in its own information mask to determine what caused
 * its wake up:
 *
 * - XNRMID means that the caller must assume that the pended
 * synchronization object has been destroyed (see xnsynch_flush()).
 *
 * - XNTIMEO means that the delay elapsed, or the watchdog went off
 * before the corresponding synchronization object was signaled.
 *
 * - XNBREAK means that the wait has been forcibly broken by a call to
 * xnpod_unblock_thread().
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

void xnpod_resume_thread(xnthread_t *thread, xnflags_t mask)
{
	xnsched_t *sched;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_thread_resume,
		   "thread %p thread_name %s mask %lu",
		   thread, xnthread_name(thread), mask);
	xnarch_trace_pid(xnthread_user_task(thread) ?
			 xnarch_user_pid(xnthread_archtcb(thread)) : -1,
			 xnthread_current_priority(thread));

	sched = thread->sched;

	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS)) {
		if (xnthread_test_state(thread, XNREADY))
			xnsched_dequeue(thread);
		goto enqueue;
	}

	/* Clear the specified block bit(s) */
	xnthread_clear_state(thread, mask);

	/*
	 * If XNDELAY was set in the clear mask, xnpod_unblock_thread()
	 * was called for the thread, or a timeout has elapsed. In the
	 * latter case, stopping the timer is a no-op.
	 */
	if (mask & XNDELAY)
		xntimer_stop(&thread->rtimer);

	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS))
		goto clear_wchan;

	if (mask & XNDELAY) {
		mask = xnthread_test_state(thread, XNPEND);
		if (mask == 0)
			goto unlock_and_exit;

		if (thread->wchan) {
			xnsynch_forget_sleeper(thread);
			if (xnthread_test_state(thread, XNTHREAD_BLOCK_BITS))
				goto unlock_and_exit;
		}
	} else if (xnthread_test_state(thread, XNDELAY)) {
		if ((mask & XNPEND) != 0) {
			/*
			 * The thread is woken up due to the
			 * availability of the requested
			 * resource. Cancel the watchdog timer.
			 */
			xntimer_stop(&thread->rtimer);
			xnthread_clear_state(thread, XNDELAY);
		}

		if (xnthread_test_state(thread, XNTHREAD_BLOCK_BITS))
			goto unlock_and_exit;
	} else {
		/*
		 * The thread is still suspended, but is no more
		 * pending on a resource.
		 */
		if ((mask & XNPEND) != 0 && thread->wchan)
			xnsynch_forget_sleeper(thread);
		goto unlock_and_exit;
	}

clear_wchan:
	if ((mask & ~XNDELAY) != 0 && thread->wchan != NULL)
		/*
		 * If the thread was actually suspended, clear the
		 * wait channel.  -- this allows requests like
		 * xnpod_suspend_thread(thread,XNDELAY,...)  not to
		 * run the following code when the suspended thread is
		 * woken up while undergoing a simple delay.
		 */
		xnsynch_forget_sleeper(thread);

enqueue:
	xnsched_enqueue(thread);
	xnthread_set_state(thread, XNREADY);
	xnsched_set_resched(sched);

#ifdef __XENO_SIM__
	if (thread != sched->curr && nkpod->schedhook)
		nkpod->schedhook(thread, XNREADY);
#endif /* __XENO_SIM__ */

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);
}

/*!
 * \fn int xnpod_unblock_thread(xnthread_t *thread)
 * \brief Unblock a thread.
 *
 * Breaks the thread out of any wait it is currently in.  This call
 * removes the XNDELAY and XNPEND suspensive conditions previously put
 * by xnpod_suspend_thread() on the target thread. If all suspensive
 * conditions are gone, the thread is left in a READY state at which
 * point it becomes eligible anew for scheduling.
 *
 * @param thread The descriptor address of the unblocked thread.
 *
 * This call neither releases the thread from the XNSUSP, XNRELAX nor
 * the XNDORMANT suspensive conditions.
 *
 * When the thread resumes execution, the XNBREAK bit is set in the
 * unblocked thread's information mask. Unblocking a non-blocked
 * thread is perfectly harmless.
 *
 * @return non-zero is returned if the thread was actually unblocked
 * from a pending wait state, 0 otherwise.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_unblock_thread(xnthread_t *thread)
{
	int ret = 1;
	spl_t s;

	/*
	 * Attempt to abort an undergoing wait for the given thread.
	 * If this state is due to an alarm that has been armed to
	 * limit the sleeping thread's waiting time while it pends for
	 * a resource, the corresponding XNPEND state will be cleared
	 * by xnpod_resume_thread() in the same move. Otherwise, this
	 * call may abort an undergoing infinite wait for a resource
	 * (if any).
	 */
	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_thread_unblock,
		   "thread %p thread_name %s state %lu",
		   thread, xnthread_name(thread),
		   xnthread_state_flags(thread));

	if (xnthread_test_state(thread, XNDELAY))
		xnpod_resume_thread(thread, XNDELAY);
	else if (xnthread_test_state(thread, XNPEND))
		xnpod_resume_thread(thread, XNPEND);
	else
		ret = 0;

	/*
	 * We should not clear a previous break state if this service
	 * is called more than once before the target thread actually
	 * resumes, so we only set the bit here and never clear
	 * it. However, we must not raise the XNBREAK bit if the
	 * target thread was already awake at the time of this call,
	 * so that downstream code does not get confused by some
	 * "successful but interrupted syscall" condition. IOW, a
	 * break state raised here must always trigger an error code
	 * downstream, and an already successful syscall cannot be
	 * marked as interrupted.
	 */
	if (ret)
		xnthread_set_info(thread, XNBREAK);

	xnlock_put_irqrestore(&nklock, s);

	return ret;
}

/*!
 * \fn void xnpod_renice_thread(xnthread_t *thread,int prio)
 * \brief Change the base priority of a thread.
 *
 * Changes the base priority of a thread. If the reniced thread is
 * currently blocked, waiting in priority-pending mode (XNSYNCH_PRIO)
 * for a synchronization object to be signaled, the nucleus will
 * attempt to reorder the object's wait queue so that it reflects the
 * new sleeper's priority, unless the XNSYNCH_DREORD flag has been set
 * for the pended object.
 *
 * @param thread The descriptor address of the affected thread.
 *
 * @param prio The new thread priority.
 *
 * It is absolutely required to use this service to change a thread
 * priority, in order to have all the needed housekeeping chores
 * correctly performed. i.e. Do *not* change the thread.cprio field by
 * hand, unless the thread is known to be in an innocuous state
 * (e.g. dormant).
 *
 * Side-effects:
 *
 * - This service does not call the rescheduling procedure but may
 * affect the ready queue.
 *
 * - Assigning the same priority to a running or ready thread moves it
 * to the end of the ready queue, thus causing a manual round-robin.
 *
 * - If the reniced thread is a user-space shadow, propagate the
 * request to the mated Linux task.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

void xnpod_renice_thread(xnthread_t *thread, int prio)
{
	xnpod_renice_thread_inner(thread, prio, 1);
}

void xnpod_renice_thread_inner(xnthread_t *thread, int prio, int propagate)
{
	int oldprio;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_thread_renice,
		   "thread %p thread_name %s priority %d",
		   thread, xnthread_name(thread), prio);

	oldprio = thread->cprio;
	/*
	 * Change the thread priority, taking in account an undergoing PIP
	 * boost.
	 */
	thread->bprio = prio;

	/*
	 * Since we don't want to mess with the priority inheritance
	 * scheme, we must take care of never lowering the target
	 * thread's priority level if it is undergoing a PIP boost.
	 * Note that different priority levels for a given thread are
	 * by definition consistent within its scheduling class, so we
	 * don't need to use weighted priority values here.
	 */
	if (!xnthread_test_state(thread, XNBOOST) || prio > oldprio) {
		thread->cprio = prio;
		if (prio != oldprio && thread->wchan != NULL &&
		    !testbits(thread->wchan->status, XNSYNCH_DREORD))
			/*
			 * Renice the pending order of the thread
			 * inside its wait queue, unless this
			 * behaviour has been explicitly disabled for
			 * the pended synchronization object, or the
			 * requested priority has not changed, thus
			 * preventing spurious round-robin effects.
			 */
			xnsynch_renice_sleeper(thread);

		if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS | XNLOCK))
			xnsched_putback(thread);
	}
#ifdef CONFIG_XENO_OPT_PERVASIVE
	if (propagate) {
		if (xnthread_test_state(thread, XNRELAX))
			xnshadow_renice(thread);
		else if (xnthread_test_state(thread, XNSHADOW))
			xnthread_set_info(thread, XNPRIOSET);
	}
#endif /* CONFIG_XENO_OPT_PERVASIVE */

	xnlock_put_irqrestore(&nklock, s);
}

/** 
 * \fn int xnpod_migrate_thread(int cpu)
 *
 * \brief Migrate the current thread.
 *
 * This call makes the current thread migrate to another CPU if its
 * affinity allows it.
 * 
 * @param cpu The destination CPU.
 * 
 * @retval 0 if the thread could migrate ;
 * @retval -EPERM if the calling context is asynchronous, or the
 * current thread affinity forbids this migration ;
 * @retval -EBUSY if the scheduler is locked.
 */

int xnpod_migrate_thread(int cpu)
{
	xnthread_t *thread;
	int ret;
	spl_t s;

	if (xnpod_asynch_p())
		return -EPERM;

	if (xnpod_locked_p())
		return -EBUSY;

	xnlock_get_irqsave(&nklock, s);

	thread = xnpod_current_thread();

	if (!xnarch_cpu_isset(cpu, thread->affinity)) {
		ret = -EPERM;
		goto unlock_and_exit;
	}

	ret = 0;

	if (cpu == xnarch_current_cpu())
		goto unlock_and_exit;

	trace_mark(xn_nucleus_thread_migrate,
		   "thread %p thread_name %s cpu %d",
		   thread, xnthread_name(thread), cpu);

	__xnpod_release_fpu(thread);

	if (xnthread_test_state(thread, XNREADY)) {
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	xnsched_set_resched(thread->sched);
	thread->sched = xnpod_sched_slot(cpu);

	/* Migrate the thread periodic timer. */
	xntimer_set_sched(&thread->ptimer, thread->sched);

#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	/*
	 * Mark the thread in flight, xnsched_finish_unlocked_switch()
	 * will put the thread on the remote runqueue.
	 */
	xnthread_set_state(thread, XNMIGRATE);
#else /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */
	/* Move thread to the remote runnable queue. */
	xnsched_putback(thread);
#endif /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */

	xnpod_schedule();

	/*
	 * Reset execution time measurement period so that we don't
	 * mess up per-CPU statistics.
	 */
	xnstat_exectime_reset_stats(&thread->stat.lastperiod);

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return ret;
}

/*! 
 * \fn void xnpod_activate_rr(xnticks_t quantum)
 * \brief Globally activate the round-robin scheduling.
 *
 * This service activates the round-robin scheduling for all threads
 * which have the XNRRB flag set in their status mask (see
 * xnpod_set_thread_mode()). Each of them will run for the given time
 * quantum, then preempted and moved to the end of its priority group
 * in the ready queue. This process is repeated until the round-robin
 * scheduling is disabled for those threads.
 *
 * @param quantum The time credit which will be given to each
 * rr-enabled thread (in ticks).
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

void xnpod_activate_rr(xnticks_t quantum)
{
	xnholder_t *holder;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_sched_rractivate, "quantum %Lu", quantum);

	holder = getheadq(&nkpod->threadq);

	while (holder) {
		xnthread_t *thread = link2thread(holder, glink);

		if (xnthread_test_state(thread, XNRRB)) {
			thread->rrperiod = quantum;
			thread->rrcredit = quantum;
		}

		holder = nextq(&nkpod->threadq, holder);
	}

	xnlock_put_irqrestore(&nklock, s);
}

/*! 
 * \fn void xnpod_deactivate_rr(void)
 * \brief Globally deactivate the round-robin scheduling.
 *
 * This service deactivates the round-robin scheduling for all threads
 * which have the XNRRB flag set in their status mask (see
 * xnpod_set_thread_mode()).
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

void xnpod_deactivate_rr(void)
{
	xnholder_t *holder;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_sched_rrdeactivate, MARK_NOARGS);

	holder = getheadq(&nkpod->threadq);

	while (holder) {
		xnthread_t *thread = link2thread(holder, glink);

		if (xnthread_test_state(thread, XNRRB))
			thread->rrcredit = XN_INFINITE;

		holder = nextq(&nkpod->threadq, holder);
	}

	xnlock_put_irqrestore(&nklock, s);
}

/*! 
 * @internal
 * \fn void xnpod_dispatch_signals(void)
 * \brief Deliver pending asynchronous signals to the running thread.
 *
 * This internal routine checks for the presence of asynchronous
 * signals directed to the running thread, and attempts to start the
 * asynchronous service routine (ASR) if any. Called with nklock
 * locked, interrupts off.
 */

void xnpod_dispatch_signals(void)
{
	xnthread_t *thread = xnpod_current_thread();
	int asrimask, savedmask;
	xnflags_t oldmode;
	xnsigmask_t sigs;
	xnasr_t asr;

	/* Process user-defined signals if the ASR is enabled for this
	   thread. */

	if (thread->signals == 0 || xnthread_test_state(thread, XNASDI)
	    || thread->asr == XNTHREAD_INVALID_ASR)
		return;

	trace_mark(xn_nucleus_sched_sigdispatch, "signals %lu",
		   thread->signals);

	/* Start the asynchronous service routine */
	oldmode = xnthread_test_state(thread, XNTHREAD_MODE_BITS);
	sigs = thread->signals;
	asrimask = thread->asrimask;
	asr = thread->asr;

	/* Clear pending signals mask since an ASR can be reentrant */
	thread->signals = 0;

	/* Reset ASR mode bits */
	xnthread_clear_state(thread, XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, thread->asrmode);
	thread->asrlevel++;

	/* Setup ASR interrupt mask then fire it. */
	savedmask = xnarch_setimask(asrimask);
	asr(sigs);
	xnarch_setimask(savedmask);

	/* Reset the thread mode bits */
	thread->asrlevel--;
	xnthread_clear_state(thread, XNTHREAD_MODE_BITS);
	xnthread_set_state(thread, oldmode);
}

/*!
 * @internal
 * \fn void xnpod_welcome_thread(xnthread_t *thread, int imask)
 * \brief Thread prologue.
 *
 * This internal routine is called on behalf of a (re)starting
 * thread's prologue before the user entry point is invoked. This call
 * is reserved for internal housekeeping chores and cannot be inlined.
 *
 * Entered with nklock locked, irqs off.
 */

void xnpod_welcome_thread(xnthread_t *thread, int imask)
{
	xnsched_t *sched = xnsched_finish_unlocked_switch(thread->sched);

	xnpod_finalize_zombie(sched);

	trace_mark(xn_nucleus_thread_boot, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	xnarch_trace_pid(-1, xnthread_current_priority(thread));

	if (xnthread_test_state(thread, XNLOCK))
		/* Actually grab the scheduler lock. */
		xnpod_lock_sched();

	__xnpod_init_fpu(sched, thread);

	xnthread_clear_state(thread, XNRESTART);

	if (xnthread_signaled_p(thread))
		xnpod_dispatch_signals();

	xnlock_clear_irqoff(&nklock);
	splexit(!!imask);

	xnsched_resched_after_unlocked_switch();
}

static inline void xnpod_switch_to(xnsched_t *sched,
				   xnthread_t *prev, xnthread_t *next)
{
#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	sched->last = prev;
	__setbits(sched->status, XNSWLOCK);
	xnlock_clear_irqon(&nklock);
#endif /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */

	xnarch_switch_to(xnthread_archtcb(prev),
			 xnthread_archtcb(next));
}

/*! 
 * \fn void xnpod_schedule(void)
 * \brief Rescheduling procedure entry point.
 *
 * This is the central rescheduling routine which should be called to
 * validate and apply changes which have previously been made to the
 * nucleus scheduling state, such as suspending, resuming or changing
 * the priority of threads.  This call first determines if a thread
 * switch should take place, and performs it as
 * needed. xnpod_schedule() schedules out the current thread if:
 *
 * - the current thread is now blocked or deleted.
 * - a runnable thread from a higher priority scheduling class is
 * waiting for the CPU.
 * - the current thread does not lead the runnable threads from its
 * own scheduling class (e.g. round-robin in the RT class).
 *
 * The nucleus implements a lazy rescheduling scheme so that most
 * of the services affecting the threads state MUST be followed by a
 * call to the rescheduling procedure for the new scheduling state to
 * be applied. In other words, multiple changes on the scheduler state
 * can be done in a row, waking threads up, blocking others, without
 * being immediately translated into the corresponding context
 * switches, like it would be necessary would it appear that a higher
 * priority thread than the current one became runnable for
 * instance. When all changes have been applied, the rescheduling
 * procedure is then called to consider those changes, and possibly
 * replace the current thread by another one.
 *
 * As a notable exception to the previous principle however, every
 * action which ends up suspending or deleting the current thread
 * begets an immediate call to the rescheduling procedure on behalf of
 * the service causing the state transition. For instance,
 * self-suspension, self-destruction, or sleeping on a synchronization
 * object automatically leads to a call to the rescheduling procedure,
 * therefore the caller does not need to explicitly issue
 * xnpod_schedule() after such operations.
 *
 * The rescheduling procedure always leads to a null-effect if it is
 * called on behalf of an ISR or callout. Any outstanding scheduler
 * lock held by the outgoing thread will be restored when the thread
 * is scheduled back in.
 *
 * Calling this procedure with no applicable context switch pending is
 * harmless and simply leads to a null-effect.
 *
 * Side-effects:

 * - If an asynchronous service routine exists, the pending
 * asynchronous signals are delivered to a resuming thread or on
 * behalf of the caller before it returns from the procedure if no
 * context switch has taken place. This behaviour can be disabled by
 * setting the XNASDI flag in the thread's status mask by calling
 * xnpod_set_thread_mode().
 * 
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine, although this leads to a no-op.
 * - Kernel-based task
 * - User-space task
 *
 * @note The switch hooks are called on behalf of the resuming thread.
 */

static inline int __xnpod_test_resched(struct xnsched *sched)
{
	int cpu = xnsched_cpu(sched), resched;
	
	resched = xnarch_cpu_isset(cpu, sched->resched);
	xnarch_cpu_clear(cpu, sched->resched);
#ifdef CONFIG_SMP
	/* Send resched IPI to remote CPU(s). */
	if (unlikely(xnsched_resched_p(sched))) {
		xnarch_send_ipi(sched->resched);
		xnarch_cpus_clear(sched->resched);
	}
#endif
	return resched;
}

void __xnpod_schedule(struct xnsched *sched)
{
	struct xnthread *prev, *next, *curr = sched->curr;
	int zombie, switched = 0, need_resched, shadow;
	spl_t s;

	if (xnarch_escalate())
		return;

	trace_mark(xn_nucleus_sched, MARK_NOARGS);

	xnlock_get_irqsave(&nklock, s);

	xnarch_trace_pid(xnthread_user_task(curr) ?
			 xnarch_user_pid(xnthread_archtcb(curr)) : -1,
			 xnthread_current_priority(curr));

	need_resched = __xnpod_test_resched(sched);
	zombie = xnthread_test_state(curr, XNZOMBIE);

	next = xnsched_pick_next(sched);
	if (next == curr && !xnthread_test_state(curr, XNRESTART))
		/* Note: the root thread never restarts. */
		goto signal_unlock_and_exit;

	XENO_BUGON(NUCLEUS, need_resched == 0);

	prev = curr;

	trace_mark(xn_nucleus_sched_switch,
		   "prev %p prev_name %s "
		   "next %p next_name %s",
		   prev, xnthread_name(prev),
		   next, xnthread_name(next));

#ifdef CONFIG_XENO_OPT_PERVASIVE
	shadow = xnthread_test_state(prev, XNSHADOW);
#else
	(void)shadow;
#endif /* CONFIG_XENO_OPT_PERVASIVE */

	if (xnthread_test_state(next, XNROOT)) {
		xnsched_reset_watchdog(sched);
		xnfreesync();
	}

	if (zombie)
		xnsched_zombie_hooks(prev);

	sched->curr = next;

	if (xnthread_test_state(prev, XNROOT))
		xnarch_leave_root(xnthread_archtcb(prev));
	else if (xnthread_test_state(next, XNROOT))
		xnarch_enter_root(xnthread_archtcb(next));

	xnstat_exectime_switch(sched, &next->stat.account);
	xnstat_counter_inc(&next->stat.csw);

	xnpod_switch_to(sched, prev, next);

	switched = 1;
	sched = xnsched_finish_unlocked_switch(sched);
	/*
	 * Re-read the currently running thread, this is needed
	 * because of relaxed/hardened transitions.
	 */
	curr = sched->curr;

	xnarch_trace_pid(xnthread_user_task(curr) ?
			 xnarch_user_pid(xnthread_archtcb(curr)) : -1,
			 xnthread_current_priority(curr));

#ifdef CONFIG_XENO_OPT_PERVASIVE
	/*
	 * Test whether we are relaxing a thread. In such a case, we
	 * are here the epilogue of Linux' schedule, and should skip
	 * xnpod_schedule epilogue.
	 */
	if (shadow && xnthread_test_state(curr, XNROOT))
		goto relax_epilogue;
#endif /* CONFIG_XENO_OPT_PERVASIVE */

	if (zombie)
		xnpod_fatal("zombie thread %s (%p) would not die...",
			    prev->name, prev);

	xnpod_finalize_zombie(sched);

	__xnpod_switch_fpu(sched);

#ifdef __XENO_SIM__
	if (nkpod->schedhook)
		nkpod->schedhook(curr, XNRUNNING);
#endif /* __XENO_SIM__ */

	if (!emptyq_p(&nkpod->tswitchq) && !xnthread_test_state(curr, XNROOT)) {
		trace_mark(xn_nucleus_thread_callout,
			   "thread %p thread_name %s hook %s",
			   curr, xnthread_name(curr), "SWITCH");
		xnpod_fire_callouts(&nkpod->tswitchq, curr);
	}

      signal_unlock_and_exit:

	if (xnthread_signaled_p(curr))
		xnpod_dispatch_signals();

	xnlock_put_irqrestore(&nklock, s);

	if (switched)
		xnsched_resched_after_unlocked_switch();

	return;

#ifdef CONFIG_XENO_OPT_PERVASIVE
      relax_epilogue:
	{
		spl_t ignored;

		/* Shadow on entry and root without shadow extension on exit? 
		   Mmmm... This must be the user-space mate of a deleted real-time
		   shadow we've just rescheduled in the Linux domain to have it
		   exit properly.  Reap it now. */
		if (xnshadow_thrptd(current) == NULL) {
			xnlock_clear_irqon(&nklock);
			xnshadow_exit();
		}

		/* We need to relock nklock here, since it is not locked and
		   the caller may expect it to be locked. */
		xnlock_get_irqsave(&nklock, ignored);
		xnlock_put_irqrestore(&nklock, s);

		return;
	}
#endif /* CONFIG_XENO_OPT_PERVASIVE */
}

/*! 
 * \fn int xnpod_add_hook(int type,void (*routine)(xnthread_t *))
 * \brief Install a nucleus hook.
 *
 * The nucleus allows to register user-defined routines which get
 * called whenever a specific scheduling event occurs. Multiple hooks
 * can be chained for a single event type, and get called on a FIFO
 * basis.
 *
 * The scheduling is locked while a hook is executing.
 *
 * @param type Defines the kind of hook to install:
 *
 *        - XNHOOK_THREAD_START: The user-defined routine will be
 *        called on behalf of the starter thread whenever a new thread
 *        starts. The descriptor address of the started thread is
 *        passed to the routine.
 *
 *        - XNHOOK_THREAD_DELETE: The user-defined routine will be
 *        called on behalf of the deletor thread whenever a thread is
 *        deleted. The descriptor address of the deleted thread is
 *        passed to the routine.
 *
 *        - XNHOOK_THREAD_SWITCH: The user-defined routine will be
 *        called on behalf of the resuming thread whenever a context
 *        switch takes place. The descriptor address of the thread
 *        which has been switched out is passed to the routine.
 *
 * @param routine The address of the user-supplied routine to call.
 *
 * @return 0 is returned on success. Otherwise, one of the following
 * error codes indicates the cause of the failure:
 *
 *         - -EINVAL is returned if type is incorrect.
 *
 *         - -ENOMEM is returned if not enough memory is available
 *         from the system heap to add the new hook.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_add_hook(int type, void (*routine) (xnthread_t *))
{
	xnqueue_t *hookq;
	xnhook_t *hook;
	int err = 0;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_sched_addhook, "type %d routine %p",
		   type, routine);

	switch (type) {
	case XNHOOK_THREAD_START:
		hookq = &nkpod->tstartq;
		break;
	case XNHOOK_THREAD_SWITCH:
		hookq = &nkpod->tswitchq;
		break;
	case XNHOOK_THREAD_DELETE:
		hookq = &nkpod->tdeleteq;
		break;
	default:
		err = -EINVAL;
		goto unlock_and_exit;
	}

	hook = xnmalloc(sizeof(*hook));

	if (hook) {
		inith(&hook->link);
		hook->routine = routine;
		prependq(hookq, &hook->link);
	} else
		err = -ENOMEM;

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}

/*! 
 * \fn int xnpod_remove_hook(int type,void (*routine)(xnthread_t *))
 * \brief Remove a nucleus hook.
 *
 * This service removes a nucleus hook previously registered using
 * xnpod_add_hook().
 *
 * @param type Defines the kind of hook to remove among
 * XNHOOK_THREAD_START, XNHOOK_THREAD_DELETE and XNHOOK_THREAD_SWITCH.
 *
 * @param routine The address of the user-supplied routine to remove.
 *
 * @return 0 is returned on success. Otherwise, -EINVAL is returned if
 * type is incorrect or if the routine has never been registered
 * before.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

int xnpod_remove_hook(int type, void (*routine) (xnthread_t *))
{
	xnhook_t *hook = NULL;
	xnholder_t *holder;
	xnqueue_t *hookq;
	int err = 0;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_sched_removehook, "type %d routine %p",
		   type, routine);

	switch (type) {
	case XNHOOK_THREAD_START:
		hookq = &nkpod->tstartq;
		break;
	case XNHOOK_THREAD_SWITCH:
		hookq = &nkpod->tswitchq;
		break;
	case XNHOOK_THREAD_DELETE:
		hookq = &nkpod->tdeleteq;
		break;
	default:
		goto bad_hook;
	}

	for (holder = getheadq(hookq);
	     holder != NULL; holder = nextq(hookq, holder)) {
		hook = link2hook(holder);

		if (hook->routine == routine) {
			removeq(hookq, holder);
			xnfree(hook);
			goto unlock_and_exit;
		}
	}

      bad_hook:

	err = -EINVAL;

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}

/*! 
 * \fn void xnpod_trap_fault(xnarch_fltinfo_t *fltinfo);
 * \brief Default fault handler.
 *
 * This is the default handler which is called whenever an uncontrolled
 * exception or fault is caught. If the fault is caught on behalf of a
 * real-time thread, the fault is not propagated to the host system.
 * Otherwise, the fault is unhandled by the nucleus and simply propagated.
 *
 * @param fltinfo An opaque pointer to the arch-specific buffer
 * describing the fault. The actual layout is defined by the
 * xnarch_fltinfo_t type in each arch-dependent layer file.
 *
 */

int xnpod_trap_fault(xnarch_fltinfo_t *fltinfo)
{
	xnthread_t *thread;

	if (!xnpod_active_p() ||
	    (!xnpod_interrupt_p() && xnpod_idle_p()))
		return 0;

	thread = xnpod_current_thread();

	trace_mark(xn_nucleus_thread_fault,
		   "thread %p thread_name %s address %lu type %d",
		   thread, xnthread_name(thread), xnarch_fault_pc(fltinfo),
		   xnarch_fault_trap(fltinfo));

#ifdef __KERNEL__
	if (xnarch_fault_fpu_p(fltinfo)) {
		if (__xnpod_fault_init_fpu(thread))
			return 1;
		print_symbol("invalid use of FPU in Xenomai context at %s\n",
			     xnarch_fault_pc(fltinfo));
	}

	if (!xnpod_userspace_p()) {
		xnprintf
		    ("suspending kernel thread %p ('%s') at 0x%lx after exception #%u\n",
		     thread, thread->name, xnarch_fault_pc(fltinfo),
		     xnarch_fault_trap(fltinfo));

		xnpod_suspend_thread(thread, XNSUSP, XN_INFINITE, XN_RELATIVE, NULL);
		return 1;
	}

#ifdef CONFIG_XENO_OPT_PERVASIVE
	/* If we experienced a trap on behalf of a shadow thread, just
	   move the second to the Linux domain, so that the host O/S
	   (e.g. Linux) can attempt to process the exception. This is
	   especially useful in order to handle user-space errors or debug
	   stepping properly. */

	if (xnpod_shadow_p()) {
#if XENO_DEBUG(NUCLEUS)
		if (!xnarch_fault_um(fltinfo)) {
			xnarch_trace_panic_freeze();
			xnprintf
			    ("Switching %s to secondary mode after exception #%u in "
			     "kernel-space at 0x%lx (pid %d)\n", thread->name,
			     xnarch_fault_trap(fltinfo),
			     xnarch_fault_pc(fltinfo),
			     xnthread_user_pid(thread));
			xnarch_trace_panic_dump();
		} else if (xnarch_fault_notify(fltinfo))	/* Don't report debug traps */
			xnprintf
			    ("Switching %s to secondary mode after exception #%u from "
			     "user-space at 0x%lx (pid %d)\n", thread->name,
			     xnarch_fault_trap(fltinfo),
			     xnarch_fault_pc(fltinfo),
			     xnthread_user_pid(thread));
#endif /* XENO_DEBUG(NUCLEUS) */
		if (xnarch_fault_pf_p(fltinfo))
			/* The page fault counter is not SMP-safe, but it's a
			   simple indicator that something went wrong wrt memory
			   locking anyway. */
			xnstat_counter_inc(&thread->stat.pf);

		xnshadow_relax(xnarch_fault_notify(fltinfo));
	}
#endif /* CONFIG_XENO_OPT_PERVASIVE */
#endif /* __KERNEL__ */

	return 0;
}

/*! 
 * \fn int xnpod_enable_timesource(void)
 * \brief Activate the core time source.
 *
 * Xenomai implements the notion of time base, by which software
 * timers that belong to different skins may be clocked separately
 * according to distinct frequencies, or aperiodically. In the
 * periodic case, delays and timeouts are given in counts of ticks;
 * the duration of a tick is specified by the time base. In the
 * aperiodic case, timings are directly specified in nanoseconds.
 *
 * Only a single aperiodic (i.e. tick-less) time base may exist in the
 * system, and the nucleus provides for it through the nktbase
 * object. All skins depending on aperiodic timings should bind to the
 * latter, also known as the master time base. Skins depending on
 * periodic timings may create and bind to their own time base. Such a
 * periodic time base is managed as a slave object of the master one.
 * A cascading software timer, which is fired by the master time base
 * according to the appropriate frequency, triggers in turn the update
 * process of the associated slave time base, which eventually fires
 * the elapsed software timers controlled by the latter.
 *
 * Xenomai always controls the underlying hardware timer in a
 * tick-less fashion, also known as the oneshot mode. The
 * xnpod_enable_timesource() service configures the timer chip as
 * needed, and activates the master time base.
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -ENODEV is returned if a failure occurred while configuring the
 * hardware timer.
 *
 * - -ENOSYS is returned if no active pod exists.
 *
 * Side-effect: A host timing service is started in order to relay the
 * canonical periodical tick to the underlying architecture,
 * regardless of the frequency used for Xenomai's system tick. This
 * routine does not call the rescheduling procedure.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - User-space task in secondary mode
 *
 * Rescheduling: never.
 *
 * @note Built-in support for periodic timing depends on
 * CONFIG_XENO_OPT_TIMING_PERIODIC.
 */

int xnpod_enable_timesource(void)
{
	int err,  htickval, cpu;
	xnsched_t *sched;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (!xnpod_active_p()) {
		err = -ENOSYS;
		xnlock_put_irqrestore(&nklock, s);
		return err;
	}

	trace_mark(xn_nucleus_tbase_start, "base %s", nktbase.name);

#ifdef CONFIG_XENO_OPT_STATS
	/*
	 * Only for statistical purpose, the clock interrupt will be
	 * attached directly by the arch-dependent layer
	 * (xnarch_start_timer).
	 */
	xnintr_init(&nkclock, "[timer]", XNARCH_TIMER_IRQ, NULL, NULL, 0);
#endif /* CONFIG_XENO_OPT_STATS */

	nktbase.status = XNTBRUN;

	xnlock_put_irqrestore(&nklock, s);

	nktbase.wallclock_offset =
		xnarch_get_host_time() + xnarch_get_cpu_time();

	for (cpu = 0; cpu < xnarch_num_online_cpus(); cpu++) {

		sched = xnpod_sched_slot(cpu);

		htickval = xnarch_start_timer(&xnintr_clock_handler, cpu);

		if (htickval < 0) {
			while (--cpu >= 0)
				xnarch_stop_timer(cpu);

			return htickval;
		}

		xnlock_get_irqsave(&nklock, s);

		/* If the current tick device for the target CPU is
		 * periodic, we won't be called back for host tick
		 * emulation. Therefore, we need to start a periodic
		 * nucleus timer which will emulate the ticking for
		 * that CPU, since we are going to hijack the hw clock
		 * chip for managing our own system timer.
		 *
		 * CAUTION:
		 *
		 * - nucleus timers may be started only _after_ the hw
		 * timer has been set up for the target CPU through a
		 * call to xnarch_start_timer().
		 *
		 * - we don't compensate for the elapsed portion of
		 * the current host tick, since we cannot get this
		 * information easily for all CPUs except the current
		 * one, and also because of the declining relevance of
		 * the jiffies clocksource anyway.
		 *
		 * - we must not hold the nklock across calls to
		 * xnarch_start_timer().
		 */

		if (htickval > 1)
			xntimer_start(&sched->htimer, htickval, htickval, XN_RELATIVE);
		else
			xntimer_start(&sched->htimer, 0, 0, XN_RELATIVE);

#if defined(CONFIG_XENO_OPT_WATCHDOG)
		xntimer_start(&sched->wdtimer, 1000000000UL, 1000000000UL, XN_RELATIVE);
		xnsched_reset_watchdog(sched);
#endif /* CONFIG_XENO_OPT_WATCHDOG */
		xnlock_put_irqrestore(&nklock, s);
	}

	return 0;
}

/*! 
 * \fn void xnpod_disable_timesource(void)
 * \brief Stop the core time source.
 *
 * Releases the hardware timer, and deactivates the master time base.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - User-space task in secondary mode
 *
 * Rescheduling: never.
 */

void xnpod_disable_timesource(void)
{
	spl_t s;
	int cpu;

	trace_mark(xn_nucleus_tbase_stop, "base %s", nktbase.name);

	xnlock_get_irqsave(&nklock, s);

	if (!xnpod_active_p() || !xntbase_enabled_p(&nktbase)) {
		xnlock_put_irqrestore(&nklock, s);
		return;
	}

	__clrbits(nktbase.status, XNTBRUN);

	xnlock_put_irqrestore(&nklock, s);

	/* We must not hold the nklock while stopping the hardware
	   timer, since this could cause deadlock situations to arise
	   on SMP systems. */
	for (cpu = 0; cpu < xnarch_num_online_cpus(); cpu++)
		xnarch_stop_timer(cpu);

	xntimer_freeze();

	/* NOTE: The nkclock interrupt object is not destroyed on
	   purpose since this would be mostly redundant after
	   xnarch_stop_timer() has been called. In any case, no
	   resource is associated with this object. */
}

/*!
 * \fn int xnpod_set_thread_periodic(xnthread_t *thread,xnticks_t idate,xnticks_t period)
 * \brief Make a thread periodic.
 *
 * Make a thread periodic by programming its first release point and
 * its period in the processor time line.  Subsequent calls to
 * xnpod_wait_thread_period() will delay the thread until the next
 * periodic release point in the processor timeline is reached.
 *
 * @param thread The descriptor address of the affected thread. This
 * thread is immediately delayed until the first periodic release
 * point is reached.
 *
 * @param idate The initial (absolute) date of the first release
 * point, expressed in clock ticks (see note). The affected thread
 * will be delayed until this point is reached. If @a idate is equal
 * to XN_INFINITE, the current system date is used, and no initial
 * delay takes place.

 * @param period The period of the thread, expressed in clock ticks
 * (see note). As a side-effect, passing XN_INFINITE attempts to stop
 * the thread's periodic timer; in the latter case, the routine always
 * exits succesfully, regardless of the previous state of this timer.
 *
 * @return 0 is returned upon success. Otherwise:
 *
 * - -ETIMEDOUT is returned @a idate is different from XN_INFINITE and
 * represents a date in the past.
 *
 * - -EWOULDBLOCK is returned if the relevant time base has not been
 * initialized by a call to xnpod_init_timebase().
 *
 * - -EINVAL is returned if @a period is different from XN_INFINITE
 * but shorter than the scheduling latency value for the target
 * system, as available from /proc/xenomai/latency.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: possible if the operation affects the current thread
 * and @a idate has not elapsed yet.
 *
 * @note The @a idate and @a period values will be interpreted as
 * jiffies if @a thread is bound to a periodic time base (see
 * xnpod_init_thread), or nanoseconds otherwise.
 */

int xnpod_set_thread_periodic(xnthread_t *thread,
			      xnticks_t idate, xnticks_t period)
{
	int err = 0;
	spl_t s;

	if (!xnthread_timed_p(thread))
		return -EWOULDBLOCK;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus_thread_setperiodic,
		   "thread %p thread_name %s idate %Lu period %Lu timer %p",
		   thread, xnthread_name(thread), idate, period,
		   &thread->ptimer);

	if (period == XN_INFINITE) {
		if (xntimer_running_p(&thread->ptimer))
			xntimer_stop(&thread->ptimer);

		goto unlock_and_exit;
	} else if (!xntbase_periodic_p(xnthread_time_base(thread))
		   && period < nklatency) {
		/* LART: detect periods which are shorter than the
		 * intrinsic latency figure; this must be a joke... */
		err = -EINVAL;
		goto unlock_and_exit;
	}

	xntimer_set_sched(&thread->ptimer, thread->sched);

	if (idate == XN_INFINITE) {
		xntimer_start(&thread->ptimer, period, period, XN_RELATIVE);
	} else {
		idate -= xntbase_get_wallclock_offset(
			xntimer_base(&thread->ptimer));
		err = xntimer_start(&thread->ptimer, idate, period,
				    XN_ABSOLUTE);
		if (err)
			goto unlock_and_exit;

		/* We could call xntimer_get_overruns after
		   xnpod_suspend_thread, but we would need to return the count
		   of overruns to the caller, otherwise, these overruns
		   would be lost. */
		xntimer_pexpect_forward(&thread->ptimer,
					xntimer_interval(&thread->ptimer));
		xnpod_suspend_thread(thread, XNDELAY, XN_INFINITE,
				     XN_RELATIVE, NULL);
	}

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}

/**
 * @fn int xnpod_wait_thread_period(unsigned long *overruns_r)
 * @brief Wait for the next periodic release point.
 *
 * Make the current thread wait for the next periodic release point in
 * the processor time line.
 *
 * @param overruns_r If non-NULL, @a overruns_r must be a pointer to a
 * memory location which will be written with the count of pending
 * overruns. This value is copied only when xnpod_wait_thread_period()
 * returns -ETIMEDOUT or success; the memory location remains
 * unmodified otherwise. If NULL, this count will never be copied
 * back.
 *
 * @return 0 is returned upon success; if @a overruns_r is valid, zero
 * is copied to the pointed memory location. Otherwise:
 *
 * - -EWOULDBLOCK is returned if xnpod_set_thread_periodic() has not
 * previously been called for the calling thread.
 *
 * - -EINTR is returned if xnpod_unblock_thread() has been called for
 * the waiting thread before the next periodic release point has been
 * reached. In this case, the overrun counter is reset too.
 *
 * - -ETIMEDOUT is returned if the timer has overrun, which indicates
 * that one or more previous release points have been missed by the
 * calling thread. If @a overruns_r is valid, the count of pending
 * overruns is copied to the pointed memory location.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: always, unless the current release point has already
 * been reached.  In the latter case, the current thread immediately
 * returns from this service without being delayed.
 */

int xnpod_wait_thread_period(unsigned long *overruns_r)
{
	xnticks_t now;
	unsigned long overruns = 0;
	xnthread_t *thread;
	xntbase_t *tbase;
	int err = 0;
	spl_t s;

	thread = xnpod_current_thread();

	xnlock_get_irqsave(&nklock, s);

	if (unlikely(!xntimer_running_p(&thread->ptimer))) {
		err = -EWOULDBLOCK;
		goto unlock_and_exit;
	}

	trace_mark(xn_nucleus_thread_waitperiod, "thread %p thread_name %s",
		   thread, xnthread_name(thread));

	/* Work with either TSC or periodic ticks. */
	tbase = xnthread_time_base(thread);
	now = xntbase_get_rawclock(tbase);

	if (likely((xnsticks_t)(now - xntimer_pexpect(&thread->ptimer)) < 0)) {
		xnpod_suspend_thread(thread, XNDELAY, XN_INFINITE, XN_RELATIVE, NULL);

		if (unlikely(xnthread_test_info(thread, XNBREAK))) {
			err = -EINTR;
			goto unlock_and_exit;
		}

		now = xntbase_get_rawclock(tbase);
	}

	overruns = xntimer_get_overruns(&thread->ptimer, now);
	if (overruns) {
		err = -ETIMEDOUT;

		trace_mark(xn_nucleus_thread_missedperiod,
			   "thread %p thread_name %s overruns %lu",
			   thread, xnthread_name(thread), overruns);
	}

	if (likely(overruns_r != NULL))
		*overruns_r = overruns;

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return err;
}

/*@}*/

EXPORT_SYMBOL(xnpod_activate_rr);
EXPORT_SYMBOL(xnpod_add_hook);
EXPORT_SYMBOL(xnpod_deactivate_rr);
EXPORT_SYMBOL(xnpod_delete_thread);
EXPORT_SYMBOL(xnpod_abort_thread);
EXPORT_SYMBOL(xnpod_fatal_helper);
EXPORT_SYMBOL(xnpod_init);
EXPORT_SYMBOL(xnpod_init_thread);
EXPORT_SYMBOL(xnpod_migrate_thread);
EXPORT_SYMBOL(xnpod_remove_hook);
EXPORT_SYMBOL(xnpod_renice_thread);
EXPORT_SYMBOL(xnpod_restart_thread);
EXPORT_SYMBOL(xnpod_resume_thread);
EXPORT_SYMBOL(xnpod_set_thread_mode);
EXPORT_SYMBOL(xnpod_set_thread_periodic);
EXPORT_SYMBOL(xnpod_shutdown);
EXPORT_SYMBOL(xnpod_start_thread);
EXPORT_SYMBOL(xnpod_enable_timesource);
EXPORT_SYMBOL(xnpod_disable_timesource);
EXPORT_SYMBOL(xnpod_suspend_thread);
EXPORT_SYMBOL(xnpod_trap_fault);
EXPORT_SYMBOL(xnpod_unblock_thread);
EXPORT_SYMBOL(xnpod_wait_thread_period);
EXPORT_SYMBOL(xnpod_welcome_thread);
EXPORT_SYMBOL(__xnpod_schedule);

EXPORT_SYMBOL(nkpod_struct);

#ifdef CONFIG_SMP
EXPORT_SYMBOL(nklock);
#endif /* CONFIG_SMP */
EXPORT_SYMBOL(nklatency);
