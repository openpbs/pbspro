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

#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <assert.h>
#include <ctype.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pbs_ifl.h>
#include "list_link.h"
#include "attribute.h"
#include "resource.h"
#include "server_limits.h"
#include "job.h"
#include "pbs_error.h"
#include "libutil.h"
#include "pbs_share.h"


/**
 * @file	attr_fn_f.c
 * @brief
 * This file contains functions for manipulating attributes of type float
 * @details
 * Each set has functions for:
 *	Decoding the value string to the machine representation.
 *	Encoding the internal attribute to external form
 *	Setting the value by =, + or - operators.
 *	Comparing a (decoded) value with the attribute value.
 *
 * Some or all of the functions for an attribute type may be shared with
 * other attribute types.
 *
 * The prototypes are declared in "attribute.h"
 *
 * --------------------------------------------------
 * The Set of Attribute Functions for attributes with
 * value type "float"
 * --------------------------------------------------
 */


/**
 * @brief
 * 	decode_f - decode float into attribute structure
 *
 *
 * @param[in] patr - ptr to attribute to decode
 * @param[in] name - attribute name
 * @param[in] rescn - resource name or null
 * @param[out] val - string holding values for attribute structure
 *
 * @retval      int
 * @retval      0       if ok
 * @retval      >0      error number1 if error,
 * @retval      *patr   members set
 *
 */

int
decode_f(struct attribute *patr, char *name, char *rescn, char *val)
{
	size_t	len;
	if ((val != NULL) && ((len = strlen(val)) != 0)) {
		char	*end;
		float	fval;

		errno = 0;
		/*
		 * The function strtof cannot be used because on some machines
		 * it is only available in C99 mode.  Use strtod instead.
		 * @see https://lists.debian.org/debian-glibc/2004/02/msg00176.html
		 */
		fval = (float)strtod(val, &end);
		/* if any part of val is not converted or errno set, error */
		if (&val[len] != end || errno != 0)
			return (PBSE_BADATVAL);	 /* invalid string */
		patr->at_flags |= ATR_SET_MOD_MCACHE;
		patr->at_val.at_float = fval;
	} else {
		patr->at_flags = (patr->at_flags & ~ATR_VFLAG_SET) | ATR_MOD_MCACHE;
		patr->at_val.at_float = 0.0;
	}
	return (0);
}

/**
 * @brief
 *	encode attribute of type float into attr_extern
 *
 * @param[in] attr - ptr to attribute
 * @param[in] phead - head of attrlist list
 * @param[in] atname - attribute name
 * @param[in] rsname - resource name or null
 * @param[in] mode - encode mode, unused here
 * @param[out] rtnl - Return: ptr to svrattrl
 *
 * @return int
 * @retval >0 if ok
 * @retval =0 if no value, no attrlist link added
 * @retval <0 if error
 *
 */

/*ARGSUSED*/

#define CVNBUFSZ 32

int
encode_f(const attribute *attr, pbs_list_head *phead, char *atname, char *rsname, int mode, svrattrl **rtnl)
{
	size_t	  ct;
	char	  cvnbuf[CVNBUFSZ];
	svrattrl *pal;

	if (!attr)
		return (-1);
	if (!(attr->at_flags & ATR_VFLAG_SET))
		return (0);

	(void)snprintf(cvnbuf, CVNBUFSZ, "%-.*f",
		float_digits(attr->at_val.at_float, FLOAT_NUM_DIGITS),
		attr->at_val.at_float);
	ct = strlen(cvnbuf) + 1;

	pal = attrlist_create(atname, rsname, ct);
	if (pal == NULL)
		return (-1);

	(void)memcpy(pal->al_value, cvnbuf, ct);
	pal->al_flags = attr->at_flags;
	if (phead)
		append_link(phead, &pal->al_link, pal);
	if (rtnl)
		*rtnl = pal;

	return (1);
}

/**
 * @brief
 * 	set_f - set attribute A to attribute B,
 *	either A=B, A += B, or A -= B
 *
 * @param[in]   attr - pointer to new attribute to be set (A)
 * @param[in]   new  - pointer to attribute (B)
 * @param[in]   op   - operator
 *
 * @return      int
 * @retval      0       if ok
 * @retval     >0       if error
 *
 */

int
set_f(struct attribute *attr, struct attribute *new, enum batch_op op)
{
	assert(attr && new && (new->at_flags & ATR_VFLAG_SET));

	switch (op) {
		case SET:	attr->at_val.at_float = new->at_val.at_float;
			break;

		case INCR:	attr->at_val.at_float += new->at_val.at_float;
			break;

		case DECR:	attr->at_val.at_float -= new->at_val.at_float;
			break;

		default:	return (PBSE_INTERNAL);
	}
	attr->at_flags |= ATR_SET_MOD_MCACHE;
	return (0);
}

/**
 * @brief
 * 	comp_f - compare two attributes of type float
 *
 * @param[in] attr - pointer to attribute structure
 * @param[in] with - pointer to attribute structure
 *
 * @return      int
 * @retval      0       if the set of strings in "with" is a subset of "attr"
 * @retval      1       otherwise
 *
 */

int
comp_f(struct attribute *attr, struct attribute *with)
{
	if (!attr || !with)
		return (-1);
	if (attr->at_val.at_float < with->at_val.at_float)
		return (-1);
	else if (attr->at_val.at_float >  with->at_val.at_float)
		return (1);
	else
		return (0);
}

/*
 * free_f - use free_null to (not) free space
 */
