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
 * @file    run_sched.c
 *
 * @brief
 * 		run_sched.c	-	Functions related to the scheduler.
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include "portability.h"
#include <errno.h>
#include <netinet/in.h>
#include "log.h"
#include "libpbs.h"
#include "net_connect.h"
#include "sched_cmds.h"
#include "dis.h"
#include "linked_list.h"
#include "attribute.h"
#include "job.h"
#include "server.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "work_task.h"
#include "pbs_error.h"
#include "batch_request.h"


/* Global Data */

extern struct server server;
extern pbs_net_t pbs_scheduler_addr;
extern unsigned int pbs_scheduler_port;
extern char      server_name[];
extern int	 svr_do_schedule;
extern int	 svr_do_sched_high;
extern char     *msg_sched_called;
extern char     *msg_sched_nocall;
extern pbs_list_node svr_deferred_req;
extern void  est_start_timed_task(struct work_task *);
extern char	*msg_noloopbackif;
extern char	*msg_daemonname;

int scheduler_sock = -1;	/* socket open to scheduler during a cycle */
int scheduler_sock2 = -1;
int scheduler_jobs_stat = 0;	/* set to 1 once scheduler queried jobs in a cycle*/
extern int svr_unsent_qrun_req;

/**
 * @brief
 * 		am_jobs - array of pointers to jobs which were moved or which had certain
 * 		attributes altered (qalter) while a schedule cycle was in progress.
 *		If a job in the array is run by the scheduler in the cycle, that run
 *		request is rejected as the move/modification may impact the job's
 *		requirements and placement.
 */
static struct   am_jobs {
	int   am_used;		/* number of jobs in the array  */
	int   am_max;		/* number of slots in the array */
	job **am_array;		/* pointer the malloc-ed array  */
} am_jobs = { 0, 0, NULL };


/* Functions private to this file */
static void scheduler_close(int);

#define SCHEDULER_ALARM_TIME 20
/**
 * @brief
 * 		catchalrm	-	put a timeout alarm in case of timeout occurs when contacting the scheduler.
 *
 * @param[in]	sig	-	not used here.
 */
void
catchalrm(int sig)
{
	log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_SERVER, LOG_NOTICE,
		server_name, "timeout attempting to contact scheduler");
}

/**
 * @brief
 *		Sends 'cmd'  over to network 'sock', and if 'cmd' is SCH_SCHEDULE_AJOB,  *	sends also the 'jobid'.
 *
 * @param[in]	sock	-	communication endpoint
 * @param[in]	cmd	-	the command to send
 * @param[in]	jobid	-	the jobid to send if 'cmd' is SCH_SCHEDULE_AJOB
 *
 * @return	int
 * @retval	0	for success
 * @retval	-1	for failure
 */

int
put_sched_cmd(int sock, int cmd, char *jobid)
{
	int   ret;

	DIS_tcp_setup(sock);
	if ((ret = diswsi(sock, cmd)) != DIS_SUCCESS)
		goto err;

	if (cmd == SCH_SCHEDULE_AJOB) {
		if ((ret = (diswst(sock, jobid))) != DIS_SUCCESS)
			goto err;
	}

	(void)DIS_tcp_wflush(sock);
	return 0;

err:
	sprintf(log_buffer, "write to scheduler failed, err=%d", ret);
	log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_SERVER, LOG_INFO, server_name,
		log_buffer);
	return -1;
}

/**
 * @brief
 * 		contact_sched - open connection to the scheduler and send it a command
 *		Jobid is passed if and only if the cmd is SCH_SCHEDULE_AJOB
 *
 * @param[in]	cmd	- the command to send
 * @param[in]	jobid	- the jobid to send if 'cmd' is SCH_SCHEDULE_AJOB
 */

int
contact_sched(int cmd, char *jobid)
{
	int sock;
	conn_t *conn;
#ifndef WIN32
	struct sigaction act, oact;
#endif

	if ((cmd == SCH_SCHEDULE_AJOB) && (jobid == NULL))
		return -1;	/* need a jobid */

#ifndef WIN32

	/* connect to the Scheduler */
	/* put a timeout alarm around the connect */

	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = catchalrm;
	if (sigaction(SIGALRM, &act, &oact) == -1)
		return (PBS_NET_RC_RETRY);
	alarm(SCHEDULER_ALARM_TIME);
#endif

	/* Under win32, this function does a timeout wait on the non-blocking socket */
	sock = client_to_svr(pbs_scheduler_addr, pbs_scheduler_port, 1); /* scheduler connection still uses resv-ports */
	if (pbs_errno == PBSE_NOLOOPBACKIF)
		log_err(PBSE_NOLOOPBACKIF, "client_to_svr" , msg_noloopbackif);

#ifndef WIN32
	alarm(0);

	(void)sigaction(SIGALRM, &oact, NULL);	/* reset handler for SIGALRM */
#endif

	if (sock < 0) {
		log_err(errno, __func__, msg_sched_nocall);
		return (-1);
	}
	conn = add_conn(sock, FromClientDIS, pbs_scheduler_addr,
		pbs_scheduler_port, process_request);
	if (!conn) {
		log_err(errno, __func__, "could not find sock in connection table");
		return (-1);
	}

	conn->cn_authen |=
		PBS_NET_CONN_FROM_PRIVIL | PBS_NET_CONN_AUTHENTICATED;

	net_add_close_func(sock, scheduler_close);

	/* send command to Scheduler */

	if (put_sched_cmd(sock, cmd, jobid) < 0) {
		close_conn(sock);
		return (-1);
	}
	(void)sprintf(log_buffer, msg_sched_called, cmd);
	log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_SERVER, LOG_INFO,
		server_name, log_buffer);
	return (sock);
}

/**
 * @brief
 * 		schedule_high	-	send high priority commands to the scheduler
 *
 * @return	int
 * @retval  1	: scheduler busy
 * @retval  0	: scheduler notified
 * @retval	-1	: error
 */
int
schedule_high()
{
	int s;
	if (scheduler_sock == -1) {
		if ((s = contact_sched(svr_do_sched_high, NULL)) < 0)
			return (-1);
		set_sched_sock(s);
		if (scheduler_sock2 == -1) {
			if ((s = contact_sched(SCH_SCHEDULE_NULL, NULL)) >= 0)
				scheduler_sock2 = s;
		}
		svr_do_sched_high = SCH_SCHEDULE_NULL;
		return 0;
	}
	return 1;
}

/**
 * @brief
 * 		Contact scheduler and direct it to run a scheduling cycle
 *		If a request is already outstanding, skip this one.
 *
 * @return	int
 * @retval	-1	: error
 * @reval	0	: scheduler notified
 * @retval	+1	: scheduler busy
 *
 * @par Side Effects:
 *     the global variable (first_time) is changed.
 *
 * @par MT-safe: No
 */

int
schedule_jobs()
{
	int cmd;
	int s;
	static int first_time = 1;
	struct deferred_request *pdefr;
	char  *jid = NULL;

	if (first_time)
		cmd = SCH_SCHEDULE_FIRST;
	else
		cmd = svr_do_schedule;

	if (scheduler_sock == -1) {

		/* are there any qrun requests from manager/operator */
		/* which haven't been sent,  they take priority      */
		pdefr = (struct deferred_request *)GET_NEXT(svr_deferred_req);
		while (pdefr) {
			if (pdefr->dr_sent == 0) {
				s = is_job_array(pdefr->dr_id);
				if (s == IS_ARRAY_NO) {
					if (find_job(pdefr->dr_id) != NULL) {
						jid = pdefr->dr_id;
						cmd = SCH_SCHEDULE_AJOB;
						break;
					}
				} else if ((s == IS_ARRAY_Single) ||
					(s == IS_ARRAY_Range)) {
					if (find_arrayparent(pdefr->dr_id) != NULL) {
						jid = pdefr->dr_id;
						cmd = SCH_SCHEDULE_AJOB;
						break;
					}
				}
			}
			pdefr = (struct deferred_request *)GET_NEXT(pdefr->dr_link);
		}

		if ((s = contact_sched(cmd, jid)) < 0)
			return (-1);
		else if (pdefr != NULL)
			pdefr->dr_sent = 1;   /* mark entry as sent to sched */
		set_sched_sock(s);
		if (scheduler_sock2 == -1) {
			if ((s = contact_sched(SCH_SCHEDULE_NULL, NULL)) >= 0)
				scheduler_sock2 = s;
		}
		svr_do_schedule = SCH_SCHEDULE_NULL;
		first_time = 0;

		/* if there are more qrun requests queued up, reset cmd so */
		/* they are sent when the Scheduler completes this cycle   */
		pdefr = GET_NEXT(svr_deferred_req);
		while (pdefr) {
			if (pdefr->dr_sent == 0) {
				svr_do_schedule = SCH_SCHEDULE_AJOB;
				break;
			}
			pdefr = (struct deferred_request *)GET_NEXT(pdefr->dr_link);
		}

		return (0);
	} else
		return (1);	/* scheduler was busy */
}

/**
 * @brief
 * 		scheduler_close - connection to scheduler has closed, clear scheduler_called
 * @par
 * 		Connection to scheduler has closed, mark scheduler sock as
 *		closed with -1 and if any clean up any outstanding deferred scheduler
 *		requests (qrun).
 * @par
 * 		Perform some cleanup as connection to scheduler has closed
 *
 * @param[in]	sock	-	communication endpoint.
 * 							closed (scheduler connection) socket, not used but
 *							required to match general prototype of functions called when
 *							a socket is closed.
 * @return	void
 */

static void
scheduler_close(int sock)
{
	struct deferred_request *pdefr;

	if ((sock != -1) && (sock == scheduler_sock2)) {
		scheduler_sock2 = -1;
		return;	/* nothing to check if scheduler_sock2 */
	}

	set_sched_sock(-1);

	/* clear list of jobs which were altered/modified during cycle */
	am_jobs.am_used = 0;
	scheduler_jobs_stat = 0;

	/**
	 *	If a deferred (from qrun) had been sent to the Scheduler and is still
	 *	there, then the Scheduler must have closed the connection without
	 *	dealing with the job.  Tell qrun it failed if the qrun connection
	 *	is still there.
	 *      If any qrun request is pending in the deffered list, set svr_unsent_qrun_req so
	 * 	they are sent when the Scheduler completes this cycle 
	 */
	pdefr = (struct deferred_request *)GET_NEXT(svr_deferred_req);
	while (pdefr) {
		struct deferred_request *next_pdefr = (struct deferred_request *)GET_NEXT(pdefr->dr_link);
		if (pdefr->dr_sent != 0) {
			log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_JOB,
				LOG_NOTICE, pdefr->dr_id,
				"deferred qrun request to scheduler failed");
			if (pdefr->dr_preq != NULL)
				req_reject(PBSE_INTERNAL, 0, pdefr->dr_preq);
			/* unlink and free the deferred request entry */
			delete_node(&pdefr->dr_link);
			free(pdefr);
		}
		else if((pdefr->dr_sent == 0) && (svr_unsent_qrun_req == 0)) {
			svr_unsent_qrun_req = 1;
		}
		pdefr = next_pdefr;
	}
}

/**
 * @brief
 * 		Add a job to the am_jobs array, called when a job is moved (locally)
 *		or modified (qalter) during a scheduling cycle
 *
 * @param[in]	pjob	-	pointer to job to add to the array.
 */
void
am_jobs_add(job *pjob)
{
	if (am_jobs.am_used == am_jobs.am_max) {
		/* Need to expand the array, increase by 4 slots */
		job **tmp = realloc(am_jobs.am_array, sizeof(job *) * (am_jobs.am_max + 4));
		if (tmp == NULL)
			return;	/* cannot increase array, so be it */
		am_jobs.am_array = tmp;
		am_jobs.am_max  += 4;
	}
	*(am_jobs.am_array + am_jobs.am_used++) = pjob;
}

/**
 * @brief
 * 		Determine if the job in question is in the list of moved/altered
 *		jobs.  Called when a run request for a job comes from the Scheduler.
 *
 * @param[in]	pjob	-	pointer to job in question.
 *
 * @return	int
 * @retval	0	- job not in list
 * @retval	1	- job is in list
 */
int
was_job_alteredmoved(job *pjob)
{
	int i;
	for (i=0; i<am_jobs.am_used; ++i) {
		if (*(am_jobs.am_array+i) == pjob)
			return 1;
	}
	return 0;
}

/**
 * @brief
 * 		set_scheduler_flag - set the flag to call the Scheduler
 *		certain flag values should not be overwritten
 *
 * @param[in]	flag	-	pointer to job in question.
 */
void
set_scheduler_flag(int flag)
{
	/* high priority commands:
	 * Note: A) usually SCH_QUIT is sent directly and not via here
	 *       B) if we ever add a 3rd high prio command, we can lose them
	 */
	if (flag == SCH_CONFIGURE || flag == SCH_QUIT) {
		if (svr_do_sched_high == SCH_QUIT)
			return; /* keep only SCH_QUIT */

		svr_do_sched_high = flag;
	}
	else
		svr_do_schedule = flag;
}
