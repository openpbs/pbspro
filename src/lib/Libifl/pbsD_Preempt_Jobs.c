/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
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


/*	pbsD_modify_resv.c
 *
 *	The Modify Reservation request.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include "dis.h"
#include "libpbs.h"
#include "net_connect.h"
#include "pbs_ecl.h"
#include "pbs_idx.h"
#include "pbs_share.h"
#include "portability.h"
#include "tpp.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief	Helper function to send the preempt request out
 *
 * @param[in] connect - connection handler
 * @param[in] preempt_jobs_list - list of jobs to be preempted
 *
 * @return	int
 * @retval	0 for Success
 * @retval	1 for Error
 */
int
preempt_jobs_send(int connect, char **preempt_jobs_list)
{
	int rc = -1;

	DIS_tcp_funcs();

	if ((rc = encode_DIS_ReqHdr(connect, PBS_BATCH_PreemptJobs, pbs_current_user)) ||
	    (rc = encode_DIS_JobsList(connect, preempt_jobs_list, -1)) ||
	    (rc = encode_DIS_ReqExtend(connect, NULL))) {
		if (set_conn_errtxt(connect, dis_emsg[rc]) != 0) {
			pbs_errno = PBSE_SYSTEM;
			return 1;
		}
		if (pbs_errno == PBSE_PROTOCOL)
			return 1;
	}

	if (dis_flush(connect)) {
		pbs_errno = PBSE_PROTOCOL;
		return 1;
	}
	return 0;
}

/**
 * @brief	Helper function to recieve preempt request reply
 *
 * @param[in]	connect - fd of the server
 * @param[out]	ret_count - return buffer to store count of jobs sent
 *
 * @return	preempt_job_info *
 * @retval	list of jobs and their preempt_method
 * @retval	NULL for Error
 */
static preempt_job_info *
preempt_jobs_recv(int connect, int *ret_count)
{
	struct batch_reply *reply = NULL;
	preempt_job_info *ppj_reply = NULL;

	reply = PBSD_rdrpy(connect);
	if (reply == NULL)
		pbs_errno = PBSE_PROTOCOL;
	else {
		int i = 0;
		int count = 0;
		preempt_job_info *ppj_temp = NULL;

		ppj_temp = reply->brp_un.brp_preempt_jobs.ppj_list;
		count = reply->brp_un.brp_preempt_jobs.count;

		ppj_reply = calloc(sizeof(struct preempt_job_info), count);
		if (ppj_reply == NULL)
			return NULL;

		for (i = 0; i < count; i++) {
			strcpy(ppj_reply[i].job_id, ppj_temp[i].job_id);
			strcpy(ppj_reply[i].order, ppj_temp[i].order);
		}
		PBSD_FreeReply(reply);

		if (ret_count != NULL)
			*ret_count = count;
	}
	return ppj_reply;
}

/**
 * @brief
 *	-Pass-through call to send preempt jobs batch request
 *
 * @param[in] c - connection handler
 * @param[in] preempt_jobs_list - list of jobs to be preempted
 *
 * @return	preempt_job_info *
 * @retval	list of jobs and their preempt_method
 * @retval	NULL for Error/Failure
 *
 */
preempt_job_info *
__pbs_preempt_jobs(int c, char **preempt_jobs_list)
{
	preempt_job_info **p_replies = NULL;
	svr_conn_t **svr_connections = get_conn_svr_instances(c);
	int num_cfg_svrs = get_num_servers();
	int i;
	int count = 0;
	int last_count = -1;
	int retidx = 0;	/* first server's list will be used to collate results from all */
	preempt_job_info *ret = NULL;
	void *missing_jobs = NULL;

	if (!svr_connections)
		return NULL;

	p_replies = calloc(num_cfg_svrs, sizeof(preempt_job_info *));
	if (p_replies == NULL) {
		pbs_errno = PBSE_SYSTEM;
		goto err;
	}

	missing_jobs = pbs_idx_create(0, 0);
	if (missing_jobs == NULL) {
		pbs_errno = PBSE_SYSTEM;
		goto err;
	}

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		goto err;

	/* lock pthread mutex here for this connection
	 * blocking call, waits for mutex release */
	if (pbs_client_thread_lock_connection(c) != 0)
		goto err;

	/* With multi-server, jobs are sharded across multiple servers.
	 * So, send the request to all the active servers and collate their replies
	 */
	for (i = 0; i < num_cfg_svrs; i++) {
		if (preempt_jobs_send(svr_connections[i]->sd, preempt_jobs_list) != 0)
			goto err;
	}

	/**
	 * Each server will return an array containing all jobs
	 * We will do the following:
	 * 	- go through the first array, find the index which has order == "D",
	 * 	this implies that the job was either preempted by deletion or not found on the server.
	 *  - Because the servers can return the jobs in a different order than we sent it to them,
	 *  we can't just check the index of a missing job on other lists
	 *  - So, add the missing jobs to an AVL tree along with the index of the array.
	 *  - Go through other servers' lists, find indices which have order != "D", check
	 *  AVL tree to find the index of the first array for the job and copy over the state.
	 *  This implies that the job was found and preempted on another server.
	 * 	- Using the AVL tree avoids the cost of N strcmps otherwise needed to find the jobid
	 *  in the first array
	 */
	for (i = 0; i < num_cfg_svrs; last_count = count, i++) {
		if ((p_replies[i] = preempt_jobs_recv(svr_connections[i]->sd, &count)) == NULL)
			goto err;
		if (last_count != -1 && count != last_count) {
			/* something went wrong, this server did not return the same count, abort */
			goto err;
		}
	}

	/* Find jobs which couldn't be found on the first server and store them in AVL tree */
	for (i = 0; i < count; i++) {
		if (p_replies[retidx][i].order[0] == 'D' && num_cfg_svrs > 1) {
			int *data = malloc(sizeof(int));
			if (data == NULL) {
				pbs_errno = PBSE_SYSTEM;
				goto err;
			}
			*data = i;
			pbs_idx_insert(missing_jobs, p_replies[retidx][i].job_id, data);
		}
	}
	ret = p_replies[retidx];

	if (missing_jobs != NULL && num_cfg_svrs > 1) {
		for (i = 1; i < num_cfg_svrs; i++) {	/* Starting from 1 as retidx == 0 */
			int j;

			if (p_replies[i] == NULL)
				continue;

			/* Check if the missing jobs were found on any other server */
			for (j = 0; j < count; j++) {
				if (p_replies[i][j].order[0] != 'D') {
					/* Job was found on another server!
					 * Let's find the index of this job in 'ret' list
					 */
					int *index = NULL;
					char *jid = strdup(p_replies[i][j].job_id);
					pbs_idx_find(missing_jobs, (void **) &jid, (void **) &index, NULL);
					free(jid);
					if (index != NULL && *index < count) {
						strcpy(ret[*index].order, p_replies[i][j].order);
						pbs_idx_delete(missing_jobs, jid);
						free(index);
					}
				}
			}
		}
	}

	/* unlock the thread lock and update the thread context data */
	if (pbs_client_thread_unlock_connection(c) != 0)
		goto err;

	/* Free up all the other lists */
	for (i = 1; i < num_cfg_svrs; i++) {
		free(p_replies[i]);
	}
	free(p_replies);
	pbs_idx_destroy(missing_jobs);

	return ret;

err:
	if (p_replies != NULL) {
		for (i = 0; i < num_cfg_svrs; i++) {
			free(p_replies[i]);
		}
		free(p_replies);
	}
	pbs_idx_destroy(missing_jobs);
	return NULL;
}
