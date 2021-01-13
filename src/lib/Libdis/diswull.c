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
 * @file	diswull.c
 *
 * @par Synopsis:
 *	int diswull(int stream, u_Long value)
 *
 *	Converts <value> into a Data-is-Strings unsigned integer and sends it to
 *	<stream>.
 *
 *	This format for character strings representing unsigned integers can
 *	best be understood through the decoding algorithm:
 *
 *	1. Initialize the digit count to 1.
 *
 *	2. Read the next character; if it is a plus sign, go to step (4); if it
 *	   is a minus sign, post an error.
 *
 *	3. Decode a new count from the digit decoded in step (2) and the next
 *	   count - 1 digits; repeat step (2).
 *
 *	4. Decode the next count digits as the unsigned integer.
 *
 *	Returns DIS_SUCCESS if everything works well.  Returns an error code
 *	otherwise.  In case of an error, no characters are sent to <stream>.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "dis.h"
#include "dis_.h"

/**
 * @brief
 *  Function to convert a numeric value of data type u_Long to string
 *  form and  writes over network to send that string data from mom to
 *  PBS server
 *
 * @param[in] stream    socket fd
 * @param[in] value     value to be converted
 *
 * @return      int
 * @retval      DIS_SUCCESS     success
 * @retval      error code      error
 *
 */

int
diswull(int stream, u_Long value)
{
	int		retval;
	unsigned	ndigs;
	char		*cp;

	assert(stream >= 0);


	cp = discull_(&dis_buffer[DIS_BUFSIZ], value, &ndigs);
	*--cp = '+';
	while (ndigs > 1)
		cp = discui_(cp, ndigs, &ndigs);
	retval = dis_puts(stream, cp,
		(size_t)(&dis_buffer[DIS_BUFSIZ] - cp)) < 0 ?
		DIS_PROTO : DIS_SUCCESS;
	return retval;
}
