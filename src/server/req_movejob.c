/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file    req_movejob.c
 *
 * @brief
 * 		req_movejob.c - function to move a job to another queue
 *
 * Included functions are:
 * 	req_movejob()
 * 	req_orderjob()
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <sys/types.h>
#include <sys/param.h>
#include "libpbs.h"
#include <errno.h>

#include "server_limits.h"
#include "linked_list.h"
#include "attribute.h"
#include "credential.h"
#include "batch_request.h"
#include "resv_node.h"
#include "queue.h"
#include "job.h"
#include "reservation.h"
#include "log.h"
#include "pbs_error.h"
#include "hook.h"
#include "pbs_nodes.h"
#include "svrfunc.h"


/* Global Data Items: */

extern char	*msg_unkjobid;
extern char	*msg_badstate;
extern char	*msg_manager;
extern char	*msg_movejob;
extern int	 pbs_errno;

/**
 * @brief
 * 		req_movejob = move a job to a new destination (local or remote)
 *
 * @param[in,out]	req	-	the batch request
 */

void
req_movejob(struct batch_request *req)
{
	int      jt;            /* job type */
	job	*jobp;
	char	hook_msg[HOOK_MSG_SIZE];

	switch (process_hooks(req, hook_msg, sizeof(hook_msg),
			pbs_python_set_interrupt)) {
		case 0:	/* explicit reject */
			reply_text(req, PBSE_HOOKERROR, hook_msg);
			return;
		case 1:   /* explicit accept */
			if (recreate_request(req) == -1) { /* error */
				/* we have to reject the request, as 'req' */
				/* may have been partly modified           */
				strcpy(hook_msg,
					"movejob event: rejected request");
				log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_HOOK,
					LOG_ERR, "", hook_msg);
				reply_text(req, PBSE_HOOKERROR, hook_msg);
				return;
			}
			break;
		case 2:	/* no hook script executed - go ahead and accept event*/
			break;
		default:
			log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_HOOK,
				LOG_INFO, "", "movejob event: accept req by default");
	}

	jobp = chk_job_request(req->rq_ind.rq_move.rq_jid, req, &jt);

	if (jobp == NULL)
		return;

	if ((jt != IS_ARRAY_NO) && (jt != IS_ARRAY_ArrayJob)) {
		req_reject(PBSE_IVALREQ, 0, req);
		return;
	}

	if (jobp->ji_qs.ji_state != JOB_STATE_QUEUED &&
		jobp->ji_qs.ji_state != JOB_STATE_HELD &&
		jobp->ji_qs.ji_state != JOB_STATE_WAITING) {
#ifndef NDEBUG
		(void)sprintf(log_buffer, "(%s) %s, state=%d",
			__func__, msg_badstate, jobp->ji_qs.ji_state);
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_DEBUG,
			jobp->ji_qs.ji_jobid, log_buffer);
#endif /* NDEBUG */
		req_reject(PBSE_BADSTATE, 0, req);
		return;
	}

	if (jt != IS_ARRAY_NO) {
		/* cannot move Subjob and can only move array job if */
		/* no subjobs are running			     */
		if ((jt != IS_ARRAY_ArrayJob) ||
			(jobp->ji_ajtrk->tkm_subjsct[JOB_STATE_RUNNING] != 0)) {
			req_reject(PBSE_IVALREQ, 0, req);
			return;
		}
	}

	/*
	 * svr_movejob() does the real work, handles both local and
	 * network moves
	 */

	switch (svr_movejob(jobp, req->rq_ind.rq_move.rq_destin, req)) {
		case 0:			/* success */
			(void)strcpy(log_buffer, msg_movejob);
			(void)sprintf(log_buffer+strlen(log_buffer),
				msg_manager, req->rq_ind.rq_move.rq_destin,
				req->rq_user, req->rq_host);
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
				jobp->ji_qs.ji_jobid, log_buffer);
			reply_ack(req);
			break;
		case -1:
		case 1:			/* fail */
			if (jobp->ji_clterrmsg)
				reply_text(req, pbs_errno, jobp->ji_clterrmsg);
			else
				req_reject(pbs_errno, 0, req);
			break;
		case 2:			/* deferred, will be handled by 	   */
			/* post_movejob() when the child completes */
			break;
	}
	return;
}
/**
 * @brief
 * 		req_orderjob - reorder the jobs in a queue
 *
 * @param[in,out]	req	-	the batch request
 */

void
req_orderjob(struct batch_request *req)
{
	int      jt1, jt2;            /* job type */
	job	*pjob;
	job	*pjob1;
	job	*pjob2;
	long	 rank;
	int	 rc;
	char	 tmpqn[PBS_MAXQUEUENAME+1];

	if ((pjob1=chk_job_request(req->rq_ind.rq_move.rq_jid, req, &jt1)) == NULL)
		return;
	if ((pjob2=chk_job_request(req->rq_ind.rq_move.rq_destin, req, &jt2)) == NULL)
		return;
	if ((jt1 == IS_ARRAY_Single) || (jt2 == IS_ARRAY_Single) ||
		(jt1 == IS_ARRAY_Range)  || (jt2 == IS_ARRAY_Range)) {
		/* can only move regular or Array Job, not Subjobs */
		req_reject(PBSE_IVALREQ, 0, req);
		return;
	}

	if (((pjob = pjob1)->ji_qs.ji_state == JOB_STATE_RUNNING) ||
		((pjob = pjob2)->ji_qs.ji_state == JOB_STATE_RUNNING) ||
		((pjob = pjob1)->ji_qs.ji_state == JOB_STATE_BEGUN)   ||
		((pjob = pjob2)->ji_qs.ji_state == JOB_STATE_BEGUN)) {
#ifndef NDEBUG
		(void)sprintf(log_buffer, "(%s) %s, state=%d",
			__func__, msg_badstate, pjob->ji_qs.ji_state);
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_DEBUG,
			pjob->ji_qs.ji_jobid, log_buffer);
#endif	/* NDEBUG */
		req_reject(PBSE_BADSTATE, 0, req);
		return;
	} else if (pjob1->ji_qhdr != pjob2->ji_qhdr) {

		/* Jobs are in different queues */

		if ((rc = svr_chkque(pjob1, pjob2->ji_qhdr,
			get_hostPart(pjob1->ji_wattr[(int)JOB_ATR_job_owner].at_val.at_str),
			MOVE_TYPE_Order)) ||
			(rc = svr_chkque(pjob2, pjob1->ji_qhdr,
			get_hostPart(pjob2->ji_wattr[(int)JOB_ATR_job_owner].at_val.at_str),
			MOVE_TYPE_Order))) {
			req_reject(rc, 0, req);
			return;
		}
	}

	/* now swap the order of the two jobs in the queue lists */

	rank = pjob1->ji_wattr[(int)JOB_ATR_qrank].at_val.at_long;
	pjob1->ji_wattr[(int)JOB_ATR_qrank].at_val.at_long =
		pjob2->ji_wattr[(int)JOB_ATR_qrank].at_val.at_long;
	pjob1->ji_wattr[(int)JOB_ATR_qrank].at_flags |= ATR_VFLAG_MODCACHE;
	pjob2->ji_wattr[(int)JOB_ATR_qrank].at_val.at_long = rank;
	pjob2->ji_wattr[(int)JOB_ATR_qrank].at_flags |= ATR_VFLAG_MODCACHE;

	if (pjob1->ji_qhdr != pjob2->ji_qhdr) {
		(void)strcpy(tmpqn, pjob1->ji_qs.ji_queue);
		(void)strcpy(pjob1->ji_qs.ji_queue, pjob2->ji_qs.ji_queue);
		(void)strcpy(pjob2->ji_qs.ji_queue, tmpqn);
		svr_dequejob(pjob1);
		svr_dequejob(pjob2);
		(void)svr_enquejob(pjob1);
		(void)svr_enquejob(pjob2);

	} else {
		swap_node(&pjob1->ji_jobque,  &pjob2->ji_jobque);
		swap_node(&pjob1->ji_alljobs, &pjob2->ji_alljobs);
	}

	/* need to update disk copy of both jobs to save new order */

	(void)job_save(pjob1, SAVEJOB_FULL);
	(void)job_save(pjob2, SAVEJOB_FULL);

	reply_ack(req);
}
