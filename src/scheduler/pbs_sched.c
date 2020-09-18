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
 * contains functions related to PBS scheduler
 */
#include <pbs_config.h> /* the master config generated by configure */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef _POSIX_MEMLOCK
#include <sys/mman.h>
#endif /* _POSIX_MEMLOCK */

#if defined(FD_SET_IN_SYS_SELECT_H)
#include <sys/select.h>
#endif
#include <sys/resource.h>

#include "auth.h"
#include "config.h"
#include "fifo.h"
#include "globals.h"
#include "libpbs.h"
#include "libsec.h"
#include "list_link.h"
#include "log.h"
#include "multi_threading.h"
#include "net_connect.h"
#include "pbs_ecl.h"
#include "pbs_error.h"
#include "pbs_ifl.h"
#include "pbs_share.h"
#include "pbs_undolr.h"
#include "pbs_version.h"
#include "portability.h"
#include "rm.h"
#include "misc.h"
#include "sched_cmds.h"
#include "server_limits.h"
#include "tpp.h"

#define START_CLIENTS 2	     /* minimum number of clients */
pbs_net_t *okclients = NULL; /* accept connections from */
int numclients = 0;	     /* the number of clients */
char *configfile = NULL;     /* name of file containing
						 client names to be added */

extern char *msg_daemonname;
char **glob_argv;
char usage[] = "[-d home][-L logfile][-p file][-I schedname][-n][-N][-c clientsfile][-t num threads]";
struct sockaddr_in saddr;
sigset_t allsigs;

/* if we received a sigpipe, this probably means the server went away. */
int got_sigpipe = 0;

/* used in segv restart */
time_t segv_start_time;
time_t segv_last_time;

#ifdef NAS /* localmod 030 */
extern int do_soft_cycle_interrupt;
extern int do_hard_cycle_interrupt;
#endif /* localmod 030 */

extern char *msg_startup1;

static pthread_mutex_t cleanup_lock;

static void close_servers();
static void reconnect_server(sched_svrconn *);

/**
 * @brief
 * 		cleanup after a segv and re-exec.  Trust as little global mem
 * 		as possible... we don't know if it could be corrupt
 *
 * @param[in]	sig	-	signal
 */
void
on_segv(int sig)
{
	int ret_lock = -1;

	/* We want any other threads to block here, we want them alive until abort() is called
	 * as it dumps core for all threads
	 */
	ret_lock = pthread_mutex_lock(&cleanup_lock);
	if (ret_lock != 0)
		pthread_exit(NULL);

	/* we crashed less then 5 minutes ago, lets not restart ourself */
	if ((segv_last_time - segv_start_time) < 300) {
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__,
				"received a sigsegv within 5 minutes of start: aborting.");

		/* Not unlocking mutex on purpose, we need to hold on to it until the process is killed */
		abort();
	}

	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__,
			"received segv and restarting");

	if (fork() > 0) { /* the parent rexec's itself */
		sleep(10); /* allow the child to die */
		execv(glob_argv[0], glob_argv);
		exit(3);
	} else {
		abort(); /* allow to core and exit */
	}
}

/**
 * @brief
 * 		signal function for receiving a sigpipe - set flag so we know not to talk
 * 		to the server any more and leave the cycle as soon as possible
 *
 * @param[in]	sig	-	sigpipe
 */
void
sigfunc_pipe(int sig)
{
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, "sigfunc_pipe", "We've received a sigpipe: The server probably died.");
	got_sigpipe = 1;
}


/**
 * @brief
 *       Clean up after a signal.
 *
 *  @param[in]	sig	-	signal
 */
void
die(int sig)
{
	int ret_lock = -1;

	ret_lock = pthread_mutex_trylock(&cleanup_lock);
	if (ret_lock != 0)
		pthread_exit(NULL);

	if (sig > 0)
		log_eventf(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, "caught signal %d", sig);
	else
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, "abnormal termination");

	close_servers();
	schedexit();

	{
		int csret;
		if ((csret = CS_close_app()) != CS_SUCCESS) {
			/*had some problem closing the security library*/

			sprintf(log_buffer, "problem closing security library (%d)", csret);
			log_err(-1, "pbs_sched", log_buffer);
		}
	}

	unload_auths();

	log_close(1);
	exit(1);
}

/**
 * @brief
 * 		add a new client to the list of clients.
 *
 * @param[in]	name	-	Client name.
 */
int
addclient(char *name)
{
	int	i;
	struct	hostent		*host, *gethostbyname();
	struct  in_addr saddr;

	if ((host = gethostbyname(name)) == NULL) {
		sprintf(log_buffer, "host %s not found", name);
		log_err(-1, __func__, log_buffer);
		return -1;
	}

	for (i = 0; host->h_addr_list[i]; i++) {
		if (numclients >= START_CLIENTS) {
			pbs_net_t	*newclients;

			newclients = realloc(okclients,
				sizeof(pbs_net_t)*(numclients+1));
			if (newclients == NULL)
				return -1;
			okclients = newclients;
		}
		memcpy((char *)&saddr, host->h_addr_list[i], host->h_length);
		okclients[numclients++] = saddr.s_addr;
	}
	return 0;
}

/**
 * @brief
 * 		read_config - read and process the configuration file (see -c option)
 * @par
 *		Currently, the only statement is $clienthost to specify which systems
 *		can contact the scheduler.
 *
 * @param[in]	file	-	configuration file
 *
 * @return	int
 * @retval	0	: Ok
 * @retval	-1	: !nOtOk!
 */
#define CONF_LINE_LEN 120

static
int
read_config(char *file)
{
	FILE	*conf;
	int	i;
	char	line[CONF_LINE_LEN];
	char	*token;
	struct	specialconfig {
		char	*name;
		int	(*handler)();
	} special[] = {
		{"clienthost",	addclient },
		{ NULL,		NULL }
	};


#if !defined(DEBUG) && !defined(NO_SECURITY_CHECK)
	if (chk_file_sec_user(file, 0, 0, S_IWGRP|S_IWOTH, 1, getuid()))
		return (-1);
#endif

	if ((conf = fopen(file, "r")) == NULL) {
		log_err(errno, __func__, "cannot open config file");
		return (-1);
	}
	while (fgets(line, CONF_LINE_LEN, conf)) {

		if ((line[0] == '#') || (line[0] == '\n'))
			continue;		/* ignore comment & null line */
		else if (line[0] == '$') {	/* special */

			if ((token = strtok(line, " \t")) == NULL)
				token = "";
			for (i=0; special[i].name; i++) {
				if (strcmp(token+1, special[i].name) == 0)
					break;
			}
			if (special[i].name == NULL) {
				sprintf(log_buffer, "config name %s not known",
					token);
				log_record(PBSEVENT_ERROR,
					PBS_EVENTCLASS_SERVER, LOG_INFO,
					msg_daemonname, log_buffer);
				continue;
			}
			token = strtok(NULL, " \t");
			if (*(token+strlen(token)-1) == '\n')
				*(token+strlen(token)-1) = '\0';
			if (special[i].handler(token)) {
				fclose(conf);
				return (-1);
			}

		} else {
			log_record(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER,
				LOG_INFO, msg_daemonname,
				"invalid line in config file");
			fclose(conf);
			return (-1);
		}
	}
	fclose(conf);
	return (0);
}
/**
 * @brief
 * 		restart on signal
 *
 * @param[in]	sig	-	signal
 */
void
restart(int sig)
{
	sched_svrconn sconn = {NULL, -1, -1};
	sched_cmd cmd = {SCH_CONFIGURE, NULL, -1};

	if (sig) {
		log_close(1);
		log_open(logfile, path_log);
		sprintf(log_buffer, "restart on signal %d", sig);
	} else {
		sprintf(log_buffer, "restart command");
	}
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, log_buffer);
	if (configfile) {
		if (read_config(configfile) != 0)
			die(0);
	}
	schedule(&sconn, &cmd);
}

#ifdef NAS /* localmod 030 */
/**
 * @brief
 * 		make soft cycle interrupt active
 *
 * @param[in]	sig	-	signal
 */
void
soft_cycle_interrupt(int sig)
{
	do_soft_cycle_interrupt = 1;
}
/**
 * @brief
 * 		make hard cycle interrupt active
 *
 * @param[in]	sig	-	signal
 */
void
hard_cycle_interrupt(int sig)
{
	do_hard_cycle_interrupt = 1;
}
#endif /* localmod 030 */
/**
 * @brief
 * 		log the bad connection message
 *
 * @param[in]	msg	-	The message to be logged.
 */
void
badconn(char *msg)
{
	struct in_addr addr;
	char buf[5 * sizeof(addr) + 100];
	struct hostent *phe;

	addr = saddr.sin_addr;
	phe = gethostbyaddr((void *) &addr, sizeof(addr), AF_INET);
	if (phe == NULL) {
		char hold[6];
		int i;
		union {
			struct in_addr aa;
			u_char bb[sizeof(addr)];
		} uu;

		uu.aa = addr;
		sprintf(buf, "%u", (unsigned int) uu.bb[0]);
		for (i = 1; i < sizeof(addr); i++) {
			sprintf(hold, ".%u", (unsigned int) uu.bb[i]);
			strcat(buf, hold);
		}
	} else
		pbs_strncpy(buf, phe->h_name, sizeof(buf));

	log_errf(-1, __func__, "%s on port %u %s", buf, (unsigned int) ntohs(saddr.sin_port), msg);
	return;
}

/**
 * @brief
 * 		lock_out - lock out other daemons from this directory.
 *
 * @param[in]	fds	-	file descriptor
 * @param[in]	op	-	F_WRLCK  or  F_UNLCK
 *
 * @return	1
 */

static void
lock_out(int fds, int op)
{
	struct flock flock;

	(void)lseek(fds, (off_t)0, SEEK_SET);
	flock.l_type   = op;
	flock.l_whence = SEEK_SET;
	flock.l_start  = 0;
	flock.l_len    = 0;	/* whole file */
	if (fcntl(fds, F_SETLK, &flock) < 0) {
		log_err(errno, msg_daemonname, "another scheduler running");
		fprintf(stderr, "pbs_sched: another scheduler running\n");
		exit(1);
	}
}

/**
 * @brief
 * 		are_we_primary - are we on the primary Server host
 *		If either the only configured Server or the Primary in a failover
 *		configuration - return true
 *
 * @return	int
 * @retval	0	: we are the secondary
 * @retval	-1	: cannot be neither
 * @retval	1	: we are the listed primary
 */
static int
are_we_primary()
{
	char server_host[PBS_MAXHOSTNAME+1];
	char hn1[PBS_MAXHOSTNAME+1];

	if (pbs_conf.pbs_leaf_name) {
		char *endp;
		snprintf(server_host, sizeof(server_host), "%s", pbs_conf.pbs_leaf_name);
		endp = strchr(server_host, ','); /* find the first name */
		if (endp)
			*endp = '\0';
		endp = strchr(server_host, ':'); /* cut out the port */
		if (endp)
			*endp = '\0';
	} else if ((gethostname(server_host, (sizeof(server_host) - 1)) == -1) ||
		(get_fullhostname(server_host, server_host, (sizeof(server_host) - 1)) == -1)) {
		log_err(-1, __func__, "Unable to get my host name");
		return -1;
	}

	/* both secondary and primary should be set or neither set */
	if ((pbs_conf.pbs_secondary == NULL) && (pbs_conf.pbs_primary == NULL))
		return 1;
	if ((pbs_conf.pbs_secondary == NULL) || (pbs_conf.pbs_primary == NULL))
		return -1;

	if (get_fullhostname(pbs_conf.pbs_primary, hn1, (sizeof(hn1) - 1))==-1) {
		log_err(-1, __func__, "Unable to get full host name of primary");
		return -1;
	}

	if (strcmp(hn1, server_host) == 0)
		return 1;	/* we are the listed primary */

	if (get_fullhostname(pbs_conf.pbs_secondary, hn1, (sizeof(hn1) - 1))==-1) {
		log_err(-1, __func__, "Unable to get full host name of secondary");
		return -1;
	}
	if (strcmp(hn1, server_host) == 0)
		return 0;	/* we are the secondary */

	return -1;		/* cannot be neither */
}

/**
 * @brief close connections to given server and free its struct
 *
 * @param[in] - sconn - pointer to server struct to close
 *
 * @return void
 */
static void
close_server(sched_svrconn *sconn)
{
	if (sconn) {
		remove_ptr_from_array(servers, sconn);
		if (count_array(servers) == 0) {
			free(servers);
			servers = NULL;
		}
		tpp_em_del_fd(poll_context, sconn->secondary_sock);
		pbs_disconnect(sconn->primary_sock);
		pbs_disconnect(sconn->secondary_sock);
		log_eventf(PBSEVENT_ADMIN | PBSEVENT_FORCE, PBS_EVENTCLASS_SCHED, LOG_INFO,
			   msg_daemonname, "Disconnected from server %s", sconn->svrhost);
		free(sconn->svrhost);
		free(sconn);
	}
}

/**
 * @brief close connections to all servers
 *
 * @return void
 */
static void
close_servers(void)
{
	while (count_array(servers))
		close_server(servers[0]);
	if (poll_context) {
		tpp_em_destroy(poll_context);
		poll_context = NULL;
	}
	while (!ds_queue_is_empty(sched_cmds)) {
		sched_cmd *cmd = ds_dequeue(sched_cmds);
		free_sched_cmd(cmd);
	}
	free_ds_queue(sched_cmds);
	sched_cmds = NULL;
}

/**
 * @brief connect to given host and send register sched request on it
 *        and wait for server reply
 *
 * @param[in] - host       - address of server in host[:port] format
 * @param[in] - is_primary - is primary connection or secondary connection
 *                           0 - secondary else primary
 *
 * @return int
 * @retval -1  - failure
 * @return !-1 - success, and opened connection sock
 */
static int
connect_server_helper(char *host, int is_primary)
{
	int rc;
	int sock = -1;
	struct batch_reply *reply = NULL;

	sock = pbs_connect(host);
	if (sock < 0)
		goto rerr;
	rc = encode_DIS_ReqHdr(sock, PBS_BATCH_RegisterSched, pbs_current_user);
	if (rc != DIS_SUCCESS)
		goto rerr;
	rc = diswst(sock, sc_name);
	if (rc != DIS_SUCCESS)
		goto rerr;
	rc = encode_DIS_ReqExtend(sock, NULL);
	if (rc != DIS_SUCCESS)
		goto rerr;
	if (dis_flush(sock) != 0)
		goto rerr;
	pbs_errno = 0;
	reply = PBSD_rdrpy(sock);
	if (reply == NULL)
		goto rerr;
	if (pbs_errno != 0) {
		char *errmsg = get_conn_errtxt(sock);
		if (errmsg) {
			log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE, PBS_EVENTCLASS_SCHED, LOG_NOTICE,
				   msg_daemonname, "Server rejected register request for %s connection with error: %s",
				   is_primary ? "primary" : "secondary", errmsg);
			die(-1);
		}
		goto rerr;
	}

	PBSD_FreeReply(reply);
	return sock;

rerr:
	pbs_disconnect(sock);
	PBSD_FreeReply(reply);
	return -1;
}

/**
 * @brief connect to given server, create and add server struct in servers list
 *        also add secondary connection to poll list
 *
 * @param[in] svrhost - address of server in host[:port] format
 *
 * @return void
 */
static void
connect_server(char *svrhost)
{
	int primary_sock = -1;
	int secondary_sock = -1;
	sched_svrconn *svr;
	void *svrs_newarr;

	if (svrhost == NULL)
		return;

	while (primary_sock < 0 || secondary_sock < 0) {
		if (primary_sock < 0) {
			primary_sock = connect_server_helper(svrhost, 1);
			if (primary_sock < 0) {
				/* wait for 2s for not to burn too much CPU, and then retry connection */
				sleep(2);
				continue;
			}
		}
		secondary_sock = connect_server_helper(svrhost, 0);
		if (secondary_sock < 0) {
			/* wait for 2s for not to burn too much CPU, and then retry connection */
			sleep(2);
			continue;
		}
	}
	if (tpp_em_add_fd(poll_context, secondary_sock, EM_IN | EM_HUP | EM_ERR) < 0) {
		log_errf(errno, __func__, "Couldn't add secondary connection to poll list for server %s", svrhost);
		pbs_disconnect(primary_sock);
		pbs_disconnect(secondary_sock);
		die(-1);
	}

	svr = malloc(sizeof(sched_svrconn));
	if (!svr) {
		log_err(errno, __func__, MEM_ERR_MSG);
		die(-1);
	}

	svr->svrhost = strdup(svrhost);
	if (!svr->svrhost) {
		free(svr);
		log_err(errno, __func__, MEM_ERR_MSG);
		die(-1);
	}
	svr->primary_sock = primary_sock;
	svr->secondary_sock = secondary_sock;
	svrs_newarr = add_ptr_to_array(servers, svr);
	if (svrs_newarr == NULL) {
		close_server(svr);
		die(-1);
	}
	servers = (sched_svrconn **)svrs_newarr;
	log_eventf(PBSEVENT_ADMIN | PBSEVENT_FORCE, PBS_EVENTCLASS_SCHED, LOG_INFO, msg_daemonname, "Connected to server %s", svrhost);
}

/**
 * @brief Connect to all configured servers and populate servers list with
 *        connections to servers
 *
 * @return void
 */
static void
connect_servers(void)
{
	int max_connection = 1;
	char *svrhost = pbs_default();

	if (poll_context == NULL) {
		poll_context = tpp_em_init(max_connection);
		if (poll_context == NULL) {
			log_err(errno, __func__, "Failed to init cmd connections context");
			die(-1);
		}

		sched_cmds = new_ds_queue();
		if (sched_cmds == NULL) {
			log_errf(errno, __func__, MEM_ERR_MSG);
			die(-1);
		}
	}

	connect_server(svrhost ? svrhost : "");
}

/**
 * @brief reconnect to given server
 *
 * @param[in] sconn - pointer server struct to reconnect
 *
 * @return void
 */
static void
reconnect_server(sched_svrconn *sconn)
{
	char svrhost[PBS_MAXSERVERNAME + PBS_MAXPORTNUM + 2];

	strcpy(svrhost, sconn->svrhost);
	close_server(sconn);
	connect_server(svrhost);
}

/**
 * @brief find server structure based on given secondary connection
 *
 * @param[in] sock - secondary connection to server
 *
 * @return sched_svrconn *
 * @retval NULL  - failure
 * @retval !NULL - success
 */
static sched_svrconn *
find_server(int sock)
{
	int i = 0;
	for (; servers[i] != NULL && servers[i]->secondary_sock == sock; i++)
		return servers[i];
	return NULL;
}

/**
 * @brief read incoming command from given secondary connection
 *        and add it into sched_cmds array
 *
 * @param[in]  sock   - secondary connection to server
 *
 * @return int
 * @retval -2 - failure due to memory operation failed
 * @retval -1 - failure while reading command
 * @return  0 - no cmd, server might have closed connection
 * @return  1 - success, read atleast one command
 */
static int
read_sched_cmd(int sock)
{
	int rc = -1;
	sched_cmd *cmd;

	cmd = new_sched_cmd();
	if (cmd == NULL)
		return -2;

	rc = get_sched_cmd(sock, cmd);
	if (rc != 1) {
		free_sched_cmd(cmd);
		return rc;
	} else {
		sched_cmd *cmd_prio;
		/*
		 * There is possibility that server has sent
		 * priority command after first non-priority command,
		 * while we were in schedule()
		 *
		 * so try read it in non-blocking mode, but don't
		 * return any failure if fails to read, as we have
		 * successfully enqueued first command
		 *
		 * and if we get priority command then just ignore it
		 * since we are not yet in middle of schedule cycle
		 */
		cmd_prio = new_sched_cmd();
		if (cmd_prio != NULL) {
			int rc_prio = get_sched_cmd_noblk(sock, cmd_prio);
			free_sched_cmd(cmd_prio);
			if (rc_prio == -2) {
				free_sched_cmd(cmd);
				return 0;
			}
		}
	}
	if (!ds_enqueue(sched_cmds, cmd)) {
		free_sched_cmd(cmd);
		return -1;
	}
	return rc;
}

/**
 * @brief wait for commands from servers
 *
 * @return void
 */
static void
wait_for_cmds()
{
	int nsocks;
	int i;
	em_event_t *events;
	int err;
	int hascmd = 0;
	sigset_t emptyset;


	while (!hascmd) {
		sigemptyset(&emptyset);
		nsocks = tpp_em_pwait(poll_context, &events, -1, &emptyset);
		err = errno;

		if (nsocks < 0) {
			if (!(err == EINTR || err == EAGAIN || err == 0)) {
				log_errf(err, __func__, " tpp_em_wait() error, errno=%d", err);
				sleep(1); /* wait for 1s for not to burn too much CPU */
			}
			if (err == EINTR) {
				/* wait woke up on signal, allow small window to proccess signal */
				if (sigprocmask(SIG_UNBLOCK, &allsigs, NULL) == -1)
					log_err(errno, __func__, "sigprocmask(UNBLOCK)");
				if (sigprocmask(SIG_BLOCK, &allsigs, NULL) == -1)
					log_err(errno, __func__, "sigprocmask(BLOCK)");
			}
		} else {
			for (i = 0; i < nsocks; i++) {
				int sock = EM_GET_FD(events, i);
				err = read_sched_cmd(sock);
				if (err != 1) {
					/* if memory error ignore, else reconnect server */
					if (err != -2) {
						sched_svrconn *sconn = find_server(sock);
						if (sconn)
							reconnect_server(sconn);
					}
				} else {
					hascmd = 1;
				}
			}
		}
	}

}

/**
 *
 * @brief sends end of cycle indication to the Server
 *
 * @param[in] sconn - connection info to server
 *
 * @return void
 */
static void
send_cycle_end(sched_svrconn *sconn)
{
	static int cycle_end_marker = 0;

	if (diswsi(sconn->secondary_sock, cycle_end_marker) != DIS_SUCCESS) {
		log_eventf(PBSEVENT_SYSTEM | PBSEVENT_FORCE, PBS_EVENTCLASS_SCHED, LOG_ERR, __func__,
			   "Not able to send end of cycle, errno = %d", errno);
		goto err;
	}

	if (dis_flush(sconn->secondary_sock) != 0)
		goto err;

	return;
err:
	reconnect_server(sconn);
}

int
main(int argc, char *argv[])
{
	int go;
	int c;
	int errflg = 0;
	int lockfds;
	pid_t pid;
	char host[PBS_MAXHOSTNAME + 1];
#ifndef DEBUG
	char *dbfile = "sched_out";
#endif
	struct sigaction act;
	sigset_t oldsigs;
	int opt_no_restart = 0;
	time_t now;
	int stalone = 0;
#ifdef _POSIX_MEMLOCK
	int do_mlockall = 0;
#endif /* _POSIX_MEMLOCK */
	int nthreads = -1;
	int num_cores;
	char *endp = NULL;
	pthread_mutexattr_t attr;

	/*the real deal or show version and exit?*/

	PRINT_VERSION_AND_EXIT(argc, argv);

	num_cores = sysconf(_SC_NPROCESSORS_ONLN);

	if(set_msgdaemonname("pbs_sched")) {
		fprintf(stderr, "Out of memory\n");
		return (1);
	}

	if (pbs_loadconf(0) == 0)
		return (1);

	if (validate_running_user(argv[0]) == 0)
		return (1);

	/* disable attribute verification */
	set_no_attribute_verification();

	/* initialize the thread context */
	if (pbs_client_thread_init_thread_context() != 0) {
		fprintf(stderr, "%s: Unable to initialize thread context\n",
			argv[0]);
		return (1);
	}

	set_log_conf(pbs_conf.pbs_leaf_name, pbs_conf.pbs_mom_node_name,
			pbs_conf.locallog, pbs_conf.syslogfac,
			pbs_conf.syslogsvr, pbs_conf.pbs_log_highres_timestamp);

	nthreads = pbs_conf.pbs_sched_threads;

	glob_argv = argv;
	segv_start_time = segv_last_time = time(NULL);

	opterr = 0;
	while ((c = getopt(argc, argv, "lL:NI:d:p:c:nt:")) != EOF) {
		switch (c) {
			case 'l':
#ifdef _POSIX_MEMLOCK
				do_mlockall = 1;
#else
				fprintf(stderr, "-l option - mlockall not supported\n");
#endif	/* _POSIX_MEMLOCK */
				break;
			case 'L':
				logfile = optarg;
				break;
			case 'N':
				stalone = 1;
				break;
			case 'I':
				sc_name = optarg;
				break;
			case 'd':
				if (pbs_conf.pbs_home_path != NULL)
					free(pbs_conf.pbs_home_path);
				pbs_conf.pbs_home_path = optarg;
				break;
			case 'p':
#ifndef DEBUG
				dbfile = optarg;
#endif
				break;
			case 'c':
				configfile = optarg;
				break;
			case 'n':
				opt_no_restart = 1;
				break;
			case 't':
				nthreads = strtol(optarg, &endp, 10);
				if (*endp != '\0') {
					fprintf(stderr, "%s: bad num threads value\n", optarg);
					errflg = 1;
				}
				if (nthreads < 1) {
					fprintf(stderr, "%s: bad num threads value (should be in range 1-99999)\n", optarg);
					errflg = 1;
				}
				if (nthreads > num_cores) {
					fprintf(stderr, "%s: cannot be larger than number of cores %d, using number of cores instead\n",
							optarg, num_cores);
					nthreads = num_cores;
				}
				break;
			default:
				errflg = 1;
				break;
		}
	}

	if (sc_name == NULL) {
		sc_name = PBS_DFLT_SCHED_NAME;
		dflt_sched = 1;
	}

	if (errflg) {
		fprintf(stderr, "usage: %s %s\n", argv[0], usage);
		fprintf(stderr, "       %s --version\n", argv[0]);
		exit(1);
	}

	if (dflt_sched) {
		(void)sprintf(log_buffer, "%s/sched_priv", pbs_conf.pbs_home_path);
	} else {
		(void)sprintf(log_buffer, "%s/sched_priv_%s", pbs_conf.pbs_home_path, sc_name);
	}
#if !defined(DEBUG) && !defined(NO_SECURITY_CHECK)
	c  = chk_file_sec_user(log_buffer, 1, 0, S_IWGRP|S_IWOTH, 1, getuid());
	c |= chk_file_sec(pbs_conf.pbs_environment, 0, 0, S_IWGRP|S_IWOTH, 0);
	if (c != 0) exit(1);
#endif  /* not DEBUG and not NO_SECURITY_CHECK */
	if (chdir(log_buffer) == -1) {
		perror("chdir");
		exit(1);
	}
	if (dflt_sched) {
		(void)sprintf(path_log, "%s/sched_logs", pbs_conf.pbs_home_path);
	} else {
		(void)sprintf(path_log, "%s/sched_logs_%s", pbs_conf.pbs_home_path, sc_name);
	}
	if (log_open(logfile, path_log) == -1) {
		fprintf(stderr, "%s: logfile could not be opened\n", argv[0]);
		exit(1);
	}

	/* The following is code to reduce security risks                */
	/* start out with standard umask, system resource limit infinite */

	umask(022);
	if (setup_env(pbs_conf.pbs_environment)==-1)
		exit(1);
	c = getgid();
	(void)setgroups(1, (gid_t *)&c);	/* secure suppl. groups */

	set_proc_limits(pbs_conf.pbs_core_limit, 0); /* set_proc_limits can call log_record, so call only after opening log file */

	if (gethostname(host, (sizeof(host) - 1)) == -1) {
		log_err(errno, __func__, "gethostname");
		die(0);
	}

	/*Initialize security library's internal data structures*/
	if (load_auths(AUTH_SERVER)) {
		log_err(-1, "pbs_sched", "Failed to load auth lib");
		die(0);
	}

	{
		int	csret;

		/* let Libsec do logging if part of PBS daemon code */
		p_cslog = log_err;

		if ((csret = CS_server_init()) != CS_SUCCESS) {
			sprintf(log_buffer,
				"Problem initializing security library (%d)", csret);
			log_err(-1, "pbs_sched", log_buffer);
			die(0);
		}
	}

	okclients = (pbs_net_t *)calloc(START_CLIENTS, sizeof(pbs_net_t));
	if (okclients == NULL) {
		log_err(errno, __func__, "Unable to allocate memory (malloc error)");
		die(0);
	}
	addclient("localhost");   /* who has permission to call MOM */
	addclient(host);
	if (pbs_conf.pbs_server_name)
		addclient(pbs_conf.pbs_server_name);
	if (pbs_conf.pbs_primary && pbs_conf.pbs_secondary) {
		/* Failover is configured when both primary and secondary are set. */
		addclient(pbs_conf.pbs_primary);
		addclient(pbs_conf.pbs_secondary);
	} else if (pbs_conf.pbs_server_host_name) {
		/* Failover is not configured, but PBS_SERVER_HOST_NAME is. */
		addclient(pbs_conf.pbs_server_host_name);
	}
	if (pbs_conf.pbs_leaf_name)
		addclient(pbs_conf.pbs_leaf_name);

	if (configfile) {
		if (read_config(configfile) != 0)
			die(0);
	}

	if ((c = are_we_primary()) == 1) {
		lockfds = open("sched.lock", O_CREAT|O_WRONLY, 0644);
	} else if (c == 0) {
		lockfds = open("sched.lock.secondary", O_CREAT|O_WRONLY, 0644);
	} else {
		log_err(-1, "pbs_sched", "neither primary or secondary server");
		exit(1);
	}
	if (lockfds < 0) {
		log_err(errno, __func__, "open lock file");
		exit(1);
	}

	if (sigemptyset(&allsigs) == -1) {
		perror("sigemptyset");
		exit(1);
	}
	if (sigprocmask(SIG_SETMASK, &allsigs, NULL) == -1) {	/* unblock */
		perror("sigprocmask");
		exit(1);
	}
	act.sa_flags = 0;
	sigaddset(&allsigs, SIGHUP);    /* remember to block these */
	sigaddset(&allsigs, SIGINT);    /* during critical sections */
	sigaddset(&allsigs, SIGTERM);   /* so we don't get confused */
	sigaddset(&allsigs, SIGUSR1);
	act.sa_mask = allsigs;

	act.sa_handler = restart;       /* do a restart on SIGHUP */
	sigaction(SIGHUP, &act, NULL);

#ifdef PBS_UNDOLR_ENABLED
	extern void catch_sigusr1(int);
	act.sa_handler = catch_sigusr1;
	sigaction(SIGUSR1, &act, NULL);
#endif

#ifdef NAS /* localmod 030 */
	act.sa_handler = soft_cycle_interrupt; /* do a cycle interrupt on */
					       /* SIGUSR1, subject to     */
					       /* configurable parameters */
	sigaction(SIGUSR1, &act, NULL);
	act.sa_handler = hard_cycle_interrupt; /* do a cycle interrupt on */
					       /* SIGUSR2                 */
	sigaction(SIGUSR2, &act, NULL);
#endif /* localmod 030 */

	act.sa_handler = die;           /* bite the biscuit for all following */
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);

	act.sa_handler = sigfunc_pipe;
	sigaction(SIGPIPE, &act, NULL);

	if (!opt_no_restart) {
		act.sa_handler = on_segv;
		sigaction(SIGSEGV, &act, NULL);
		sigaction(SIGBUS, &act, NULL);
	}

#ifndef	DEBUG
	if (stalone != 1) {
		if ((pid = fork()) == -1) {     /* error on fork */
			perror("fork");
			exit(1);
		}
		else if (pid > 0)               /* parent exits */
			exit(0);

		if (setsid() == -1) {
			perror("setsid");
			exit(1);
		}
	}
	lock_out(lockfds, F_WRLCK);
	freopen(dbfile, "a", stdout);
	setvbuf(stdout, NULL, _IOLBF, 0);
	dup2(fileno(stdout), fileno(stderr));
#else
	if (stalone != 1) {
		(void) sprintf(log_buffer, "Debug build does not fork.");
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
				__func__, log_buffer);
	}
	lock_out(lockfds, F_WRLCK);
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
#endif
	pid = getpid();
	daemon_protect(0, PBS_DAEMON_PROTECT_ON);
	freopen("/dev/null", "r", stdin);

	/* write schedulers pid into lockfile */
	(void)ftruncate(lockfds, (off_t)0);
	(void)sprintf(log_buffer, "%ld\n", (long)pid);
	(void)write(lockfds, log_buffer, strlen(log_buffer));

#ifdef _POSIX_MEMLOCK
	if (do_mlockall == 1) {
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			log_err(errno, __func__, "mlockall failed");
		}
	}
#endif	/* _POSIX_MEMLOCK */

	(void)sprintf(log_buffer, msg_startup1, PBS_VERSION, 0);
	log_event(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE,
		LOG_NOTICE, PBS_EVENTCLASS_SERVER, msg_daemonname, log_buffer);

	sprintf(log_buffer, "%s startup pid %ld", argv[0], (long)pid);
	log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, log_buffer);

	/*
	 *  Local initialization stuff
	 */
	if (schedinit(nthreads)) {
		(void) sprintf(log_buffer,
			"local initialization failed, terminating");
		log_record(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO,
				__func__, log_buffer);
		exit(1);
	}

	sprintf(log_buffer, "Out of memory");

	/* Initialize cleanup lock */
	if (init_mutex_attr_recursive(&attr) == 0)
		die(0);

	pthread_mutex_init(&cleanup_lock, &attr);

	connect_servers();

	for (go=1; go;) {

		wait_for_cmds();
		while (go && !ds_queue_is_empty(sched_cmds)) {
			sched_cmd *cmd = ds_dequeue(sched_cmds);
			sched_svrconn *sconn = NULL;

			if (!cmd)
				continue;

			sconn = find_server(cmd->from_sock);
			if (!sconn) {
				/* shouldn't happen, but if it does just continue */
				free_sched_cmd(cmd);
				continue;
			}

#ifdef PBS_UNDOLR_ENABLED
			if (sigusr1_flag)
				undolr();
#endif
			if (sigprocmask(SIG_BLOCK, &allsigs, &oldsigs) == -1)
				log_err(errno, __func__, "sigprocmask(SIG_BLOCK)");

			/* Keep track of time to use in SIGSEGV handler */
			now = time(NULL);
			if (!opt_no_restart)
				segv_last_time = now;
#ifdef DEBUG
			{
				strftime(log_buffer, sizeof(log_buffer), "%Y-%m-%d %H:%M:%S", localtime(&now));
				DBPRT(("%s Scheduler received command %d\n", log_buffer, sconn->cmd));
			}
#endif

			if (schedule(sconn, cmd)) /* magic happens here */
				go = 0;
			else
				send_cycle_end(sconn);

			free_sched_cmd(cmd);

			if (sigprocmask(SIG_SETMASK, &oldsigs, NULL) == -1)
				log_err(errno, __func__, "sigprocmask(SIG_SETMASK)");
		}
	}

	close_servers();
	schedexit();

	log_eventf(PBSEVENT_SYSTEM, PBS_EVENTCLASS_SERVER, LOG_INFO, __func__, "%s normal finish pid %ld", argv[0], (long)pid);
	lock_out(lockfds, F_UNLCK);

	unload_auths();
	log_close(1);
	exit(0);
}
