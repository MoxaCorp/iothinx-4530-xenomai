/*
 * Copyright (C) 2011 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#include <errno.h>
#include <string.h>
#include <copperplate/threadobj.h>
#include <copperplate/heapobj.h>
#include "internal.h"
#include "cond.h"
#include "timer.h"
#include "mutex.h"

/*
 * XXX: Alchemy condvars are paired with Alchemy mutex objects, so we
 * must rely on POSIX condvars directly.
 */

struct syncluster alchemy_cond_table;

static struct alchemy_namegen cond_namegen = {
	.prefix = "cond",
	.length = sizeof ((struct alchemy_cond *)0)->name,
};

static struct alchemy_cond *find_alchemy_cond(RT_COND *cond, int *err_r)
{
	struct alchemy_cond *ccb;

	if (cond == NULL || ((intptr_t)cond & (sizeof(intptr_t)-1)) != 0)
		goto bad_handle;

	ccb = mainheap_deref(cond->handle, struct alchemy_cond);
	if (ccb == NULL || ((intptr_t)ccb & (sizeof(intptr_t)-1)) != 0)
		goto bad_handle;

	if (ccb->magic == ~cond_magic) {
		*err_r = -EIDRM;
		return NULL;
	}

	if (ccb->magic == cond_magic)
		return ccb;

bad_handle:
	*err_r = -EINVAL;
	return NULL;
}

int rt_cond_create(RT_COND *cond, const char *name)
{
	struct alchemy_cond *ccb;
	pthread_condattr_t cattr;
	struct service svc;

	if (threadobj_async_p())
		return -EPERM;

	COPPERPLATE_PROTECT(svc);

	ccb = xnmalloc(sizeof(*ccb));
	if (ccb == NULL) {
		COPPERPLATE_UNPROTECT(svc);
		return -ENOMEM;
	}

	alchemy_build_name(ccb->name, name, &cond_namegen);

	if (syncluster_addobj(&alchemy_cond_table, ccb->name, &ccb->cobj)) {
		xnfree(ccb);
		COPPERPLATE_UNPROTECT(svc);
		return -EEXIST;
	}

	__RT(pthread_condattr_init(&cattr));
	__RT(pthread_condattr_setpshared(&cattr, mutex_scope_attribute));
	__RT(pthread_condattr_setclock(&cattr, CLOCK_COPPERPLATE));
	__RT(pthread_cond_init(&ccb->cond, &cattr));
	__RT(pthread_condattr_destroy(&cattr));
	ccb->magic = cond_magic;
	cond->handle = mainheap_ref(ccb, uintptr_t);

	COPPERPLATE_UNPROTECT(svc);

	return 0;
}

int rt_cond_delete(RT_COND *cond)
{
	struct alchemy_cond *ccb;
	struct service svc;
	int ret = 0;

	if (threadobj_async_p())
		return -EPERM;

	COPPERPLATE_PROTECT(svc);

	ccb = find_alchemy_cond(cond, &ret);
	if (ccb == NULL)
		goto out;

	ret = -__RT(pthread_cond_destroy(&ccb->cond));
	if (ret)
		goto out;

	ccb->magic = ~cond_magic;
	syncluster_delobj(&alchemy_cond_table, &ccb->cobj);
	xnfree(ccb);
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_cond_signal(RT_COND *cond)
{
	struct alchemy_cond *ccb;
	struct service svc;
	int ret = 0;

	COPPERPLATE_PROTECT(svc);

	ccb = find_alchemy_cond(cond, &ret);
	if (ccb == NULL)
		goto out;

	ret = -__RT(pthread_cond_signal(&ccb->cond));
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_cond_broadcast(RT_COND *cond)
{
	struct alchemy_cond *ccb;
	struct service svc;
	int ret = 0;

	COPPERPLATE_PROTECT(svc);

	ccb = find_alchemy_cond(cond, &ret);
	if (ccb == NULL)
		goto out;

	ret = -__RT(pthread_cond_broadcast(&ccb->cond));
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_cond_wait_until(RT_COND *cond, RT_MUTEX *mutex,
		       RTIME timeout)
{
	struct alchemy_mutex *mcb;
	struct alchemy_cond *ccb;
	struct service svc;
	struct timespec ts;
	int ret = 0;

	if (timeout == TM_NONBLOCK)
		return -EWOULDBLOCK;

	COPPERPLATE_PROTECT(svc);

	ccb = find_alchemy_cond(cond, &ret);
	if (ccb == NULL)
		goto out;

	mcb = find_alchemy_mutex(mutex, &ret);
	if (mcb == NULL)
		goto out;

	if (timeout != TM_INFINITE) {
		clockobj_ticks_to_timespec(&alchemy_clock, timeout, &ts);
		ret = -__RT(pthread_cond_timedwait(&ccb->cond, &mcb->lock, &ts));
	} else
		ret = -__RT(pthread_cond_wait(&ccb->cond, &mcb->lock));
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_cond_wait(RT_COND *cond, RT_MUTEX *mutex,
		 RTIME timeout)
{
	timeout = alchemy_rel2abs_timeout(timeout);
	return rt_cond_wait_until(cond, mutex, timeout);
}

int rt_cond_inquire(RT_COND *cond, RT_COND_INFO *info)
{
	struct alchemy_cond *ccb;
	struct service svc;
	int ret = 0;

	COPPERPLATE_PROTECT(svc);

	ccb = find_alchemy_cond(cond, &ret);
	if (ccb == NULL)
		goto out;

	strcpy(info->name, ccb->name);
out:
	COPPERPLATE_UNPROTECT(svc);

	return ret;
}

int rt_cond_bind(RT_COND *cond,
		 const char *name, RTIME timeout)
{
	return alchemy_bind_object(name,
				   &alchemy_cond_table,
				   timeout,
				   offsetof(struct alchemy_cond, cobj),
				   &cond->handle);
}

int rt_cond_unbind(RT_COND *cond)
{
	cond->handle = 0;
	return 0;
}
