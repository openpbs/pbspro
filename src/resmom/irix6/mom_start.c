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
#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <sys/syssgi.h>
#include <sys/arsess.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <proj.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <optional_sym.h>
#include "portability.h"
#include "libpbs.h"
#include "linked_list.h"
#include "log.h"
#include "server_limits.h"
#include "attribute.h"
#include "resource.h"
#include "job.h"
#include "mom_mach.h"
#include "mom_func.h"

/**
 * @file
 */
/* Global Variables */

extern int	 exiting_tasks;
extern char	 mom_host[];
extern pbs_list_node svr_alljobs;
extern int	 termin_child;

/* Private variables */


/**
 * @brief
 * 	set_sgi_proj - set SGI project id for job/task
 *
 *	As a side effect, the access to the project files is closed.
 *
 * @param[in] usern - he user name
 * @param[in] acct - the account attribute or NULL
 *
 * @return	int
 * @retval	0	Success
 * @retval	-1	Error
 *
 */

int
set_sgi_proj(char *usern, attribute *acct)
{
	prid_t    prid;

	if ((acct == 0) || ((acct->at_flags & ATR_VFLAG_SET) == 0)) {

		/* use default projid for user */

		prid = getdfltprojuser(usern);
	} else {

		/* use Account as project name, if valid --- convert to id */

		prid = validateproj(usern, acct->at_val.at_str);
	}

	if ((prid == -1) || (setprid(prid) == -1))
		return -1;
	return 0;
}

/**
 * @brief
 *      Set session id and whatever else is required on this machine
 *      to create a new job.
 *      On a Cray, an ALPS reservation will be created and confirmed.
 *
 * @param[in]   pjob - pointer to job structure
 * @param[in]   sjr  - pointer to startjob_rtn structure
 *
 * @return session/job id
 * @retval -1 error from setsid(), no message in log_buffer
 * @retval -2 temporary error, retry job, message in log_buffer
 * @retval -3 permanent error, abort job, message in log_buffer
 *
 */

int
set_job(job *pjob, struct startjob_rtn *sjr)
{
	int		rv;
	jid_t		sgijid;
	char 		jl_domain[PBS_MAXQUEUENAME+15] = "PBS_";
	static	int jlimiterr = 0;

	/* Set up SGI Job container */

	if (pjob->ji_extended.ji_ext.ji_jid > 0) {

		/*
		 * already have a job id - from Mother Superior
		 * join it or create one with that id
		 */

		sjr->sj_jid = pjob->ji_extended.ji_ext.ji_jid;

		if (_MIPS_SYMBOL_PRESENT(getjid) && _MIPS_SYMBOL_PRESENT(makenewjob)) {
			/* we are on a system that knows about job limits */

			if ((getjid() != pjob->ji_extended.ji_ext.ji_jid) &&
				(syssgi(SGI_JOINJOB, pjob->ji_extended.ji_ext.ji_jid)!=0)) {
				/* attempt to join job failed */
				if (errno == ENOPKG) {
					log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB,
						LOG_ERR,
						pjob->ji_qs.ji_jobid,
						"job limits ENOPKG");
				} else {
					/* have to use makenewjob() to force jid */
					sgijid = makenewjob(pjob->ji_extended.ji_ext.ji_jid,
						pjob->ji_qs.ji_un.ji_momt.ji_exuid);
					if (sgijid != pjob->ji_extended.ji_ext.ji_jid) {
						/* bad news */
						(void)sprintf(log_buffer,
							"join job limits failed: %d",
							errno);
						log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB,
							LOG_ERR,
							pjob->ji_qs.ji_jobid,
							log_buffer);
						return (-2);
					}
				}
			}	/* joined exiting job ok */
		}		/* end of have existing jid */

	} else if (_MIPS_SYMBOL_PRESENT(getjid) &&
		_MIPS_SYMBOL_PRESENT(jlimit_startjob)) {
		/* set up new job id */
		(void)strcat(jl_domain, pjob->ji_wattr[(int)JOB_ATR_in_queue].at_val.at_str);	/* PBS_{queue_name} */
		(void)strcat(jl_domain, ":PBS:batch");
		sgijid = jlimit_startjob(
			pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str,
			pjob->ji_qs.ji_un.ji_momt.ji_exuid,
			jl_domain);
		DBPRT(("set job %s SGI jid to 0x%llx\n", pjob->ji_qs.ji_jobid, sgijid))
		if (sgijid > 0) {
			/* valid job */
			sjr->sj_jid = sgijid;
			sprintf(log_buffer, "set jobid 0x%llx", sgijid);
			log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, LOG_INFO,
				pjob->ji_qs.ji_jobid,
				log_buffer);
		} else if (errno == ENOPKG) {
			if (jlimiterr == 0) {
				/* startjob failed */
				log_err(errno, "set_job", "jlimit_startjob failed");
				jlimiterr = 1;
			}
		} else {
			/* startjob failed */
			log_err(errno, "set_job", "jlimit_startjob failed");

			return (-1);
		}
	}

	rv = -1;

	/* if there is an existing array for this job, join it */

	if ((pjob->ji_extended.ji_ext.ji_ash != 0) &&
		(getash() != pjob->ji_extended.ji_ext.ji_ash)) {
		rv = syssgi(SGI_JOINARRAYSESS, 0, &pjob->ji_extended.ji_ext.ji_ash);
	}
	if (rv < 0) {
		/* join failed or no session - create new array session */
		if (newarraysess() == -1) {
			(void)sprintf(log_buffer, "newarraysess failed, err=%d",
				errno);
			return (-2);
		}
	}

	sjr->sj_ash = getash();

	if ((pjob->ji_extended.ji_ext.ji_ash != 0) &&
		(sjr->sj_ash != pjob->ji_extended.ji_ext.ji_ash)) {
		/* may not have arrayd running here */
		/* try to force ash 		    */
		if (setash(pjob->ji_extended.ji_ext.ji_ash) < 0) {
			sprintf(log_buffer, "setash failed to %lld, err %d",
				pjob->ji_extended.ji_ext.ji_ash, errno);
			return (-2);
		} else {
			sjr->sj_ash = pjob->ji_extended.ji_ext.ji_ash;
		}
	}

	if (set_sgi_proj(pjob->ji_wattr[(int)JOB_ATR_euser].at_val.at_str,
		&pjob->ji_wattr[(int)JOB_ATR_account]) < 0) {
		(void)sprintf(log_buffer, "Invalid project id");
		return (-3);
	}
	return (sjr->sj_session = setsid());
}

/**
 * @brief
 *      set_globid - set the global id for a machine type.
 *
 * @param[in] pjob - pointer to job structure
 * @param[in] sjr  - pointer to startjob_rtn structure
 *
 * @return Void
 *
 */

void
set_globid(job *pjob, struct startjob_rtn *sjr)
{
	char cbuf[50];

	(void)sprintf(cbuf, "jid=0x%llx,ash=0x%llx", sjr->sj_jid, sjr->sj_ash);
	(void)decode_str(&pjob->ji_wattr[JOB_ATR_altid], ATTR_altid, NULL, cbuf);
	pjob->ji_extended.ji_ext.ji_jid = sjr->sj_jid;
	pjob->ji_extended.ji_ext.ji_ash = sjr->sj_ash;
}


/**
 * @brief
 *      set_mach_vars - setup machine dependent environment variables
 *
 * @param[in] pjob - pointer to job structure
 * @param[in] vtab - pointer to var_table structure
 *
 * @return      int
 * @retval      0       Success
 *
 */

int
set_mach_vars(job *pjob, struct var_table *vtab)
{
	return 0;
}

/**
 * @brief
 *      sets the shell to be used
 *
 * @param[in] pjob - pointer to job structure
 * @param[in] pwdp - pointer to passwd structure
 *
 * @return      string
 * @retval      shellname       Success
 *
 */

char *
set_shell(job *pjob, struct passwd *pwdp)
{
	char *cp;
	int   i;
	char *shell;
	struct array_strings *vstrs;
	/*
	 * find which shell to use, one specified or the login shell
	 */

	shell = pwdp->pw_shell;
	if ((pjob->ji_wattr[(int)JOB_ATR_shell].at_flags & ATR_VFLAG_SET) &&
		(vstrs = pjob->ji_wattr[(int)JOB_ATR_shell].at_val.at_arst)) {
		for (i = 0; i < vstrs->as_usedptr; ++i) {
			cp = strchr(vstrs->as_string[i], '@');
			if (cp) {
				if (!strncmp(mom_host, cp+1, strlen(cp+1))) {
					*cp = '\0';	/* host name matches */
					shell = vstrs->as_string[i];
					break;
				}
			} else {
				shell = vstrs->as_string[i];	/* wildcard */
			}
		}
	}
	return (shell);
}

/**
 * @brief
 * 	scan_for_terminated - scan the list of runnings jobs for one whose
 *	session id matched that of a terminated child pid.  Mark that
 *	job as Exiting.
 *
 * @return	Void
 *
 */

void
scan_for_terminated()
{
	int		exiteval;
	pid_t		pid;
	job		*pjob;
	task		*ptask;
	int		statloc;

	/* update the latest intelligence about the running jobs;         */
	/* must be done before we reap the zombies, else we lose the info */

	termin_child = 0;

	if (mom_get_sample() == PBSE_NONE) {
		pjob = (job *)GET_NEXT(svr_alljobs);
		while (pjob) {
			if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_RUNNING)
				mom_set_use(pjob);
			pjob = (job *)GET_NEXT(pjob->ji_alljobs);
		}
	}

	/* Now figure out which task(s) have terminated (are zombies) */

	while ((pid = waitpid(-1, &statloc, WNOHANG)) > 0) {

		pjob = (job *)GET_NEXT(svr_alljobs);
		while (pjob) {
			/*
			 ** see if process was a child doing a special
			 ** function for MOM
			 */
			if (pid == pjob->ji_momsubt)
				break;
			/*
			 ** look for task
			 */
			ptask = (task *)GET_NEXT(pjob->ji_tasks);
			while (ptask) {
				if (ptask->ti_qs.ti_sid == pid)
					break;
				ptask = (task *)GET_NEXT(ptask->ti_jobtask);
			}
			if (ptask != NULL)
				break;
			pjob = (job *)GET_NEXT(pjob->ji_alljobs);
		}
		if (WIFEXITED(statloc))
			exiteval = WEXITSTATUS(statloc);
		else if (WIFSIGNALED(statloc))
			exiteval = WTERMSIG(statloc) + 10000;
		else
			exiteval = 1;

		if (pjob == NULL) {
			DBPRT(("%s: pid %d not tracked, exit %d\n",
				__func__, pid, exiteval))
			continue;
		}

		if (pid == pjob->ji_momsubt) {
			pjob->ji_momsubt = 0;
			if (pjob->ji_mompost) {
				pjob->ji_mompost(pjob, exiteval);
			}
			(void)job_save(pjob, SAVEJOB_QUICK);
			continue;
		}
		/*
		 ** We found task within the job which has exited.
		 */
		DBPRT(("%s: task %08.8X pid %d exit value %d\n", __func__,
			ptask->ti_qs.ti_task, pid, exiteval))
		kill_session(ptask->ti_qs.ti_sid, SIGKILL, 0);
		ptask->ti_qs.ti_exitstat = exiteval;
		ptask->ti_qs.ti_status = TI_STATE_EXITED;
		(void)task_save(ptask);
		sprintf(log_buffer, "task %08.8X terminated",
			ptask->ti_qs.ti_task);
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_DEBUG,
			pjob->ji_qs.ji_jobid, log_buffer);

		exiting_tasks = 1;
	}
}


/**
 * @brief
 *	creat the master pty, this particular
 * 	piece of code depends on multiplexor /dev/ptc
 *
 * @param[out] rtn_name - holds info of tty
 *
 * @return      int
 * @retval      fd      Success
 * @retval      -1      Failure
 *
 */


int
oien_master(char **rtn_name)
{
	int	fds;

	*rtn_name = _getpty(&fds, O_RDWR | O_NOCTTY, 0600, 1);
	if (*rtn_name == (char *)0)
		return (-1);
	else
		return (fds);
}


/*
 * struct sig_tbl = map of signal names to numbers,
 * see req_signal() in ../requests.c
 */

struct sig_tbl sig_tbl[] = {
	{ "NULL", 0 },
	{ "HUP", SIGHUP },
	{ "INT", SIGINT },
	{ "QUIT", SIGQUIT },
	{ "ILL", SIGILL },
	{ "TRAP", SIGTRAP },
	{ "IOT", SIGIOT },
	{ "ABRT", SIGABRT },
	{ "EMT", SIGEMT },
	{ "FPE", SIGFPE },
	{ "KILL", SIGKILL },
	{ "BUS", SIGBUS },
	{ "SEGV", SIGSEGV },
	{ "SYS", SIGSYS },
	{ "PIPE", SIGPIPE },
	{ "ALRM", SIGALRM },
	{ "TERM", SIGTERM },
	{ "USR1", SIGUSR1 },
	{ "USR2", SIGUSR2 },
	{ "CHLD", SIGCHLD },
	{ "PWR", SIGPWR },
	{ "WINCH", SIGWINCH },
	{ "URG", SIGURG },
	{ "POLL", SIGPOLL },
	{ "IO", SIGIO },
	{ "STOP", SIGSTOP },
	{ "TSTP", SIGTSTP },
	{ "CONT", SIGCONT },
	{ "TTIN", SIGTTIN },
	{ "TTOU", SIGTTOU },
	{ "VTALRM", SIGVTALRM },
	{ "PROF", SIGPROF },
	{ "XCPU", SIGXCPU },
	{ "XFSZ", SIGXFSZ },
	{(char *)0, -1}
};
