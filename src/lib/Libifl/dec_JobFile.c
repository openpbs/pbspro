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
 * @file	dec_JobFile.c
 * @brief
 * decode_DIS_JobFile() - decode a Job Related Job File Move request
 *
 *	Data items are:
 *		      :	u int	block sequence number\n
 *		      :	u int	file type (stdout, stderr, ...)\n
 *		      :	u int	size of data in block\n
 *		      :	string	job id\n
 *		      :	cnt str	data\n
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <stdlib.h>
#include "libpbs.h"
#include "list_link.h"
#include "server_limits.h"
#include "attribute.h"
#include "credential.h"
#include "batch_request.h"
#include "dis.h"

/**
 * @brief -
 *	decode a Job Related Job File Move request
 *
 * @param[in] sock - socket descriptor
 * @param[out] preq - pointer to batch_request structure
 *
 *
 * @par Data items are:
 *			u int -	block sequence number\n
 *		 	u int -  file type (stdout, stderr, ...)\n
 *		 	u int -  size of data in block\n
 *		 	string - job id\n
 *		 	cnt str - data\n
 *
 * @return      int
 * @retval      DIS_SUCCESS(0)  success
 * @retval      error code      error
 *
 */

int
decode_DIS_JobFile(int sock, struct batch_request *preq)
{
	int   rc;
	size_t amt;

	preq->rq_ind.rq_jobfile.rq_data = 0;

	preq->rq_ind.rq_jobfile.rq_sequence = disrui(sock, &rc);
	if (rc) return rc;

	preq->rq_ind.rq_jobfile.rq_type = disrui(sock, &rc);
	if (rc) return rc;

	preq->rq_ind.rq_jobfile.rq_size = disrui(sock, &rc);
	if (rc) return rc;

	if ((rc = disrfst(sock, PBS_MAXSVRJOBID+1, preq->rq_ind.rq_jobfile.rq_jobid)) != 0)
		return rc;

	preq->rq_ind.rq_jobfile.rq_data = disrcs(sock, &amt, &rc);
	if ((amt != preq->rq_ind.rq_jobfile.rq_size) && (rc == 0))
		rc = DIS_EOD;
	if (rc) {
		if (preq->rq_ind.rq_jobfile.rq_data)
			(void)free(preq->rq_ind.rq_jobfile.rq_data);
		preq->rq_ind.rq_jobfile.rq_data = 0;
	}

	return rc;
}
