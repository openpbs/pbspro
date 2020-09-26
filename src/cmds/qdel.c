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

/**
 * @file	qdel.c
 * @brief
 * 	qdel - (PBS) delete batch job
 *
 * @author  Terry Heidelberg
 * 			Livermore Computing
 *
 * @author  Bruce Kelly
 * 			National Energy Research Supercomputer Center
 *
 * @author  Lawrence Livermore National Laboratory
 * 			University of California
 */

#include <unistd.h>
#include "cmds.h"
#include "pbs_ifl.h"
#include <pbs_config.h>   /* the master config generated by configure */
#include <pbs_version.h>

#define no_of_jobids 1000

typedef struct {
	int max_sz;
	int job_ct;
	char *server_name;
	char **jobids;
} pbs_svr_t;

int no_of_pbsservers;

void
append_jobid(pbs_svr_t *svr, char *jobid)
{
	if (svr->max_sz == 0) {
		svr->jobids = calloc(no_of_jobids + 1, PBS_MAXCLTJOBID + 1);
		if (svr->jobids == NULL) {
			fprintf(stderr, "qdel: unable to allocate memory.\n");
			exit(2);
		}
		svr->max_sz = no_of_jobids;
	} else if (svr->job_ct >= svr->max_sz) {
		svr->max_sz *= 2;
		svr->jobids = realloc(&svr->jobids, svr->max_sz);
		if (svr->jobids == NULL) {
			fprintf(stderr, "qdel: unable to allocate memory.\n");
			exit(2);
		}
	}
	
	svr->jobids[svr->job_ct++] = jobid;
}

int
add_jobid_to_pbs_servers(char *job_id, char *server_out, pbs_svr_t** pbs_servers)
{
	int i;
	int found = 0;
	
tryagain:
	for (i = 0; i < no_of_pbsservers; i++) {
		if (pbs_servers[i]->server_name == NULL) {
			pbs_servers[i]->server_name = strdup(server_out);
			if (pbs_servers[i]->server_name == NULL) {
				fprintf(stderr, "qdel: unable to allocate memory.\n");
				exit(2);
			}
			append_jobid(pbs_servers[i], job_id);
			found = 1;
			break;
		} else if (strcmp(server_out, pbs_servers[i]->server_name) == 0) {
			append_jobid(pbs_servers[i], job_id);
			found = 1;
			break;
		}
	}
	
	if (!found) {
		no_of_pbsservers *= 2;
		pbs_servers = realloc(&pbs_servers, no_of_pbsservers);
		if (pbs_servers == NULL) {
			fprintf(stderr, "qdel: unable to allocate memory.\n");
			exit(2);
		}
		goto tryagain;
	}
	return 0;
}


int
main(argc, argv, envp) /* qdel */
int argc;
char **argv;
char **envp;
{
	int c;
	int errflg=0;
	int any_failed=0;
	char *pc;

	int forcedel = FALSE;
	int deletehist = FALSE;
	int mails_suppressed = FALSE;


	char job_id[PBS_MAXCLTJOBID];	/* from the command line */

	char job_id_out[PBS_MAXCLTJOBID];
	char server_out[MAXSERVERNAME];
	char rmt_server[MAXSERVERNAME];

	char *keystr, *valuestr;
	int dfltmail = 0;
	int dfltmailflg = FALSE;
	int mails;				/* number of emails we can send */
	int num_deleted = 0;
	struct attrl *attr;
	struct batch_status *ss = NULL;
	char *errmsg;
	pbs_svr_t *pbs_servers;
	int i = 0;
	pbs_svr_t *pbs_svr;

#define MAX_TIME_DELAY_LEN 32
	/* -W no longer supports a time delay */
	/* max length is "nomailforcedeletehist" plus terminating '\0' */
	char warg[MAX_TIME_DELAY_LEN+1];
	char warg1[MAX_TIME_DELAY_LEN+7];

#define GETOPT_ARGS "W:x"
#define MAX_JOBS_LIST 1000

	/*test for real deal or just version and exit*/

	PRINT_VERSION_AND_EXIT(argc, argv);

	if (initsocketlib())
		return 1;

	warg[0]='\0';
	strcpy(warg1, NOMAIL);
	while ((c = getopt(argc, argv, GETOPT_ARGS)) != EOF) {
		switch (c) {
			case 'W':
				pc = optarg;
				if (strlen(pc) == 0) {
					fprintf(stderr, "qdel: illegal -W value\n");
					errflg++;
					break;
				}
				if (strcmp(pc, FORCE) == 0) {
					forcedel = TRUE;
					break;
				}
				if (parse_equal_string(optarg, &keystr, &valuestr)) {
					if (strcmp(keystr, SUPPRESS_EMAIL) == 0) {
						dfltmail = atol(valuestr);
						dfltmailflg = TRUE;
						break;
					}
				}

				while (*pc != '\0') {
					if (! isdigit(*pc)) {
						fprintf(stderr, "qdel: illegal -W value\n");
						errflg++;
						break;
					}
					pc++;
				}
				break;
			case  'x' :
				deletehist = TRUE;
				break;
			default :
				errflg++;
		}
	}

	if (errflg || optind >= argc) {
		static char usage[] =
			"usage:\n"
		"\tqdel [-W force|suppress_email=X] [-x] job_identifier...\n"
		"\tqdel --version\n";
		fprintf(stderr, "%s", usage);
		exit(2);
	}

	if (forcedel && deletehist)
		snprintf(warg, sizeof(warg), "%s%s", FORCE, DELETEHISTORY);
	else if (forcedel)
		strcpy(warg, FORCE);
	else if (deletehist)
		strcpy(warg, DELETEHISTORY);

	/*perform needed security library initializations (including none)*/

	if (CS_client_init() != CS_SUCCESS) {
		fprintf(stderr, "qdel: unable to initialize security library.\n");
		exit(1);
	}
	
	/* allocate enough memory to store array of server instances */
	no_of_pbsservers = 10;
	pbs_servers = (pbs_svr_t *)calloc(no_of_pbsservers, sizeof(pbs_svr_t));
	if (pbs_servers == NULL) {
		fprintf(stderr, "qdel: unable to allocate memory.\n");
		exit(2);
	}
		
	for (; optind < argc; optind++) {
		pbs_strncpy(job_id, argv[optind], sizeof(job_id));
		if (get_server(job_id, job_id_out, server_out)) {
			fprintf(stderr, "qdel: illegally formed job identifier: %s\n", job_id);
			any_failed = 1;
			continue;
		}
		if (server_out[0] == '\0')
			strcpy(server_out, pbs_default());
		add_jobid_to_pbs_servers(argv[optind], server_out, &pbs_servers);
	}

	while((pbs_svr = &pbs_servers[i++]) && pbs_svr->jobids != NULL) {
		int connect;
		struct batch_deljob_status *p_delstatus;
		
//cnt:
		connect = cnt2server(pbs_svr->server_name);
		if (connect <= 0) {
			fprintf(stderr, "qdel: cannot connect to server %s (errno=%d)\n",
				pbs_server, pbs_errno);
			any_failed = pbs_errno;
			continue;
		}

		/* retrieve default: suppress_email from server: default_qdel_arguments */
		if (dfltmailflg == FALSE) {
			ss = pbs_statserver(connect, NULL, NULL);
			if (ss == NULL && pbs_errno != PBSE_NONE) {
				any_failed = pbs_errno;
				if ((errmsg = pbs_geterrmsg(connect)) != NULL)
					fprintf(stderr, "qdel: %s\n", errmsg);
				else
					fprintf(stderr, "qdel: Error %d\n", pbs_errno);
				break;
			}

			while (ss != NULL && dfltmailflg != TRUE) {
				attr = ss->attribs;
				while (attr != NULL) {
					if (strcmp(attr->name, ATTR_dfltqdelargs) == 0) {
						if (attr->value != NULL && dfltmailflg != TRUE) {
							if (parse_equal_string(attr->value, &keystr, &valuestr)) {
								if (strcmp(keystr, "-Wsuppress_email") == 0) {
									dfltmail = atol(valuestr);
									dfltmailflg = TRUE;
								}
								else {
									fprintf(stderr,
										"qdel: unsupported %s \'%s\'\n",
										attr->name, attr->value);
								}
							}
						}
					}
					attr = attr->next;
				}
				ss = ss->next;
			}
		}

		/* when jobs to be deleted over 1000, mail function is disabled
		 * by sending the flag below to server via its extend field:
		 *   "" -- delete a job with a mail
		 *   "nomail" -- delete a job without sending a mail
		 *   "force" -- force job to be deleted with a mail
		 *   "nomailforce" -- force job to be deleted without sending a mail
		 *   "nomaildeletehist" -- delete history of a job without sending mail
		 *   "nomailforcedeletehist" -- force delete history of a job without sending mail.
		 */
		mails = dfltmail ? dfltmail : 1000;
		if (num_deleted >= mails && !mails_suppressed) {
			mails_suppressed = TRUE;
			/* current warg1 "nomail" should be at start */
			strcat(warg1, warg);
			pbs_strncpy(warg, warg1, sizeof(warg));
		}

		p_delstatus = pbs_deljoblist(connect, pbs_svr->jobids, warg);
		
		while (p_delstatus != NULL) {
			if ((pbse_to_txt(p_delstatus->code) != NULL) && (p_delstatus->code != PBSE_HISTJOBDELETED)) {
				fprintf(stderr, "%s: %s %s\n", "qdel", pbse_to_txt(p_delstatus->code), p_delstatus->name);
				any_failed = p_delstatus->code;
			}

			if (p_delstatus->code != PBSE_HISTJOBDELETED)
				num_deleted++;
			
			if (p_delstatus->code == PBSE_UNKJOBID) {
				if (locate_job(p_delstatus->name, server_out, rmt_server))
					add_jobid_to_pbs_servers(rmt_server, rmt_server, &pbs_servers);
			}
			
			p_delstatus = p_delstatus->next;
		}

		pbs_disconnect(connect);
	}

	/*cleanup security library initializations before exiting*/
	CS_close_app();

	exit(any_failed);
}
