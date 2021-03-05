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
 * @file	mom_info.c
 * @brief
 * 		mom_info.c - functions relating to the mominfo structures and vnodes
 *
 *		Some of the functions here in are used by both the Server and Mom,
 *		others are used by one or the other but not both.
 *
 * Included functions are:
 *
 * 	create_mom_entry()
 * 	delete_mom_entry()
 * 	find_mom_entry()
 * 	create_svrmom_entry()
 * 	delete_svrmom_entry()
 * 	create_mommap_entry()
 * 	delete_momvmap_entry()
 * 	find_vmap_entry()
 * 	add_mom_data()
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include "libpbs.h"
#include "list_link.h"
#include "attribute.h"
#include "resource.h"
#include "credential.h"
#include "server_limits.h"
#include "batch_request.h"
#include "server.h"
#include "pbs_nodes.h"
#include "pbs_error.h"
#include "log.h"
#include "svrfunc.h"
#include "tpp.h"
#include "pbs_internal.h"
#include "work_task.h"
#include "hook_func.h"

static char merr[] = "malloc failed";

/* Global Data Itmes */

/* mominfo_array is an array of mominfo_t pointers, one per host */

mominfo_t **mominfo_array = NULL;
int         mominfo_array_size = 0;     /* num entries in the array */
mominfo_time_t  mominfo_time = {0};	/* time stamp of mominfo update */
int	    svr_num_moms = 0;
vnpool_mom_t    *vnode_pool_mom_list = NULL;

extern char	*msg_daemonname;
extern char	*path_hooks_rescdef;
extern char	*msg_new_inventory_mom;

extern int remove_mom_ipaddresses_list(mominfo_t *pmom);

/*
 * The following function are used by both the Server and Mom
 *	create_mom_entry()
 *	delete_mom_entry()
 *	find_mom_entry()
 */

#define GROW_MOMINFO_ARRAY_AMT 10

/**
 * @brief
 *		create_mom_entry - create both a mominfo_t entry and insert a pointer
 *		to that element into the mominfo_array which may be expanded if needed
 *
 * @par Functionality:
 *		Searches for existing mominfo_t entry with matching hostname and port;
 *		if found returns it, otherwise adds entry.   An empty slot in the
 *		mominfo_array[] will be used to hold pointer to created entry.  If no
 *		empty slot, the array is expanded by GROW_MOMINFO_ARRAY_AMT amount.
 *
 * @param[in]	hostname - hostname of host on which Mom will be running
 * @param[in]	port     - port number to which Mom will be listening
 *
 * @return	mominfo_t *
 * @retval	Returns pointer to the mominfo entry, existing or created
 * @retval	NULL on error.
 *
 * @par Side Effects: None
 *
 * @par MT-safe: no, would need lock on realloc of global mominfo_array[]
 *
 */

mominfo_t *
create_mom_entry(char *hostname, unsigned int port)
{
	int	    empty = -1;
	int         i;
	mominfo_t  *pmom;
	mominfo_t **tp;

	for (i=0; i < mominfo_array_size; ++i) {
		pmom = mominfo_array[i];
		if (pmom) {
			if ((strcasecmp(pmom->mi_host, hostname) == 0) &&
				(pmom->mi_port == port))
				return pmom;
		} else if (empty == -1) {
			empty = i;  /* save index of first empty slot */
		}
	}

	if (empty == -1) {
		/* there wasn't an empty slot in the array we can use */
		/* need to grow the array			      */

		tp = (mominfo_t **)realloc(mominfo_array,
			(size_t)(sizeof(mominfo_t *) * (mominfo_array_size+GROW_MOMINFO_ARRAY_AMT)));
		if (tp) {
			empty = mominfo_array_size;
			mominfo_array = tp;
			mominfo_array_size += GROW_MOMINFO_ARRAY_AMT;
			for (i = empty; i < mominfo_array_size; ++i)
				mominfo_array[i] = NULL;
		} else {
			log_err(errno, __func__, merr);
			return NULL;
		}
	}

	/* now allocate the memory for the mominfo_t element itself */

	pmom = (mominfo_t *)malloc(sizeof(mominfo_t));
	if (pmom) {
		(void)strncpy(pmom->mi_host, hostname, PBS_MAXHOSTNAME);
		pmom->mi_host[PBS_MAXHOSTNAME] = '\0';
		pmom->mi_port = port;
		pmom->mi_rmport = port + 1;
		pmom->mi_modtime = (time_t)0;
		pmom->mi_dmn_info = NULL;
		pmom->mi_data    = NULL;
		pmom->mi_action = NULL;
		pmom->mi_num_action = 0;
		CLEAR_LINK(pmom->mi_link);
#ifndef PBS_MOM
		if (mom_hooks_seen_count() > 0) {
			struct stat sbuf;
			/*
			 * there should be at least one hook to
			 * add mom actions below, which are in
			 * behalf of existing hooks.
			 */
			add_pending_mom_allhooks_action(pmom, MOM_HOOK_ACTION_SEND_ATTRS|MOM_HOOK_ACTION_SEND_CONFIG|MOM_HOOK_ACTION_SEND_SCRIPT);
			if (stat(path_hooks_rescdef, &sbuf) == 0)
				add_pending_mom_hook_action(pmom, PBS_RESCDEF, MOM_HOOK_ACTION_SEND_RESCDEF);
		}
#endif

		mominfo_array[empty] = pmom;
		++svr_num_moms;			/* increment number of Moms */
	} else {
		log_err(errno, __func__, merr);
	}

	return pmom;
}


/**
 *
 * @brief
 *		Destory a mominfo_t element and null the pointer to that
 *		element in the mominfo_array;
 * @par Functionality:
 *		The heap entry pointed to by the mi_data member is freed also.
 *		However, any extra malloc-ed space in that member must be freed
 *		independently. Note, this means the mominfo_array may have null
 *		entries anywhere.
 *
 * @param[in]	pmom - the element being operated on.
 *
 * @return	void
 */

void
delete_mom_entry(mominfo_t *pmom)
{
	int i;

	if (pmom == NULL)
		return;

	/*
	 * Remove any work_task entries that may be referencing this mom
	 * BEFORE we free any data.
	 */
	delete_task_by_parm1_func((void *) pmom, NULL, DELETE_ONE);

	/* find the entry in the arry that does point here */
	for (i=0; i < mominfo_array_size; ++i) {
		if (mominfo_array[i] == pmom) {
			mominfo_array[i] = NULL;
			break;
		}
	}

	if (pmom->mi_action != NULL) {

#ifndef PBS_MOM
		for (i=0; i < pmom->mi_num_action; ++i) {
			if (pmom->mi_action[i] != NULL) {
				free(pmom->mi_action[i]);
				pmom->mi_action[i] = NULL;
			}
		}
#endif
		free(pmom->mi_action);
	}

	/* free the mi_data after all hook work is done, since the hook actions
	 * use the mi_data.
	 */
	free(pmom->mi_data);

	delete_link(&pmom->mi_link);
	memset(pmom, 0, sizeof(mominfo_t));
	free(pmom);
	--svr_num_moms;

	return;
}


/**
 * @brief
 * 		find_mom_entry - find and return a pointer to a mominfo_t element
 *		defined by the hostname and port
 * @note
 *		the mominfo_array may have null entries anywhere.
 *
 * @param[in]	hostname - hostname of host on which Mom will be running
 * @param[in]	port     - port number to which Mom will be listening
 *
 * @return	pointer to a mominfo_t element
 * @reval	NULL	- couldn't find.
 */

mominfo_t *
find_mom_entry(char *hostname, unsigned int port)
{
	int i;
	mominfo_t *pmom;

	for (i=0; i<mominfo_array_size; ++i) {
		pmom = mominfo_array[i];
		if (pmom &&
			(strcasecmp(pmom->mi_host, hostname) == 0) &&
			(pmom->mi_port == port))
			return pmom;
	}

	return NULL; 	/* didn't find it */
}


#ifndef PBS_MOM	/* Not Mom, i.e. the Server */

/*
 * The following functions are used by the Server only !
 */

/**
 * @brief initialize daemon info structure
 * This struct is common for all service end points
 * inlcluding mom/peer-svr
 * 
 * @param[in] pul - list of IP addresses of host; will be freed on error
 *			or saved in structure; caller must not free pul
 * @param[in] port - port of service end point 
 * @param[in] pmi - machine info struct
 * @return dmn_info_t* 
 */
dmn_info_t *
init_daemon_info(ulong *pul, uint port, struct machine_info *pmi)
{
	dmn_info_t *dmn_info = calloc(1, sizeof(dmn_info_t));
	if (!dmn_info)
		return NULL;

	dmn_info->dmn_state =INUSE_UNKNOWN | INUSE_DOWN | INUSE_NEEDS_HELLOSVR;
	dmn_info->dmn_stream  = -1;
	CLEAR_HEAD(dmn_info->dmn_deferred_cmds);
	dmn_info->dmn_addrs = pul;

	while (*pul) {
		tinsert2(*pul, port, pmi, &ipaddrs);
		pul++;
	}

	return dmn_info;
}

/**
 * @brief free up daemon info struct and associated data
 * 
 * @param[in] pmi - mom/peer-svr struct
 */
void
delete_daemon_info(struct machine_info *pmi)
{
	dmn_info_t *pdmninfo;
	ulong *up;

	if (!pmi || !pmi->mi_dmn_info)
		return;

	pdmninfo = pmi->mi_dmn_info;

	/* take stream out of tree */
	tpp_close(pdmninfo->dmn_stream);
	tdelete2((unsigned long)pdmninfo->dmn_stream , 0, &streams);

	if (pdmninfo->dmn_addrs) {
		for (up = pdmninfo->dmn_addrs; *up; up++) {
			/* del Mom's IP addresses from tree  */
			tdelete2(*up, pmi->mi_port,  &ipaddrs);
		}
		free(pdmninfo->dmn_addrs);
		pdmninfo->dmn_addrs = NULL;
	}

	free(pdmninfo);
	pmi->mi_dmn_info = NULL;
}

/**
 * @brief
 * 		create_svrmom_entry - create both a mominfo entry and the mom_svrinfo
 *		entry associated with it.
 *		Also used as a peer server structure for multi-server.
 * @par Functionality:
 *		Finds an existing mominfo_t structure for the hostname/port tuple,
 *		create mominfo_t and associated mom_svrinfo_t structures; and array
 *		(size 1) of pointers to pbs nodes for the children vnodes.
 * @note
 * 		use delete_mom_entry() to delete both the mominfo and
 *		mom_svrinfo entries.
 * @see
 * 		create_pbs_node2
 *
 * @param[in]	hostname - hostname of host on which Mom will be running
 * @param[in]	port     - port number to which Mom will be listening
 * @param[in]	pul      - list of IP addresses of host; will be freed on error
 *			   				or saved in structure; caller must not free pul
 *
 * @return	mominfo_t *
 * @retval	pointer to the created mominfo entry	- success
 * @retval	NULL	- error.
 *
 * @par Side Effects: None
 *
 * @par MT-safe: see create_mom_entry() and tinsert2()
 *
 */

mominfo_t *
create_svrmom_entry(char *hostname, unsigned int port, unsigned long *pul)
{
	mominfo_t     *pmom;
	mom_svrinfo_t *psvrmom;

	pmom = create_mom_entry(hostname, port);

	if (pmom == NULL) {
		free(pul);
		return pmom;
	}

	if (pmom->mi_data != NULL) {
		free(pul);
		return pmom;	/* already there */
	}

	psvrmom = (mom_svrinfo_t *)malloc(sizeof(mom_svrinfo_t));
	if (!psvrmom) {
		log_err(PBSE_SYSTEM, __func__, merr);
		delete_mom_entry(pmom);
		return NULL;
	}

	psvrmom->msr_pcpus = 0;
	psvrmom->msr_acpus = 0;
	psvrmom->msr_pmem  = 0;
	psvrmom->msr_numjobs = 0;
	psvrmom->msr_arch  = NULL;
	psvrmom->msr_pbs_ver  = NULL;
	psvrmom->msr_timedown = (time_t)0;
	psvrmom->msr_wktask  = 0;
	psvrmom->msr_jbinxsz = 0;
	psvrmom->msr_jobindx = NULL;
	psvrmom->msr_numvnds = 0;
	psvrmom->msr_numvslots = 1;
	psvrmom->msr_vnode_pool = 0;
	psvrmom->msr_has_inventory = 0;
	psvrmom->msr_children =
		(struct pbsnode **)calloc((size_t)(psvrmom->msr_numvslots),
		sizeof(struct pbsnode *));
	if (psvrmom->msr_children == NULL) {
		log_err(errno, __func__, merr);
		free(psvrmom);
		delete_mom_entry(pmom);
		return NULL;
	}
	pmom->mi_data = psvrmom;	/* must be done before call tinsert2 */

	if (pmom->mi_dmn_info) {
		free(pul);
		return pmom;	/* already there */
	}

	pmom->mi_dmn_info = init_daemon_info(pul, port, pmom);
	if (!pmom->mi_dmn_info) {
		log_err(PBSE_SYSTEM, __func__, merr);
		delete_svrmom_entry(pmom);
		return NULL;
	}

	return pmom;
}

/**
 * @brief
 * 		open_conn_stream - do an tpp_open if it is safe to do so.
 *
 * @param[in]	pmom	- pointer to mominfo structure
 *
 * @return	int
 * @retval	-1: cannot be opened or error on opening
 * @retval	>=0: success
 */
int
open_conn_stream(mominfo_t *pmom)
{
	int stream = -1;
	dmn_info_t *pdmninfo;

	pdmninfo = pmom->mi_dmn_info;
	if (pdmninfo->dmn_stream >= 0)
		return pdmninfo->dmn_stream;

	if ((stream = tpp_open(pmom->mi_host, pmom->mi_rmport)) < 0) {
		log_eventf(PBSEVENT_DEBUG, PBS_EVENTCLASS_SERVER, LOG_DEBUG,
			   msg_daemonname, "Failed to open connection stream for %s", pmom->mi_host);
		return -1;
	}

	pdmninfo->dmn_stream = stream;
	pdmninfo->dmn_state &= ~(INUSE_UNKNOWN | INUSE_DOWN);
	tinsert2((u_long)stream, 0, pmom, &streams);

	return stream;
}

/**
 * @brief
 * 		delete_svrmom_entry - destroy a mom_svrinfo_t element and the parent
 *		mominfo_t element.
 *
 * @see
 * 		effective_node_delete
 *
 * @param[in]	pmom	- pointer to mominfo structure
 *
 * @return	void
 */

void
delete_svrmom_entry(mominfo_t *pmom)
{
	mom_svrinfo_t *psvrmom = (mom_svrinfo_t *)pmom->mi_data;

	if (psvrmom) {

#ifndef PBS_MOM
		/* send request to this mom to delete all hooks known from this server. */
		/* we'll just send this delete request only once */
		/* if a hook fails to delete, then that mom host when it */
		/* come back will still have the hook. */
		if (pmom->mi_dmn_info && !(pmom->mi_dmn_info->dmn_state & INUSE_UNKNOWN) && (mom_hooks_seen_count() > 0)) {
			uc_delete_mom_hooks(pmom);
		}
#endif

		if (psvrmom->msr_arch)
			free(psvrmom->msr_arch);

		if (psvrmom->msr_pbs_ver)
			free(psvrmom->msr_pbs_ver);

		if (psvrmom->msr_children)
			free(psvrmom->msr_children);

		if (psvrmom->msr_jobindx) {
			free(psvrmom->msr_jobindx);
			psvrmom->msr_jbinxsz = 0;
			psvrmom->msr_jobindx = NULL;
		}

		if (remove_mom_ipaddresses_list(pmom) != 0) {
			snprintf(log_buffer, sizeof(log_buffer), "Could not remove IP address for mom %s:%d from cache",
					pmom->mi_host, pmom->mi_port);
			log_err(errno, __func__, log_buffer);
		}
	}
	memset((void *)psvrmom, 0, sizeof(mom_svrinfo_t));
	delete_daemon_info(pmom);
	delete_mom_entry(pmom);
}

/**
 * @brief
 *	Find the pool that matches what is set on the node
 *
 * @param[in]	pmom - pointer to the mom
 * @param[out]	ppool - pointer to the matching pool structure
 *
 * @return	vnpool_mom_t *
 * @retval	pointer to the matching pool structure
 * @retval	NULL - if there is no match
 */
vnpool_mom_t *
find_vnode_pool(mominfo_t *pmom)
{
	mom_svrinfo_t *psvrmom = (mom_svrinfo_t *)(pmom->mi_data);
	vnpool_mom_t *ppool = vnode_pool_mom_list;

	if (psvrmom->msr_vnode_pool != 0) {
		while (ppool != NULL) {
			if (ppool->vnpm_vnode_pool == psvrmom->msr_vnode_pool)
				return (ppool);

			ppool = ppool->vnpm_next;
		}
	}
	return NULL;
}

/**
 * @brief
 *	Reset the "inventory Mom" for a vnode_pool if the specified Mom is the
 *	current inventory Mom.  Done when she is down or deleted from the pool.
 *
 * @param[in] pmom - Pointer to the Mom (mominfo_t) structure of the Mom
 *	being removed/marked down.
 */
void
reset_pool_inventory_mom(mominfo_t *pmom)
{
	int           i;
	vnpool_mom_t  *ppool;
	mominfo_t     *pxmom;
	mom_svrinfo_t *pxsvrmom;
	mom_svrinfo_t *psvrmom = (mom_svrinfo_t *)(pmom->mi_data);

	/* If this Mom is in a vnode pool and is the inventory Mom for that */
	/* pool remove her from that role and if another Mom in the pool and */
	/* is up, make that Mom the new inventory Mom */

	if (psvrmom->msr_vnode_pool != 0) {
		ppool = find_vnode_pool(pmom);
		if (ppool != NULL) {
			if (ppool->vnpm_inventory_mom != pmom)
				return;	/* in the pool but is not the inventory mom */

			/* this newly down/deleted Mom was the inventory Mom, */
			/* clear her as the inventory mom in the pool */
			ppool->vnpm_inventory_mom = NULL;
			psvrmom->msr_has_inventory = 0;

			/* see if another Mom is up to become "the one" */
			for (i = 0; i < ppool->vnpm_nummoms; ++i) {
				pxmom = ppool->vnpm_moms[i];
				pxsvrmom = (mom_svrinfo_t *)pxmom->mi_data;
				if ((pxmom->mi_dmn_info->dmn_state & INUSE_DOWN) == 0) {
					ppool->vnpm_inventory_mom = pxmom;
					pxsvrmom->msr_has_inventory = 1;
					log_eventf(PBSEVENT_DEBUG, PBS_EVENTCLASS_SERVER,
						LOG_DEBUG, msg_daemonname, msg_new_inventory_mom,
						ppool->vnpm_vnode_pool, pxmom->mi_host);
				}
			}
		}
	}
}

/**
 * @brief
 *	Add a Mom (mominfo_t) to the list of Moms associated with managing
 *	a vnode pool.  Create the pool if need be (not yet exists).
 *
 * @param[in] pmom - Pointer to the mominfo_t for the Mom
 * @return Error code
 * @retval - 0 - Success
 * @retval - pbs_errno - Failure code
 *
 * @par MT-safe: No
 */
int
add_mom_to_pool(mominfo_t *pmom)
{
	int             i;
	vnpool_mom_t    *ppool;
	mominfo_t       **tmplst;
	int             added_pool = 0;
	mom_svrinfo_t   *psvrmom = (mom_svrinfo_t *)pmom->mi_data;

	if (psvrmom->msr_vnode_pool == 0)
		return PBSE_NONE;	/* Mom not in a pool */

	ppool = find_vnode_pool(pmom);
	if (ppool != NULL) {
		/* Found existing pool. Is Mom already in it? */
		for (i = 0; i < ppool->vnpm_nummoms; ++i) {
			if (ppool->vnpm_moms[i] == pmom) {
				log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_NODE,
					LOG_INFO, pmom->mi_host,
					"POOL: add_mom_to_pool - "
					"Mom already in pool %ld",
					psvrmom->msr_vnode_pool);
				return PBSE_NONE; /* she is already there */
			}
		}
	}

	/* The pool doesn't exist yet, we need to add a pool entry */
	if (ppool == NULL) {
		ppool = (vnpool_mom_t *) calloc(1, (size_t)sizeof(struct vnpool_mom));
		if (ppool == NULL) {
			/* no memory */
			log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_NODE, LOG_ERR,
				  pmom->mi_host, "Failed to expand vnode_pool_mom_list");
			return PBSE_SYSTEM;
		}
		added_pool = 1;
		ppool->vnpm_vnode_pool = psvrmom->msr_vnode_pool;
	}

	/* now add Mom to pool list, expanding list if need be */

	/* expand the array, perhaps from nothingness */
	tmplst = (mominfo_t **) realloc(ppool->vnpm_moms, (ppool->vnpm_nummoms + 1) * sizeof(mominfo_t *));
	if (tmplst == NULL) {
		log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_NODE, LOG_ERR,
                          pmom->mi_host, "unable to add mom to pool, no memory");

		if (added_pool)
			free(ppool);
		return PBSE_SYSTEM;
	}
	ppool->vnpm_moms = tmplst;
	ppool->vnpm_moms[ppool->vnpm_nummoms++] = pmom;

	log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, msg_daemonname,
			"Mom %s added to vnode_pool %ld", pmom->mi_host, psvrmom->msr_vnode_pool);
	if (ppool->vnpm_inventory_mom == NULL) {
		ppool->vnpm_inventory_mom = pmom;
		psvrmom->msr_has_inventory = 1;
		log_eventf(PBSEVENT_DEBUG, PBS_EVENTCLASS_SERVER, LOG_DEBUG, msg_daemonname,
				msg_new_inventory_mom, psvrmom->msr_vnode_pool, pmom->mi_host);
	}

	if (vnode_pool_mom_list == NULL) {
		vnode_pool_mom_list = ppool;
	} else if (added_pool == 1) {
		ppool->vnpm_next = vnode_pool_mom_list;
		vnode_pool_mom_list = ppool;
	}

	return PBSE_NONE;
}

/**
 * @brief
 *	remove a Mom (mominfo_t) from the list of Moms associated with managing
 *	a vnode pool.
 *
 * @param[in] pmom - Pointer to the mominfo_t for the Mom
 *
 * @par MT-safe: No
 */
void remove_mom_from_pool(mominfo_t *pmom)
{
	int             i;
	int             j;
	vnpool_mom_t    *ppool;
	mom_svrinfo_t   *psvrmom = (mom_svrinfo_t *)pmom->mi_data;

	if (psvrmom->msr_vnode_pool == 0)
		return;	/* Mom not in a pool */

	ppool = find_vnode_pool(pmom);
	if (ppool != NULL) {
		/* found existing pool, if Mom is in it remove her */
		/* from it.  If not, nothing to do. */
		for (i = 0; i < ppool->vnpm_nummoms; ++i) {
			if (ppool->vnpm_moms[i] == pmom) {
				ppool->vnpm_moms[i] = NULL;
				for (j = i+1; j < ppool->vnpm_nummoms; ++j) {
					ppool->vnpm_moms[j-1] = ppool->vnpm_moms[j];
				}
				--ppool->vnpm_nummoms;
				/* find someone else to be the inventory Mom if need be */
				reset_pool_inventory_mom(pmom);
				psvrmom->msr_vnode_pool = 0;
			}
		}
	}
}

#else   /* PBS_MOM */

/*
 * The following functions are used by Mom only !
 */

/**
 * @brief
 * 		create_mommap_entry - create an entry to map a vnode to its parent Mom
 *		and initialize it.   If the actual host of the vnode, used only for
 *		MPI is not the same as the Mom host, then set it.  If the two hosts
 *		are the same, then mvm_hostn is null and the Mom name should be used
 *
 * @param[in]	vnode	- vnode for which entry needs to be made
 * @param[in]	hostn	- host name for MPI via PBS_NODEFILE
 * @param[in]	pmom	- pointer to mominfo structure
 * @param[in]	notask	- mvm_notask
 *
 * @return	momvmap_t
 * @retval	NULL	- failure
 */
momvmap_t *
create_mommap_entry(char *vnode, char *hostn, mominfo_t *pmom, int notask)
{
	int         empty = -1;
	int	    i;
	momvmap_t  *pmmape;
	momvmap_t **tpa;

#ifdef DEBUG
	assert((vnode != NULL) && (*vnode != '\0') && (pmom != NULL));
#else
	if ((vnode == NULL) || (*vnode == '\0') || (pmom == NULL)) {
		return NULL;
	}
#endif

	/* find a empty slot in the map array */

	for (i=0; i<mommap_array_size; ++i) {
		if (mommap_array[i] == NULL) {
			empty = i;
			break;
		}
	}
	if (empty == -1) {	/* need to expand array */
		tpa = (momvmap_t **)realloc(mommap_array, (size_t)(sizeof(momvmap_t *) * (mommap_array_size + GROW_MOMINFO_ARRAY_AMT)));
		if (tpa) {
			empty = mommap_array_size;
			mommap_array = tpa;
			mommap_array_size += GROW_MOMINFO_ARRAY_AMT;
			for (i=empty; i<mommap_array_size; ++i)
				mommap_array[i] = NULL;
		} else {
			log_err(errno, __func__, merr);
			return NULL;
		}
	}

	/* now allocate the entry itself and initalize it */

	pmmape = malloc(sizeof(momvmap_t));
	if (pmmape) {
		(void)strncpy(pmmape->mvm_name, vnode, PBS_MAXNODENAME);
		pmmape->mvm_name[PBS_MAXNODENAME] ='\0';
		if ((hostn == NULL) || (*hostn == '\0')) {
			pmmape->mvm_hostn = NULL;
		} else {
			pmmape->mvm_hostn = strdup(hostn);
			if (pmmape->mvm_hostn == NULL) {
				log_err(errno, __func__, merr);
			}
		}
		pmmape->mvm_notask = notask;
		pmmape->mvm_mom = pmom;

		mommap_array[empty] = pmmape;
	} else {
		log_err(errno, __func__, merr);
	}
	return (pmmape);
}

/**
 * @brief
 *		delete_momvmap_entry - delete a momvmap_t entry
 * @see
 * 		free_vnodemap
 * @param[in,out]	- a momvmap_t entry
 *
 * @return	void
 */
void
delete_momvmap_entry(momvmap_t *pmmape)
{
	if (pmmape->mvm_hostn)
		free(pmmape->mvm_hostn);
	memset(pmmape, 0, sizeof(momvmap_t));
	free(pmmape);
}

/**
 * @brief
 * 		find_vmap_entry - find the momvmap_t entry for a vnode name
 *
 * @param[in]	vname	- vnode name
 *
 * @return	momvmap_t *
 * @retval	mom_vmap entry	- success
 * @retval	NULL	- failure
 */

momvmap_t *
find_vmap_entry(const char *vname)
{
	int          i;
	momvmap_t  *pmap;

	for (i=0; i < mommap_array_size;++i) {
		pmap = mommap_array[i];
		if ((pmap != NULL) && (strcasecmp(pmap->mvm_name, vname) == 0))
			return pmap;
	}
	return NULL;
}


mominfo_t *find_mom_by_vnodename(const char *vname)
{
	momvmap_t	*pmap;

	pmap = find_vmap_entry(vname);
	if (pmap)
		return (pmap->mvm_mom);
	else
		return NULL;
}

mominfo_t *
add_mom_data(const char *vnid, void *data)
{
	mominfo_t	*pmom;

	if ((pmom = find_mom_by_vnodename(vnid)) != NULL) {
		pmom->mi_data = data;
		return (pmom);
	}

	return NULL;
}
#endif  /* PBS_MOM */
