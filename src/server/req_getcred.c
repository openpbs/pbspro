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
 * @file	req_getcred.c
 *
 * This file contains function relating to the PBS credential system,
 * it includes the major functions:
 *   req_connect    - validate the credential in a Connection Request (old)
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include "libpbs.h"
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "credential.h"
#include "net_connect.h"
#include "batch_request.h"
#include "pbs_share.h"
#include "log.h"

/**
 * @brief
 * 		req_connect - process a Connection Request
 * 		Almost does nothing.
 *
 * @param[in]	preq	- Connection Request
 */

void
req_connect(struct batch_request *preq)
{
	conn_t *conn = get_conn(preq->rq_conn);

	if (!conn) {
		req_reject(PBSE_SYSTEM, 0, preq);
		return;
	}

	if (preq->rq_extend != NULL) {
		if (strcmp(preq->rq_extend, QSUB_DAEMON) == 0)
			conn->cn_authen |= PBS_NET_CONN_FROM_QSUB_DAEMON;
		else if (strstr(preq->rq_extend, ATTR_security_context) == preq->rq_extend) {
			/*
			 * Skip the ATTR_security_context + "=" prefix.
			 * Note that using sizeof() rather than strlen()
			 * skips the '=' as well.
			 */
			conn->cn_security_context =
				strdup(preq->rq_extend + sizeof(ATTR_security_context));
			if (conn->cn_security_context == NULL) {
				log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_ERR,
					preq->rq_ind.rq_cpyfile.rq_jobid,
					"could not save security context");
				req_reject(PBSE_SYSTEM, errno, preq);
				return;
			} else {
				log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_REQUEST, LOG_INFO,
					"saved security context",
					preq->rq_extend);
			}
		}
		
	}

	reply_ack(preq);
}
