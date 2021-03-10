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

#include <pbs_config.h>   /* the master config generated by configure */

#include <assert.h>
#include <ctype.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pbs_ifl.h>
#include "list_link.h"
#include "attribute.h"
#include "pbs_error.h"


/**
 * @file	attr_fn_ll.c
 * @brief
 * 	This file contains functions for manipulating attributes of type
 *	Long integer, where "Long" is defined as the largest integer
 *	available.
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
 * value type "Long" (_ll)
 * --------------------------------------------------
 */

#define CVNBUFSZ 23

/**
 * @brief
 * 	decode_ll - decode Long integer into attribute structure
 *	Unlike decode_long, this function will decode octal (leading zero) and
 *	hex (leading 0x or 0X) data as well as decimal
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
decode_ll(attribute *patr, char *name, char *rescn, char *val)
{
	char *pc;

	if ((val != NULL) && (strlen(val) != 0)) {

		patr->at_val.at_ll = (Long)strTouL(val, &pc, 0);
		if (*pc != '\0')
			return (PBSE_BADATVAL);	 /* invalid string */
		post_attr_set(patr);
	} else {
		ATR_UNSET(patr);
		patr->at_val.at_ll = 0;
	}
	return (0);
}

/**
 * @brief
 * 	encode_ll - encode attribute of type Long into attr_extern
 *
 * @param[in] attr - ptr to attribute to encode
 * @param[in] phead - ptr to head of attrlist list
 * @param[in] atname - attribute name
 * @param[in] rsname - resource name or null
 * @param[in] mode - encode mode
 * @param[out] rtnl - ptr to svrattrl
 *
 * @retval      int
 * @retval      >0      if ok, entry created and linked into list
 * @retval      =0      no value to encode, entry not created
 * @retval      -1      if error
 *
 */
/*ARGSUSED*/


int
encode_ll(const attribute *attr, pbs_list_head *phead, char *atname, char *rsname, int mode, svrattrl **rtnl)
{
	size_t	  ct;
	const char *cvn;
	svrattrl *pal;

	if (!attr)
		return (-1);
	if (!(attr->at_flags & ATR_VFLAG_SET))
		return (0);

	cvn = uLTostr(attr->at_val.at_ll, 10);
	ct = strlen(cvn) + 1;

	pal = attrlist_create(atname, rsname, ct);
	if (pal == NULL)
		return (-1);

	(void)memcpy(pal->al_value, cvn, ct);
	pal->al_flags = attr->at_flags;
	if (phead)
		append_link(phead, &pal->al_link, pal);
	if (rtnl)
		*rtnl = pal;

	return (1);
}

/**
 * @brief
 * 	set_ll - set attribute A to attribute B,
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
set_ll(attribute *attr, attribute *new, enum batch_op op)
{
	assert(attr && new && (new->at_flags & ATR_VFLAG_SET));

	switch (op) {
		case SET:	attr->at_val.at_ll = new->at_val.at_ll;
			break;

		case INCR:	attr->at_val.at_ll += new->at_val.at_ll;
			break;

		case DECR:	attr->at_val.at_ll -= new->at_val.at_ll;
			break;

		default:	return (PBSE_INTERNAL);
	}
	post_attr_set(attr);
	return (0);
}

/**
 * @brief
 * 	comp_ll - compare two attributes of type Long
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
comp_ll(attribute *attr, attribute *with)
{
	if (!attr || !with)
		return (-1);
	if (attr->at_val.at_ll < with->at_val.at_ll)
		return (-1);
	else if (attr->at_val.at_ll >  with->at_val.at_ll)
		return (1);
	else
		return (0);
}

/**
 * @brief	Attribute setter function for long type values
 *
 * @param[in]	pattr	-	pointer to attribute being set
 * @param[in]	value	-	value to be set
 * @param[in]	op		-	operation to do
 *
 * @return	void
 *
 * @par MT-Safe: No
 * @par Side Effects: None
 *
 */
void
set_attr_ll(attribute *pattr, Long value, enum batch_op op)
{
	if (pattr == NULL) {
		log_err(-1, __func__, "Invalid pointer to attribute");
		return;
	}

	switch (op) {
		case SET:
			pattr->at_val.at_ll = value;
			break;
		case INCR:
			pattr->at_val.at_ll += value;
			break;
		case DECR:
			pattr->at_val.at_ll -= value;
			break;
		default:
			return;
	}

	post_attr_set(pattr);
}

/**
 * @brief	Attribute getter function for long long type values
 *
 * @param[in]	pattr	-	pointer to the attribute
 *
 * @return	long
 * @retval	long value of the attribute
 *
 * @par MT-Safe: No
 * @par Side Effects: None
 */
Long
get_attr_ll(const attribute *pattr)
{
	return  pattr->at_val.at_ll;
}


/*
 * free_ll - use free_null to (not) free space
 */
