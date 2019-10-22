/*
 * Copyright (C) 1994-2019 Altair Engineering, Inc.
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
 * @file	req_cred.c
 *
 * @brief
 *  Server routines providing and sending credentials to superior mom.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include "libpbs.h"
#include "job.h"
#include "batch_request.h"
#include "pbs_error.h"
#include "log.h"
#include "pbs_nodes.h"
#include "server.h"

#define	CRED_DATA_SIZE	4096

/* credential type */
#define CRED_NONE	0
#define CRED_KRB5	1

extern struct server server;
extern void release_req(struct work_task *);
extern int relay_to_mom(job *, struct batch_request *, void (*)(struct work_task *));
extern time_t time_now;
pbs_list_head svr_creds_cache;	/* all credentials cached and available to send */
extern long svr_cred_renew_cache_period;

struct cred_cache {
	pbs_list_link	cr_link;
	char	  credid[PBS_MAXUSER+1];
	long	  validity;
	int	  type;
	char	  *data; /* credentials in base64 */
	size_t	  size;
};
typedef struct cred_cache cred_cache;

/* @brief
 *	First, this function checks whether the credentials for credid (e.g. principal) of the
 *	job are stored in server's memory cache and whether the credentials are
 *	not too old. Such credentials are returned. If they are not present
 *	in cache or are too old new credentials are requested with the 
 *	SRV_ATR_cred_renew_tool and renewed credentials are stored in the cache
 *	(server's memory).
 *
 * @param[in] pjob - pointer to job, the credentials are requested for this job
 *
 * @return	cred_cache
 * @retval	structure with credentials on success
 * @retval	NULL otherwise
 */
static struct cred_cache *
get_cached_cred(job  *pjob)
{
#if defined(PBS_SECURITY) && (PBS_SECURITY == KRB5)
	cred_cache *cred = NULL;
	cred_cache *nxcred = NULL;
	char cmd[MAXPATHLEN + PBS_MAXUSER + 2]; /* +1 for space and +1 for EOL */
	char buf[CRED_DATA_SIZE];
	FILE *fp;
	long validity = 0;
	int cred_type = CRED_NONE;
	int ret = 0;

	/* try the cache first */
	cred = (cred_cache *)GET_NEXT(svr_creds_cache);
	while (cred) {
		nxcred = (cred_cache *)GET_NEXT(cred->cr_link);

		if (strcmp(cred->credid, pjob->ji_wattr[(int)JOB_ATR_cred_id].at_val.at_str) == 0 &&
			cred->validity - svr_cred_renew_cache_period >  time_now) {
			/* valid credential found */
			return cred;
		}

		/* too old credential - delete from cache */
		if (cred->validity - svr_cred_renew_cache_period <= time_now) {
		    delete_link(&cred->cr_link);
		    free(cred->data);
		    free(cred);
		}

		cred = nxcred;
	}

	/* valid credentials not cached, get new one */

	if ((server.sv_attr[(int)SRV_ATR_cred_renew_tool].at_flags & ATR_VFLAG_SET) == 0) {
		snprintf(log_buffer, LOG_BUF_SIZE, "%s is not set", ATTR_cred_renew_tool);
		log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
			LOG_ERR, msg_daemonname, log_buffer);
		return NULL;
	}

	snprintf(log_buffer, LOG_BUF_SIZE, "using %s '%s' to acquire credentials for user: %s",
		ATTR_cred_renew_tool,
		server.sv_attr[(int)SRV_ATR_cred_renew_tool].at_val.at_str,
		pjob->ji_wattr[(int)JOB_ATR_cred_id].at_val.at_str);
	log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_SERVER,
		LOG_DEBUG, msg_daemonname, log_buffer);

	snprintf(cmd, MAXPATHLEN + PBS_MAXUSER + 2, "%s %s", /* +1 for space and +1 for EOL */
		server.sv_attr[(int)SRV_ATR_cred_renew_tool].at_val.at_str,
		pjob->ji_wattr[(int)JOB_ATR_cred_id].at_val.at_str);

	if ((fp = popen(cmd, "r")) == NULL) {
		snprintf(log_buffer, LOG_BUF_SIZE, "%s failed to open pipe, command: '%s'",
			ATTR_cred_renew_tool, cmd);
		log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
			LOG_ERR, msg_daemonname, log_buffer);
		return NULL;
	}

	while (fgets(buf, CRED_DATA_SIZE, fp) != NULL) {
		strtok(buf, "\n");
		if (strncmp(buf, "Valid until:", strlen("Valid until:")) == 0)
			validity = strtol(buf + strlen("Valid until:"), NULL, 10);

		if (strncmp(buf, "Type: ", strlen("Type: ")) == 0) {
			if (strncmp(buf + strlen("Type: "), "Kerberos", strlen("Kerberos")) == 0)
				cred_type = CRED_KRB5;
		}

		/* last line in buf is credential in base64 - will be read later */
	}

	if ((ret = pclose(fp))) {
		snprintf(log_buffer, LOG_BUF_SIZE, "%s command '%s' failed, exitcode: %d",
			ATTR_cred_renew_tool, cmd, WEXITSTATUS(ret));
		log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
			LOG_ERR, msg_daemonname, log_buffer);
		return NULL;
	}

	if (buf == NULL || strlen(buf) <= 1 || validity < time_now) {
		snprintf(log_buffer, LOG_BUF_SIZE, "%s command '%s' returned invalid credentials for %s",
			ATTR_cred_renew_tool, cmd,
			pjob->ji_wattr[(int)JOB_ATR_cred_id].at_val.at_str);
		log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
			LOG_ERR, msg_daemonname, log_buffer);

		return NULL;
	}

	if ((cred = (cred_cache *)malloc(sizeof(cred_cache))) == NULL) {
		log_err(errno, __func__, "Unable to allocate Memory!\n");
		return NULL;
	}

	strncpy(cred->credid, pjob->ji_wattr[(int)JOB_ATR_cred_id].at_val.at_str,
		strlen(pjob->ji_wattr[(int)JOB_ATR_cred_id].at_val.at_str));
	cred->type = cred_type;
	cred->validity = validity;
	cred->size = strlen(buf);

	if ((cred->data = (char *)malloc(cred->size + 1)) == NULL) {
		log_err(errno, __func__, "Unable to allocate Memory!\n");
		free(cred);
		return NULL;
	}

	/* here we read the credential in base64 */
	strcpy(cred->data, buf);

	/* store cred to cache */
	CLEAR_LINK(cred->cr_link);
	append_link(&svr_creds_cache, &cred->cr_link, cred);
	return cred;
#else
	return NULL;
#endif
}

/* @brief
 *	Prepare batch request structure for sending credentials to superior mom
 *	and fill in the structure with data like credentials or credid.
 *
 * @param[in] preq - batch request
 * @param[in] pjob - pointer to job
 *
 * @return	preq
 * @retval	structure with batch request
 */
static struct batch_request *
setup_cred(struct batch_request *preq, job  *pjob)
{
	cred_cache *cred;

	if (preq == NULL) {
		preq = alloc_br(PBS_BATCH_Cred);

		if (preq == NULL) {
			return preq;
		}
	}

	preq->rq_ind.rq_cred.rq_cred_data = NULL;

	if ((cred = get_cached_cred(pjob)) == NULL){
		free_br(preq);
		return NULL;
	}

	strcpy(preq->rq_ind.rq_cred.rq_credid, pjob->ji_wattr[(int)JOB_ATR_cred_id].at_val.at_str);
	strcpy(preq->rq_ind.rq_cred.rq_jobid, pjob->ji_qs.ji_jobid);
	preq->rq_ind.rq_cred.rq_cred_type = cred->type;
	preq->rq_ind.rq_cred.rq_cred_validity = cred->validity;
	if ((preq->rq_ind.rq_cred.rq_cred_data = (char *)malloc(cred->size + 1)) == NULL) {
		log_err(errno, __func__, "Unable to allocate Memory!\n");
		req_reject(PBSE_SYSTEM, 0, preq);
	}
	strcpy(preq->rq_ind.rq_cred.rq_cred_data, cred->data);

	return preq;
}

/* @brief
 *	Once the credentials are sent to to superior mom, this function is called
 *	and if the credentials were sent successfully then the job attribute
 *	JOB_ATR_cred_validity is changed to the validity of credentials.
 *
 * @param[in] pwt - pointer to work task
 *
 */
static void
post_cred(struct work_task *pwt)
{
	int		      code;
	job		     *pjob;
	struct batch_request *preq;

	preq = pwt->wt_parm1;
	code = preq->rq_reply.brp_code;
	pjob = find_job(preq->rq_ind.rq_cred.rq_jobid);

	if (pjob != NULL) {

		if (code != 0) {
			snprintf(log_buffer, LOG_BUF_SIZE, "sending credential to mom failed, returned code: %d", code);
			log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_JOB,
				LOG_INFO, pjob->ji_qs.ji_jobid, log_buffer);
		} else {
			/* send_cred was successful  - update validity*/

			pjob->ji_wattr[(int) JOB_ATR_cred_validity].at_val.at_long = preq->rq_ind.rq_cred.rq_cred_validity;
			pjob->ji_wattr[(int) JOB_ATR_cred_validity].at_flags |= ATR_VFLAG_SET | ATR_VFLAG_MODCACHE;
			pjob->ji_modified = 1;
			/* save the full job */
			(void)job_save(pjob, SAVEJOB_FULL);

			log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_JOB, LOG_INFO, pjob->ji_qs.ji_jobid,
				"sending credential to mom succeed");
		}
	} else {
		log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_JOB, LOG_INFO, __func__, "failed, job unknown");
	}

	release_req(pwt);	/* close connection and release request */
}

/* @brief
 *	Retrieve and send credentials for a particular job to the superior
 *	mom of the job.
 *
 * @param[in] pjob - pointer to job
 *
 * @return	int
 * @retval	0 on success
 * @retval	!= 0 on error
 */
int
send_cred(job *pjob)
{
	struct batch_request *credreq = NULL;
	int		      rc;

	if (pjob == NULL) {
	    return PBSE_SYSTEM;
	}

	credreq = setup_cred(credreq, pjob);
	if (credreq) {
		rc = relay_to_mom(pjob, credreq, post_cred);
		if (rc == PBSE_NORELYMOM) /* otherwise the post_cred will free the request */
			free_br(credreq);
		return rc;
	}

	return PBSE_IVALREQ;
}
