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

/**
 * contains functions related to PBS scheduler
 */
#include <pbs_config.h> /* the master config generated by configure */

#include <stdio.h>

#ifdef PYTHON
#include <Python.h>
#include <pythonrun.h>
#include <wchar.h>
#endif

#include "check.h"
#include "constant.h"
#include "data_types.h"
#include "fifo.h"
#include "globals.h"
#include "libpbs.h"
#include "log.h"
#include "resource.h"
#include "server_info.h"

extern int sched_main(int argc, char *argv[], int (*schedule_func)(int, const sched_cmd *));

/**
 * @brief	Perform scheduling in "mock run" mode
 *
 * @param[in]	policy	-	policy info
 * @param[in]	sd	-	primary socket descriptor to the server pool
 * @param[in]	sinfo	-	pbs universe we're going to loop over
 * @param[out]	rerr	-	error bits from the last job considered
 *
 *	@return return code of last job scheduled
 *	@retval -1	: on error
 */
int
main_sched_loop_bare(int sd, server_info *sinfo)
{
	node_info **nodes = sinfo->nodes;
	resource_resv **jobs = sinfo->jobs;
	int ij;
	int in = 0;
	char execvnode[PBS_MAXHOSTNAME + strlen("(:ncpus=1)") + 1];

	/* Algorithm:
     * - Loop over all jobs, assume that they need just 1 ncpu to run, and
     *  choose the next free node for it
     */
	for (ij = 0; jobs[ij] != NULL; ij++) {
		execvnode[0] = '\0';

		/* Find the first free node and fill it */
		for (; nodes[in] != NULL; in++) {
			node_info *node = nodes[in];
			schd_resource *ncpures = NULL;

			if (node->is_job_busy)
				continue;

			ncpures = find_resource(node->res, allres["ncpus"]);
			if (ncpures == NULL)
				continue;

			/* Assign a cpu on this node */
			ncpures->assigned += 1;
			if (dynamic_avail(ncpures) == 0) {
				node->is_job_busy = 1;
				node->is_free = 0;
			}

			/* Create the exec_node for the job */
			snprintf(execvnode, sizeof(execvnode), "(%s:ncpus=1)", node->name.c_str());

			/* Send the run request */
			send_run_job(sd, 0, jobs[ij]->name, execvnode, jobs[ij]->svr_inst_id);

			break;
		}
		if (execvnode[0] == '\0') {
			log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_JOB, LOG_NOTICE, "",
				  "No free nodes available, won't consider any more jobs");
			break;
		}
	}
	return SUCCESS;
}

/**
 * @brief
 *		scheduling_cycle - the controling function of the scheduling cycle
 *
 * @param[in]	sd	-	primary socket descriptor to the server pool
 *
 * @return	int
 * @retval	0 for normal operation
 * @retval	1 for sched exit
 */
static int
scheduling_cycle_bare(int sd, const sched_cmd *cmd)
{
	server_info *sinfo; /* ptr to the server/queue/job/node info */

	log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_REQUEST, LOG_DEBUG,
		  "", "Starting Scheduling Cycle");

	/* Decide whether we need to send "can't run" type updates this cycle */
	if (time(NULL) - last_attr_updates >= sc_attrs.attr_update_period)
		send_job_attr_updates = 1;
	else
		send_job_attr_updates = 0;

	update_cycle_status(cstat, 0);

	/* create the server / queue / job / node structures */
	if ((sinfo = query_server(&cstat, sd)) == NULL) {
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_NOTICE,
			  "", "Problem with creating server data structure");
		end_cycle_tasks(sinfo);
		return 1;
	}

	main_sched_loop_bare(sd, sinfo);

	end_cycle_tasks(sinfo);

	return 0;
}

/**
 * @brief
 *		schedule - this function gets called to start each scheduling cycle
 *		   It will handle the difference cases that caused a
 *		   scheduling cycle
 *
 * @param[in]	sd	-	primary socket descriptor to the server pool
 *
 * @return	int
 * @retval	0	: continue calling scheduling cycles
 * @retval	1	: exit scheduler
 */
static int
schedule_bare(int sd, const sched_cmd *cmd)
{
	switch (cmd->cmd) {
	case SCH_SCHEDULE_NULL:
	case SCH_RULESET:
		/* ignore and end cycle */
		break;

	case SCH_SCHEDULE_FIRST:
		/*
		 * on the first cycle after the server restarts custom resources
		 * may have been added.  Dump what we have so we'll requery them.
		 */
		update_resource_defs(sd);

		/* Get config from the qmgr sched object */
		if (!set_validate_sched_attrs(sd))
			return 0;

	case SCH_SCHEDULE_NEW:
	case SCH_SCHEDULE_TERM:
	case SCH_SCHEDULE_CMD:
	case SCH_SCHEDULE_TIME:
	case SCH_SCHEDULE_JOBRESV:
	case SCH_SCHEDULE_STARTQ:
	case SCH_SCHEDULE_MVLOCAL:
	case SCH_SCHEDULE_ETE_ON:
	case SCH_SCHEDULE_RESV_RECONFIRM:
		return scheduling_cycle_bare(sd, cmd);
	case SCH_SCHEDULE_AJOB:
		return scheduling_cycle_bare(sd, cmd);
	case SCH_CONFIGURE:
		log_event(PBSEVENT_SCHED, PBS_EVENTCLASS_SCHED, LOG_INFO,
			  "reconfigure", "Scheduler is reconfiguring");
		update_resource_defs(sd);

		/* Get config from sched_priv/ files */
		if (schedinit(-1) != 0)
			return 0;

		/* Get config from the qmgr sched object */
		if (!set_validate_sched_attrs(sd))
			return 0;
		break;
	case SCH_QUIT:
#ifdef PYTHON
		Py_Finalize();
#endif
		return 1; /* have the scheduler exit nicely */
	default:
		return 0;
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	if (set_msgdaemonname(const_cast<char *>("pbs_sched_bare"))) {
		fprintf(stderr, "Out of memory\n");
		return (1);
	}

	return sched_main(argc, argv, schedule_bare);
}
