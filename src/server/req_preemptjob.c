/*
 * Copyright (C) 1994-2021 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

/*
 * @file	svr_preemptjob.c
 *
 * Functions relating to the Hold and Release Job Batch Requests.
 *
 * Included funtions are:
 *	req_holdjob()
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include "libpbs.h"
#include "server_limits.h"
#include "list_link.h"
#include "server.h"
#include "batch_request.h"
#include "net_connect.h"
#include "job.h"
#include "pbs_error.h"
#include "log.h"
#include "acct.h"
#include "pbs_nodes.h"
#include "svrfunc.h"

extern void post_signal_req(struct work_task *);
struct preempt_ordering *svr_get_preempt_order(job *pjob, pbs_sched *psched);

/* Global Data Items: */

extern struct server server;

extern time_t time_now;

const static char *preempt_methods[] = {
	"",
	"suspend",
	"checkpoint",
	"requeue",
	"delete",
	""
    };

/**
 * @brief mark a job preemption as failed
 * @param[in] preempt_preq - the preemption preq from the scheduler
 * @param[in] job_id - the job to mark as failed
 * @return void
 */
static void job_preempt_fail(struct batch_request *preempt_preq, char *job_id)
{
	int preempt_index = preempt_preq->rq_reply.brp_un.brp_preempt_jobs.count;
	preempt_job_info *preempt_jobs_list = preempt_preq->rq_reply.brp_un.brp_preempt_jobs.ppj_list;

	preempt_preq->rq_reply.brp_code = 1;
	strcpy(preempt_jobs_list[preempt_index].order, "000");
	sprintf(preempt_jobs_list[preempt_index].job_id, "%s", job_id);
	preempt_preq->rq_reply.brp_un.brp_preempt_jobs.count++;
	log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_DEBUG, job_id, "Job failed to be preempted");
}

/**
 * @brief create a local batch_request for a suspend request
 * @param[in] job_id - the job to create the request for
 * @return batch_request *
 * @retval new batch_request for suspend
 */
static struct batch_request *create_suspend_request(char *job_id)
{
	struct batch_request *newreq;

	newreq = alloc_br(PBS_BATCH_SignalJob);
	if (newreq == NULL)
		return NULL;
	snprintf(newreq->rq_ind.rq_signal.rq_jid, sizeof(newreq->rq_ind.rq_signal.rq_jid), "%s", job_id);
	snprintf(newreq->rq_ind.rq_signal.rq_signame, sizeof(newreq->rq_ind.rq_signal.rq_signame), "%s", SIG_SUSPEND);
	return newreq;
}

/**
 * @brief create a local batch_request for a checkpoint request
 * @param[in] job_id - the job to create the request for
 * @return batch_request *
 * @retval new batch_request for holdjob (checkpoint)
 */

static struct batch_request *create_ckpt_request(char *job_id)
{
	int hold_name_size;
	int hold_val_size = 2; /* 2 for 's' and '\0' */
	struct batch_request *newreq;
	svrattrl *hold_svrattrl;

	hold_name_size = strlen(job_attr_def[(int)JOB_ATR_hold].at_name) + 1;
	newreq = alloc_br(PBS_BATCH_HoldJob);
	hold_svrattrl = attrlist_alloc(hold_name_size, 0, hold_val_size);
	if (newreq == NULL || hold_svrattrl == NULL) {
		if (newreq != NULL)
			free_br(newreq);
		if (hold_svrattrl != NULL)
			free(hold_svrattrl);
		return NULL;
	}
	snprintf(newreq->rq_ind.rq_hold.rq_orig.rq_objname, sizeof(newreq->rq_ind.rq_hold.rq_orig.rq_objname), "%s", job_id);
	snprintf(hold_svrattrl->al_name, hold_name_size, "%s", job_attr_def[(int)JOB_ATR_hold].at_name);
	snprintf(hold_svrattrl->al_value, hold_val_size, "s");
	CLEAR_HEAD(newreq->rq_ind.rq_hold.rq_orig.rq_attr);
	append_link(&newreq->rq_ind.rq_hold.rq_orig.rq_attr, &hold_svrattrl->al_link, hold_svrattrl);

	return newreq;
}

/**
 * @brief create a local batch_request for a requeue request
 * @param[in] job_id - the job to create the request for
 * @return batch_request *
 * @retval new batch_request for rerun (requeue)
 */

static struct batch_request *create_requeue_request(char *job_id)
{
	struct batch_request *newreq;

	newreq = alloc_br(PBS_BATCH_Rerun);
	if (newreq == NULL)
		return NULL;

	snprintf(newreq->rq_ind.rq_signal.rq_jid, sizeof(newreq->rq_ind.rq_signal.rq_jid), "%s", job_id);
	return newreq;
}

/**
 * @brief create a local batch_request for a delete request
 * @param[in] job_id - the job to create the request for
 * @return batch_request *
 * @retval new batch_request for delete
 */

static struct batch_request *create_delete_request(char *job_id)
{
	struct batch_request *newreq;
	newreq = alloc_br(PBS_BATCH_DeleteJob);
	if (newreq == NULL)
		return NULL;
	snprintf(newreq->rq_ind.rq_delete.rq_objname, sizeof(newreq->rq_ind.rq_delete.rq_objname), "%s", job_id);
	return newreq;
}

/**
 * @brief create and issue local preemption request for a job
 * @param[in] preempt_method - preemption method
 * @param[in] pjob - the job to be preempted
 * @param[in] preq - the preempt request from the scheduler
 * @return success/failure
 */
static int issue_preempt_request(int preempt_method, job *pjob, struct batch_request *preq)
{
	struct batch_request *newreq;
	struct work_task *pwt;

	switch (preempt_method) {
		case PREEMPT_METHOD_SUSPEND:
			newreq = create_suspend_request(pjob->ji_qs.ji_jobid);
			break;
		case PREEMPT_METHOD_CHECKPOINT:
			newreq = create_ckpt_request(pjob->ji_qs.ji_jobid);
			break;
		case PREEMPT_METHOD_REQUEUE:
			newreq = create_requeue_request(pjob->ji_qs.ji_jobid);
			break;
		case PREEMPT_METHOD_DELETE:
			newreq = create_delete_request(pjob->ji_qs.ji_jobid);
			break;
		default:
			return 1;
	}

	if (newreq != NULL) {
		newreq->rq_extend = NULL;
		snprintf(newreq->rq_user, sizeof(newreq->rq_user), "%s", preq->rq_user);
		snprintf(newreq->rq_host, sizeof(newreq->rq_host), "%s", preq->rq_host);
		newreq->rq_perm = preq->rq_perm;
		if (issue_Drequest(PBS_LOCAL_CONNECTION, newreq, release_req, &pwt, 0) == -1) {
			free_br(newreq);
			return 1;
		}
		append_link(&pjob->ji_svrtask, &pwt->wt_linkobj, pwt);
	}
	else
		return 1;

	return 0;
}

/**
 * @brief clear the system hold on a job after a checkpoint request
 * @param[in] pjob - the job to clear
 */
static void clear_preempt_hold(job *pjob)
{
	long old_hold;
	int newsub;
	char newstate;

	old_hold = get_jattr_long(pjob, JOB_ATR_hold);
	set_jattr_generic(pjob, JOB_ATR_hold, "s", NULL, DECR);

	if (old_hold != get_jattr_long(pjob, JOB_ATR_hold)) {
		svr_evaljobstate(pjob, &newstate, &newsub, 0);
		svr_setjobstate(pjob, newstate, newsub); /* saves job */
	}
	if (get_jattr_long(pjob, JOB_ATR_hold) == 0)
		free_jattr(pjob, JOB_ATR_Comment);
}

/**
 * @brief
 * req_preemptjobs- service the Preempt Jobs Request
 *
 * This request tries to preempt multiple jobs.
 * The state of the job may change as a result.
 *
 * @param[in,out]	preq	- The Request
 */

void
req_preemptjobs(struct batch_request *preq)
{
	int i = 0;
	int count = 0;
	job *pjob = NULL;
	preempt_job_info *ppj = NULL;
	pbs_sched *psched;
	int preempt_index = 0;
	int preempt_total;
	preempt_job_info *preempt_jobs_list;

	preq->rq_reply.brp_code = 0;
	count = preq->rq_ind.rq_preempt.count;
	psched = find_sched_from_sock(preq->rq_conn, CONN_SCHED_PRIMARY);
	preempt_total = preq->rq_ind.rq_preempt.count;

	if (psched == NULL) {
		req_reject(PBSE_INTERNAL, 0, preq);
		return;
	}

	if ((preempt_jobs_list = calloc(sizeof(preempt_job_info), preempt_total)) == NULL) {
		req_reject(PBSE_SYSTEM, 0, preq);
		log_err(errno, __func__, "Unable to allocate memory");
		return;
	}

	preq->rq_reply.brp_un.brp_preempt_jobs.ppj_list = preempt_jobs_list;
	preq->rq_reply.brp_choice = BATCH_REPLY_CHOICE_PreemptJobs;
	preq->rq_reply.brp_un.brp_preempt_jobs.count = 0;

	for (i = 0; i < count; i++) {
		ppj = &(preq->rq_ind.rq_preempt.ppj_list[i]);
		pjob = find_job(ppj->job_id);
		/* The job is already out of the way. This must have happened after the scheduler
		 * queried the universe and before it tried to preempt the jobs.
		 * Regardless of the preempt_order, use the correct reply code to what
		 * actually happened so the scheduler correctly handles the job.
		 */
		if (pjob == NULL) {
			sprintf(preempt_jobs_list[preempt_index].job_id, "%s", ppj->job_id);
			strcpy(preempt_jobs_list[preempt_index].order, "D");
			preempt_index++;
			continue;
		}

		if (!check_job_state(pjob, JOB_STATE_LTR_RUNNING)) {
			sprintf(preempt_jobs_list[preempt_index].job_id, "%s", ppj->job_id);
			switch (get_job_state(pjob)) {
				case JOB_STATE_LTR_QUEUED:
					strcpy(preempt_jobs_list[preempt_index].order, "Q");
					preempt_index++;
					break;
				case JOB_STATE_LTR_EXPIRED:
				case JOB_STATE_LTR_FINISHED:
				case JOB_STATE_LTR_MOVED:
					strcpy(preempt_jobs_list[preempt_index].order, "D");
					preempt_index++;
					break;
				default:
					job_preempt_fail(preq, ppj->job_id);
					preempt_index++;
			}
			continue;
		}

		pjob->ji_pmt_preq = preq;

		pjob->preempt_order = svr_get_preempt_order(pjob, psched);
		pjob->preempt_order_index = 0;
		if (issue_preempt_request((int)pjob->preempt_order[0].order[0], pjob, preq))
			reply_preempt_jobs_request(PBSE_SYSTEM, (int)pjob->preempt_order[0].order[0], pjob);
	}
	preq->rq_reply.brp_un.brp_preempt_jobs.count = preempt_index;
	/* check if all jobs failed */
	if (preempt_index == preempt_total)
		reply_send(preq);
}

/**
 * @brief
 * reply_preempt_jobs_request- synthesize and reply to Preempt Jobs Request
 *
 * If an attempt to preempt the job fails, we use the next method to preempt
 * that job as per the preemption order,
 *
 * If the job gets preempted successfully, job-id is added to the reply.
 *
 * @param[in] code - determines if the job was preempted or not.
 * @param[in] aux  - determines the method by which job was preempted.
 * @param[in] pjob - the job in which we are replying to the preemption request
 */

void
reply_preempt_jobs_request(int code, int aux, struct job *pjob)
{
	struct batch_request *preq;
	int clear_preempt_vars = 0;

	if (pjob == NULL)
		return;

	preq = pjob->ji_pmt_preq;

	if (code != PBSE_NONE) {
		snprintf(log_buffer, sizeof(log_buffer), "preemption method %s failed for job (%d)", preempt_methods[aux], code);
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_DEBUG, pjob->ji_qs.ji_jobid, log_buffer);

		if (pjob->preempt_order[0].order[pjob->preempt_order_index] == PREEMPT_METHOD_CHECKPOINT)
			clear_preempt_hold(pjob);

		pjob->preempt_order_index++;
		if (pjob->preempt_order[0].order[pjob->preempt_order_index] != PREEMPT_METHOD_LOW) {
			if (issue_preempt_request((int)pjob->preempt_order[0].order[pjob->preempt_order_index], pjob, preq)) {
				job_preempt_fail(preq, pjob->ji_qs.ji_jobid);
				clear_preempt_vars = 1;
			} else {
				/* reply_preempt_jobs_request() is somewhat recursive.  If a preemption method fails, one call will issue the next
				 * preemptiom method request.  The next preemption method request immediately is rejected, it will call
				 * reply_preempt_jobs_request() again before the first call ends.  A reject like this is considered a successful
				 * call to issue_Drequest().  If pjob->ji_pmt_preq has been NULLed, it means the last preemption method has failed.
				 * If this is the last job in the preemption list, the call to reply_preempt_job_request() will have replied to
				 * the scheduler.  In this case, we do not want to reply a second time.
				 */
				if (pjob->ji_pmt_preq == NULL)
					return;
			}
		} else {
			job_preempt_fail(preq, pjob->ji_qs.ji_jobid);
			clear_preempt_vars = 1;
		}
	} else {
		int preempt_index;
		preempt_job_info *preempt_jobs_list;

		preempt_index = preq->rq_reply.brp_un.brp_preempt_jobs.count;
		preempt_jobs_list = preq->rq_reply.brp_un.brp_preempt_jobs.ppj_list;

		/* successful preemption */
		set_jattr_l_slim(pjob, JOB_ATR_sched_preempted, time(0), SET);
		switch (aux) {
			case PREEMPT_METHOD_SUSPEND:
				strcpy(preempt_jobs_list[preempt_index].order, "S");
				break;
			case PREEMPT_METHOD_CHECKPOINT:
				strcpy(preempt_jobs_list[preempt_index].order, "C");
				clear_preempt_hold(pjob);
				break;
			case PREEMPT_METHOD_REQUEUE:
				strcpy(preempt_jobs_list[preempt_index].order, "Q");
				break;
			case PREEMPT_METHOD_DELETE:
				strcpy(preempt_jobs_list[preempt_index].order, "D");
				break;
		}
		sprintf(preempt_jobs_list[preempt_index].job_id, "%s", pjob->ji_qs.ji_jobid);
		clear_preempt_vars = 1;

		preq->rq_reply.brp_un.brp_preempt_jobs.count++;
	}
	if (clear_preempt_vars) {
		pjob->preempt_order_index = 0;
		pjob->preempt_order = NULL;
		pjob->ji_pmt_preq = NULL;
	}
	/* send reply if we're done */
	if (preq->rq_reply.brp_un.brp_preempt_jobs.count == preq->rq_ind.rq_preempt.count) {
		reply_send(preq);
	}
}

/**
 * @brief
 *  get_job_req_used_time - get a running job's req and used time for preemption
 *
 * @param[in]	pjob - the job in question
 * @param[out]	rtime - return pointer to the requested time
 * @param[out]	utime - return pointer to the used time
 *
 * @return	int
 * @retval	0 for success
 * @retval	1 for error
 */
static int
get_job_req_used_time(job *pjob, int *rtime, int *utime)
{
	double req = 0;
	double used = 0;

	if (pjob == NULL || rtime == NULL || utime == NULL)
		return 1;

	req = get_softwall(pjob);
	if (req == -1)
		req = get_wall(pjob);

	if (req == -1) {
		req = get_cput(pjob);
		used = get_used_cput(pjob);
	} else
		used = get_used_wall(pjob);

	*rtime = req;
	*utime = used;

	return 0;
}

/**
 * @brief
 *  	svr_get_preempt_order - deduce the preemption ordering to be used for a job
 *
 * @param[in]	pjob	-	the job to preempt
 * @param[in]	psched	-	Pointer to the sched object.
 *
 * @return	: struct preempt_ordering.  array containing preemption order
 *
 */
struct preempt_ordering *svr_get_preempt_order(job *pjob, pbs_sched *psched)
{
	struct preempt_ordering *po = NULL;
	int req = -1;
	int used = -1;

	if (get_job_req_used_time(pjob, &req, &used) != 0)
		return NULL;

	po = get_preemption_order(psched->preempt_order, req, used);

	return po;
}
