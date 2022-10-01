// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tegra host1x Syncpoints
 *
 * Copyright (c) 2010-2015, NVIDIA Corporation.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>

#include <trace/events/host1x.h>

#include "syncpt.h"
#include "dev.h"
#include "intr.h"
#include "debug.h"

#define SYNCPT_CHECK_PERIOD (2 * HZ)
#define MAX_STUCK_CHECK_COUNT 15

static struct host1x_syncpt_base *
host1x_syncpt_base_request(struct host1x *host)
{
	struct host1x_syncpt_base *bases = host->bases;
	unsigned int i;

	for (i = 0; i < host->info->nb_bases; i++)
		if (!bases[i].requested)
			break;

	if (i >= host->info->nb_bases)
		return NULL;

	bases[i].requested = true;
	return &bases[i];
}

static void host1x_syncpt_base_free(struct host1x_syncpt_base *base)
{
	if (base)
		base->requested = false;
}

static struct host1x_syncpt *host1x_syncpt_alloc(struct host1x *host,
						 struct host1x_client *client,
						 unsigned long flags)
{
	struct host1x_syncpt *sp = host->syncpt;
	unsigned int i;
	char *name;

	mutex_lock(&host->syncpt_mutex);

	for (i = 0; i < host->info->nb_pts && sp->name; i++, sp++)
		;

	if (i >= host->info->nb_pts)
		goto unlock;

	if (flags & HOST1X_SYNCPT_HAS_BASE) {
		sp->base = host1x_syncpt_base_request(host);
		if (!sp->base)
			goto unlock;
	}

	name = kasprintf(GFP_KERNEL, "%02u-%s", sp->id,
			 client ? dev_name(client->dev) : NULL);
	if (!name)
		goto free_base;

	sp->client = client;
	sp->name = name;

	if (flags & HOST1X_SYNCPT_CLIENT_MANAGED)
		sp->client_managed = true;
	else
		sp->client_managed = false;

	mutex_unlock(&host->syncpt_mutex);
	return sp;

free_base:
	host1x_syncpt_base_free(sp->base);
	sp->base = NULL;
unlock:
	mutex_unlock(&host->syncpt_mutex);
	return NULL;
}

/**
 * host1x_syncpt_id() - retrieve syncpoint ID
 * @sp: host1x syncpoint
 *
 * Given a pointer to a struct host1x_syncpt, retrieves its ID. This ID is
 * often used as a value to program into registers that control how hardware
 * blocks interact with syncpoints.
 */
u32 host1x_syncpt_id(struct host1x_syncpt *sp)
{
	return sp->id;
}
EXPORT_SYMBOL(host1x_syncpt_id);

/**
 * host1x_syncpt_incr_max() - update the value sent to hardware
 * @sp: host1x syncpoint
 * @incrs: number of increments
 */
u32 host1x_syncpt_incr_max(struct host1x_syncpt *sp, u32 incrs)
{
	return (u32)atomic_add_return(incrs, &sp->max_val);
}
EXPORT_SYMBOL(host1x_syncpt_incr_max);

 /*
 * Write cached syncpoint and waitbase values to hardware.
 */
void host1x_syncpt_restore(struct host1x *host)
{
	struct host1x_syncpt *sp_base = host->syncpt;
	unsigned int i;

	for (i = 0; i < host1x_syncpt_nb_pts(host); i++)
		host1x_hw_syncpt_restore(host, sp_base + i);

	for (i = 0; i < host1x_syncpt_nb_bases(host); i++)
		host1x_hw_syncpt_restore_wait_base(host, sp_base + i);

	wmb();
}

/*
 * Update the cached syncpoint and waitbase values by reading them
 * from the registers.
  */
void host1x_syncpt_save(struct host1x *host)
{
	struct host1x_syncpt *sp_base = host->syncpt;
	unsigned int i;

	for (i = 0; i < host1x_syncpt_nb_pts(host); i++) {
		if (host1x_syncpt_client_managed(sp_base + i))
			host1x_hw_syncpt_load(host, sp_base + i);
		else
			WARN_ON(!host1x_syncpt_idle(sp_base + i));
	}

	for (i = 0; i < host1x_syncpt_nb_bases(host); i++)
		host1x_hw_syncpt_load_wait_base(host, sp_base + i);
}

/*
 * Updates the cached syncpoint value by reading a new value from the hardware
 * register
 */
u32 host1x_syncpt_load(struct host1x_syncpt *sp)
{
	u32 val;

	val = host1x_hw_syncpt_load(sp->host, sp);
	trace_host1x_syncpt_load_min(sp->id, val);

	return val;
}

/*
 * Get the current syncpoint base
 */
u32 host1x_syncpt_load_wait_base(struct host1x_syncpt *sp)
{
	host1x_hw_syncpt_load_wait_base(sp->host, sp);

	return sp->base_val;
}

/**
 * host1x_syncpt_incr() - increment syncpoint value from CPU, updating cache
 * @sp: host1x syncpoint
 */
int host1x_syncpt_incr(struct host1x_syncpt *sp)
{
	return host1x_hw_syncpt_cpu_incr(sp->host, sp);
}
EXPORT_SYMBOL(host1x_syncpt_incr);

/*
 * Updated sync point form hardware, and returns true if syncpoint is expired,
 * false if we may need to wait
 */
static bool syncpt_load_min_is_expired(struct host1x_syncpt *sp, u32 thresh)
{
	host1x_hw_syncpt_load(sp->host, sp);

	return host1x_syncpt_is_expired(sp, thresh);
}

/**
 * host1x_syncpt_wait() - wait for a syncpoint to reach a given value
 * @sp: host1x syncpoint
 * @thresh: threshold
 * @timeout: maximum time to wait for the syncpoint to reach the given value
 * @value: return location for the syncpoint value
 */
int host1x_syncpt_wait(struct host1x_syncpt *sp, u32 thresh, long timeout,
		       u32 *value)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);
	void *ref;
	struct host1x_waitlist *waiter;
	int err = 0, check_count = 0;
	u32 val;

	if (value)
		*value = 0;

	/* first check cache */
	if (host1x_syncpt_is_expired(sp, thresh)) {
		if (value)
			*value = host1x_syncpt_load(sp);

		return 0;
	}

	/* try to read from register */
	val = host1x_hw_syncpt_load(sp->host, sp);
	if (host1x_syncpt_is_expired(sp, thresh)) {
		if (value)
			*value = val;

		goto done;
	}

	if (!timeout) {
		err = -EAGAIN;
		goto done;
	}

	/* allocate a waiter */
	waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
	if (!waiter) {
		err = -ENOMEM;
		goto done;
	}

	/* schedule a wakeup when the syncpoint value is reached */
	err = host1x_intr_add_action(sp->host, sp, thresh,
				     HOST1X_INTR_ACTION_WAKEUP_INTERRUPTIBLE,
				     &wq, waiter, &ref);
	if (err)
		goto done;

	err = -EAGAIN;
	/* Caller-specified timeout may be impractically low */
	if (timeout < 0)
		timeout = LONG_MAX;

	/* wait for the syncpoint, or timeout, or signal */
	while (timeout) {
		long check = min_t(long, SYNCPT_CHECK_PERIOD, timeout);
		int remain;

		remain = wait_event_interruptible_timeout(wq,
				syncpt_load_min_is_expired(sp, thresh),
				check);
		if (remain > 0 || host1x_syncpt_is_expired(sp, thresh)) {
			if (value)
				*value = host1x_syncpt_load(sp);

			err = 0;

			break;
		}

		if (remain < 0) {
			err = remain;
			break;
		}

		timeout -= check;

		if (timeout && check_count <= MAX_STUCK_CHECK_COUNT) {
			dev_warn(sp->host->dev,
				"%s: syncpoint id %u (%s) stuck waiting %d, timeout=%ld\n",
				 current->comm, sp->id, sp->name,
				 thresh, timeout);

			host1x_debug_dump_syncpts(sp->host);

			if (check_count == MAX_STUCK_CHECK_COUNT)
				host1x_debug_dump(sp->host);

			check_count++;
		}
	}

	host1x_intr_put_ref(sp->host, sp->id, ref);

done:
	return err;
}
EXPORT_SYMBOL(host1x_syncpt_wait);

/*
 * Returns true if syncpoint is expired, false if we may need to wait
 */
bool host1x_syncpt_is_expired(struct host1x_syncpt *sp, u32 thresh)
{
	u32 current_val;
	u32 future_val;

	smp_rmb();

	current_val = (u32)atomic_read(&sp->min_val);
	future_val = (u32)atomic_read(&sp->max_val);

	/* Note the use of unsigned arithmetic here (mod 1<<32).
	 *
	 * c = current_val = min_val	= the current value of the syncpoint.
	 * t = thresh			= the value we are checking
	 * f = future_val  = max_val	= the value c will reach when all
	 *				  outstanding increments have completed.
	 *
	 * Note that c always chases f until it reaches f.
	 *
	 * Dtf = (f - t)
	 * Dtc = (c - t)
	 *
	 *  Consider all cases:
	 *
	 *	A) .....c..t..f.....	Dtf < Dtc	need to wait
	 *	B) .....c.....f..t..	Dtf > Dtc	expired
	 *	C) ..t..c.....f.....	Dtf > Dtc	expired	   (Dct very large)
	 *
	 *  Any case where f==c: always expired (for any t).	Dtf == Dcf
	 *  Any case where t==c: always expired (for any f).	Dtf >= Dtc (because Dtc==0)
	 *  Any case where t==f!=c: always wait.		Dtf <  Dtc (because Dtf==0,
	 *							Dtc!=0)
	 *
	 *  Other cases:
	 *
	 *	A) .....t..f..c.....	Dtf < Dtc	need to wait
	 *	A) .....f..c..t.....	Dtf < Dtc	need to wait
	 *	A) .....f..t..c.....	Dtf > Dtc	expired
	 *
	 *   So:
	 *	   Dtf >= Dtc implies EXPIRED	(return true)
	 *	   Dtf <  Dtc implies WAIT	(return false)
	 *
	 * Note: If t is expired then we *cannot* wait on it. We would wait
	 * forever (hang the system).
	 *
	 * Note: do NOT get clever and remove the -thresh from both sides. It
	 * is NOT the same.
	 *
	 * If future valueis zero, we have a client managed sync point. In that
	 * case we do a direct comparison.
	 */
	if (!host1x_syncpt_client_managed(sp))
		return future_val - thresh >= current_val - thresh;
	else
		return (s32)(current_val - thresh) >= 0;
}

int host1x_syncpt_init(struct host1x *host)
{
	struct host1x_syncpt_base *bases;
	struct host1x_syncpt *syncpt;
	unsigned int i;

	syncpt = devm_kcalloc(host->dev, host->info->nb_pts, sizeof(*syncpt),
			      GFP_KERNEL);
	if (!syncpt)
		return -ENOMEM;

	bases = devm_kcalloc(host->dev, host->info->nb_bases, sizeof(*bases),
			     GFP_KERNEL);
	if (!bases)
		return -ENOMEM;

	for (i = 0; i < host->info->nb_pts; i++) {
		syncpt[i].id = i;
		syncpt[i].host = host;

		/*
		 * Unassign syncpt from channels for purposes of Tegra186
		 * syncpoint protection. This prevents any channel from
		 * accessing it until it is reassigned.
		 */
		host1x_hw_syncpt_assign_to_channel(host, &syncpt[i], NULL);
	}

	for (i = 0; i < host->info->nb_bases; i++)
		bases[i].id = i;

	mutex_init(&host->syncpt_mutex);
	host->syncpt = syncpt;
	host->bases = bases;

	host1x_syncpt_restore(host);
	host1x_hw_syncpt_enable_protection(host);

	/* Allocate sync point to use for clearing waits for expired fences */
	host->nop_sp = host1x_syncpt_alloc(host, NULL, 0);
	if (!host->nop_sp)
		return -ENOMEM;

	return 0;
}

/**
 * host1x_syncpt_request() - request a syncpoint
 * @client: client requesting the syncpoint
 * @flags: flags
 *
 * host1x client drivers can use this function to allocate a syncpoint for
 * subsequent use. A syncpoint returned by this function will be reserved for
 * use by the client exclusively. When no longer using a syncpoint, a host1x
 * client driver needs to release it using host1x_syncpt_free().
 */
struct host1x_syncpt *host1x_syncpt_request(struct host1x_client *client,
					    unsigned long flags)
{
	struct host1x *host = dev_get_drvdata(client->parent->parent);

	return host1x_syncpt_alloc(host, client, flags);
}
EXPORT_SYMBOL(host1x_syncpt_request);

/**
 * host1x_syncpt_free() - free a requested syncpoint
 * @sp: host1x syncpoint
 *
 * Release a syncpoint previously allocated using host1x_syncpt_request(). A
 * host1x client driver should call this when the syncpoint is no longer in
 * use. Note that client drivers must ensure that the syncpoint doesn't remain
 * under the control of hardware after calling this function, otherwise two
 * clients may end up trying to access the same syncpoint concurrently.
 */
void host1x_syncpt_free(struct host1x_syncpt *sp)
{
	if (!sp)
		return;

	mutex_lock(&sp->host->syncpt_mutex);

	host1x_syncpt_base_free(sp->base);
	kfree(sp->name);
	sp->base = NULL;
	sp->client = NULL;
	sp->name = NULL;
	sp->client_managed = false;

	mutex_unlock(&sp->host->syncpt_mutex);
}
EXPORT_SYMBOL(host1x_syncpt_free);

void host1x_syncpt_deinit(struct host1x *host)
{
	struct host1x_syncpt *sp = host->syncpt;
	unsigned int i;

	for (i = 0; i < host->info->nb_pts; i++, sp++)
		kfree(sp->name);
}

/**
 * host1x_syncpt_read_max() - read maximum syncpoint value
 * @sp: host1x syncpoint
 *
 * The maximum syncpoint value indicates how many operations there are in
 * queue, either in channel or in a software thread.
 */
u32 host1x_syncpt_read_max(struct host1x_syncpt *sp)
{
	smp_rmb();

	return (u32)atomic_read(&sp->max_val);
}
EXPORT_SYMBOL(host1x_syncpt_read_max);

/**
 * host1x_syncpt_read_min() - read minimum syncpoint value
 * @sp: host1x syncpoint
 *
 * The minimum syncpoint value is a shadow of the current sync point value in
 * hardware.
 */
u32 host1x_syncpt_read_min(struct host1x_syncpt *sp)
{
	smp_rmb();

	return (u32)atomic_read(&sp->min_val);
}
EXPORT_SYMBOL(host1x_syncpt_read_min);

/**
 * host1x_syncpt_read() - read the current syncpoint value
 * @sp: host1x syncpoint
 */
u32 host1x_syncpt_read(struct host1x_syncpt *sp)
{
	return host1x_syncpt_load(sp);
}
EXPORT_SYMBOL(host1x_syncpt_read);

unsigned int host1x_syncpt_nb_pts(struct host1x *host)
{
	return host->info->nb_pts;
}

unsigned int host1x_syncpt_nb_bases(struct host1x *host)
{
	return host->info->nb_bases;
}

unsigned int host1x_syncpt_nb_mlocks(struct host1x *host)
{
	return host->info->nb_mlocks;
}

/**
 * host1x_syncpt_get() - obtain a syncpoint by ID
 * @host: host1x controller
 * @id: syncpoint ID
 */
struct host1x_syncpt *host1x_syncpt_get(struct host1x *host, unsigned int id)
{
	if (id >= host->info->nb_pts)
		return NULL;

	return host->syncpt + id;
}
EXPORT_SYMBOL(host1x_syncpt_get);

/**
 * host1x_syncpt_get_base() - obtain the wait base associated with a syncpoint
 * @sp: host1x syncpoint
 */
struct host1x_syncpt_base *host1x_syncpt_get_base(struct host1x_syncpt *sp)
{
	return sp ? sp->base : NULL;
}
EXPORT_SYMBOL(host1x_syncpt_get_base);

/**
 * host1x_syncpt_base_id() - retrieve the ID of a syncpoint wait base
 * @base: host1x syncpoint wait base
 */
u32 host1x_syncpt_base_id(struct host1x_syncpt_base *base)
{
	return base->id;
}
EXPORT_SYMBOL(host1x_syncpt_base_id);
