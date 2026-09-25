// SPDX-License-Identifier: GPL-2.0-only
/* Flow Queue PIE discipline
 *
 * Copyright (C) 2019 Mohit P. Tahiliani <tahiliani@nitk.edu.in>
 * Copyright (C) 2019 Sachin D. Patil <sdp.sachin@gmail.com>
 * Copyright (C) 2019 V. Saicharan <vsaicharan1998@gmail.com>
 * Copyright (C) 2019 Mohit Bhasi <mohitbhasi1998@gmail.com>
 * Copyright (C) 2019 Leslie Monis <lesliemonis@gmail.com>
 * Copyright (C) 2019 Gautam Ramakrishnan <gautamramk@gmail.com>
 *
 * Backported to 4.14 for NeuroCore-r5x from upstream v5.6
 * (commit "net: sched: add Flow Queue PIE packet scheduler").
 * 4.14 adaptations:
 *  - PIE core (pie_drop_early / pie_process_dequeue /
 *    pie_calculate_probability, incl. bytemode + dq_rate_estimator)
 *    carried statically in this file: this tree keeps PIE structs
 *    private to sch_pie.c (no include/net/pie.h yet).
 *  - classic setup_timer() (no from_timer needed)
 *  - qdisc change()/init() without netlink_ext_ack (4.14 ops)
 *  - nla_parse_nested(..., NULL), 2-arg tcf_block_get()
 *  - kvzalloc() instead of kvcalloc()
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/skbuff.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>
#include <linux/math64.h>
#include <linux/reciprocal_div.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <net/inet_ecn.h>

/* ---- PIE core (from upstream v5.6 sch_pie.c, static for 4.14) ---- */

#define MAX_PROB	U64_MAX
#define DTIME_INVALID	U64_MAX
#define QUEUE_THRESHOLD	16384
#define DQCOUNT_INVALID	-1
#define PIE_SCALE	8

/**
 * struct pie_params - contains pie parameters
 * @target:		target delay in pschedtime
 * @tudpate:		interval at which drop probability is calculated
 * @limit:		total number of packets that can be in the queue
 * @alpha:		parameter to control drop probability
 * @beta:		parameter to control drop probability
 * @ecn:		is ECN marking of packets enabled
 * @bytemode:		is drop probability scaled based on pkt size
 * @dq_rate_estimator:	is Little's law used for qdelay calculation
 */
struct pie_params {
	psched_time_t target;
	u32 tupdate;
	u32 limit;
	u32 alpha;
	u32 beta;
	u8 ecn;
	u8 bytemode;
	u8 dq_rate_estimator;
};

/**
 * struct pie_vars - contains pie variables
 * @qdelay:			current queue delay
 * @qdelay_old:			queue delay in previous qdelay calculation
 * @burst_time:			burst time allowance
 * @dq_tstamp:			timestamp at which dq rate was last calculated
 * @prob:			drop probability
 * @accu_prob:			accumulated drop probability
 * @dq_count:			number of bytes dequeued in a measurement cycle
 * @avg_dq_rate:		calculated average dq rate
 * @qlen_old:			queue length during previous qdelay calculation
 * @accu_prob_overflows:	number of times accu_prob overflows
 */
struct pie_vars {
	psched_time_t qdelay;
	psched_time_t qdelay_old;
	psched_time_t burst_time;
	psched_time_t dq_tstamp;
	u64 prob;
	u64 accu_prob;
	u64 dq_count;
	u32 avg_dq_rate;
	u32 qlen_old;
	u8 accu_prob_overflows;
};

/**
 * struct pie_stats - contains pie stats
 * @packets_in:	total number of packets enqueued
 * @dropped:	packets dropped due to pie action
 * @overlimit:	packets dropped due to lack of space in queue
 * @ecn_mark:	packets marked with ECN
 * @maxq:	maximum queue size
 */
struct pie_stats {
	u32 packets_in;
	u32 dropped;
	u32 overlimit;
	u32 ecn_mark;
	u32 maxq;
};

/**
 * struct pie_skb_cb - contains private skb vars
 * @enqueue_time:	timestamp when the packet is enqueued
 * @mem_usage:		size of the skb during enqueue
 */
struct pie_skb_cb {
	psched_time_t enqueue_time;
	u32 mem_usage;
};

static inline void pie_params_init(struct pie_params *params)
{
	params->target = PSCHED_NS2TICKS(15 * NSEC_PER_MSEC);	/* 15 ms */
	params->tupdate = usecs_to_jiffies(15 * USEC_PER_MSEC);	/* 15 ms */
	params->limit = 1000;
	params->alpha = 2;
	params->beta = 20;
	params->ecn = false;
	params->bytemode = false;
	params->dq_rate_estimator = false;
}

static inline void pie_vars_init(struct pie_vars *vars)
{
	vars->burst_time = PSCHED_NS2TICKS(150 * NSEC_PER_MSEC); /* 150 ms */
	vars->dq_tstamp = DTIME_INVALID;
	vars->accu_prob = 0;
	vars->dq_count = DQCOUNT_INVALID;
	vars->avg_dq_rate = 0;
	vars->accu_prob_overflows = 0;
}

static inline struct pie_skb_cb *get_pie_cb(const struct sk_buff *skb)
{
	qdisc_cb_private_validate(skb, sizeof(struct pie_skb_cb));
	return (struct pie_skb_cb *)qdisc_skb_cb(skb)->data;
}

static inline psched_time_t pie_get_enqueue_time(const struct sk_buff *skb)
{
	return get_pie_cb(skb)->enqueue_time;
}

static inline void pie_set_enqueue_time(struct sk_buff *skb)
{
	get_pie_cb(skb)->enqueue_time = psched_get_time();
}

static bool pie_drop_early(struct Qdisc *sch, struct pie_params *params,
			   struct pie_vars *vars, u32 qlen, u32 packet_size)
{
	u64 rnd;
	u64 local_prob = vars->prob;
	u32 mtu = psched_mtu(qdisc_dev(sch));

	/* If there is still burst allowance left skip random early drop */
	if (vars->burst_time > 0)
		return false;

	/* If current delay is less than half of target, and
	 * if drop prob is low already, disable early_drop
	 */
	if ((vars->qdelay < params->target / 2) &&
	    (vars->prob < MAX_PROB / 5))
		return false;

	/* If we have fewer than 2 mtu-sized packets, disable pie_drop_early,
	 * similar to min_th in RED
	 */
	if (qlen < 2 * mtu)
		return false;

	/* If bytemode is turned on, use packet size to compute new
	 * probablity. Smaller packets will have lower drop prob in this case
	 */
	if (params->bytemode && packet_size <= mtu)
		local_prob = (u64)packet_size * div_u64(local_prob, mtu);
	else
		local_prob = vars->prob;

	if (local_prob == 0) {
		vars->accu_prob = 0;
		vars->accu_prob_overflows = 0;
	}

	if (local_prob > MAX_PROB - vars->accu_prob)
		vars->accu_prob_overflows++;

	vars->accu_prob += local_prob;

	if (vars->accu_prob_overflows == 0 &&
	    vars->accu_prob < (MAX_PROB / 100) * 85)
		return false;
	if (vars->accu_prob_overflows == 8 &&
	    vars->accu_prob >= MAX_PROB / 2)
		return true;

	prandom_bytes(&rnd, 8);
	if (rnd < local_prob) {
		vars->accu_prob = 0;
		vars->accu_prob_overflows = 0;
		return true;
	}

	return false;
}

static void pie_process_dequeue(struct sk_buff *skb, struct pie_params *params,
				struct pie_vars *vars, u32 qlen)
{
	psched_time_t now = psched_get_time();
	u32 dtime = 0;

	/* If dq_rate_estimator is disabled, calculate qdelay using the
	 * packet timestamp.
	 */
	if (!params->dq_rate_estimator) {
		vars->qdelay = now - pie_get_enqueue_time(skb);

		if (vars->dq_tstamp != DTIME_INVALID)
			dtime = now - vars->dq_tstamp;

		vars->dq_tstamp = now;

		if (qlen == 0)
			vars->qdelay = 0;

		if (dtime == 0)
			return;

		goto burst_allowance_reduction;
	}

	/* If current queue is about 10 packets or more and dq_count is unset
	 * we have enough packets to calculate the drain rate. Save
	 * current time as dq_tstamp and start measurement cycle.
	 */
	if (qlen >= QUEUE_THRESHOLD && vars->dq_count == DQCOUNT_INVALID) {
		vars->dq_tstamp = psched_get_time();
		vars->dq_count = 0;
	}

	/* Calculate the average drain rate from this value. If queue length
	 * has receded to a small value viz., <= QUEUE_THRESHOLD bytes, reset
	 * the dq_count to -1 as we don't have enough packets to calculate the
	 * drain rate anymore. The following if block is entered only when we
	 * have a substantial queue built up (QUEUE_THRESHOLD bytes or more)
	 * and we calculate the drain rate for the threshold here.  dq_count is
	 * in bytes, time difference in psched_time, hence rate is in
	 * bytes/psched_time.
	 */
	if (vars->dq_count != DQCOUNT_INVALID) {
		vars->dq_count += skb->len;

		if (vars->dq_count >= QUEUE_THRESHOLD) {
			u32 count = vars->dq_count << PIE_SCALE;

			dtime = now - vars->dq_tstamp;

			if (dtime == 0)
				return;

			count = count / dtime;

			if (vars->avg_dq_rate == 0)
				vars->avg_dq_rate = count;
			else
				vars->avg_dq_rate =
				    (vars->avg_dq_rate -
				     (vars->avg_dq_rate >> 3)) + (count >> 3);

			/* If the queue has receded below the threshold, we hold
			 * on to the last drain rate calculated, else we reset
			 * dq_count to 0 to re-enter the if block when the next
			 * packet is dequeued
			 */
			if (qlen < QUEUE_THRESHOLD) {
				vars->dq_count = DQCOUNT_INVALID;
			} else {
				vars->dq_count = 0;
				vars->dq_tstamp = psched_get_time();
			}

			goto burst_allowance_reduction;
		}
	}

	return;

burst_allowance_reduction:
	if (vars->burst_time > 0) {
		if (vars->burst_time > dtime)
			vars->burst_time -= dtime;
		else
			vars->burst_time = 0;
	}
}

static void pie_calculate_probability(struct pie_params *params,
				      struct pie_vars *vars, u32 qlen)
{
	psched_time_t qdelay = 0;	/* in pschedtime */
	psched_time_t qdelay_old = 0;	/* in pschedtime */
	s64 delta = 0;		/* determines the change in probability */
	u64 oldprob;
	u64 alpha, beta;
	u32 power;
	bool update_prob = true;

	if (params->dq_rate_estimator) {
		qdelay_old = vars->qdelay;
		vars->qdelay_old = vars->qdelay;

		if (vars->avg_dq_rate > 0)
			qdelay = (qlen << PIE_SCALE) / vars->avg_dq_rate;
		else
			qdelay = 0;
	} else {
		qdelay = vars->qdelay;
		qdelay_old = vars->qdelay_old;
	}

	/* If qdelay is zero and qlen is not, it means qlen is very small,
	 * so we do not update probabilty in this round.
	 */
	if (qdelay == 0 && qlen != 0)
		update_prob = false;

	/* In the algorithm, alpha and beta are between 0 and 2 with typical
	 * value for alpha as 0.125. In this implementation, we use values 0-32
	 * passed from user space to represent this. Also, alpha and beta have
	 * unit of HZ and need to be scaled before they can used to update
	 * probability. alpha/beta are updated locally below by scaling down
	 * by 16 to come to 0-2 range.
	 */
	alpha = ((u64)params->alpha * (MAX_PROB / PSCHED_TICKS_PER_SEC)) >> 4;
	beta = ((u64)params->beta * (MAX_PROB / PSCHED_TICKS_PER_SEC)) >> 4;

	/* We scale alpha and beta differently depending on how heavy the
	 * congestion is. Please see RFC 8033 for details.
	 */
	if (vars->prob < MAX_PROB / 10) {
		alpha >>= 1;
		beta >>= 1;

		power = 100;
		while (vars->prob < div_u64(MAX_PROB, power) &&
		       power <= 1000000) {
			alpha >>= 2;
			beta >>= 2;
			power *= 10;
		}
	}

	/* alpha and beta should be between 0 and 32, in multiples of 1/16 */
	delta += alpha * (u64)(qdelay - params->target);
	delta += beta * (u64)(qdelay - qdelay_old);

	oldprob = vars->prob;

	/* to ensure we increase probability in steps of no more than 2% */
	if (delta > (s64)(MAX_PROB / (100 / 2)) &&
	    vars->prob >= MAX_PROB / 10)
		delta = (MAX_PROB / 100) * 2;

	/* Non-linear drop:
	 * Tune drop probability to increase quickly for high delays(>= 250ms)
	 * 250ms is derived through experiments and provides error protection
	 */

	if (qdelay > (PSCHED_NS2TICKS(250 * NSEC_PER_MSEC)))
		delta += MAX_PROB / (100 / 2);

	vars->prob += delta;

	if (delta > 0) {
		/* prevent overflow */
		if (vars->prob < oldprob) {
			vars->prob = MAX_PROB;
			/* Prevent normalization error. If probability is at
			 * maximum value already, we normalize it here, and
			 * skip the check to do a non-linear drop in the next
			 * section.
			 */
			update_prob = false;
		}
	} else {
		/* prevent underflow */
		if (vars->prob > oldprob)
			vars->prob = 0;
	}

	/* Non-linear drop in probability: Reduce drop probability quickly if
	 * delay is 0 for 2 consecutive Tupdate periods.
	 */

	if (qdelay == 0 && qdelay_old == 0 && update_prob)
		/* Reduce drop probability to 98.4% */
		vars->prob -= vars->prob / 64;

	vars->qdelay = qdelay;
	vars->qlen_old = qlen;

	/* We restart the measurement cycle if the following conditions are met
	 * 1. If the delay has been low for 2 consecutive Tupdate periods
	 * 2. Calculated drop probability is zero
	 * 3. If average dq_rate_estimator is enabled, we have atleast one
	 *    estimate for the avg_dq_rate ie., is a non-zero value
	 */
	if ((vars->qdelay < params->target / 2) &&
	    (vars->qdelay_old < params->target / 2) &&
	    vars->prob == 0 &&
	    (!params->dq_rate_estimator || vars->avg_dq_rate > 0)) {
		pie_vars_init(vars);
	}

	if (!params->dq_rate_estimator)
		vars->qdelay_old = qdelay;
}

/* ---- FQ-PIE proper (from upstream v5.6) ---- */

/* Flow Queue PIE
 *
 * Principles:
 *   - Packets are classified on flows.
 *   - This is a Stochastic model (as we use a hash, several flows might
 *                                 be hashed to the same slot)
 *   - Each flow has a PIE managed queue.
 *   - Flows are linked onto two (Round Robin) lists,
 *     so that new flows have priority on old ones.
 *   - For a given flow, packets are not reordered.
 *   - Drops during enqueue only.
 *   - ECN capability is off by default.
 *   - ECN threshold (if ECN is enabled) is at 10% by default.
 *   - Uses timestamps to calculate queue delay by default.
 */

/**
 * struct fq_pie_flow - contains data for each flow
 * @vars:	pie vars associated with the flow
 * @deficit:	number of remaining byte credits
 * @backlog:	size of data in the flow
 * @qlen:	number of packets in the flow
 * @flowchain:	flowchain for the flow
 * @head:	first packet in the flow
 * @tail:	last packet in the flow
 */
struct fq_pie_flow {
	struct pie_vars vars;
	s32 deficit;
	u32 backlog;
	u32 qlen;
	struct list_head flowchain;
	struct sk_buff *head;
	struct sk_buff *tail;
};

struct fq_pie_sched_data {
	struct tcf_proto __rcu *filter_list; /* optional external classifier */
	struct tcf_block *block;
	struct fq_pie_flow *flows;
	struct Qdisc *sch;
	struct list_head old_flows;
	struct list_head new_flows;
	struct pie_params p_params;
	u32 ecn_prob;
	u32 flows_cnt;
	u32 quantum;
	u32 memory_limit;
	u32 new_flow_count;
	u32 memory_usage;
	u32 overmemory;
	struct pie_stats stats;
	struct timer_list adapt_timer;
};

static unsigned int fq_pie_hash(const struct fq_pie_sched_data *q,
				struct sk_buff *skb)
{
	return reciprocal_scale(skb_get_hash(skb), q->flows_cnt);
}

static unsigned int fq_pie_classify(struct sk_buff *skb, struct Qdisc *sch,
				    int *qerr)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	struct tcf_proto *filter;
	struct tcf_result res;
	int result;

	if (TC_H_MAJ(skb->priority) == sch->handle &&
	    TC_H_MIN(skb->priority) > 0 &&
	    TC_H_MIN(skb->priority) <= q->flows_cnt)
		return TC_H_MIN(skb->priority);

	filter = rcu_dereference_bh(q->filter_list);
	if (!filter)
		return fq_pie_hash(q, skb) + 1;

	*qerr = NET_XMIT_SUCCESS | __NET_XMIT_BYPASS;
	result = tcf_classify(skb, filter, &res, false);
	if (result >= 0) {
#ifdef CONFIG_NET_CLS_ACT
		switch (result) {
		case TC_ACT_STOLEN:
		case TC_ACT_QUEUED:
		case TC_ACT_TRAP:
			*qerr = NET_XMIT_SUCCESS | __NET_XMIT_STOLEN;
			/* fall through */
		case TC_ACT_SHOT:
			return 0;
		}
#endif
		if (TC_H_MIN(res.classid) <= q->flows_cnt)
			return TC_H_MIN(res.classid);
	}
	return 0;
}

/* add skb to flow queue (tail add) */
static inline void flow_queue_add(struct fq_pie_flow *flow,
				  struct sk_buff *skb)
{
	if (!flow->head)
		flow->head = skb;
	else
		flow->tail->next = skb;
	flow->tail = skb;
	skb->next = NULL;
}

static int fq_pie_qdisc_enqueue(struct sk_buff *skb, struct Qdisc *sch,
				struct sk_buff **to_free)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	struct fq_pie_flow *sel_flow;
	int uninitialized_var(ret);
	u8 memory_limited = false;
	u8 enqueue = false;
	u32 pkt_len;
	u32 idx;

	/* Classifies packet into corresponding flow */
	idx = fq_pie_classify(skb, sch, &ret);
	sel_flow = &q->flows[idx];

	/* Checks whether adding a new packet would exceed memory limit */
	get_pie_cb(skb)->mem_usage = skb->truesize;
	memory_limited = q->memory_usage > q->memory_limit + skb->truesize;

	/* Checks if the qdisc is full */
	if (unlikely(qdisc_qlen(sch) >= sch->limit)) {
		q->stats.overlimit++;
		goto out;
	} else if (unlikely(memory_limited)) {
		q->overmemory++;
	}

	if (!pie_drop_early(sch, &q->p_params, &sel_flow->vars,
			    sel_flow->backlog, skb->len)) {
		enqueue = true;
	} else if (q->p_params.ecn &&
		   sel_flow->vars.prob <= (MAX_PROB / 100) * q->ecn_prob &&
		   INET_ECN_set_ce(skb)) {
		/* If packet is ecn capable, mark it if drop probability
		 * is lower than the parameter ecn_prob, else drop it.
		 */
		q->stats.ecn_mark++;
		enqueue = true;
	}
	if (enqueue) {
		/* Set enqueue time only when dq_rate_estimator is disabled. */
		if (!q->p_params.dq_rate_estimator)
			pie_set_enqueue_time(skb);

		pkt_len = qdisc_pkt_len(skb);
		q->stats.packets_in++;
		q->memory_usage += skb->truesize;
		sch->qstats.backlog += pkt_len;
		sch->q.qlen++;
		flow_queue_add(sel_flow, skb);
		if (list_empty(&sel_flow->flowchain)) {
			list_add_tail(&sel_flow->flowchain, &q->new_flows);
			q->new_flow_count++;
			sel_flow->deficit = q->quantum;
			sel_flow->qlen = 0;
			sel_flow->backlog = 0;
		}
		sel_flow->qlen++;
		sel_flow->backlog += pkt_len;
		return NET_XMIT_SUCCESS;
	}
out:
	q->stats.dropped++;
	sel_flow->vars.accu_prob = 0;
	sel_flow->vars.accu_prob_overflows = 0;
	__qdisc_drop(skb, to_free);
	qdisc_qstats_drop(sch);
	return NET_XMIT_CN;
}

static const struct nla_policy fq_pie_policy[TCA_FQ_PIE_MAX + 1] = {
	[TCA_FQ_PIE_LIMIT]		= {.type = NLA_U32},
	[TCA_FQ_PIE_FLOWS]		= {.type = NLA_U32},
	[TCA_FQ_PIE_TARGET]		= {.type = NLA_U32},
	[TCA_FQ_PIE_TUPDATE]		= {.type = NLA_U32},
	[TCA_FQ_PIE_ALPHA]		= {.type = NLA_U32},
	[TCA_FQ_PIE_BETA]		= {.type = NLA_U32},
	[TCA_FQ_PIE_QUANTUM]		= {.type = NLA_U32},
	[TCA_FQ_PIE_MEMORY_LIMIT]	= {.type = NLA_U32},
	[TCA_FQ_PIE_ECN_PROB]		= {.type = NLA_U32},
	[TCA_FQ_PIE_ECN]		= {.type = NLA_U32},
	[TCA_FQ_PIE_BYTEMODE]		= {.type = NLA_U32},
	[TCA_FQ_PIE_DQ_RATE_ESTIMATOR]	= {.type = NLA_U32},
};

static inline struct sk_buff *dequeue_head(struct fq_pie_flow *flow)
{
	struct sk_buff *skb = flow->head;

	flow->head = skb->next;
	skb->next = NULL;
	return skb;
}

static struct sk_buff *fq_pie_qdisc_dequeue(struct Qdisc *sch)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	struct sk_buff *skb = NULL;
	struct fq_pie_flow *flow;
	struct list_head *head;
	u32 pkt_len;

begin:
	head = &q->new_flows;
	if (list_empty(head)) {
		head = &q->old_flows;
		if (list_empty(head))
			return NULL;
	}

	flow = list_first_entry(head, struct fq_pie_flow, flowchain);
	/* Flow has exhausted all its credits */
	if (flow->deficit <= 0) {
		flow->deficit += q->quantum;
		list_move_tail(&flow->flowchain, &q->old_flows);
		goto begin;
	}

	if (flow->head) {
		skb = dequeue_head(flow);
		pkt_len = qdisc_pkt_len(skb);
		sch->qstats.backlog -= pkt_len;
		sch->q.qlen--;
		qdisc_bstats_update(sch, skb);
	}

	if (!skb) {
		/* force a pass through old_flows to prevent starvation */
		if (head == &q->new_flows && !list_empty(&q->old_flows))
			list_move_tail(&flow->flowchain, &q->old_flows);
		else
			list_del_init(&flow->flowchain);
		goto begin;
	}

	flow->qlen--;
	flow->deficit -= pkt_len;
	flow->backlog -= pkt_len;
	q->memory_usage -= get_pie_cb(skb)->mem_usage;
	pie_process_dequeue(skb, &q->p_params, &flow->vars, flow->backlog);
	return skb;
}

static int fq_pie_change(struct Qdisc *sch, struct nlattr *opt)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_FQ_PIE_MAX + 1];
	unsigned int len_dropped = 0;
	unsigned int num_dropped = 0;
	int err;

	if (!opt)
		return -EINVAL;

	err = nla_parse_nested(tb, TCA_FQ_PIE_MAX, opt, fq_pie_policy, NULL);
	if (err < 0)
		return err;

	sch_tree_lock(sch);
	if (tb[TCA_FQ_PIE_LIMIT]) {
		u32 limit = nla_get_u32(tb[TCA_FQ_PIE_LIMIT]);

		q->p_params.limit = limit;
		sch->limit = limit;
	}
	if (tb[TCA_FQ_PIE_FLOWS]) {
		if (q->flows)
			goto flow_error;
		q->flows_cnt = nla_get_u32(tb[TCA_FQ_PIE_FLOWS]);
		if (!q->flows_cnt || q->flows_cnt > 65536)
			goto flow_error;
	}

	/* convert from microseconds to pschedtime */
	if (tb[TCA_FQ_PIE_TARGET]) {
		/* target is in us */
		u32 target = nla_get_u32(tb[TCA_FQ_PIE_TARGET]);

		/* convert to pschedtime */
		q->p_params.target =
			PSCHED_NS2TICKS((u64)target * NSEC_PER_USEC);
	}

	/* tupdate is in jiffies */
	if (tb[TCA_FQ_PIE_TUPDATE])
		q->p_params.tupdate =
			usecs_to_jiffies(nla_get_u32(tb[TCA_FQ_PIE_TUPDATE]));

	if (tb[TCA_FQ_PIE_ALPHA])
		q->p_params.alpha = nla_get_u32(tb[TCA_FQ_PIE_ALPHA]);

	if (tb[TCA_FQ_PIE_BETA])
		q->p_params.beta = nla_get_u32(tb[TCA_FQ_PIE_BETA]);

	if (tb[TCA_FQ_PIE_QUANTUM])
		q->quantum = nla_get_u32(tb[TCA_FQ_PIE_QUANTUM]);

	if (tb[TCA_FQ_PIE_MEMORY_LIMIT])
		q->memory_limit = nla_get_u32(tb[TCA_FQ_PIE_MEMORY_LIMIT]);

	if (tb[TCA_FQ_PIE_ECN_PROB])
		q->ecn_prob = nla_get_u32(tb[TCA_FQ_PIE_ECN_PROB]);

	if (tb[TCA_FQ_PIE_ECN])
		q->p_params.ecn = nla_get_u32(tb[TCA_FQ_PIE_ECN]);

	if (tb[TCA_FQ_PIE_BYTEMODE])
		q->p_params.bytemode = nla_get_u32(tb[TCA_FQ_PIE_BYTEMODE]);

	if (tb[TCA_FQ_PIE_DQ_RATE_ESTIMATOR])
		q->p_params.dq_rate_estimator =
			nla_get_u32(tb[TCA_FQ_PIE_DQ_RATE_ESTIMATOR]);

	/* Drop excess packets if new limit is lower */
	while (sch->q.qlen > sch->limit) {
		struct sk_buff *skb = fq_pie_qdisc_dequeue(sch);

		len_dropped += qdisc_pkt_len(skb);
		num_dropped += 1;
		rtnl_kfree_skbs(skb, skb);
	}
	qdisc_tree_reduce_backlog(sch, num_dropped, len_dropped);

	sch_tree_unlock(sch);
	return 0;

flow_error:
	sch_tree_unlock(sch);
	return -EINVAL;
}

static void fq_pie_timer(unsigned long arg)
{
	struct Qdisc *sch = (struct Qdisc *)arg;
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	spinlock_t *root_lock; /* to lock qdisc for probability calculations */
	u16 idx;

	root_lock = qdisc_lock(qdisc_root_sleeping(sch));
	spin_lock(root_lock);

	for (idx = 0; idx < q->flows_cnt; idx++)
		pie_calculate_probability(&q->p_params, &q->flows[idx].vars,
					  q->flows[idx].backlog);

	/* reset the timer to fire after 'tupdate' jiffies. */
	if (q->p_params.tupdate)
		mod_timer(&q->adapt_timer, jiffies + q->p_params.tupdate);

	spin_unlock(root_lock);
}

static int fq_pie_init(struct Qdisc *sch, struct nlattr *opt)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	int err;
	u16 idx;

	pie_params_init(&q->p_params);
	sch->limit = 10 * 1024;
	q->p_params.limit = sch->limit;
	q->quantum = psched_mtu(qdisc_dev(sch));
	q->sch = sch;
	q->ecn_prob = 10;
	q->flows_cnt = 1024;
	q->memory_limit = SZ_32M;

	INIT_LIST_HEAD(&q->new_flows);
	INIT_LIST_HEAD(&q->old_flows);

	if (opt) {
		err = fq_pie_change(sch, opt);

		if (err)
			return err;
	}

	err = tcf_block_get(&q->block, &q->filter_list);
	if (err)
		goto init_failure;

	q->flows = kvzalloc(q->flows_cnt * sizeof(struct fq_pie_flow),
			    GFP_KERNEL);
	if (!q->flows) {
		err = -ENOMEM;
		goto init_failure;
	}
	for (idx = 0; idx < q->flows_cnt; idx++) {
		struct fq_pie_flow *flow = q->flows + idx;

		INIT_LIST_HEAD(&flow->flowchain);
		pie_vars_init(&flow->vars);
	}

	setup_timer(&q->adapt_timer, fq_pie_timer, (unsigned long)sch);
	mod_timer(&q->adapt_timer, jiffies + HZ / 2);

	return 0;

init_failure:
	q->flows_cnt = 0;

	return err;
}

static int fq_pie_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start(skb, TCA_OPTIONS);
	if (!opts)
		return -EMSGSIZE;

	/* convert target from pschedtime to us */
	if (nla_put_u32(skb, TCA_FQ_PIE_LIMIT, sch->limit) ||
	    nla_put_u32(skb, TCA_FQ_PIE_FLOWS, q->flows_cnt) ||
	    nla_put_u32(skb, TCA_FQ_PIE_TARGET,
			((u32)PSCHED_TICKS2NS(q->p_params.target)) /
			NSEC_PER_USEC) ||
	    nla_put_u32(skb, TCA_FQ_PIE_TUPDATE,
			jiffies_to_usecs(q->p_params.tupdate)) ||
	    nla_put_u32(skb, TCA_FQ_PIE_ALPHA, q->p_params.alpha) ||
	    nla_put_u32(skb, TCA_FQ_PIE_BETA, q->p_params.beta) ||
	    nla_put_u32(skb, TCA_FQ_PIE_QUANTUM, q->quantum) ||
	    nla_put_u32(skb, TCA_FQ_PIE_MEMORY_LIMIT, q->memory_limit) ||
	    nla_put_u32(skb, TCA_FQ_PIE_ECN_PROB, q->ecn_prob) ||
	    nla_put_u32(skb, TCA_FQ_PIE_ECN, q->p_params.ecn) ||
	    nla_put_u32(skb, TCA_FQ_PIE_BYTEMODE, q->p_params.bytemode) ||
	    nla_put_u32(skb, TCA_FQ_PIE_DQ_RATE_ESTIMATOR,
			q->p_params.dq_rate_estimator))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	nla_nest_cancel(skb, opts);
	return -EMSGSIZE;
}

static int fq_pie_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	struct tc_fq_pie_xstats st = {
		.packets_in	= q->stats.packets_in,
		.overlimit	= q->stats.overlimit,
		.overmemory	= q->overmemory,
		.dropped	= q->stats.dropped,
		.ecn_mark	= q->stats.ecn_mark,
		.new_flow_count = q->new_flow_count,
		.memory_usage   = q->memory_usage,
	};
	struct list_head *pos;

	sch_tree_lock(sch);
	list_for_each(pos, &q->new_flows)
		st.new_flows_len++;

	list_for_each(pos, &q->old_flows)
		st.old_flows_len++;
	sch_tree_unlock(sch);

	return gnet_stats_copy_app(d, &st, sizeof(st));
}

static void fq_pie_reset(struct Qdisc *sch)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);
	u16 idx;

	INIT_LIST_HEAD(&q->new_flows);
	INIT_LIST_HEAD(&q->old_flows);
	for (idx = 0; idx < q->flows_cnt; idx++) {
		struct fq_pie_flow *flow = q->flows + idx;

		/* Removes all packets from flow */
		rtnl_kfree_skbs(flow->head, flow->tail);
		flow->head = NULL;

		INIT_LIST_HEAD(&flow->flowchain);
		pie_vars_init(&flow->vars);
	}

	sch->q.qlen = 0;
	sch->qstats.backlog = 0;
}

static void fq_pie_destroy(struct Qdisc *sch)
{
	struct fq_pie_sched_data *q = qdisc_priv(sch);

	tcf_block_put(q->block);
	del_timer_sync(&q->adapt_timer);
	kvfree(q->flows);
}

static struct Qdisc_ops fq_pie_qdisc_ops __read_mostly = {
	.id		= "fq_pie",
	.priv_size	= sizeof(struct fq_pie_sched_data),
	.enqueue	= fq_pie_qdisc_enqueue,
	.dequeue	= fq_pie_qdisc_dequeue,
	.peek		= qdisc_peek_dequeued,
	.init		= fq_pie_init,
	.destroy	= fq_pie_destroy,
	.reset		= fq_pie_reset,
	.change		= fq_pie_change,
	.dump		= fq_pie_dump,
	.dump_stats	= fq_pie_dump_stats,
	.owner		= THIS_MODULE,
};

static int __init fq_pie_module_init(void)
{
	return register_qdisc(&fq_pie_qdisc_ops);
}

static void __exit fq_pie_module_exit(void)
{
	unregister_qdisc(&fq_pie_qdisc_ops);
}

module_init(fq_pie_module_init);
module_exit(fq_pie_module_exit);

MODULE_DESCRIPTION("Flow Queue Proportional Integral controller Enhanced (FQ-PIE)");
MODULE_AUTHOR("Mohit P. Tahiliani");
MODULE_LICENSE("GPL");
