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

#include	<stdio.h>
#include	<stdlib.h>
#include	<unistd.h>
#include	<limits.h>
#include	<fcntl.h>
#include	<netdb.h>
#include	<string.h>
#include	<errno.h>
#include	<assert.h>
#include	<sys/types.h>
#include	<sys/socket.h>
#include	<sys/time.h>
#include	<netinet/in.h>
#include	<netdb.h>

#include	"dis.h"
#include	"tm.h"
#include	"pbs_ifl.h"
#include 	"pbs_client_thread.h"
#include	"net_connect.h"
#include	"libsec.h"
#include 	"pbs_internal.h"

/**
 * @file	tm.c
 */

/*
 ** Set up a debug print macro.
 */
#ifdef  DEBUG
#define DBPRT(x) \
{ \
	int	err = errno; \
	printf x; \
	errno = err; \
}
#else
#define DBPRT(x)
#endif

#ifndef	MIN
#define	MIN(a, b)	(((a) < (b)) ? (a) : (b))
#endif

#ifndef __PBS_TCP_TIMEOUT
#define __PBS_TCP_TIMEOUT
extern time_t * __pbs_tcptimeout_location(void);
#define pbs_tcp_timeout (*__pbs_tcptimeout_location ())
#endif

#ifndef __PBS_TCP_INTERRUPT
#define __PBS_TCP_INTERRUPT
extern int * __pbs_tcpinterrupt_location(void);
#define pbs_tcp_interrupt (*__pbs_tcpinterrupt_location ())
#endif

#ifndef __PBS_TCP_ERRNO
#define __PBS_TCP_ERRNO
extern int * __pbs_tcperrno_location(void);
#define pbs_tcp_errno (*__pbs_tcperrno_location ())
#endif

/*
 **	Allocate some string space to hold the values passed in the
 **	enviornment from MOM.
 */
static	char		*tm_jobid = NULL;
static	int		tm_jobid_len = 0;
static	char		*tm_jobcookie = NULL;
static	int		tm_jobcookie_len = 0;
static	tm_task_id	tm_jobtid = TM_NULL_TASK;
static	tm_node_id	tm_jobndid = TM_ERROR_NODE;
static	int		tm_momport = 15003;
static	int		local_conn = -1;
static	int		init_done = 0;
static	char		*localhost = LOCALHOST_SHORTNAME;

/*
 **	Events are the central focus of this library.  They are tracked
 **	in a hash table.  Many of the library calls return events.  They
 **	are recorded and as information is received from MOM's, the
 **	event is updated and marked so tm_poll() can return it to the user.
 */
#define	EVENT_HASH	128

/*
 * a bit of code to map a tm_ error number to the symbol
 */

struct tm_errcode {
	int	trc_code;
	char   *trc_name;
} tm_errcode[] = {
	{ TM_ESYSTEM, "system error - MOM cannot be contacted" },
	{ TM_ENOTCONNECTED, "not connected" },
	{ TM_EUNKNOWNCMD, "unknown command" },
	{ TM_ENOTIMPLEMENTED, "not implemented/supported" },
	{ TM_EBADENVIRONMENT, "bad environment" },
	{ TM_ENOTFOUND, "no matching job found" },
	{ TM_ESESSION, "session is already attached" },
	{ TM_EUSER, "user not permitted to attach" },
	{ TM_EOWNER, "process owner does not match job" },
	{ TM_ENOPROC, "process does not exist" },
	{ TM_EHOOK, "a hook has rejected the task manager request" },
	{ 0,		"unknown" }
};

char *
get_ecname(int rc)
{
	struct	tm_errcode	*p;
	static	char	buf[256];

	for (p=&tm_errcode[0]; p->trc_code; ++p) {
		if (p->trc_code == rc)
			break;
	}
	sprintf(buf, "%s (%d)", p->trc_name, rc);
	return buf;
}

typedef struct	event_info {
	tm_event_t		e_event;	/* event number */
	tm_node_id		e_node;		/* destination node */
	int			e_mtype;	/* message type sent */
	void			*e_info;	/* possible returned info */
	struct	event_info	*e_next;	/* link to next event */
	struct	event_info	*e_prev;	/* link to prev event */
}	event_info;
static	event_info	*event_hash[EVENT_HASH];
static	int		event_count = 0;

/**
 * @brief
 *	Find an event number or return a NULL.
 */
static event_info *
find_event(tm_event_t x)
{
	event_info	*ep;

	for (ep=event_hash[x % EVENT_HASH]; ep; ep=ep->e_next) {
		if (ep->e_event == x)
			break;
	}
	return ep;
}

/**
 * @brief
 *	Delete an event.
 *
 * @param[in] ep - pointer to event info
 *
 * @return	Void
 *
 */
static void
del_event(event_info *ep)
{

	/* unlink event from hash list */
	if (ep->e_prev)
		ep->e_prev->e_next = ep->e_next;
	else
		event_hash[ep->e_event % EVENT_HASH] = ep->e_next;
	if (ep->e_next)
		ep->e_next->e_prev = ep->e_prev;

	/*
	 **	Free any memory saved with the event.  This depends
	 **	on whay type of event it is.
	 */
	switch (ep->e_mtype) {

		case TM_INIT:
		case TM_SPAWN:
		case TM_SPAWN_MULTI:
		case TM_ATTACH:
		case TM_SIGNAL:
		case TM_OBIT:
		case TM_POSTINFO:
			break;

		case TM_TASKS:
		case TM_GETINFO:
		case TM_RESOURCES:
			free(ep->e_info);
			break;

		default:
			DBPRT(("del_event: unknown event command %d\n", ep->e_mtype))
			break;
	}
	free(ep);

	if (--event_count == 0) {
		CS_close_socket(local_conn);
		closesocket(local_conn);
		local_conn = -1;
	}
	return;
}

/**
 * @brief
 *	Create a new event number.
 *
 * @return	tm_event_t
 * @retval	eventinfo	success
 *		breaks out of loop if fails.
 */
static tm_event_t
new_event()
{
	static	tm_event_t	next_event = TM_NULL_EVENT+1;
	event_info		*ep;
	tm_event_t		ret;

	if (next_event == INT_MAX)
		next_event = TM_NULL_EVENT+1;
	for (;;) {
		ret = next_event++;

		for (ep=event_hash[ret % EVENT_HASH]; ep; ep=ep->e_next) {
			if (ep->e_event == ret)
				break;	/* innter loop: this number is in use */
		}
		if (ep == NULL)
			break;		/* this number is not in use */
	}
	return ret;
}

/**
 * @brief
 *	-Link new event number into the above hash table.
 *
 * @param[in] event - event info
 * @param[in] node - job-relative node id
 * @param[in] type - type of event
 * @param[in] info - info about event
 *
 * @return	Void
 *
 */
static void
add_event(tm_event_t event, tm_node_id node, int type, void *info)
{
	event_info		*ep, **head;

	ep = (event_info *)malloc(sizeof(event_info));
	assert(ep != NULL);

	head = &event_hash[event % EVENT_HASH];
	ep->e_event = event;
	ep->e_node = node;
	ep->e_mtype = type;
	ep->e_info = info;
	ep->e_next = *head;
	ep->e_prev = NULL;
	if (*head)
		(*head)->e_prev = ep;
	*head = ep;

	event_count++;
	return;
}

/*
 **	Sessions must be tracked by the library so tm_taskid objects
 **	can be resolved into real tasks on real nodes.
 **	We will use a hash table.
 */
#define	TASK_HASH	256
typedef	struct	task_info {
	char			*t_jobid;	/* jobid */
	tm_task_id		 t_task;	/* task id */
	tm_node_id		 t_node;	/* node id */
	struct	task_info	*t_next;	/* link to next task */
}	task_info;
static	task_info	*task_hash[TASK_HASH];

/**
 * @brief
 *	-Find a task table entry for a given task number or return a NULL.
 *
 * @param[in] x - task id
 *
 * @return 	structure handle
 * @retval	pointer to task info
 *
 */
static task_info *
find_task(tm_task_id x)
{
	task_info	*tp;

	for (tp=task_hash[x % TASK_HASH]; tp; tp=tp->t_next) {
		if (tp->t_task == x)
			break;
	}
	return tp;
}

/**
 * @brief
 *	-Create a new task entry and link it into the above hash
 *	table.
 *
 * @param[in] jobid - job identifier
 * @param[in] node - job-relative node id
 * @param[out] task - task id(0 or 1)
 *
 * @return	tm_task_id
 * @retval	TM_NULL_TASK		failure
 * @retval	initialized task	success
 *
 */
static tm_task_id
new_task(char *jobid, tm_node_id node, tm_task_id task)
{
	task_info		*tp, **head;

	DBPRT(("%s: jobid=%s node=%d task=0x%08X\n",
		__func__, jobid ? jobid : "none", node, task))
	if (jobid != tm_jobid && strcmp(jobid, tm_jobid) != 0) {
		DBPRT(("%s: task job %s not my job %s\n",
			__func__, jobid, tm_jobid))
		return TM_NULL_TASK;
	}

	if ((tp = find_task(task)) != NULL) {
		DBPRT(("%s: task 0x%08X found with node %d should be %d\n",
			__func__, task, tp->t_node, node))
		return task;
	}

	if ((tp = (task_info *)malloc(sizeof(task_info))) == NULL)
		return TM_NULL_TASK;

	head = &task_hash[task % TASK_HASH];
	tp->t_jobid = tm_jobid;
	tp->t_task  = task;
	tp->t_node  = node;
	tp->t_next = *head;
	*head = tp;

	return task;
}

/*
 **	Delete a task.
 ===
 === right now, this is not used.
 ===
 static void
 del_task(x)
 tm_task_id	x;
 {
 task_info	*tp, *prev;

 prev = NULL;
 for (tp=task_hash[x % TASK_HASH]; tp; prev=tp, tp=tp->t_next) {
 if (tp->t_task == x)
 break;
 }
 if (tp) {
 if (prev)
 prev->t_next = tp->t_next;
 else
 task_hash[x % TASK_HASH] = tp->t_next;
 tp->t_next = NULL;
 if (tp->t_jobid != tm_jobid && tp->t_jobid != NULL)
 free(tp->t_jobid);
 free(tp);
 }
 return;
 }
 */

/*
 **	The nodes are tracked in an array.
 */
static	tm_node_id	*node_table = NULL;


/**
 * @brief
 *	-localmom() - make a connection to the local pbs_mom
 *
 * @par Note:
 *	The connection will remain open as long as there is an
 *	outstanding event.
 *
 * @return	int
 * @retval	-1	connection fail
 * @retval	>=0	connection succcess
 *
 */
#define PBS_NET_RC_FATAL -1
#define PBS_NET_RC_RETRY -2

static int
localmom()
{
	static  int	       have_addr = 0;
	static  struct in_addr hostaddr;
	struct	hostent	*hp;
	int		 i;
	int		 ret;
	struct sockaddr_in remote;
	int              sock;
	struct	linger	 ltime;

	if (local_conn >= 0)
		return local_conn;	/* already have open connection */

	if (have_addr == 0) {
		/* lookup localhost and save address */
		if ((hp = gethostbyname(localhost)) == NULL) {
			DBPRT(("%s: host %s not found\n", __func__, localhost))
			return -1;
		}
		assert(hp->h_length <= sizeof(hostaddr));
		memcpy(&hostaddr, hp->h_addr_list[0], hp->h_length);
		have_addr = 1;
	}

	for (i=0; i<5; i++) {

		/* get socket */

		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0)
			return -1;

		/* make sure data goes out */

		ltime.l_onoff = 1;
		ltime.l_linger = 5;
		setsockopt(sock, SOL_SOCKET, SO_LINGER, &ltime, sizeof(ltime));

		/* connect to specified local pbs_mom and port */

		remote.sin_addr = hostaddr;
		remote.sin_port = htons((unsigned short)tm_momport);
		remote.sin_family = AF_INET;
		if (connect(sock, (struct sockaddr *)&remote,
			sizeof(remote))<0) {
			switch (errno) {
				case EADDRINUSE:
				case ETIMEDOUT:
				case ECONNREFUSED:
#ifdef WIN32
				case WSAEINTR:
#else
				case EINTR:
#endif
					closesocket(sock);
					sleep(1);
					continue;
				default:
					goto failed;
			}
		} else {
			local_conn = sock;
			break;
		}

	}

	if (CS_client_init() != CS_SUCCESS)
		goto failed;

	ret = CS_client_auth(local_conn);

	if ((ret != CS_SUCCESS) && (ret != CS_AUTH_USE_IFF)) {

		(void)CS_close_socket(local_conn);
		(void)CS_close_app();
		goto failed;
	}

	DIS_tcp_funcs();
	return (local_conn);

failed:

	closesocket(sock);
	local_conn = -1;
	return -1;
}

/**
 * @brief
 *	-startcom() - send request header to local pbs_mom.
 *	If required, make connection to her.
 *
 * @param[in] com - communication handle
 * @param[in] event - event
 *
 * @return	int
 * @retval	DIS_SUCCESS(0)	success
 * @retval	!0		error
 *
 */
static int
startcom(int com, tm_event_t event)
{
	int     ret;

	if (localmom() == -1)
		return -1;

	ret = diswsi(local_conn, TM_PROTOCOL);
	if (ret != DIS_SUCCESS)
		goto done;
	ret = diswsi(local_conn, TM_PROTOCOL_VER);
	if (ret != DIS_SUCCESS)
		goto done;
	ret = diswcs(local_conn, tm_jobid, tm_jobid_len);
	if (ret != DIS_SUCCESS)
		goto done;
	ret = diswcs(local_conn, tm_jobcookie, tm_jobcookie_len);
	if (ret != DIS_SUCCESS)
		goto done;
	ret = diswsi(local_conn, com);
	if (ret != DIS_SUCCESS)
		goto done;
	ret = diswsi(local_conn, event);
	if (ret != DIS_SUCCESS)
		goto done;
	ret = diswui(local_conn, tm_jobtid);
	if (ret != DIS_SUCCESS)
		goto done;
	return DIS_SUCCESS;

done:
	DBPRT(("startcom: send error %s\n", dis_emsg[ret]))
	CS_close_socket(local_conn);
	closesocket(local_conn);
	local_conn = -1;
	return ret;
}

/**
 * @brief
 *	-Initialize the Task Manager interface.
 *
 * @param[in] info - currently unused
 * @param[out] roots - data for the last tm_init call whose event has been polled
 *
 * @return	int
 * @retval	0			success
 * @retval	TM error msg(!0)	error
 */
int
tm_init(void *info, struct tm_roots *roots)
{
	tm_event_t		nevent, revent;
	char			*env, *hold;
	int			err;
	int			nerr = 0;
	extern	int		pbs_tcp_interrupt;

	if (init_done)
		return TM_BADINIT;

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return TM_ESYSTEM;

	pbs_tcp_interrupt = 1;

	if ((env = getenv("PBS_JOBID")) == NULL)
		return TM_EBADENVIRONMENT;
	tm_jobid_len = 0;
	free(tm_jobid);
	tm_jobid = strdup(env);
	if (!tm_jobid)
		return TM_ESYSTEM;
	tm_jobid_len = strlen(tm_jobid);

	if ((env = getenv("PBS_JOBCOOKIE")) == NULL)
		return TM_EBADENVIRONMENT;
	tm_jobcookie_len = 0;
	free(tm_jobcookie);
	tm_jobcookie = strdup(env);
	if (!tm_jobcookie)
		return TM_ESYSTEM;
	tm_jobcookie_len = strlen(tm_jobcookie);

	if ((env = getenv("PBS_NODENUM")) == NULL)
		return TM_EBADENVIRONMENT;
	tm_jobndid = (tm_node_id)strtol(env, &hold, 10);
	if (env == hold)
		return TM_EBADENVIRONMENT;
	if ((env = getenv("PBS_TASKNUM")) == NULL)
		return TM_EBADENVIRONMENT;
	if ((tm_jobtid = strtoul(env, NULL, 16)) == 0)
		return TM_EBADENVIRONMENT;
	if ((env = getenv("PBS_MOMPORT")) == NULL)
		return TM_EBADENVIRONMENT;
	if ((tm_momport = atoi(env)) == 0)
		return TM_EBADENVIRONMENT;

	init_done = 1;
	nevent = new_event();

	/*
	 * send the following request:
	 *	header		(tm_init)
	 */

	if (startcom(TM_INIT, nevent) != DIS_SUCCESS)
		return TM_ESYSTEM;
	dis_flush(local_conn);
	add_event(nevent, TM_ERROR_NODE, TM_INIT, (void *)roots);

	if ((err = tm_poll(TM_NULL_EVENT, &revent, 1, &nerr)) != TM_SUCCESS)
		return err;
	return nerr;
}

/**
 *
 * @brief
 *	Initialize and attach new task for <pid> to job <jobid>
 *
 * @param[in]	jobid  - job id to which new task will be attached
 * @param[in]	cookie - job cookie
 * @param[in]	pid    - pid of task to be attached
 * @param[in]	host   - hostname
 * @param[in] 	port   - port number
 * @param[out]	tid  - newly attached task id
 *
 * @return	int
 * @retval	TM_SUCCESS (0)  - Success
 * @retval	TM_E*	   (>0) - Failer
 *
 */
int
tm_attach(char *jobid, char *cookie, pid_t pid, tm_task_id *tid, char *host, int port)
{
	tm_event_t		nevent, revent;
	int			err;
	int			nerr = 0;
	extern	int		pbs_tcp_interrupt;
#ifdef WIN32
	char	                usern[UNLEN + 1] = {'\0'};
	int		        sz = 0;
	int		        ret = 0;
#endif

	pbs_tcp_interrupt = 1;

	tm_jobid_len = 0;
	free(tm_jobid);
	tm_jobid = NULL;
	if (jobid && (*jobid != '\0')) {
		tm_jobid = strdup(jobid);
		if (!tm_jobid)
			return TM_ESYSTEM;
		tm_jobid_len = strlen(tm_jobid);
	}

	tm_jobcookie_len = 0;
	free(tm_jobcookie);
	tm_jobcookie = NULL;
	if (cookie && (*cookie != '\0')) {
		tm_jobcookie = strdup(cookie);
		if (!tm_jobcookie)
			return TM_ESYSTEM;
		tm_jobcookie_len = strlen(tm_jobcookie);
	}

	if (host != NULL && *host != '\0')
		localhost = host;
	tm_momport = port;

	nevent = new_event();

	/*
	 * send the following request:
	 *	header		(tm_attach)
	 *	int		uid
	 *	int		pid
	 */

	if (startcom(TM_ATTACH, nevent) != DIS_SUCCESS)
		return TM_ESYSTEM;
#ifdef WIN32
	sz = sizeof(usern);
	ret = GetUserName(usern, &sz);
	if (diswcs(local_conn, usern, strlen(usern)) != DIS_SUCCESS)	/* send uid */
		return TM_ENOTCONNECTED;
#else
	if (diswsi(local_conn, getuid()) != DIS_SUCCESS)	/* send uid */
		return TM_ENOTCONNECTED;
#endif

	if (diswsi(local_conn, pid) != DIS_SUCCESS)	/* send pid */
		return TM_ENOTCONNECTED;

	dis_flush(local_conn);
	add_event(nevent, TM_ERROR_NODE, TM_ATTACH, (void *)tid);

	init_done = 1;		/* fake having called tm_init */
	err = tm_poll(TM_NULL_EVENT, &revent, 1, &nerr);
	init_done = 0;

	if (err != TM_SUCCESS)
		return err;
	return nerr;
}

/**
 * @brief
 *	-Copy out node info.  No communication with pbs_mom is needed.
 *
 * @param[in] list - pointer to job relative node list
 * @param[out] nnodes - number of nodes
 *
 * @return	int
 * @retval	TM_SUCCESS	Success
 * @retval	TM_E*		error
 *
 */
int
tm_nodeinfo(tm_node_id **list, int *nnodes)
{
	tm_node_id	*np;
	int		i;
	int		n = 0;

	if (!init_done)
		return TM_BADINIT;
	if (node_table == NULL)
		return TM_ESYSTEM;

	for (np=node_table; *np != TM_ERROR_NODE; np++)
		n++;		/* how many nodes */

	np = (tm_node_id *)calloc(n, sizeof(tm_node_id));
	if (np == NULL)
		return TM_ESYSTEM;
	for (i=0; i<n; i++)
		np[i] = node_table[i];
	*list = np;
	*nnodes = i;
	return TM_SUCCESS;
}


/**
 * @brief
 *	-Starts <argv>[0] with environment <envp> at <where>.
 *
 * @param[in] argc - argument count
 * @param[in] argv - argument list
 * @param[in] envp - environment variable list
 * @param[in] list_size - number of nodes in where
 * @param[in] where - job relative node ID(s)
 * @param[out] tids - task ID(s)
 * @param[out] event - event info
 *
 * @return	int
 * @retval	TM_SUCCESS	success
 * @retval	TM_ER*		error
 *
 */
int
tm_spawn_multi(int argc, char **argv, char **envp,
		int list_size, tm_node_id where[], tm_task_id tids[],
		tm_event_t *event)
{
	char		*cp;
	int		i;

	if (!init_done)
		return TM_BADINIT;
	if (argc <= 0 || argv == NULL || argv[0] == NULL || *argv[0] == '\0')
		return TM_ENOTFOUND;
	
	if (list_size < 1)
		return TM_EBADENVIRONMENT; 

	*event = new_event();
	if (startcom(TM_SPAWN_MULTI, *event) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	
	/* send list_size */
	if (diswui(local_conn, list_size) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	
	/* send where */
	for (i = 0; i < list_size; i++) {
		if (diswui(local_conn, where[i]) != DIS_SUCCESS)
			return TM_ENOTCONNECTED;
	}

	if (diswsi(local_conn, argc) != DIS_SUCCESS)	/* send argc */
		return TM_ENOTCONNECTED;

	/* send argv strings across */

	for (i=0; i < argc; i++) {
		cp = argv[i];
		if (diswcs(local_conn, cp, strlen(cp)) != DIS_SUCCESS)
			return TM_ENOTCONNECTED;
	}

	/* send envp strings across */
	if (envp != NULL) {
		for (i=0; (cp = envp[i]) != NULL; i++) {
#if defined(PBS_SECURITY) && (PBS_SECURITY == KRB5)
			/* never send KRB5CCNAME; it would rewrite the value on target host */
			if (strncmp(envp[i], "KRB5CCNAME", strlen("KRB5CCNAME")) == 0)
				continue;
#endif
			if (diswcs(local_conn, cp, strlen(cp)) != DIS_SUCCESS)
				return TM_ENOTCONNECTED;
		}
	}
	if (diswcs(local_conn, "", 0) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	dis_flush(local_conn);
	/* Passing the Mother Superior */
	add_event(*event, where[0], TM_SPAWN_MULTI, (void *)tids);
	return TM_SUCCESS;
}

/**
 * @brief
 *	-Starts <argv>[0] with environment <envp> at <where>.
 *
 * @param[in] argc - argument count
 * @param[in] argv - argument list
 * @param[in] envp - environment variable list
 * @param[in] where - job relative node
 * @param[out] tid - task id
 * @param[out] event - event info
 *
 * @return	int
 * @retval	TM_SUCCESS	success
 * @retval	TM_ER*		error
 *
 */
int
tm_spawn(int argc, char **argv, char **envp,
		tm_node_id where, tm_task_id *tid, tm_event_t *event)
{
	char		*cp;
	int		i;

	if (!init_done)
		return TM_BADINIT;
	if (argc <= 0 || argv == NULL || argv[0] == NULL || *argv[0] == '\0')
		return TM_ENOTFOUND;

	*event = new_event();
	if (startcom(TM_SPAWN, *event) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;

	if (diswsi(local_conn, where) != DIS_SUCCESS)	/* send where */
		return TM_ENOTCONNECTED;

	if (diswsi(local_conn, argc) != DIS_SUCCESS)	/* send argc */
		return TM_ENOTCONNECTED;

	/* send argv strings across */

	for (i=0; i < argc; i++) {
		cp = argv[i];
		if (diswcs(local_conn, cp, strlen(cp)) != DIS_SUCCESS)
			return TM_ENOTCONNECTED;
	}

	/* send envp strings across */
	if (envp != NULL) {
		for (i=0; (cp = envp[i]) != NULL; i++) {
#if defined(PBS_SECURITY) && (PBS_SECURITY == KRB5)
			/* never send KRB5CCNAME; it would rewrite the value on target host */
			if (strncmp(envp[i], "KRB5CCNAME", strlen("KRB5CCNAME")) == 0)
				continue;
#endif
			if (diswcs(local_conn, cp, strlen(cp)) != DIS_SUCCESS)
				return TM_ENOTCONNECTED;
		}
	}
	if (diswcs(local_conn, "", 0) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	dis_flush(local_conn);
	add_event(*event, where, TM_SPAWN, (void *)tid);
	return TM_SUCCESS;
}

/**
 * @brief
 *	-Sends a <sig> signal to all the process groups in the task
 *	signified by the handle, <tid>.
 *
 * @param[in] tid - task id
 * @param[in] sig - signal number
 * @param[out] event - event handle
 *
 * @return	int
 * @retval	TM_SUCCESS	Success
 * @retval	TM_ER*		error
 *
 */
int
tm_kill(tm_task_id tid, int sig, tm_event_t *event)
{
	task_info	*tp;

	if (!init_done)
		return TM_BADINIT;
	if ((tp = find_task(tid)) == NULL)
		return TM_ENOTFOUND;
	*event = new_event();
	if (startcom(TM_SIGNAL, *event) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	if (diswsi(local_conn, tp->t_node) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	if (diswui(local_conn, tid) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	if (diswsi(local_conn, sig) != DIS_SUCCESS)
		return TM_ENOTCONNECTED;
	dis_flush(local_conn);
	add_event(*event, tp->t_node, TM_SIGNAL, NULL);
	return TM_SUCCESS;
}

/**
 * @brief
 *	-Returns an event that can be used to learn when a task
 *	dies.
 *
 * @param[in] tid - task id
 * @param[out] obitval	- obit value
 * @param[out]  event - event handle
 *
 * @return      int
 * @retval      TM_SUCCESS      Success
 * @retval      TM_ER*          error
 *
 */
int
tm_obit(tm_task_id tid, int *obitval, tm_event_t *event)
{
	task_info	*tp;

	if (!init_done)
		return TM_BADINIT;
	if ((tp = find_task(tid)) == NULL)
		return TM_ENOTFOUND;
	*event = new_event();
	if (startcom(TM_OBIT, *event) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswsi(local_conn, tp->t_node) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswui(local_conn, tid) != DIS_SUCCESS)
		return TM_ESYSTEM;
	dis_flush(local_conn);
	add_event(*event, tp->t_node, TM_OBIT, (void *)obitval);
	return TM_SUCCESS;
}

struct	taskhold {
	tm_task_id	*list;
	int		size;
	int		*ntasks;
};

/**
 * @brief
 *	-Makes a request for the list of tasks on <node>.  If <node>
 *	is a valid node number, it returns the event that the list of
 *	tasks on <node> is available.
 *
 * @param[in] node - job relative node id
 * @param[out] tid_list - pointer to task list
 * @param[in] list_size - size of the task list
 * @param[out] ntasks - number of tasks
 * @param[out] event - pointer to event list
 *
 * @return	int
 * @retval	TM_SUCCESS	success
 * @retval	TM_ER*		error
 *
 */
int
tm_taskinfo(tm_node_id node, tm_task_id *tid_list,
		int list_size, int *ntasks, tm_event_t *event)
{
	struct	taskhold	*thold;

	if (!init_done)
		return TM_BADINIT;
	if (tid_list == NULL || list_size == 0 || ntasks == NULL)
		return TM_EBADENVIRONMENT;
	*event = new_event();
	if (startcom(TM_TASKS, *event) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswsi(local_conn, node) != DIS_SUCCESS)
		return TM_ESYSTEM;
	dis_flush(local_conn);

	thold = (struct taskhold *)malloc(sizeof(struct taskhold));
	assert(thold != NULL);
	thold->list = tid_list;
	thold->size = list_size;
	thold->ntasks = ntasks;
	add_event(*event, node, TM_TASKS, (void *)thold);
	return TM_SUCCESS;
}

/**
 * @brief
 *	-Returns the job-relative node number that holds or held <tid>.  In
 *	case of an error, it returns TM_ERROR_NODE.
 *
 * @param[in] tid - task id
 * @param[out] node - job relative node
 *
 * @return	int
 * @retval      TM_SUCCESS      success
 * @retval      TM_ER*          error
 *
 */
int
tm_atnode(tm_task_id tid, tm_node_id *node)
{
	task_info	*tp;

	if (!init_done)
		return TM_BADINIT;
	if ((tp = find_task(tid)) == NULL)
		return TM_ENOTFOUND;
	*node = tp->t_node;
	return TM_SUCCESS;
}

struct	reschold {
	char	*resc;
	int	len;
};

/**
 * @brief
 *	-Makes a request for a string specifying the resources
 *	available on <node>.  If <node> is a valid node number, it
 *	returns the event that the string specifying the resources on
 *	<node> is available.  It returns ERROR_EVENT otherwise.
 *
 * @param[in] node - job relative node
 * @param[out] resource - resource avlbl on node
 * @param[in] len - length of string
 * @param[out] event - pointer to event info
 *
 * @return      int
 * @retval      TM_SUCCESS      success
 * @retval      TM_ER*          error
 *
 */
int
tm_rescinfo(tm_node_id node, char *resource, int len, tm_event_t *event)
{
	struct	reschold	*rhold;

	if (!init_done)
		return TM_BADINIT;
	if (resource == NULL || len == 0)
		return TM_EBADENVIRONMENT;
	*event = new_event();
	if (startcom(TM_RESOURCES, *event) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswsi(local_conn, node) != DIS_SUCCESS)
		return TM_ESYSTEM;
	dis_flush(local_conn);

	rhold = (struct reschold *)malloc(sizeof(struct reschold));
	assert(rhold != NULL);
	rhold->resc = resource;
	rhold->len = len;

	add_event(*event, node, TM_RESOURCES, (void *)rhold);
	return TM_SUCCESS;
}

/**
 * @brief
 *	-Posts the first <nbytes> of a copy of *<info> within MOM on
 *	this node, and associated with this task.  If <info> is
 *	non-NULL, it returns the event that the effort to post *<info>
 *	is complete.  It returns ERROR_EVENT otherwise.
 *
 * @param[in] name - name of mom
 * @param[in] info - information (event)
 * @param[in] len - length of info
 * @param[out] event - pointer to event info
 *
 * @return      int
 * @retval      TM_SUCCESS (0)  - Success
 * @retval      TM_E*      () - Failure
 *
 */
int
tm_publish(char *name, void *info, int len, tm_event_t *event)
{

	if (!init_done)
		return TM_BADINIT;
	*event = new_event();
	if (startcom(TM_POSTINFO, *event) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswst(local_conn, name) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswcs(local_conn, info, len) != DIS_SUCCESS)
		return TM_ESYSTEM;

	dis_flush(local_conn);
	add_event(*event, TM_ERROR_NODE, TM_POSTINFO, NULL);
	return TM_SUCCESS;
}

struct	infohold {
	void	*info;
	int	len;
	int	*info_len;
};

/**
 * @brief
 *	Makes a request for a copy of the info posted by <tid>.  If
 *	<tid> is a valid task, it returns the event that the
 *	string specifying the info posted by <tid> is available.
 *
 * @param[in] tid - task id
 * @param[in] name - name of
 * @param[out] info - event info
 * @param[in] len -length of info
 * @param[out] info_len - info len to be output
 * @param[out] event - handle to event
 *
 * @return      int
 * @retval      0       success
 * @retval      !0      error
 *
 */
int
tm_subscribe(tm_task_id tid, char *name, void *info, int len, int *info_len, tm_event_t *event)
{
	task_info		*tp;
	struct	infohold	*ihold;

	if (!init_done)
		return TM_BADINIT;
	if ((tp = find_task(tid)) == NULL)
		return TM_ENOTFOUND;
	*event = new_event();
	if (startcom(TM_GETINFO, *event) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswsi(local_conn, tp->t_node) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswui(local_conn, tid) != DIS_SUCCESS)
		return TM_ESYSTEM;
	if (diswst(local_conn, name) != DIS_SUCCESS)
		return TM_ESYSTEM;
	dis_flush(local_conn);

	ihold = (struct infohold *)malloc(sizeof(struct infohold));
	assert(ihold != NULL);
	ihold->info = info;
	ihold->len = len;
	ihold->info_len = info_len;

	add_event(*event, tp->t_node, TM_GETINFO, (void *)ihold);
	return TM_SUCCESS;
}

/**
 * @brief
 *	-tm_finalize() - close out task manager interface
 *
 * @par	Note:
 *	This function should be the last one called.  It is illegal to call
 *	any other task manager function following this one.   All events are
 *	freed and any connection to the task manager (pbs_mom) is closed.
 *	This call is synchronous.
 *
 * @return      int
 * @retval	0	success
 * @retval	!0	error
 *
 */
int
tm_finalize()
{
	event_info	*e;
	int		 i = 0;

	if (!init_done)
		return TM_BADINIT;
	while (event_count && (i < EVENT_HASH)) {
		while ((e = event_hash[i]) != NULL) {
			del_event(e);
		}
		++i;	/* check next slot in hash table */
	}
	init_done = 0;
	free(tm_jobid);
	tm_jobid = NULL;
	tm_jobid_len = 0;
	free(tm_jobcookie);
	tm_jobcookie = NULL;
	tm_jobcookie_len = 0;
	return TM_SUCCESS;	/* what else */
}

/**
 * @brief
 *	-tm_notify() - set the signal to be sent on event arrival.
 *
 * @param[in] tm_signal - signal number
 *
 * @return      int
 * @retval      TM_ENOTIMPLEMENTED      Success
 * @retval      TM_BADINIT              error
 *
 */
int
tm_notify(int tm_signal)
{
	if (!init_done)
		return TM_BADINIT;
	return TM_ENOTIMPLEMENTED;
}

/**
 * @brief
 *	-tm_alloc() - make a request for additional resources.
 *
 * @param[in] resources - resource list
 * @param[in] event - event handle
 *
 * @return      int
 * @retval      TM_ENOTIMPLEMENTED      Success
 * @retval      TM_BADINIT              error
 *
 */
int
tm_alloc(char *resources, tm_event_t *event)
{
	if (!init_done)
		return TM_BADINIT;
	return TM_ENOTIMPLEMENTED;
}

/**
 * @brief
 *	-tm_dealloc() - drop a node from the job.
 *
 * @param[in] node - job relative node
 * @param[in] event - event handle
 *
 * @return      int
 * @retval      TM_ENOTIMPLEMENTED      Success
 * @retval      TM_BADINIT              error
 *
 */
int
tm_dealloc(tm_node_id node, tm_event_t *event)
{
	if (!init_done)
		return TM_BADINIT;
	return TM_ENOTIMPLEMENTED;
}

/**
 * @brief
 *	-tm_create_event() - create a persistent event.
 *
 * @param[in] event - event handle
 *
 * @return      int
 * @retval      TM_ENOTIMPLEMENTED      Success
 * @retval      TM_BADINIT              error
 *
 */
int
tm_create_event(tm_event_t *event)
{
	if (!init_done)
		return TM_BADINIT;
	return TM_ENOTIMPLEMENTED;
}

/**
 * @brief
 *	-tm_destroy_event() - destroy a persistent event.
 *
 * @param[in] event - event handle
 *
 * @return      int
 * @retval      TM_ENOTIMPLEMENTED      Success
 * @retval      TM_BADINIT              error
 *
 */
int
tm_destroy_event(tm_event_t *event)
{
	if (!init_done)
		return TM_BADINIT;
	return TM_ENOTIMPLEMENTED;
}

/**
 * @brief
 *	-tm_register() - link a persistent event with action requests
 *	from the task manager.
 *
 * @param[in] what - info about last event polled
 * @param[in] event - event handle
 *
 * @return      int
 * @retval      TM_ENOTIMPLEMENTED	Success
 * @retval      TM_BADINIT		error
 *
 */
int
tm_register(tm_whattodo_t *what, tm_event_t *event)
{
	if (!init_done)
		return TM_BADINIT;
	return TM_ENOTIMPLEMENTED;
}

#define	FOREVER	2147000
/**
 * @brief
 *	-tm_poll - poll to see if an event has been completed.
 *
 * @par Note:
 *	If "poll_event" is a valid event handle, see if it is completed;
 *	else if "poll_event" is the null event, check for the first event that
 *	is completed.
 *
 * @par	Functionality:
 *	result_event is set to the completed event or the null event.
 *
 *	If wait is non_zero, wait for "poll_event" to be completed.
 *
 *	If an error ocurs, set tm_errno non-zero.
 *
 * @param[in] poll_event - event handle
 * @param[in] result_event - event handle to output
 * @param[in] wait - indiacation for wait
 * @param[in] tm_errno - error number
 *
 * @return      int
 * @retval      TM_SUCCESS (0)  - Success
 * @retval      TM_E*       	- Failure
 *
 */
int
tm_poll(tm_event_t poll_event, tm_event_t *result_event, int wait, int *tm_errno)
{
	int		num, i;
	int		ret, mtype, nnodes;
	int		prot, protver;
	int		*obitvalp;
	int		count = 0;
	event_info	*ep = NULL;
	tm_task_id	tid, *tidp;
	tm_event_t	nevent;
	tm_node_id	node;
	char		*jobid;
	char		*info;
	size_t		 rdsize;
	struct tm_roots	*roots;
	struct taskhold	*thold;
	struct infohold	*ihold;
	struct reschold	*rhold;

	if (!init_done)
		return TM_BADINIT;
	if (result_event == NULL)
		return TM_EBADENVIRONMENT;
	*result_event = TM_ERROR_EVENT;
	if (poll_event != TM_NULL_EVENT)
		return TM_ENOTIMPLEMENTED;
	if (tm_errno == NULL)
		return TM_EBADENVIRONMENT;

	if (event_count == 0) {
		DBPRT(("%s: no events waiting\n", __func__))
		return TM_ENOTFOUND;
	}
	if (local_conn < 0) {
		DBPRT(("%s: INTERNAL ERROR %d events but no connection\n",
			__func__, event_count))
		return TM_ENOTCONNECTED;
	}

	/*
	 ** Setup tcp dis routines with a wait value appropriate for
	 ** the value of wait the user set.
	 */
	pbs_tcp_timeout = wait ? FOREVER : 0;
	DIS_tcp_funcs();

	prot = disrsi(local_conn, &ret);
	if (ret == DIS_EOD) {
		*result_event = TM_NULL_EVENT;
		return TM_SUCCESS;
	}
	else if (ret != DIS_SUCCESS) {
		DBPRT(("%s: protocol number dis error %d\n", __func__, ret))
		goto err;
	}
	if (prot != TM_PROTOCOL) {
		DBPRT(("%s: bad protocol number %d\n", __func__, prot))
		goto err;
	}

	/*
	 ** We have seen the start of a message.  Set the timeout value
	 ** so we wait for the remaining data of a message.
	 */
	pbs_tcp_timeout = FOREVER;
	protver = disrsi(local_conn, &ret);
	if (ret != DIS_SUCCESS) {
		DBPRT(("%s: protocol version dis error %d\n", __func__, ret))
		goto err;
	}
	if (protver != TM_PROTOCOL_VER) {
		DBPRT(("%s: bad protocol version %d\n", __func__, protver))
		goto err;
	}

	mtype = disrsi(local_conn, &ret);
	if (ret != DIS_SUCCESS) {
		DBPRT(("%s: mtype dis error %d\n", __func__, ret))
		goto err;
	}
	nevent = disrsi(local_conn, &ret);
	if (ret != DIS_SUCCESS) {
		DBPRT(("%s: event dis error %d\n", __func__, ret))
		goto err;
	}

	*result_event = nevent;
	DBPRT(("%s: got event %d return %d\n", __func__, nevent, mtype))
	if ((ep = find_event(nevent)) == NULL) {
		DBPRT(("%s: No event found for number %d\n", __func__, nevent));
		CS_close_socket(local_conn);
		closesocket(local_conn);
		local_conn = -1;
		return TM_ENOEVENT;
	}

	if (mtype == TM_ERROR) {	/* problem, read error num */
		*tm_errno = disrsi(local_conn, &ret);
		DBPRT(("%s: event %d error %d\n", __func__, nevent, *tm_errno));
		goto done;
	}

	*tm_errno = TM_SUCCESS;
	switch (ep->e_mtype) {

			/*
			 **	auxiliary info (
			 **		number of nodes	int;
			 **		nodeid[0]	int;
			 **		...
			 **		nodeid[n-1]	int;
			 **		parent jobid	string;
			 **		parent nodeid	int;
			 **		parent taskid	int;
			 **	)
			 */
		case TM_INIT:
			nnodes = disrsi(local_conn, &ret);
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: INIT failed nnodes\n", __func__))
				goto err;
			}

			node_table = (tm_node_id *)calloc(nnodes+1,
				sizeof(tm_node_id));
			if (node_table == NULL)
				goto err;
			DBPRT(("%s: INIT nodes %d\n", __func__, nnodes))
			for (i=0; i<nnodes; i++) {
				node_table[i] = disrsi(local_conn, &ret);
				if (ret != DIS_SUCCESS) {
					DBPRT(("%s: INIT failed nodeid %d\n", __func__, i))
					goto err;
				}
			}
			node_table[nnodes] = TM_ERROR_NODE;

			jobid = disrst(local_conn, &ret);
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: INIT failed jobid\n", __func__))
				goto err;
			}
			DBPRT(("%s: INIT daddy jobid %s\n", __func__, jobid))
			node = disrsi(local_conn, &ret);
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: INIT failed parent nodeid\n", __func__))
				goto err;
			}
			DBPRT(("%s: INIT daddy node %d\n", __func__, node))
			tid = disrui(local_conn, &ret);
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: INIT failed parent taskid\n", __func__))
				goto err;
			}
			DBPRT(("%s: INIT daddy tid %lu\n", __func__, (unsigned long)tid))

			roots = (struct tm_roots *)ep->e_info;
			roots->tm_parent = new_task(jobid, node, tid);
			roots->tm_me = new_task(tm_jobid,
				tm_jobndid,
				tm_jobtid);
			roots->tm_nnodes = nnodes;
			roots->tm_ntasks = 0;		/* TODO */
			roots->tm_taskpoolid = -1;	/* what? */
			roots->tm_tasklist = NULL;	/* TODO */

			break;

		case TM_TASKS:
			thold = (struct taskhold *)ep->e_info;
			tidp = thold->list;
			num = thold->size;
			for (i=0;; i++) {
				tid = disrui(local_conn, &ret);
				if (tid == TM_NULL_TASK)
					break;
				if (ret != DIS_SUCCESS)
					goto err;
				if (i < num) {
					tidp[i] = new_task(tm_jobid,
						ep->e_node, tid);
				}
			}
			if (i < num)
				tidp[i] = TM_NULL_TASK;
			*(thold->ntasks) = i;
			break;

		case TM_SPAWN:
		case TM_ATTACH:
			tid = disrui(local_conn, &ret);
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: SPAWN failed tid\n", __func__))
				goto err;
			}
			tidp = (tm_task_id *)ep->e_info;
			*tidp = new_task(tm_jobid, ep->e_node, tid);
			break;

		case TM_SPAWN_MULTI:
			/*
			** read (
			** 	int		count
			** 	tm_task_id	taskid 0
			** 	tm_node_id	node 0
			**	...
			** 	tm_task_id	taskid count-1
			** 	tm_node_id	node count-1
			** )
			*/
			count = disrui(local_conn, &ret);
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: SPAWN_MULTI failed count\n", __func__))
				goto err;
			}
			for (i = 0; i < count; i++) {
				/*
				** Read a task ID and node ID for each
				** task that was spawned
				*/
				tid = disrui(local_conn, &ret);
				if (ret != DIS_SUCCESS) {
					DBPRT(("%s: SPAWN_MULTI failed tid\n", __func__))
					goto err;
				}

				node = disrui(local_conn, &ret);
				if (ret != DIS_SUCCESS) {
					DBPRT(("%s: SPAWN_MULTI failed node\n", __func__))
					goto err;
				}
				tidp = ((tm_task_id *)ep->e_info)+i;
				*tidp = new_task(tm_jobid, node, tid);
			}
			break;

		case TM_SIGNAL:
			break;

		case TM_OBIT:
			obitvalp = (int *)ep->e_info;
			*obitvalp = disrsi(local_conn, &ret);
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: OBIT failed obitval\n", __func__))
				goto err;
			}
			break;

		case TM_POSTINFO:
			break;

		case TM_GETINFO:
			ihold = (struct infohold *)ep->e_info;
			info = disrcs(local_conn, &rdsize, &ret);
			/* save the returned length to return to user in an int, */
			/* truncation is not an issue because the length of the  */
			/* message  published lenght  must fit in a "int"        */
			*ihold->info_len = (int)rdsize;
			if (ret != DIS_SUCCESS) {
				DBPRT(("%s: GETINFO failed info\n", __func__))
				break;
			}
			memcpy(ihold->info, info, MIN(*ihold->info_len, ihold->len));
			free(info);
			break;

		case TM_RESOURCES:
			rhold = (struct reschold *)ep->e_info;
			info = disrst(local_conn, &ret);
			if (ret != DIS_SUCCESS)
				break;
			strncpy(rhold->resc, info, rhold->len);
			free(info);
			break;

		default:
			DBPRT(("%s: unknown event command %d\n", __func__, ep->e_mtype))
			goto err;
	}
done:
	del_event(ep);
	return TM_SUCCESS;

err:
	if (ep)
		del_event(ep);
	CS_close_socket(local_conn);
	closesocket(local_conn);
	local_conn = -1;
	return TM_ENOTCONNECTED;
}
