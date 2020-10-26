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
 * @file	qterm.c
 * @brief
 *  The qterm command terminates the batch server.
 *
 * @par	Synopsis:
 *  qterm [-t type] [-F|-f|-i] [-s] [-m] [server ...]
 *
 * @par	Options:
 *  -t  delay   Jobs are (1) checkpointed if possible; otherwise, (2) jobs are
 *		rerun (requeued) if possible; otherwise, (3) jobs are left to
 *              run.
 *
 *      immediate
 *              Jobs are (1) checkpointed if possible; otherwise, (2) jobs are
 *		rerun if possible; otherwise, (3) jobs are aborted.
 *
 *	quick (the new default)
 *		The server will save state and exit leaving running jobs
 *		still running.  Good for shutting down when you wish to
 *		quickly restart the server.
 *
 *  -F	shutdonw the Secndary Server only (Primary stays up),
 *  -f  shutdown Secondary Servers as well, or
 *  -i	idle the Secondary Server
 *
 *  -s	shutdown scheduler as well
 *
 *  -m  shutdown Moms also
 *
 * @par	Arguments:
 *  server ...
 *      A list of servers to terminate.
 *
 * @author	Bruce Kelly
 *  National Energy Research Supercomputer Center
 *  Livermore, CA
 *  May, 1993
 */

#include "cmds.h"
#include <pbs_config.h>   /* the master config generated by configure */
#include <pbs_version.h>


int exitstatus = 0; /* Exit Status */

static void execute(int, char *);

int
main(int argc, char **argv)
{
	/*
	 *  This routine sends a Server Shutdown request to the batch server.  If the
	 * batch request is accepted, and the type is IMMEDIATE, then no more jobs
	 * are accepted and all jobs are checkpointed or killed.  If the type is
	 * DELAY, then only privileged users can submit jobs, and jobs will be
	 * checkpointed if available.
	 */

	static char opts[] = "t:sm";     /* See man getopt */
	int s;                  /* The execute line option */
	static char usage[] = "Usage: qterm [-t immediate|delay|[quick]] [-m] [-s] [server ...]\n";
	static char usag2[] = "       qterm --version\n";
	char *type = NULL;      /* Pointer to the type of termination */
	int   downsched = 0;
	int   downmom   = 0;
	int   idlesecd  = 0;
	int manner;             /* The type of termination */
	int errflg = 0;         /* Error flag */

	/*test for real deal or just version and exit*/

	PRINT_VERSION_AND_EXIT(argc, argv);

	if (initsocketlib())
		return 1;

	/* Command line options */
	while ((s = getopt(argc, argv, opts)) != EOF)
		switch (s) {
			case 't':
				type = optarg;
				break;
			case 's':
				downsched = 1;
				break;
			case 'm':
				if (idlesecd == 2)
					errflg++;
				downmom = 1;
				break;
			case '?':
			default:
				errflg++;
				break;
		}
	if (errflg) {
		fprintf(stderr, "%s", usage);
		fprintf(stderr, "%s", usag2);
		exit(1);
	} else if (type != NULL) {
		if (strcmp(type, "delay") == 0)
			manner = SHUT_DELAY;
		else if (strcmp(type, "immediate") == 0)
			manner = SHUT_IMMEDIATE;
		else if (strcmp(type, "quick") == 0)
			manner = SHUT_QUICK;
		else {
			fprintf(stderr, "%s", usage);
			fprintf(stderr, "%s", usag2);
			exit(1);
		}
	} else
		manner = SHUT_QUICK;

	if (downsched)
		manner |= SHUT_WHO_SCHED;
	if (downmom)
		manner |= SHUT_WHO_MOM;

	/*perform needed security library initializations (including none)*/

	if (CS_client_init() != CS_SUCCESS) {
		fprintf(stderr, "qterm: unable to initialize security library.\n");
		exit(1);
	}

	if (optind < argc)
		for (; optind < argc; optind++)
			execute(manner, argv[optind]);
	else
		execute(manner, "");

	/*cleanup security library initializations before exiting*/
	CS_close_app();

	exit(exitstatus);
}


/**
 * @brief
 *	executes to terminate server
 *
 * @param[in] manner - The manner in which to terminate the server.
 * @param[in] server - The name of the server to terminate.
 *
 * @return - Void
 *
 * @File Variables:
 * exitstatus  Set to two if an error occurs.
 *
 */
static void
execute(int manner, char *server)
{
	int ct;         /* Connection to the server */
	int err;        /* Error return from pbs_terminate */
	char *errmsg;   /* Error message from pbs_terminate */

	if ((ct = cnt2server(server)) > 0) {
		err = pbs_terminate(ct, manner, NULL);
		if (err != 0) {
			errmsg = pbs_geterrmsg(ct);
			if (errmsg != NULL) {
				fprintf(stderr, "qterm: %s ", errmsg);
			} else {
				fprintf(stderr, "qterm: Error (%d) terminating server ", pbs_errno);
			}
			fprintf(stderr, "%s\n", server);
			exitstatus = 2;
		}
		pbs_disconnect(ct);
	} else {
		fprintf(stderr, "qterm: could not connect to server %s (%d)\n", server, pbs_errno);
		exitstatus = 2;
	}
}
