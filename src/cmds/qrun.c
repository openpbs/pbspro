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
 * @file	qrun.c
 * @brief
 *  	The qrun command forces a batch job to run.
 *
 * @par	Synopsis:
 *  	qrun [-H host][-a] job_identifier ...
 *
 * @par	Arguments:
 *  host
 *      The host to run the job at.
 *  job_identifier ...
 *      A list of job_identifiers.  A job_identifier has the following form:
 *          sequence_number[.server_name][@server]
 *
 *  @author	Bruce Kelly
 *  		National Energy Research Supercomputer Center
 *  		Livermore, CA
 *  		May, 1993
 */

#include "cmds.h"
#include "pbs_ifl.h"
#include <pbs_config.h>   /* the master config generated by configure */
#include <pbs_version.h>
#include <pbs_share.h>    /* the comment buffer size declaration*/
#include "grunt.h"


int exitstatus = 0; /* Exit Status */
int async = 0;
static void execute(char *, char *, char *);



int
main(int argc, char **argv)
{
	/*
	 *  This routine sends a Run Job request to the batch server.  If the
	 * batch request is accepted, the server will have started the execution
	 * of the job.
	 */

	char job[PBS_MAXCLTJOBID];      /* Job Id */
	char server[MAXSERVERNAME];	    /* Server name */
	char *location = NULL;          /* Where to run the job */

	static char opts[] = "H:a";     /* See man getopt */
	static char *usage = "Usage: qrun [-a] [-H vnode_specification ] job_identifier_list\n"
			     "       qrun [-a] [-H - ] job_identifier_list\n"
			     "       qrun --version\n";
	int s;
	int errflg = 0;

	/*test for real deal or just version and exit*/

	PRINT_VERSION_AND_EXIT(argc, argv);

	if (initsocketlib())
		return 1;

	/* Command line options */
	while ((s = getopt(argc, argv, opts)) != EOF)
		switch (s) {

			case 'H':
				if (strlen(optarg) == 0) {
					fprintf(stderr, "qrun: illegal -H value\n");
					errflg++;
					break;
				}
				location = optarg;
				break;

			case 'a':
				async = 1;
				break;

			case '?':
			default:
				errflg++;
				break;
		}

	if (errflg || (optind >= argc)) {
		fprintf(stderr, "%s", usage);
		exit(1);
	}

	/*perform needed security library initializations (including none)*/

	if (CS_client_init() != CS_SUCCESS) {
		fprintf(stderr, "qrun: unable to initialize security library.\n");
		exit(2);
	}

	for (; optind < argc; optind++) {
		if (get_server(argv[optind], job, server)) {
			fprintf(stderr,
				"qrun: illegally formed job identifier: %s\n", argv[optind]);
			exitstatus = 1;
			continue;
		}
		execute(job, server, location);
	}

	/*cleanup security library initializations before exiting*/
	CS_close_app();

	exit(exitstatus);
}

/**
 * @brief
 * 	executes a job
 *
 * @param[in] job - The fully qualified job id.
 * @param[in] server - The name of the server that manages the job.
 * @param[in] location -  location indicating where to run job
 *
 * @return - Void
 *
 * @File Variables:
 *  exitstatus  Set to two if an error occurs.
 *
 */
static void
execute(char *job, char *server, char *location)
{
	int ct;         /* Connection to the server */
	int err;        /* Error return from pbs_run */
        int out;        /* Stores the size of err_msg_buf*/
	int located = FALSE;
	char *errmsg;
	char err_msg_buf[COMMENT_BUF_SIZE] = {'\0'};	/* generic buffer - comments & logging*/
        char rmt_server[MAXSERVERNAME];

cnt:
	if ((ct = cnt2server(server)) > 0) {

		show_svr_inst_fail(ct, "qrun");
		if (async)
			err = pbs_asyrunjob(ct, job, location, NULL);
		else
			err = pbs_runjob(ct, job, location, NULL);

		if (err && (pbs_errno != PBSE_UNKJOBID)) {
			errmsg = pbs_geterrmsg(ct);
			if (errmsg != NULL) {
				if (pbs_errno == PBSE_UNKNODE) {
					out = snprintf(err_msg_buf, sizeof(err_msg_buf),"qrun: %s %s",errmsg, location);
                                        if (out >= sizeof(err_msg_buf)) {
                                                    fprintf(stderr,"%s...\n", err_msg_buf);
                                                } else {
                                                    fprintf(stderr, "%s\n", err_msg_buf);
                                                }

				} else {
					prt_job_err("qrun", ct, job);
				}
			} else {
				fprintf(stderr, "qrun : Server returned error %d for job\n", pbs_errno);
			}
			exitstatus = 2;
		} else if (err && (pbs_errno == PBSE_UNKJOBID) && !located) {
			located = TRUE;
			if (locate_job(job, server, rmt_server)) {
				pbs_disconnect(ct);
				strcpy(server, rmt_server);
				goto cnt;
			}
			prt_job_err("qrun", ct, job);
			exitstatus = 2;
		}
		pbs_disconnect(ct);
	} else {
		fprintf(stderr,
			"qrun: could not connect to server %s (%d)\n", server, pbs_errno);
		exitstatus = 2;
	}
}
