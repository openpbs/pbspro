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
 * @file	pbs_rsub.c
 * @brief
 *  pbs_rdel - PBS command to submit reservations
 */
#include <pbs_config.h>   /* the master config generated by configure */
#include <pbs_version.h>

#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <pbs_ifl.h>
#include "cmds.h"
#include "net_connect.h"
#include "attribute.h"

#define DEFAULT_INTERACTIVE "-10"

static struct attrl *attrib = NULL;
static int qmoveflg = FALSE;
static time_t dtstart;
static time_t dtend;
static int is_stdng_resv = 0;
static int is_maintenance_resv = 0;
static char **maintenance_hosts = NULL;

/* The maximum buffer size that is allowed not to exceed 80 columns.
 * The number 67 (66 chars + 1 EOL) is the result of subtracting the number
 * of characters to print "reserve_rrule=" (14 chars) via pbs_rstat.
 */
char rrule[67];

/**
 * @brief
 *	processes the argument list for pbs_rsub and validates
 *	and sets attribute according to the argument value
 *
 * @param[in] argc - commandline args count
 * @param[in] argv - pointer to argument list
 * @param[in] dest - server option
 *
 * @return errflag
 * @retval 0  Success
 * @retval  !0 Failure
 *
 */
int
process_opts(int argc, char **argv, struct attrl **attrp, char *dest)
{
	int c, i;
	char *erp;
	int errflg = 0;
	char *keyword;
	char *valuewd;
	time_t t;

	char time_buf[80];
	char dur_buf[800];
	char badw[] = "pbs_rsub: illegal -W value\n";
	int opt_re_flg = FALSE;
	int opt_inter_flg = FALSE;
	int opt_res_req_flg = FALSE;
#ifdef WIN32
	struct attrl *ap = NULL;
	short nSizeofHostName = 0;
	char* orig_apvalue = NULL;
	char* temp_apvalue = NULL;
#endif

	while ((c = getopt(argc, argv, "D:E:I:l:m:M:N:q:r:R:u:U:g:G:H:W:-:")) != EOF) {
		switch (c) {
			case 'D':
				sprintf(dur_buf, "walltime=%s", optarg);
				if ((i = set_resources(&attrib, dur_buf, 0, &erp)) != 0) {
					fprintf(stderr, "pbs_rsub: illegal -D value\n");
					errflg++;
				}
				break;

			case 'E':
				opt_re_flg = TRUE;
				t = cvtdate(optarg);
				if (t >= 0) {
					(void)sprintf(time_buf, "%ld", (long)t);
					set_attr_error_exit(&attrib, ATTR_resv_end, time_buf);
					dtend = t;
				}
				else {
					fprintf(stderr, "pbs_rsub: illegal -E time value\n");
					errflg++;
				}
				break;

			case 'I':
				opt_inter_flg = TRUE;
				if ((optarg == NULL) || (*optarg == '\0'))
					set_attr_error_exit(&attrib, ATTR_inter, "0");
				else {
					char* endptr;
					(void)strtol(optarg, &endptr, 0);
					if (*endptr == '\0') {
						set_attr_error_exit(&attrib, ATTR_inter, optarg);
					}
					else {
						fprintf(stderr, "pbs_rsub: illegal -I time value\n");
						errflg++;
					}
				}
				break;

			case 'l':
				opt_res_req_flg = TRUE;
				if ((i = set_resources(&attrib, optarg, 0, &erp)) != 0) {
					if (i > 1) {
						pbs_prt_parse_err("pbs_rsub: illegal -l value\n", optarg,
							(int)(erp-optarg), i);
					} else
						fprintf(stderr, "pbs_rsub: illegal -l value\n");
					errflg++;
				}
				break;

			case 'm':
				while (isspace((int)*optarg))
					optarg++;
				set_attr_error_exit(&attrib, ATTR_m, optarg);
				break;

			case 'M':
				set_attr_error_exit(&attrib, ATTR_M, optarg);
				break;

			case 'N':
				set_attr_error_exit(&attrib, ATTR_resv_name, optarg);
				break;

			case 'q':
				/* destination can only be another server */
				if (optarg[0] != '@') {
					fprintf(stderr, "pbs_rsub: illegal -q value: format \"@server\"\n");
					errflg++;
					break;
				}
				strcpy(dest, &optarg[1]);
				break;

			case 'R':
				opt_re_flg = TRUE;
				t = cvtdate(optarg);
				if (t >= 0) {
					(void)sprintf(time_buf, "%ld", (long)t);
					set_attr_error_exit(&attrib, ATTR_resv_start, time_buf);
					dtstart = t;
				}
				else {
					fprintf(stderr, "pbs_rsub: illegal -R time value\n");
					errflg++;
				}
				break;

			case 'r':
				is_stdng_resv = 1;
				set_attr_error_exit(&attrib, ATTR_resv_rrule, optarg);
				set_attr_error_exit(&attrib, ATTR_resv_standing, "1");
				if (strlen(optarg) > sizeof(rrule)-1) {
					fprintf(stderr, "pbs_rsub: illegal -r value (expression too long)\n");
					errflg++;
					break;
				}
				strcpy(rrule, optarg);
				break;

			case 'u':
				set_attr_error_exit(&attrib, ATTR_u, optarg);
				break;

			case 'U':
				set_attr_error_exit(&attrib, ATTR_auth_u, optarg);
				break;

			case 'g':
				set_attr_error_exit(&attrib, ATTR_g, optarg);
				break;

			case 'G':
				set_attr_error_exit(&attrib, ATTR_auth_g, optarg);
				break;

			case 'H':
				set_attr_error_exit(&attrib, ATTR_auth_h, optarg);
				break;

			case 'W':
				while (isspace((int)*optarg))
					optarg++;

				if (strlen(optarg) == 0) {
					fprintf(stderr, "pbs_rsub: illegal -W value\n");
					errflg++;
					break;
				}

				i = parse_equal_string(optarg, &keyword, &valuewd);
				while (i == 1) {
					if (strcmp(keyword, ATTR_convert) == 0) {
						qmoveflg = TRUE;
					} else {
						/* unknown "-W" attribute in reservation req */
						fprintf(stderr, "pbs_rsub: unrecognized pair, %s=%s\n",
							keyword, valuewd);
						errflg++;
					}
					if (errflg == 0)
						set_attr_error_exit(&attrib, keyword, valuewd);

					/* move to next attribute in this "-W" specification */

					i = parse_equal_string(NULL, &keyword, &valuewd);

				}   /* bottom of inner while loop */

				if (i == -1) {
					fprintf(stderr, "%s", badw);
					errflg++;
				}
				break;

			case '-':
				if (strcmp(optarg, "hosts") == 0)
					is_maintenance_resv = 1;
				else
					errflg++;
				break;

			default:
				/* pbs_rsub option not recognized */
				errflg++;

		} /* End of lengthy 'switch on option' constuction */
	}   /* End of lengthy while loop on 'options' */

	if (opt_re_flg == TRUE && qmoveflg ==TRUE) {
		fprintf(stderr, "pbs_rsub: -Wqmove is not compatible with -R or -E option\n");
		errflg++;
	}

	if (opt_inter_flg && is_maintenance_resv) {
		fprintf(stderr, "pbs_rsub: can't use -I with --hosts\n");
		errflg++;
	}

	if (opt_res_req_flg && is_maintenance_resv) {
		fprintf(stderr, "pbs_rsub: can't use -l with --hosts\n");
		errflg++;
	}

	if (is_maintenance_resv) {
		char **hostp = NULL;
		int num_hosts = argc - optind;

		if (num_hosts > 0) {
			int i;

			maintenance_hosts = malloc(sizeof(char *) * (num_hosts + 1));
			if (maintenance_hosts == NULL) {
				fprintf(stderr, "pbs_rsub: Out of memory\n");
				return (++errflg);
			}

			maintenance_hosts[num_hosts] = NULL;

			for (i = 0; optind < argc; optind++, i++) {
				hostp = maintenance_hosts;
				for (; *hostp; hostp++) {
					if (strcmp(*hostp, argv[optind]) == 0) {
						fprintf(stderr, "pbs_rsub: Duplicate host: %s\n", argv[optind]);
						return (++errflg);
					}
				}

				if (strlen(argv[optind]) == 0) {
					num_hosts--;
					i--;
					maintenance_hosts[num_hosts] = NULL;
					continue;
				}

				maintenance_hosts[i] = strdup(argv[optind]);
				if (maintenance_hosts[i] == NULL) {
					fprintf(stderr, "pbs_rsub: Out of memory\n");
					return (++errflg);
				}
			}
		}

		if (maintenance_hosts == NULL) {
			fprintf(stderr, "pbs_rsub: missing host(s)\n");
			return (++errflg);
		}
	}

	if (!errflg) {
		errflg = (optind != argc);
		if (errflg) {
			fprintf(stderr, "pbs_rsub: directive error: ");
			for (optind=1; optind<argc; optind++)
				fprintf(stderr, "%s ", argv[optind]);
			fprintf(stderr, "\n");
		}
	}

	*attrp = attrib;
	return (errflg);
}

/**
 * @brief
 *	sets the environment for reservation
 *
 * @param[in] envp - pointer to pointer to the environment variable
 *
 * @return - Boolean value
 * @retval   TRUE  Success
 * @retval   FALSE Failure
 *
 */
int
set_resv_env(char **envp)
{
	char *job_env;
	char *c, *env;
	char host[PBS_MAXHOSTNAME+1];
	int len;
	int rc;

	/* Calculate how big to make the variable string. */
	len = 0;
	env = getenv("HOME");
	if (env != NULL) len += strlen(env);
	env = getenv("LANG");
	if (env != NULL) len += strlen(env);
	env = getenv("LOGNAME");
	if (env != NULL) len += strlen(env);
	env = getenv("PATH");
	if (env != NULL) len += strlen(env);
	env = getenv("MAIL");
	if (env != NULL) len += strlen(env);
	env = getenv("SHELL");
	if (env != NULL) len += strlen(env);
	env = getenv("TZ");
	if (env != NULL) len += strlen(env);
	len += PBS_MAXHOSTNAME;
	len += MAXPATHLEN;
	len += len;     /* Double it for all the commas, etc. */
	if ((job_env = (char *) malloc(len)) == NULL) {
		fprintf(stderr, "pbs_rsub: Out of memory\n");
		return FALSE;
	}
	*job_env = '\0';

	/* Send the required variables with the job. */
	c = getenv("LOGNAME");
	if (c != NULL) {
		strcat(job_env, "PBS_O_LOGNAME=");
		strcat(job_env, c);
	}
	if ((rc = gethostname(host, (sizeof(host) - 1))) == 0) {
		if ((rc = get_fullhostname(host, host, (sizeof(host) - 1))) == 0) {
			if (*job_env)
				strcat(job_env, ",PBS_O_HOST=");
			else
				strcat(job_env, "PBS_O_HOST=");
			strcat(job_env, host);
		}
	}

	c = getenv("MAIL");
	if (c != NULL) {
#ifdef WIN32
		back2forward_slash(c);
#endif
		strcat(job_env, ",PBS_O_MAIL=");
		strcat(job_env, c);
	}
	if (rc != 0) {
		fprintf(stderr, "pbs_rsub: cannot get full local host name\n");
		exit(3);
	}
	c = getenv("PBS_TZID");
	if (c != NULL) {
		strcat(job_env, ",PBS_TZID=");
		strcat(job_env, c);
		set_attr_error_exit(&attrib, ATTR_resv_timezone, c);
	}
	else if (is_stdng_resv) {
		fprintf(stderr, "pbs_rsub error: a valid PBS_TZID timezone environment variable is required.\n");
		exit(2);
	}

	set_attr_error_exit(&attrib, ATTR_v, job_env);
	free(job_env);
	return TRUE;
}

/**
 * @brief
 *	converts and processes the attribute values
 *
 * @param[in] connect - indiacation for connection of server
 * @param[in] attrp   - attribute list
 * @param[in] dest    - server option
 *
 * @return - int
 * @retval   0 Success
 * @retval   exits on failure
 *
 */
int
cnvrt_proc_attrib(int connect, struct attrl **attrp, char *dest)
{
	char *str;
	int setflag, cnt = 0;
	struct attropl *jobid_ptr;
	struct batch_status *p, *p_status;
	struct attrl *a, *ap, *apx, *attr, *cmd_attr;
	char time_buf[80];
	char job[PBS_MAXCLTJOBID];
	char server[MAXSERVERNAME];

	jobid_ptr = (struct attropl *)attrib;
	while (jobid_ptr != NULL) {
		if (strcmp(jobid_ptr->name, ATTR_convert) == 0)
			break;
		jobid_ptr = jobid_ptr->next;
	}

	if (get_server(jobid_ptr->value, job, server)) {
		fprintf(stderr, "pbs_rsub: illegally formed job identifier: %s\n", jobid_ptr->value);
		exit(-1);
	}
	/* update value string with full job-id (seqnum.server) */
	(void)free(jobid_ptr->value);
	jobid_ptr->value = strdup(job);
	if (jobid_ptr->value == NULL) {
		fprintf(stderr, "Out of memory\n");
		exit(2);
	}

	p_status = pbs_statjob(connect, jobid_ptr->value, NULL, NULL);
	if (p_status == NULL) {
		fprintf(stderr, "Job %s does not exist\n", jobid_ptr->value);
		exit(2);
	}

	p = p_status;
	while (p != NULL) {
		a = p->attribs;
		while (a != NULL) {
			if (a->name != NULL) {
				/* avoid qmove job in R, T or E state */
				if (strcmp(a->name, ATTR_state) == 0) {
					if (strcmp(a->value, "R") == 0 ||
						strcmp(a->value, "T") == 0 ||
						strcmp(a->value, "E") == 0) {
						fprintf(stderr, "Job not in qmove state\n");
						exit(2);
					}
				} else {
					if (strcmp(a->name, ATTR_l) == 0 &&
						strcmp(a->resource, "nodect") != 0 &&
						strcmp(a->resource, "neednodes") != 0) {
						setflag = FALSE;
						ap = attrib;
						while (ap != NULL) {
							if (ap->resource != NULL) {
								if (strcmp(ap->resource, a->resource) == 0) {
									setflag = TRUE;
								}
							}
							if (ap->next == NULL && setflag == FALSE) {
								attr = new_attrl();
								if (attr == NULL) {
									fprintf(stderr, "pbs_rsub: Out of memory\n");
									exit(2);
								}

								str = strdup(ATTR_l);
								if (str == NULL) {
									fprintf(stderr, "pbs_rsub: Out of memory\n");
									exit(2);
								}
								attr->name = str;

								str = strdup(a->resource);
								if (str == NULL) {
									fprintf(stderr, "pbs_rsub: Out of memory\n");
									exit(2);
								}
								attr->resource = str;

								if (a->value != NULL) {
									str = strdup(a->value);
									if (str == NULL) {
										fprintf(stderr, "pbs_rsub: Out of memory\n");
										exit(2);
									}
									attr->value = str;
								} else {
									str = (char *) malloc(1);
									if (str == NULL) {
										fprintf(stderr, "pbs_rsub: Out of memory\n");
										exit(2);
									}
									str[0] = '\0';
									attr->value = str;
								}
								attr->next = NULL;
								ap->next = attr;
								ap = ap->next;
							}
							setflag = FALSE;
							ap = ap->next;
						}
					}
				}
			}
			a = a->next;
		}
		p = p->next;
	}
	pbs_statfree(p_status);

	cmd_attr = attrib;
	while (cmd_attr != NULL) {
		if (strcmp(cmd_attr->name, ATTR_resv_start) == 0 ||
			strcmp(cmd_attr->name, ATTR_resv_end) == 0) {
			if (cmd_attr->name != NULL)
				free(cmd_attr->name);
			if (cmd_attr->resource != NULL)
				free(cmd_attr->resource);
			if (cmd_attr->value != NULL)
				free(cmd_attr->value);
			apx = cmd_attr->next;
			free(cmd_attr);
			cmd_attr = apx;
			if (cnt == 0) attrib = cmd_attr;
			cnt++;
		} else
			cmd_attr = cmd_attr->next;
	}

	(void)sprintf(time_buf, "%ld", PBS_RESV_FUTURE_SCH);
	set_attr_error_exit(&attrib, ATTR_resv_start, time_buf);
	*attrp = attrib;

	return (0);
}

/**
 * @brief
 * 	prints usage format for pbs_rsub command
 *
 * @return - Void
 *
 */
static void
print_usage()
{
	static char usag2[]="       pbs_rsub --version\n";
	static char usage[]=
		"usage: pbs_rsub [-I seconds] [-m mail_points] [-M mail_list]\n"
	"                [-N reservation_name] [-u user_list] [-g group_list]\n"
	"                [-U auth_user_list] [-G auth_group_list] [-H auth_host_list]\n"
	"                [-R start_time] [-E end_time] [-D duration] [-q destination]\n"
	"                [-r rrule_expression] [-W otherattributes=value...]\n"
	"                -l resource_list | --hosts host1 [... hostn]\n";

	fprintf(stderr, "%s", usage);
	fprintf(stderr, "%s", usag2);
}

/**
 * @brief
 * 	handles attribute errors and prints appropriate errmsg
 *
 * @param[in] err_list - list of possible attribute errors
 *
 * @return - Void
 *
 */
static void
handle_attribute_errors(struct ecl_attribute_errors *err_list)
{
	struct attropl *attribute;
	char * opt;
	int i;

	for (i=0; i<err_list->ecl_numerrors; i++) {
		attribute = err_list->ecl_attrerr[i].ecl_attribute;
		if (strcmp(attribute->name, ATTR_resv_end) == 0)
			opt="E";
		else if (strcmp(attribute->name, ATTR_g)==0)
			opt="g";
		else if (strcmp(attribute->name, ATTR_auth_g)==0)
			opt="G";
		else if (strcmp(attribute->name, ATTR_auth_h)==0)
			opt="H";
		else if (strcmp(attribute->name, ATTR_inter) == 0)
			opt="I";
		else if (strcmp(attribute->name, ATTR_l) == 0)
			opt="l";
		else if (strcmp(attribute->name, ATTR_m) == 0)
			opt="m";
		else if (strcmp(attribute->name, ATTR_M) == 0)
			opt="M";
		else if (strcmp(attribute->name, ATTR_resv_name)==0)
			opt="N";
		else if (strcmp(attribute->name, ATTR_resv_start)==0)
			opt="R";
		else if (strcmp(attribute->name, ATTR_resv_rrule)==0)
			opt="r";
		else if (strcmp(attribute->name, ATTR_u)==0)
			opt="u";
		else if (strcmp(attribute->name, ATTR_auth_u)==0)
			opt="U";
		else if (strcmp(attribute->name, ATTR_convert)==0)
			opt="W";
		else
			return ;

		CS_close_app();
		if (*opt == 'l') {
			fprintf(stderr, "pbs_rsub: %s\n",
				err_list->ecl_attrerr[i].ecl_errmsg);
			exit(err_list->ecl_attrerr[i].ecl_errcode);
		} else if (err_list->ecl_attrerr->ecl_errcode == PBSE_JOBNBIG) {
			fprintf(stderr, "pbs_rsub: Reservation %s \n", err_list->ecl_attrerr->ecl_errmsg);
			exit(2);
		} else {
			fprintf(stderr, "pbs_rsub: illegal -%s value\n", opt);
			print_usage();
			exit(2);
		}
	}
}

/**
 * @brief
 *	The main function in C - entry point
 *
 * @param[in]  argc - argument count
 * @param[in]  argv - pointer to argument array
 * @param[in]  envp - pointer to environment values
 *
 * @return  int
 * @retval  0 - success
 * @retval  !0 - error
 */
int
main(int argc, char *argv[], char *envp[])
{
	int errflg;			/* command line option error */
	int connect;			/* return from pbs_connect */
	char *errmsg;			/* return from pbs_geterrmsg */
	char destbuf[256];		/* buffer for option server */
	struct attrl *attrib;		/* the attrib list */
	char *new_resvname;		/* the name returned from pbs_submit_resv */
	struct ecl_attribute_errors *err_list;
	char *interactive = NULL;
	char *reservid = NULL;
	char extend[2];
	struct batch_status *bstat_head = NULL;
	struct batch_status *bstat = NULL;
	struct attrl *pattr = NULL;
	char *execvnodes_str = NULL;
	int execvnodes_str_size = 0;
	char *select_str = NULL;
	int select_str_size = 0;
	char **hostp = NULL;
	struct attrl *pal;
	char *erp;
	char *host = NULL;
	char *endp;   /* used for strtol() */

	/*test for real deal or just version and exit*/

	PRINT_VERSION_AND_EXIT(argc, argv);

#ifdef WIN32
	if (winsock_init()) {
		return 1;
	}
#endif

	destbuf[0] = '\0';
	extend[0] = '\0';
	errflg = process_opts(argc, argv, &attrib, destbuf); /* get cmdline options */

	if (errflg || ((optind+1) < argc) || argc == 1) {
		print_usage();
		exit(2);
	}

	if (is_maintenance_resv) {
		pal = attrib;
		while (pal) {
			if ((strcasecmp(pal->name, ATTR_l) == 0) &&
				(strcasecmp(pal->resource, "select") == 0)){
				fprintf(stderr, "pbs_rsub: can't use -l select with --hosts\n");
				print_usage();
				exit(2);
			}
			if ((strcasecmp(pal->name, ATTR_l) == 0) &&
				(strcasecmp(pal->resource, "place") == 0)) {
				fprintf(stderr, "pbs_rsub: can't use -l place with --hosts\n");
				print_usage();
				exit(2);
			}
			pal = pal->next;
		}
	}

	/* Get any required environment variables needing to be sent. */
	if (! set_resv_env(envp)) {
		fprintf(stderr, "pbs_rsub: can't send environment with the reservation\n");
		exit(3);
	}

	/*perform needed security library initializations (including none)*/

	if (CS_client_init() != CS_SUCCESS) {
		fprintf(stderr, "pbs_rsub: unable to initialize security library.\n");
		exit(1);
	}

	/* Connect to the server */
	connect = cnt2server(destbuf);
	if (connect <= 0) {
		fprintf(stderr, "pbs_rsub: cannot connect to server %s (errno=%d)\n",
			pbs_server, pbs_errno);
		CS_close_app();
		exit(pbs_errno);
	}

	if (qmoveflg == TRUE) {
		qmoveflg = FALSE;
		interactive = get_attr(attrib, ATTR_inter, NULL);
		if (interactive == NULL) {
			set_attr_error_exit(&attrib, ATTR_inter, DEFAULT_INTERACTIVE);
		} else {
			if (atoi(interactive) > -1) {
				fprintf(stderr, "pbs_rsub: -I <timeout> value must be negative when used with -Wqmove option.\n");
				CS_close_app();
				exit(2);
			}
		}
		errflg = cnvrt_proc_attrib(connect, &attrib, destbuf);
		if (errflg) {
			fprintf(stderr, "pbs_rsub: can't make a reservation with the qmove option\n");
			CS_close_app();
			exit(2);
		}
	}

	if (is_maintenance_resv) {
		int i;
		char tmp_str[BUF_SIZE];
		char *endp;

		pbs_errno = 0;
		bstat_head = pbs_statvnode(connect, "", NULL, NULL);
		if (bstat_head == NULL) {
			if (pbs_errno) {
				errmsg = pbs_geterrmsg(connect);
				if (errmsg != NULL) {
					fprintf(stderr, "pbs_rsub: %s\n", errmsg);
				} else {
					fprintf(stderr, "pbs_rsub: Error (%d) submitting reservation\n", pbs_errno);
				}
			} else {
				fprintf(stderr, "pbs_rsub: No nodes found\n");
			}

			CS_close_app();
			exit(pbs_errno);
		}

		hostp = maintenance_hosts;
		for (; *hostp; hostp++) {
			int host_ncpus = 0;

			for (bstat = bstat_head; bstat; bstat = bstat->next) {
				char *ncpus_str = NULL;
				int ncpus = 0;

				for (pattr = bstat->attribs; pattr; pattr = pattr->next) {
					if (pattr->resource && strcmp(pattr->name, ATTR_rescavail) == 0 && strcmp(pattr->resource, "host") == 0)
						host = pattr->value;
					if (pattr->resource && strcmp(pattr->name, ATTR_rescavail) == 0 && strcmp(pattr->resource, "ncpus") == 0)
						ncpus_str = pattr->value;
				}

				if (ncpus_str != NULL)
					ncpus = strtol(ncpus_str, &endp, 0);
				
				if (*endp != '\0') {
					fprintf(stderr, "pbs_rsub: Attribute value error\n");
					CS_close_app();
					exit(2);
				}

				/* here, the execvnodes is crafted */
				if (strcmp(host, *hostp) == 0 && ncpus > 0) {
					/* count ncpus of a host across vnodes
					 * it will be used for crafting select
					 */
					host_ncpus += ncpus;

					if (!execvnodes_str) {
						execvnodes_str = malloc(BUF_SIZE);
						if (execvnodes_str == NULL) {
							fprintf(stderr, "pbs_rsub: Out of memory\n");
							CS_close_app();
							exit(2);
						}
						execvnodes_str_size = BUF_SIZE;

						snprintf(execvnodes_str, BUF_SIZE, "(%s:ncpus=%d)", bstat->name, ncpus);
					} else {
						snprintf(tmp_str, BUF_SIZE, "+(%s:ncpus=%d)", bstat->name, ncpus);

						if (pbs_strcat(&execvnodes_str, &execvnodes_str_size, tmp_str) == NULL) {
							fprintf(stderr, "pbs_rsub: Out of memory\n");
							CS_close_app();
							exit(2);
						}
					}
				} /* end of part that crafts execvnodes */
			}

			/* host not found or host has zero ncpus */
			if (host_ncpus == 0) {
				fprintf(stderr, "pbs_rsub: Host with resources not found: %s\n", *hostp);
				CS_close_app();
				exit(2);
			}

			/* here, the select is crafted */
			if (host_ncpus > 0) {
				if (!select_str) {
					select_str = malloc(BUF_SIZE);
					if (select_str == NULL) {
						fprintf(stderr, "pbs_rsub: Out of memory\n");
						CS_close_app();
						exit(2);
					}
					select_str_size = BUF_SIZE;

					snprintf(select_str, BUF_SIZE, "select=host=%s:ncpus=%d", *hostp, host_ncpus);
				} else {
					snprintf(tmp_str, BUF_SIZE, "+host=%s:ncpus=%d", *hostp, host_ncpus);

					if (pbs_strcat(&select_str, &select_str_size, tmp_str) == NULL) {
						fprintf(stderr, "pbs_rsub: Out of memory\n");
						CS_close_app();
						exit(2);
					}
				}
			} /* end of part that crafts select */
		}

		pbs_statfree(bstat_head);	/* free info returned by pbs_statvnodes() */

		if (select_str == NULL) {
			fprintf(stderr, "pbs_rsub: missing host(s)\n");
			print_usage();

			CS_close_app();
			exit(2);
		}

		/* add crafted select */
		if ((i = set_resources(&attrib, select_str, 0, &erp)) != 0) {
			if (i > 1) {
				pbs_prt_parse_err("pbs_rsub: illegal -l value\n", select_str,
					    (int)(erp-select_str), i);
			} else {
				fprintf(stderr, "pbs_rsub: illegal -l value\n");
			}

			CS_close_app();
			exit(pbs_errno);
		}

		/* add place=exclhost */
		if (set_resources(&attrib, "place=exclhost", 0, &erp) != 0) {
			fprintf(stderr, "pbs_rsub: illegal -l value\n");

			CS_close_app();
			exit(pbs_errno);
		}

		strcat(extend, "m");
	}

	pbs_errno = 0;
	new_resvname = pbs_submit_resv(connect, (struct attropl *)attrib, extend);
	if (new_resvname == NULL) {
		if ((err_list = pbs_get_attributes_in_error(connect)))
			handle_attribute_errors(err_list);

		errmsg = pbs_geterrmsg(connect);
		if (errmsg != NULL) {
			fprintf(stderr, "pbs_rsub: %s\n", errmsg);
		}
		else
			fprintf(stderr, "pbs_rsub: Error (%d) submitting reservation\n", pbs_errno);
		CS_close_app();
		exit(pbs_errno);
	}

	if (is_maintenance_resv) {
		char *rest;
		char *resv_start_time_str;
		time_t resv_start_time = 0;

		reservid = strtok_r(new_resvname, " ", &rest);

		resv_start_time_str = get_attr(attrib, ATTR_resv_start, NULL);
		if (resv_start_time_str)
			resv_start_time = strtol(resv_start_time_str, &endp, 10);

		pbs_errno = 0;
		if (pbs_confirmresv(connect, reservid, execvnodes_str, resv_start_time, PBS_RESV_CONFIRM_SUCCESS) > 0) {
			errmsg = pbs_geterrmsg(connect);
			if (errmsg == NULL)
				errmsg = "";

			fprintf(stderr, "pbs_rsub: PBS Failed to confirm resv: %s (%d)\n", errmsg, pbs_errno);

			CS_close_app();
			exit(pbs_errno);
		}

		printf("%s CONFIRMED\n", reservid);
	} else {
		printf("%s\n", new_resvname);
	}

	free(new_resvname);

	if (maintenance_hosts) {
		hostp = maintenance_hosts;
		for (; *hostp; hostp++)
		    free(*hostp);
		free(maintenance_hosts);
	}

	/* Disconnet from the server. */
	pbs_disconnect(connect);

	CS_close_app();
	exit(0);
}
