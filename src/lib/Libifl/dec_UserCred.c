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
 * @file	dec_UserCred.c
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include "libpbs.h"
#include "linked_list.h"
#include "server_limits.h"
#include "attribute.h"
#include "credential.h"
#include "batch_request.h"
#include "dis.h"

/**
 * @brief- 
 *	decode a User Credential batch request
 *
 * @par	NOTE:
 *	The batch_request structure must already exist (be allocated by the
 *	caller.   It is assumed that the header fields (protocol type,
 *	protocol version, request type, and user name) have already be decoded.
 *
 * @par	Data items are:\n
 *		string          user whose credential is being set\n
 *		unsigned int	credential type\n
 *		counted string	the credential data
 *
 * @param[in] sock - socket descriptor
 * @param[out] preq - pointer to batch_request structure
 *
 * @return      int
 * @retval      DIS_SUCCESS(0)  success
 * @retval      error code      error
 *
 */

int
decode_DIS_UserCred(int sock, struct batch_request *preq)
{
	int rc;

	rc = disrfst(sock, PBS_MAXUSER+1, preq->rq_ind.rq_usercred.rq_user);
	if (rc) return rc;

	preq->rq_ind.rq_usercred.rq_type = disrui(sock, &rc);
	if (rc) return rc;

	preq->rq_ind.rq_usercred.rq_data = 0;
	preq->rq_ind.rq_usercred.rq_data = disrcs(sock,
		(size_t *)&preq->rq_ind.rq_usercred.rq_size,
		&rc);
	return rc;
}

/**
 * @brief-
 * 	decode a User Migrate batch request
 *
 * @par	NOTE:
 *	The batch_request structure must already exist (be allocated by the
 *	caller.   It is assumed that the header fields (protocol type,
 *	protocol version, request type, and user name) have already be decoded.
 *
 * @par	Data items are:\n
 *		string         the destination host to migrate users to
 *
 *
 * @param[in] sock - socket descriptor
 * @param[out] preq - pointer to batch_request structure
 *
 * @return      int
 * @retval      DIS_SUCCESS(0)  success
 * @retval      error code      error
 *
 */

int
decode_DIS_UserMigrate(int sock, struct batch_request *preq)
{
	int rc;

	rc = disrfst(sock, PBS_MAXHOSTNAME+1,
		preq->rq_ind.rq_user_migrate.rq_tohost);

	return rc;
}
