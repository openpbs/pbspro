/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
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
 * @file	pbsnodes.c
 * @brief
 *	 This program exists to give a way to mark nodes
 *	 Down, Offline, or Free in PBS.
 *
 * @par	usage:	pbsnodes [-s server] -[F format] host host ...
 *		pbsnodes [-s server] -[F format]-v vnode vnode ...
 *		pbsnodes [-s server] -[F format]-H host host...
 *		pbsnodes [-s server] -[C comment]{o|r} host host ...
 *		pbsnodes [-s server] -a[v][F format]
 *		pbsnodes [-s server] -a[v][S[j][L]][F format]
 *		pbsnodes [-s server] -[S[j][L]][F format][H] host host ...
 *	where the node(s) are the names given in the node
 *	description file.
 *
 * @note
 *		pbsnodes -c and -d have been deprecated.
 *		Text about -c and -d are removed from the usage
 *		statement.  The code will be removed in the future.
 *
 *	pbsnodes			print command usage
 *
 *	pbsnodes -d			clear "DOWN" from all nodes so marked
 *
 *	pbsnodes -d node1 node2		set nodes node1, node2 "DOWN"
 *					unmark "DOWN" from any other node
 *
 *	pbsnodes -a			list all hosts
 *	pbsnodes host [host...]		list specified hosts
 *	pbsnodes -av			list all hosts and v-nodes
 *	pbsnodes -v vnode [vnode...]	list specified vnodes
 *
 *	pbsnodes -C <comment> host ...	set a comment on hosts
 *
 *	pbsnodes -F <format> host ...	list the output in specified format for specified nodes.
 *
 *	pbsnodes -H host [host ...]	list the hosts and vnodes on them.
 *
 *	pbsnodes -l			list all nodes marked in any way
 *	pbsnodes -l node1 node2		list specified nodes
 *
 *	pbsnodes -o host1 host2		mark hosts host1, host2 as OFF_LINE
 *					even if currently in use.
 *
 *	pbsnodes -r host1 host2		clear OFF_LINE from listed hosts
 *
 *	pbsnodes -S host1 host2 ...	single line Node summary of specified nodes
 *	pbsnodes -Sj			single line Jobs summary of specified nodes
 *	pbsnodes -S[j]L			list expanded version of each field in the single line summary
 *
 *	pbsnodes -c host1 host2		clear OFF_LINE or DOWN from listed hosts
 */
#include	<pbs_config.h>   /* the master config generated by configure */
#include	<pbs_version.h>

#include	"cmds.h"
#include	"portability.h"
#include	"pbs_ifl.h"
#include	"pbs_internal.h"
#include	"pbs_json.h"

/* Field width for Single line summary */
#define NODE_NAME	15
#define NODE_STATE	15
#define NODE_OS		 8
#define NODE_HARDW	 8
#define NODE_HOST	15
#define QUEUE		10
#define NCPUS		 7
#define MEM		 8
#define NMIC		 7
#define NGPUS		 7
#define COMMENT		20
#define NJOBS		 6
#define RUNNING_JOBS	 5
#define SUSP_JOBS	 6
#define NCPUS_FT	 7
#define MEM_FT		12
#define NMIC_FT		 7
#define NGPUS_FT	 7


typedef enum mgr_operation {
	DOWN, /* Set nodes DOWN */
	LISTMRK, /* List nodes marked DOWN or OFF_LINE */
	CLEAR, /* Clear DOWN and OFF_LINE */
	OFFLINE, /* Set nodes OFF_LINE */
	RESET, /* Clear nodes OFF_LINE */
	UPDATE_COMMENT, /* add comment to nodes */
	ALL, /* List all nodes */
	LISTSP, /* List specified nodes */
	LISTSPNV /* List specified nodes and their associated vnodes*/
}mgr_operation_t;

enum output_format_enum {
	FORMAT_DEFAULT = 0,
	FORMAT_DSV,
	FORMAT_JSON,
	FORMAT_MAX /* Add new formats before FORMAT_MAX */
	/* and update output_format_names[]  */
};

/* This array contains the names users may specify for output format. */
static char *output_format_names[] = {"default", "dsv", "json", NULL};

static int output_format = FORMAT_DEFAULT;
static int quiet = 0;
static char *dsv_delim = "|";


/** 
 * @brief
 *	cmp_node_name - compare two node names, allow the second to match the
 *	first if the same upto a dot ('.') in the second; i.e.
 *	"foo" == "foo.bar"
 * 
 * @param[in]  n1 - first node name to be matched with
 * @param[in]  n2 - second node name to be matched
 *
 *
 * @return - Error code	
 * @retval   1 - Failure
 * @retval   0 - Success
 *
 */

static int
cmp_node_name(char *n1, char *n2)
{
	while ((*n1 != '\0') && (*n2 != '\0')) {
		if (*n1 != *n2)
			break;
		n1++;
		n2++;
	}
	if (*n1 == *n2)
		return 0;
	else if ((*n1 == '.') && (*n2 == '\0'))
		return 0;
	else
		return 1;
}

/**
 * @brief
 *	Encodes the information in batch_status structure to json format
 *
 * @param[in] *bstat - structure containing node information
 *
 * @return - Error code
 * @retval   1 - Failure
 * @retval   0 - Success
 *
 */
static int
encode_to_json(struct batch_status *bstat)
{
	struct attrl *next;
	struct attrl *pattr;
	char	     *str;
	char	     *pc;
	char	     *pc1;
	double	      value = 0;
	char	     *prev_jobid = "";

	if (add_json_node(JSON_OBJECT, JSON_NULL, JSON_NOVALUE, bstat->name, NULL) == NULL)
		return 1;
	for (pattr = bstat->attribs; pattr; pattr = pattr->next) {
		if (strcmp(pattr->name, "resources_available") == 0) {
			if (add_json_node(JSON_OBJECT, JSON_NULL, JSON_NOVALUE, pattr->name, NULL) == NULL)
				return 1;
			for (next = pattr; next; ) {
				if (add_json_node(JSON_VALUE, JSON_NULL, JSON_FULLESCAPE, next->resource, next->value) == NULL)
					return 1;
				if (next->next == NULL || strcmp(next->next->name, "resources_available")) {
					/* Nothing left in resources_available, close object */
					if (add_json_node(JSON_OBJECT_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL)
						return 1;
					pattr = next;
					next = NULL;
				} else {
					next = next->next;
				}
			}
		} else if (strcmp(pattr->name, "resources_assigned") == 0) {
			if (add_json_node(JSON_OBJECT, JSON_NULL, JSON_NOVALUE, pattr->name, NULL) == NULL)
				return 1;
			for (next = pattr; next; ) {
				str = next->value;
				value = strtod(str, &pc);
				while (pc) {
					if (isspace(*pc))
						pc++;
					else
						break;
				}
				/* Adding only non zero values.*/
				if (value) {
					if (add_json_node(JSON_VALUE, JSON_NULL, JSON_FULLESCAPE, next->resource, next->value) == NULL)
						return 1;
				} else {
					if (str[0] != '0') {
						if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, next->resource, str) == NULL)
							return 1;
					}
				}
				if (next->next == NULL || strcmp(next->next->name, "resources_assigned")) {
					/* Nothing left in resources_assigned, close object */
					if (add_json_node(JSON_OBJECT_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL)
						return 1;
					pattr = next;
					next = NULL;
				} else {
					next = next->next;
				}
			}
		} else if (strcmp(pattr->name, "jobs") == 0) {
			if (add_json_node(JSON_ARRAY, JSON_NULL, JSON_NOVALUE, pattr->name, NULL) == NULL)
				return 1;
			pc = pc1 = str = pattr->value;
			while (*pc1) {
				if (*pc1 != ' ')
					*pc++ = *(pc1);
				pc1++;
			}
			*pc = '\0';
			for (pc = strtok(str, ","); pc != NULL; pc = strtok(NULL, ",")) {
				pc1 = strchr(pc, '/');
				if (pc1)
					*pc1 = '\0';
				if (strcmp(pc, prev_jobid) != 0) {
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, NULL, pc) == NULL)
						return 1;
				}
				prev_jobid = pc;
			}
			if (add_json_node(JSON_ARRAY_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL)
				return 1;
		} else {
			if (add_json_node(JSON_VALUE, JSON_NULL, JSON_FULLESCAPE, pattr->name, pattr->value) == NULL)
				return 1;
		}

	}
	if (add_json_node(JSON_OBJECT_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL)
		return 1;
	return 0;

}

/**
 * @brief
 *	prints the nodes summary in specified format
 * 
 * @param[in] *def_server - server name
 * @param[in] *bstatus - structure with node information
 * @param[in] job_summary - value to test wheteher job running on node
 * @param[in] long_summary - value to test whether to print long summary of node 
 *
 * @retval - Error code
 * @retval   1 - Failure
 * @retval   0 - Success
 *
 */
static int
prt_node_summary(char *def_server, struct batch_status *bstatus, int job_summary, int long_summary)
{
	struct batch_status *bstat = NULL;
	struct attrl	    *pattr;
	struct attrl	    *next;
	struct JsonNode     *node = NULL;
	char		     suffixletter[]  = " kmgtp?";
	char		    *pc;
	char		    *pc1;
	char		     mem_info[50] = "0kb";
	char		     ncpus_info[20] = "0";
	char		     nmic_info[20] = "0";
	char		     ngpus_info[20] = "0";
	char		    *prev_jobid = NULL;
	char		    *cur_jobid = NULL;
	int		     prefix_assigned = 0;
	long int	     assigned_mem = 0;
	long int	     njobs = 0;
	long int	     run_jobs = 0;
	long int	     susp_jobs = 0;
	long int	     value = 0;
	static int	     done_headers = 0;

	if (output_format == FORMAT_DEFAULT && !done_headers) {
		if (job_summary) {
			printf("                                                        mem       ncpus   nmics   ngpus\n");
			printf("vnode           state           njobs   run   susp      f/t        f/t     f/t     f/t   jobs\n");
			printf("--------------- --------------- ------ ----- ------ ------------ ------- ------- ------- -------\n");
		} else {
			printf("vnode           state           OS       hardware host            queue        mem     ncpus   nmics   ngpus  comment\n");
			printf("--------------- --------------- -------- -------- --------------- ---------- -------- ------- ------- ------- ---------\n");
		}
		done_headers = 1;
	}
	if (def_server == NULL)
		def_server = "";

	for (bstat = bstatus; bstat; bstat = bstat->next) {
		char *name;
		char *state;
		char *hardware;
		char *queue;
		char *os;
		char *host;
		char *comment;
		char *jobs;
		int count;
		int prefix_total;
		int prefix_available;	/* magnitude of value when printed */
		long int total_mem;
		long int available_mem;
		long int total_cpus;
		long int available_cpus;
		long int total_nmic;
		long int available_nmic;
		long int total_ngpus;
		long int available_ngpus;
		long int resource_assigned;

		name = bstat->name;
		state = "--";
		hardware = "--";
		queue = "--";
		os = "--";
		host = "--";
		comment = "--";
		jobs ="--";
		count = 0;
		prefix_total = 0;
		prefix_available = 0;
		total_mem = 0;
		available_mem = 0;
		total_cpus = 0;
		available_cpus = 0;
		total_nmic = 0;
		available_nmic = 0;
		total_ngpus = 0;
		available_ngpus = 0;
		resource_assigned = 0;
		njobs = 0;
		run_jobs = 0;
		susp_jobs = 0;
		value = 0;
		prev_jobid = "";
		cur_jobid = "";
		pc = NULL;

		if (job_summary) {
			strcpy(mem_info, "0kb/0kb");
			strcpy(ncpus_info, "0/0");
			strcpy(nmic_info, "0/0");
			strcpy(ngpus_info, "0/0");
		} else {
			strcpy(mem_info, "0kb");
			strcpy(ncpus_info, "0");
			strcpy(nmic_info, "0");
			strcpy(ngpus_info, "0");
		}

		for (pattr = bstat->attribs; pattr; pattr = pattr->next) {
			if (pattr->resource && (strcmp(pattr->name, "resources_assigned") != 0)) {
				if ((strcmp(pattr->resource, "mem") == 0)) {
					total_mem = strtol(pattr->value, &pc, 10);
					if (*pc == 'k')      prefix_total  = 1;
					else if (*pc == 'm') prefix_total  = 2;
					else if (*pc == 'g') prefix_total  = 3;
					else if (*pc == 't') prefix_total  = 4;
					else prefix_total = 0;
					prefix_available = prefix_total;
					for (next = pattr->next; next && job_summary; next = next->next) {
						if (!next->resource)
							continue;
						if (strcmp(next->resource, "mem") == 0) {
							assigned_mem = strtol(next->value, &pc, 10);
							if (*pc == 'k')      prefix_assigned  = 1;
							else if (*pc == 'm') prefix_assigned  = 2;
							else if (*pc == 'g') prefix_assigned  = 3;
							else if (*pc == 't') prefix_assigned  = 4;
							else prefix_assigned = 0;
							while (prefix_assigned != prefix_total){
								if (prefix_assigned < prefix_total) {
									assigned_mem = ((assigned_mem%1024) + assigned_mem) >> 10;
									prefix_assigned++;
								}else {
									assigned_mem = assigned_mem << 10;
									prefix_assigned--;
								}
							}
							available_mem = total_mem - assigned_mem;
							prefix_available = prefix_total;
							while (available_mem > 999) {
								available_mem = ((available_mem%1024) + available_mem)  >> 10;
								prefix_available++;
							}
							break;
						}
					}
					while (total_mem > 999) {
						total_mem = ((total_mem%1024) + total_mem) >> 10;
						prefix_total++;
					}
					if (job_summary)
						snprintf(mem_info, sizeof(mem_info), "%ld%cb/%ld%cb", available_mem, suffixletter[prefix_available],
							total_mem, suffixletter[prefix_total]);
					else
						snprintf(mem_info, sizeof(mem_info), "%ld%cb", total_mem, suffixletter[prefix_total]);
				} else if ((strcmp(pattr->resource, "ncpus") == 0)) {
					total_cpus = atol(pattr->value);
					resource_assigned = 0;
					for (next = pattr->next; next && job_summary; next = next->next) {
						if (!next->resource)
							continue;
						if (strcmp(next->resource, "ncpus") == 0) {
							resource_assigned = atol(next->value);
							break;
						}
					}
					if (job_summary) {
						available_cpus = total_cpus - resource_assigned;
						snprintf(ncpus_info, 20, "%ld/%ld", available_cpus, total_cpus);
					}
					else
						snprintf(ncpus_info, 20, "%ld", total_cpus);
				} else if (strcmp(pattr->resource, "nmics") == 0) {
					total_nmic = atol(pattr->value);
					resource_assigned = 0;
					for (next = pattr->next; next && job_summary; next = next->next) {
						if (!next->resource)
							continue;
						if (strcmp(next->resource, "nmics") == 0) {
							resource_assigned = atol(next->value);
							break;
						}
					}
					if (job_summary) {
						available_nmic = total_nmic - resource_assigned;
						snprintf(nmic_info, 20, "%ld/%ld", available_nmic, total_nmic);
					}
					else
						snprintf(nmic_info, 20, "%ld", total_nmic);
				} else if (strcmp(pattr->resource, "ngpus") == 0) {
					total_ngpus = atol(pattr->value);
					resource_assigned = 0;
					for (next = pattr->next; next && job_summary; next = next->next) {
						if (!next->resource)
							continue;
						if (strcmp(next->resource, "ngpus") == 0) {
							resource_assigned = atol(next->value);
							break;
						}
					}
					if (job_summary) {
						available_ngpus = total_ngpus - resource_assigned;
						snprintf(ngpus_info, 20, "%ld/%ld", available_ngpus, total_ngpus);
					} else
						snprintf(ngpus_info, 20, "%ld", total_ngpus);

				} else if (strcmp(pattr->resource, "host") == 0)
					host = pattr->value;
				else if (strcmp(pattr->resource, "OS") == 0)
					os = pattr->value;
				else if (strcmp(pattr->resource, "hardware") == 0)
					hardware = pattr->value;
			} else if (strcmp(pattr->name, "state") == 0) {
				if (!long_summary)
					state = strtok(pattr->value, ",");
				else
					state = pattr->value;
			} else if (strcmp(pattr->name, "comment") == 0)
				comment= pattr->value;
			else if (strcmp(pattr->name, "queue") == 0)
				queue = pattr->value;
			else if (job_summary) {
				if (strcmp(pattr->name, "jobs") == 0) {
					jobs = pattr->value;
					count = 1;
					pc = strtok(pattr->value, ", ");
					while (pc != NULL) {
						pc1 = strchr(pc, (int)'/'); /* remove virtual core description from jobid. */
						if(pc1)
							*pc1 = '\0';
						cur_jobid = pc;
						if(output_format == FORMAT_DEFAULT){
							pc1 = strchr(pc, (int)'.');
							if(strcmp((pc1 + 1), def_server) == 0) /* if not equal, job is from a peer_server. */
							*pc1 = '\0';
						}
						if ((strcmp(cur_jobid, prev_jobid)!=0) && strcmp(cur_jobid, jobs) != 0) { /* will skip concatenating if only one job.*/
							strcat(jobs, ",");
							strcat(jobs, cur_jobid);
							count ++;
						}
						prev_jobid = cur_jobid;
						pc = strtok(NULL, ", ");
					}
					run_jobs = count;
					njobs = susp_jobs + run_jobs;
				}
			}
		}
		switch (output_format) {

			case FORMAT_DSV:
				if (job_summary) {
					printf("vnode=%s%sstate=%s%snjobs=%ld%srun=%ld%ssusp=%ld%smem(f/t)=%s%sncpus(f/t)=%s%snmics(f/t)=%s%sngpus(f/t)=%s%sjobs=%s\n",
						name, dsv_delim, state, dsv_delim, njobs, dsv_delim, run_jobs, dsv_delim, susp_jobs,
						dsv_delim, mem_info, dsv_delim, ncpus_info, dsv_delim, nmic_info, dsv_delim, ngpus_info, dsv_delim, jobs);
				} else {
					printf("vnode=%s%sstate=%s%sOS=%s%shardware=%s%shost=%s%squeue=%s%smem=%s%sncpus=%s%snmics=%s%sngpus=%s%scomment=%s\n",
						name, dsv_delim, state, dsv_delim, os, dsv_delim, hardware, dsv_delim, host, dsv_delim, queue,
						dsv_delim, mem_info, dsv_delim, ncpus_info, dsv_delim, nmic_info, dsv_delim, ngpus_info, dsv_delim, show_nonprint_chars(comment));
				}
				break;

			case FORMAT_JSON:
				if (add_json_node(JSON_OBJECT, JSON_NULL, JSON_NOVALUE, name, NULL) == NULL)
					return 1;
				if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "State", state) == NULL)
					return 1;
				if (job_summary) {

					if (add_json_node(JSON_VALUE, JSON_INT, JSON_FULLESCAPE, "Total Jobs", &njobs) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_INT, JSON_FULLESCAPE, "Running Jobs", &run_jobs) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_INT, JSON_FULLESCAPE, "Suspended Jobs", &susp_jobs) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "mem f/t", mem_info) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "ncpus f/t", ncpus_info) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "nmics f/t", nmic_info) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "ngpus f/t", ngpus_info) == NULL)
						return 1;
					if (add_json_node(JSON_ARRAY, JSON_NULL, JSON_NOVALUE, "jobs", NULL) == NULL)
						return 1;
					if (strcmp(jobs, "--") != 0) {
						pc = strtok(jobs, ",");
						while (pc != NULL) {
							node = add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, NULL, pc);
							if (node == NULL)
								return 1;
							pc = strtok(NULL, ",");
						}
					}
					if (add_json_node(JSON_ARRAY_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL)
						return 1;
					if (add_json_node(JSON_OBJECT_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL)
						return 1;
				} else {

					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "OS", os) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "hardware", hardware) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "host", host) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "queue", queue) == NULL)
						return 1;
					if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "Memory", mem_info) == NULL)
						return 1;
					value = atol(ncpus_info);
					if (add_json_node(JSON_VALUE, JSON_INT, JSON_FULLESCAPE, "ncpus", &value) == NULL)
						return 1;
					value = atol(nmic_info);
					if (add_json_node(JSON_VALUE, JSON_INT, JSON_FULLESCAPE, "nmics", &value) == NULL)
						return 1;
					value = atol(ngpus_info);
					if (add_json_node(JSON_VALUE, JSON_INT, JSON_FULLESCAPE, "ngpus", &value) == NULL)
						return 1;
					if (add_json_node(JSON_OBJECT_END, JSON_STRING, JSON_FULLESCAPE, "comment", comment) == NULL)
						return 1;
				}
				break;
			case FORMAT_DEFAULT:
				if (job_summary) {
					if (long_summary)
						printf("%-*s %-*s %*ld %*ld %*ld %*s %*s %*s %*s %s\n",
							NODE_NAME, name, NODE_STATE, state, NJOBS, njobs, RUNNING_JOBS, run_jobs, SUSP_JOBS, susp_jobs,
							MEM_FT, mem_info, NCPUS_FT, ncpus_info, NMIC_FT, nmic_info,
							NGPUS_FT, ngpus_info, jobs);
					else
						printf("%-*.*s %-*.*s %*ld %*ld %*ld %*.*s %*.*s %*.*s %*.*s %s\n",
							NODE_NAME, NODE_NAME, name, NODE_STATE, NODE_STATE, state, NJOBS, njobs, RUNNING_JOBS, run_jobs,
							SUSP_JOBS, susp_jobs, MEM_FT, MEM_FT, mem_info, NCPUS_FT, NCPUS_FT, ncpus_info, NMIC_FT, NMIC_FT, nmic_info,
							NGPUS_FT, NGPUS_FT, ngpus_info, jobs);
				} else {
					if (long_summary)
						printf("%-*s %-*s %-*s %-*s %-*s %-*s %*s %*s %*s %*s %s\n", NODE_NAME, name, NODE_STATE, state,
							NODE_OS, os, NODE_HARDW, hardware, NODE_HOST, host, QUEUE, queue, MEM, mem_info, NCPUS, ncpus_info,
							NMIC, nmic_info, NGPUS, ngpus_info, show_nonprint_chars(comment));
					else
						printf("%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %*.*s %*.*s %*.*s %*.*s %s\n", NODE_NAME, NODE_NAME, name,
							NODE_STATE, NODE_STATE, state, NODE_OS, NODE_OS, os, NODE_HARDW, NODE_HARDW, hardware,
							NODE_HOST, NODE_HOST, host, QUEUE, QUEUE, queue, MEM, MEM, mem_info, NCPUS, NCPUS, ncpus_info,
							NMIC, NMIC, nmic_info, NGPUS, NGPUS, ngpus_info, show_nonprint_chars(comment));
				}
		}
	}
	return 0;
}

/**
 * @brief 
 * 	print node information without summary
 *
 * @param[in] bstat - structure pointer having node information
 *
 * @retval Void
 *
 */
static void
prt_node(struct batch_status *bstat)
{
	char	     *pc;
	struct attrl *pattr = NULL;
	time_t       epoch;
	if (bstat == NULL)
		return;

	switch (output_format) {
		case FORMAT_JSON:
			if (encode_to_json(bstat)) {
				fprintf(stderr, "pbsnodes: out of memory\n");
				exit(1);
			}
			break;
		case FORMAT_DSV:
			printf("Name=%s%s", bstat->name, dsv_delim);
			for (pattr = bstat->attribs; pattr; pattr = pattr->next) {
				if (pattr->resource)
					printf("%s.%s=%s", pattr->name, pattr->resource, show_nonprint_chars(pattr->value));
				else if (strcmp(pattr->name, "jobs") == 0) {
					printf("%s=", pattr->name);
					pc = pattr->value;
					while (*pc) {
						char *sbuf;
						char char_buf[2];
						if (*pc == ' ') {
							pc++;
							continue;
						}

						sprintf(char_buf, "%c", *pc);
						sbuf = show_nonprint_chars(char_buf);
						if (sbuf != NULL) {
							int  c;
							for (c=0; c < strlen(sbuf); c++)
								printf("%c", sbuf[c]);
						} else {
							printf("%c", *pc);
						}
						pc++;
					}
				} else
					printf("%s=%s", pattr->name, show_nonprint_chars(pattr->value));
				if (pattr->next)
					printf("%s", dsv_delim);
			}
			printf("\n");
			break;
		default:
			printf("%s\n", bstat->name);
			for (pattr = bstat->attribs; pattr; pattr = pattr->next) {
				printf("     %s", pattr->name);
				if (pattr->resource)
					printf(".%s", pattr->resource);
				if ((strcmp(pattr->name, ATTR_NODE_last_used_time) == 0) ||
				(strcmp(pattr->name, ATTR_NODE_last_state_change_time) == 0)) {
					epoch = (time_t) atol(pattr->value);
					printf(" = %s", ctime(&epoch));
				} else				
					printf(" = %s\n", show_nonprint_chars(pattr->value));
			}
			printf("\n");
			break;
	}
}

/**
 * @brief
 *	returns the state of node 
 * 
 * @param[in] pbs - structure pointer containing node information
 * 
 * @return - string
 * @retval   "" - Failure
 * @retval   "value" - Success
 *
 */
static char *
get_nstate(struct batch_status *pbs)
{
	struct attrl *pat;

	for (pat = pbs->attribs; pat; pat = pat->next) {
		if (strcmp(pat->name, ATTR_NODE_state) == 0)
			return pat->value;
	}
	return "";
}

/**
 * @brief
 *	returns the comment for the node
 *
 * @param pbs - structure pointer containing node information
 * 
 * @return - string
 * @retval   "" - Failure
 * @retval   "value" - Success
 *
 */
static char *
get_comment(struct batch_status *pbs)
{
	struct attrl *pat;

	for (pat = pbs->attribs; pat; pat = pat->next) {
		if (strcmp(pat->name, ATTR_comment) == 0)
			return pat->value;
	}
	return "";
}

/*
 * @brief 
 *	returns indication if node is marked down or not
 *
 * @param pbs - structure pointer containing node information
 * 
 * @return - Error code
 * @retval   1 - Failure indiacating node is down
 * @retval   0 - Success indicating node is not down
 *
 */
static int
is_down(struct batch_status *pbs)
{
	if (strstr(get_nstate(pbs), ND_down) != NULL)
		return 1;
	else
		return 0;
}

/*
 * @brief
 *      returns indication if node is marked offline or not
 *
 * @param pbs - structure pointer containing node information
 *
 * @return - Error code
 * @retval   1 - Failure indiacating node is  offline
 * @retval   0 - Success indicating node is not ofline
 *
 */
static int
is_offline(struct batch_status *pbs)
{
	if (strstr(get_nstate(pbs), ND_offline) != NULL)
		return 1;
	else
		return 0;
}

/**
 * @brief 
 *	Mark the node with values sent as parameters
 *
 * @param[in] con - value to test connected to server or not
 * @param[in] name - name of node
 * @param[in] state1 - current state
 * @param[in] op1 - integer value corresponding to  state1
 * @param[in] state2 - transition to this state
 * @param[in] op2 - integere value corresponding to state2 
 *
 * @return	int
 * @retval	0		success
 * @retval	pbse error	failure
 *
 */
static int
marknode(int con, char *name,
	char *state1, enum batch_op op1,
	char *state2, enum batch_op op2,
	char *comment)
{
	char		Comment[80];
	struct attropl	new[3];
	int		i;
	int		rc;

	i = 0;
	if (state1 != NULL) {
		new[i].name = ATTR_NODE_state;
		new[i].resource = NULL;
		new[i].value = state1;
		new[i].op = op1;
		new[i].next = NULL;
	}
	if (state2 != NULL) {
		if (state1 != NULL) {
			new[i].next = &new[i+1];
			++i;
		}
		new[i].name = ATTR_NODE_state;
		new[i].resource = NULL;
		new[i].value = state2;
		new[i].op = op2;
		new[i].next = NULL;
	}

	if (comment != NULL) {
		if (state1 != NULL || state2 != NULL) {
			new[i].next = &new[i+1];
			++i;
		}
		snprintf(Comment, 80, "%s", comment);
		new[i].name = ATTR_comment;
		new[i].resource = NULL;
		new[i].value = Comment;
		new[i].op = SET;
		new[i].next = NULL;
	}

	rc = pbs_manager(con, MGR_CMD_SET, MGR_OBJ_HOST, name, new, NULL);
	if (rc && !quiet) {
		char *errmsg;

		fprintf(stderr, "Error marking node %s - ", name);
		if ((errmsg = pbs_geterrmsg(con)) != NULL)
			fprintf(stderr, "%s\n", errmsg);
		else
			fprintf(stderr, "error: %d\n", pbs_errno);
	}
	return (rc);
}
/**
 * @brief
 *	The main function in C - entry point
 *
 * @param[in]  argc - argument count
 * @param[in]  argv - pointer to argument array
 *
 * @return  int
 * @retval  0 - success
 * @retval  !0 - error
 */
int
main(int argc, char *argv[])
{
	time_t		     timenow;
	struct attrl	    *pattr = NULL;
	int		     con;
	char		    *def_server;
	int		     errflg = 0;
	char		    *errmsg;
	int		     i;
	int		     rc = 0;
	extern	char	    *optarg;
	extern	int	     optind;
	char		   **pa;
	char		    *comment = NULL;
	struct batch_status *bstat = NULL;
	struct batch_status *bstat_head = NULL;
	struct batch_status *next_bstat = NULL;
	int		     do_vnodes = 0;
	mgr_operation_t      oper = LISTSP;
	int		     ret = 0;
	int		     job_summary  = 0;
	int		     long_summary = 0;
	int		     format = 0;
	int		     prt_summary = 0;

	/*test for real deal or just version and exit*/

	PRINT_VERSION_AND_EXIT(argc, argv);

#ifdef WIN32
	if (winsock_init()) {
		return 1;
	}
#endif

	/* get default server, may be changed by -s option */

	def_server = pbs_default();
	if (def_server == NULL)
		def_server = "";

	if (argc == 1)
		errflg = 1;
	while ((i = getopt(argc, argv, "acC:dD:F:HjlLoqrs:Sv")) != EOF)
		switch (i) {

			case 'a':
				if (oper == LISTSP)
					oper = ALL;
				else
					errflg = 1;
				break;

			case 'c':
				if (oper == LISTSP || do_vnodes == 1)
					oper = CLEAR;
				else
					errflg = 1;
				break;

			case 'C':
				if (optarg && (oper == LISTSP)) {
					oper = UPDATE_COMMENT;
					comment = optarg;
				} else if (optarg &&(oper == OFFLINE || oper == RESET))
					comment = optarg;
				else
					errflg = 1;
				break;

			case 'd':
				if (oper == LISTSP || do_vnodes == 1)
					oper = DOWN;
				else
					errflg = 1;
				break;

			case 'D':
				if (oper == LISTSP || oper == ALL || oper == LISTSPNV)
					dsv_delim = optarg;
				else
					errflg = 1;
				break;

			case 'F':
				for (format = FORMAT_DEFAULT; format < FORMAT_MAX; format++) {
					if (strcasecmp(optarg, output_format_names[format]) == 0) {
						output_format = format;
						break;
					}
				}
				if (format >= FORMAT_MAX)
					errflg = 1;
				break;

			case 'H':
				if (oper == LISTSP)
					oper = LISTSPNV;
				else
					errflg = 1;
				break;

			case 'j':
				if (oper == LISTSP || oper == ALL || oper == LISTSPNV)
					job_summary = 1;
				else
					errflg = 1;
				break;

			case 'l':
				if (oper == LISTSP || do_vnodes == 1)
					oper = LISTMRK;
				else
					errflg = 1;
				break;

			case 'L':
				if (oper == LISTSP || oper == ALL || oper == LISTSPNV)
					long_summary = 1;
				else
					errflg = 1;
				break;

			case 'o':
				if (oper == LISTSP || do_vnodes == 1 || oper == UPDATE_COMMENT)
					oper = OFFLINE;
				else
					errflg = 1;
				break;

			case 'q':
				quiet = 1;
				break;

			case 'r':
				if (oper == LISTSP || do_vnodes == 1 || oper == UPDATE_COMMENT)
					oper = RESET;
				else
					errflg = 1;
				break;

			case 's':
				def_server = optarg;
				break;

			case 'S':
				if (oper == LISTSP || oper == ALL || oper == LISTSPNV)
					prt_summary = 1;
				else
					errflg = 1;
				break;

			case 'v':
				if (oper == LISTSP || oper == ALL)
					do_vnodes = 1;
				else
					errflg = 1;
				break;

			case '?':
			default:
				errflg = 1;
				break;
		}


	if (errflg ||
		(oper == LISTMRK && optind != argc) ||
		(oper == CLEAR   && optind == argc) ||
		(oper == OFFLINE && optind == argc) ||
		(oper == RESET   && optind == argc) ||
		(oper == LISTSPNV && optind == argc) ||
		(oper == LISTSP && optind == argc) ||
		(oper == UPDATE_COMMENT && optind == argc) ||
		(prt_summary && (oper != LISTSP && oper != LISTSPNV && oper != ALL))) {
		if (!quiet)
			fprintf(stderr,
				"usage:\t%s [-{o|r}][-C comment][-s server] host host ...\n"
				"\t%s -l [-s server]\n"
				"\t%s [-s server] -v vnode vnode ...\n"
				"\t%s -a[v][S[j][L]][-F format][-D delim][-s server]\n"
				"\t%s -[H][S[j][L]][-F format][-D delim] host host ...\n"
				"\t%s --version\n\n",
				argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
		exit(1);
	}

	if (CS_client_init() != CS_SUCCESS) {
		fprintf(stderr, "pbsnodes: unable to initialize security library.\n");
		exit(1);
	}

	con = cnt2server(def_server);
	if (con <= 0) {
		if (!quiet)
			fprintf(stderr, "%s: cannot connect to server %s, error=%d\n",
				argv[0], def_server, pbs_errno);
		CS_close_app();
		exit(1);
	}

	/* if do_vnodes is set, get status of all virtual nodes (vnodes) */
	/* else if oper is ALL then get status of all hosts              */

	if ((do_vnodes == 1) || (oper == ALL) ||
		(oper == DOWN)   || (oper == LISTMRK) || (oper == LISTSPNV)) {
		if (do_vnodes || oper == LISTSPNV)
			bstat_head = pbs_statvnode(con, "", NULL, NULL);
		else
			bstat_head = pbs_stathost(con, "", NULL, NULL);

		if (bstat_head == NULL) {
			if (pbs_errno) {
				if (!quiet) {
					if ((errmsg = pbs_geterrmsg(con)) != NULL)
						fprintf(stderr, "%s: %s\n", argv[0], errmsg);
					else
						fprintf(stderr, "%s: Error %d\n", argv[0], pbs_errno);
				}
				exit(1);
			} else {
				if (!quiet)
					fprintf(stderr, "%s: No nodes found\n", argv[0]);
				exit(0);
			}
		}
	}
	/* adding prologue to json output. */
	if (output_format == FORMAT_JSON) {
		timenow = time(0);
		if (add_json_node(JSON_VALUE, JSON_INT, JSON_FULLESCAPE, "timestamp", &timenow) == NULL) {
			fprintf(stderr, "pbsnodes: out of memory\n");
			exit(1);
		}
		if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "pbs_version", PBS_VERSION) == NULL) {
			fprintf(stderr, "pbsnodes: out of memory\n");
			exit(1);
		}
		if (add_json_node(JSON_VALUE, JSON_STRING, JSON_FULLESCAPE, "pbs_server", def_server) == NULL) {
			fprintf(stderr, "pbsnodes: out of memory\n");
			exit(1);
		}
		if (add_json_node(JSON_OBJECT, JSON_NULL, JSON_NOVALUE, "nodes", NULL) == NULL) {
			fprintf(stderr, "pbsnodes: out of memory\n");
			exit(1);
		}
	}
	switch (oper) {

		case DOWN:

			/*
			 * loop through the list of nodes returned above:
			 *   if node is up and is in argv list, mark it down;
			 *   if node is down and not in argv list, mark it up;
			 * for all changed nodes, send in request to server
			 */

			for (bstat = bstat_head; bstat; bstat = bstat->next) {
				for (pa = argv+optind; *pa; pa++) {
					if (cmp_node_name(*pa, bstat->name) == 0) {
						if (is_down(bstat) == 0) {
							ret = marknode(con, bstat->name,
								ND_down, INCR, NULL, INCR, comment);
							if (ret > 0)
								rc = ret;
						}
						break;
					}
				}
				if (*pa == NULL) {

					/* node not in list, if down now, set up */
					if (is_down(bstat) == 1)
						ret = marknode(con, bstat->name,
							ND_down, DECR, NULL, DECR, comment);
					if (ret > 0)
						rc = ret;
				}
			}
			break;

		case CLEAR:

			/* clear DOWN and OFF_LINE from specified nodes		*/

			for (pa = argv+optind; *pa; pa++) {
				ret = marknode(con, *pa, ND_offline, DECR, ND_down, DECR, comment);
				if (ret > 0)
					rc = ret;
			}

			break;

		case RESET:

			/* clear OFF_LINE from specified nodes			*/

			for (pa = argv+optind; *pa; pa++) {
				ret = marknode(con, *pa, ND_offline, DECR, NULL, DECR, comment);
				if (ret > 0)
					rc  = ret;
			}
			break;

		case OFFLINE:

			/* set OFF_LINE on specified nodes			*/
			for (pa = argv+optind; *pa; pa++) {
				ret = marknode(con, *pa, ND_offline, INCR, NULL, INCR, comment);
				if (ret > 0)
					rc = ret;
			}
			break;

		case UPDATE_COMMENT:

			/*just add comment to specified nodes*/
			for (pa = argv+optind; *pa; pa++) {
				if (strcmp(*pa, "") == 0)
					continue;
				ret = marknode(con, *pa, NULL, INCR, NULL, INCR, comment);
				if (ret > 0)
					rc = ret;
			}
			break;

		case ALL:

			if (prt_summary) {
				if (prt_node_summary(def_server, bstat_head, job_summary, long_summary)) {
					fprintf(stderr, "pbsnodes: out of memory\n");
					return 1;
				}
			} else {
				for (bstat = bstat_head; bstat; bstat = bstat->next)
					prt_node(bstat);
			}
			if (output_format == FORMAT_JSON) {
				if (add_json_node(JSON_OBJECT_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL) {
					fprintf(stderr, "pbsnodes: out of memory\n");
					return 1;
				}
				generate_json(stdout);
				free_json_node_list();
			}
			pbs_statfree(bstat_head);

			break;

		case LISTMRK:

			/* list any node that is marked DOWN or OFF_LINE	*/
			for (bstat = bstat_head; bstat; bstat = bstat->next) {
				if (is_down(bstat) || is_offline(bstat)) {
					printf("%-20s %s %s\n", bstat->name,
						get_nstate(bstat), show_nonprint_chars(get_comment(bstat)));
				}
			}
			break;

		case LISTSP:

			/* list the specified nodes or vnodes */
			for (pa = argv+optind; *pa; pa++) {
				if (do_vnodes)
					bstat = pbs_statvnode(con, *pa, NULL, NULL);
				else
					bstat = pbs_stathost(con, *pa, NULL, NULL);
				if (!bstat) {
					if (pbs_errno != 0) {

						if (output_format == FORMAT_JSON) {
							add_json_node(JSON_OBJECT, JSON_NULL, JSON_NOVALUE, *pa, NULL);
							add_json_node(JSON_OBJECT_END, JSON_STRING, JSON_FULLESCAPE, "Error", pbs_geterrmsg(con));
						} else {
							fprintf(stderr, "Node: %s,  Error: %s\n", *pa, pbs_geterrmsg(con));
							rc = 1;
						}
					}
				} else {
					if (prt_summary) {
						if (prt_node_summary(def_server, bstat, job_summary, long_summary)) {
							fprintf(stderr, "pbsnodes: out of memory\n");
							exit(1);
						}
					} else {
						prt_node(bstat);
					}
				}
			}
			if (output_format == FORMAT_JSON) {
				if (add_json_node(JSON_OBJECT_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL) {
					fprintf(stderr, "pbsnodes: out of memory\n");
					exit(1);
				}
				generate_json(stdout);
				free_json_node_list();
			}

			break;

		case LISTSPNV:

			/*list nodes and vnodes associated with them.*/
			if (argc-optind) {
				for (bstat = bstat_head; bstat;bstat = bstat->next) {
					int matched;

					matched = 0;
					pa = argv+optind;
					while (*pa) {
						if (pa == NULL) {
							pa++;
							continue;
						}
						pattr = bstat->attribs;
						while (pattr) {
							if (pattr->resource) {
								if (strcmp(pattr->resource, "host") == 0) {
									if (strcmp(pattr->value, *pa) == 0)
										matched = 1;
								}
							}
							if (matched)
								break;
							pattr = pattr->next;
						}
						if (matched) {
							if (prt_summary) {
								next_bstat = bstat->next;
								bstat->next = NULL;
								prt_node_summary(def_server, bstat, job_summary, long_summary);
								bstat->next = next_bstat;
							} else {
								prt_node(bstat);
							}
							matched = 0;
							break;
						}
						pa++;
					}
				}

			}
			pa = argv+optind;
			while (*pa) {
				bstat = pbs_stathost(con, *pa, NULL, NULL);
				if (!bstat) {
					if (pbs_errno != 0) {
						if (output_format == FORMAT_JSON) {
							add_json_node(JSON_OBJECT, JSON_NULL, JSON_NOVALUE, *pa, NULL);
							add_json_node(JSON_OBJECT_END, JSON_STRING, JSON_FULLESCAPE, "Error", pbs_geterrmsg(con));
						} else {
							fprintf(stderr, "Node: %s,  Error: %s\n", *pa, pbs_geterrmsg(con));
							rc = 1;
						}
					} else
						pbs_statfree(bstat);
				}
				pa++;
			}
			if (output_format == FORMAT_JSON) {
				if (add_json_node(JSON_OBJECT_END, JSON_NULL, JSON_NOVALUE, NULL, NULL) == NULL) {
					fprintf(stderr, "pbsnodes : out of memory");
					exit(1);
				}
				generate_json(stdout);
				free_json_node_list();
			}
			pbs_statfree(bstat_head);
			break;
	}
	(void)pbs_disconnect(con);
	return (rc ? 1 : 0);
}
