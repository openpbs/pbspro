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
 * @file    setup_resc.c
 *
 * @brief
 * 		setup_resc.c	-	File contains functions related to setting up of resources.
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include "linked_list.h"
#include "attribute.h"
#include "resource.h"
#include "pbs_error.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "log.h"
#include "pbs_python.h"
#include "sched_cmds.h"
#include "pbs_nodes.h"
#include <sys/file.h>
#include "libutil.h"

extern char *msg_daemonname;
#ifndef PBS_MOM
extern struct python_interpreter_data  svr_interp_data;

struct resc_sum *svr_resc_sum;

/**
 * @brief
 * 		Helper function to restart the Python interpreter and record the
 * 		occurrence in the log.
 *
 * @param[in]	caller	-	The name of the calling function (for logging)
 */
void
restart_python_interpreter(const char *caller)
{
	log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_HOOK,
		LOG_INFO, (char *) caller,
		"Restarting Python interpreter as resourcedef file has changed.");
	pbs_python_ext_shutdown_interpreter(&svr_interp_data);
	pbs_python_ext_start_interpreter(&svr_interp_data);
}
#endif

/**
 * @brief
 * 		Add a resource to the resource definition array and update the
 * 		resourcedef file
 *
 * @param[in]	name	-	The name of the resource to operate on
 * @param[in]	type	-	The type of the resource
 * @param[in]	perms	-	The permissions/flags of the resource
 *
 * @return	int
 * @retval	-2	: if resource already exists as a different type or the h flag
 * 					is being modified
 * @retval	-1	: on any other error
 * @retval	0	: if ok
 */
int
add_resource_def(char *name, int type, int perms)
{
	resource_def *prdef;
	int rc;

	/* first see if the resource "name" already exists */
	if ((prdef=find_resc_def(svr_resc_def, name, svr_resc_size)) != NULL) {
		if (prdef->rs_type != type)
			return -2;
		if ((prdef->rs_flags & ATR_DFLAG_CVTSLT) != (perms & ATR_DFLAG_CVTSLT))
			return -2;
		return 0;	/* there are correct, just return */
	}

	if (expand_resc_array(name, type, perms) == -1) {
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_RESC, LOG_ERR, msg_daemonname, "Error creating resource");
		return -1;
	}

	rc = update_resource_def_file(name, RESDEF_CREATE, type, perms);
	if (rc < 0) {
		snprintf(log_buffer, sizeof(log_buffer),
				"resource %s can not be defined", name);
		log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER,
		LOG_ERR, msg_daemonname, log_buffer);
		return -1;

	}
#ifndef PBS_MOM
	set_scheduler_flag(SCH_CONFIGURE);
#endif

	return 0;
}

/**
 * @brief
 * 		Helper function to determine whether a line in the resourcedef file
 * 		is an exact match to a given resource name
 *
 * @param[in]	line	-	The line in a resourcedef file
 * @param[name]	name	-	The name of the resource to match
 * @note
 * 		Note that entries in the resourcedef file are of the form
 *		<resource name><white space>type=<type><white space>[flag=<flag>]
 *
 * @return	int
 * @retval	1	: if a match is found
 * @retval	0	: otherwise
 */
static int
is_res_in_line(char *line, char *name)
{
	int i, j;

	if ((line == NULL) || (name == NULL))
		return 0;

	for (i=0; (line[i] != '\0') && isspace(line[i]); i++)
		;

	for (j=0; (line[i] != '\0') && (name[j] != '\0') && (line[i] == name[j]); i++, j++)
		;

	if ((j == 0) || (name[j] != '\0'))
		return 0;

	if (!isspace(line[i]) && (line[i] != '\0'))
		return 0;

	return 1;
}

/**
 * @brief
 * 		modify a resource type/flag in the resourcedef file
 *
 * @param[in]	name	-	The name of the resource to operate on
 * @param[in]	op	-	The operation to perform, one of RESDEF_CREATE,
 * 						RESDEF_UPDATE, RESDEF_DELETE
 * @param[in]	type	-	The type of the resource
 * @param[in]	perms	-	The permissions/flags of the resource
 *
 * @return	Whether the operation was successful or not
 * @retval	-1	: on error
 * @retval	0	: on success
 */
int
update_resource_def_file(char *name, resdef_op_t op, int type, int perms)
{
	FILE *rfile;
	FILE *tmpfile;
	int tmp_fd;
	int fd;
	extern char  *path_rescdef;
	char template[] = "pbstmpXXXXXX";
	char *line;
	int line_len = 256;
	char msg[LOG_BUF_SIZE];
	struct resc_type_map *p_resc_type_map = NULL;
	char *flags = NULL;
	int rc;

	fd = open(path_rescdef, O_CREAT | O_RDONLY, 0644);
	if (fd == -1)
		return -1;

	if ((rfile = fdopen(fd, "r")) == NULL) {
		close(fd);
		return -1;
	}
	tmp_fd = mkstemp(template);
#ifdef WIN32
	if (fopen_s(&tmpfile, template, "w") != 0)
		return -1;

	secure_file2(template, "Administrators",
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
			getlogin_full(),
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
#else
	tmpfile = fdopen(tmp_fd, "w");
	/* set mode bits because mkstemp() created files don't ensure 0644 */
	fchmod(tmp_fd, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
#endif
	line = malloc(line_len * sizeof(char));
	if (line == NULL) {
		log_err(errno, __func__, MALLOC_ERR_MSG);
		fclose(tmpfile);
		unlink(template);
		return -1;
	}

	if (lock_file(rfile, F_RDLCK, path_rescdef, LOCK_RETRY_DEFAULT, msg, sizeof(msg)) != 0) {
		log_err(errno, __func__, msg);
		fclose(rfile);
		fclose(tmpfile);
		unlink(template);
		free(line);
		return -1;
	}
	if ((op == RESDEF_UPDATE) || (op == RESDEF_CREATE)) {
		p_resc_type_map = find_resc_type_map_by_typev(type);
		if (p_resc_type_map == NULL) {
			(void)fclose(rfile);
			free(line);
			fclose(tmpfile);
			unlink(template);
			return -1;
		}
		flags = find_resc_flag_map(perms);
	}

	while (pbs_fgets(&line, &line_len, rfile)) {
		if (((op == RESDEF_UPDATE) || (op == RESDEF_DELETE)) &&
			((line[0] != '#') && (is_res_in_line(line, name)))) {
			if (op == RESDEF_UPDATE) {
				fprintf(tmpfile, "%s type=%s", name, p_resc_type_map->rtm_rname);
				if ((flags != NULL) && (flags[0] != '\0')) {
					fprintf(tmpfile, " flag=%s", flags);
				}
				fprintf(tmpfile, "\n");
			}
			else if (op == RESDEF_DELETE) {
				continue;
			}
		}
		else {
			fprintf(tmpfile, "%s", line);
		}
	}
	if (op == RESDEF_CREATE) {
		fprintf(tmpfile, "%s type=%s", name, p_resc_type_map->rtm_rname);
		if ((flags != NULL) && (strcmp(flags, "") != 0)) {
			fprintf(tmpfile, " flag=%s", flags);
		}
		fprintf(tmpfile, "\n");
	}

	if (lock_file(rfile, F_UNLCK, path_rescdef, LOCK_RETRY_DEFAULT, msg, sizeof(msg)) != 0)
		log_err(errno, __func__, msg);

	(void)fclose(rfile);
	(void) fclose(tmpfile);

	free(line);
	free(flags);

	rc = 0;
#ifdef WIN32
	if (MoveFileEx(template, path_rescdef, MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) == 0) {
		errno = GetLastError();
		rc = 1;
	}
#else
	if (rename(template, path_rescdef) != 0) {
		rc = 1;
	}
#endif
	if (rc != 0) {
		snprintf(log_buffer, sizeof(log_buffer), "error renaming resourcedef file");
		log_err(errno, __func__, log_buffer);
		unlink(template);
		return -1;
	}

	unlink(template);

	return 0;
}

/**
 * @brief
 * 		expand_resc_array - expand the list (no longer an array) of resource
 *		definitions, linking the new to the current last entry
 *
 * @param[in]	rname	-	The name of the resource to operate on
 * @param[in]	rtype	-	The type of the resource
 * @param[in]	rflag	-	The permissions/flags of the resource
 *
 * @return	int
 * @retval	-1	- error
 * @retval	0	- success
 */
int
expand_resc_array(char *rname, int rtype, int rflag)
{
	resource_def *pnew;
	resource_def *pold;
	struct resc_type_map *p_resc_type_map;

	/* get mapping between type and functions */

	p_resc_type_map = find_resc_type_map_by_typev(rtype);
	if (p_resc_type_map == NULL)
		return -1;

	/* find the old last entry */
	pold = svr_resc_def;
	while (pold->rs_next)
		pold = pold->rs_next;

	/* allocate new resc_def entry */

	pnew = (resource_def *) malloc(sizeof(resource_def));
	if (pnew == NULL)
		return (-1);

	if ((pnew->rs_name = strdup(rname)) == NULL) {
		free(pnew);
		return (-1);
	}
	pnew->rs_decode = p_resc_type_map->rtm_decode;
	pnew->rs_encode = p_resc_type_map->rtm_encode;
	pnew->rs_set    = p_resc_type_map->rtm_set;
	pnew->rs_comp   = p_resc_type_map->rtm_comp;
	pnew->rs_free   = p_resc_type_map->rtm_free;
	pnew->rs_action = NULL_FUNC;
	pnew->rs_flags = rflag;
	pnew->rs_type  = rtype;
	pnew->rs_entlimflg = 0;
	pnew->rs_next  = (struct resource_def *)0;

	pold->rs_next  = pnew;
	svr_resc_size++;

	return 0;
}

/**
 * @brief
 * 		Setup resource definitions
 * @par
 * 		Read the file, "resourcedef", which defines new resources.
 * 		Expand the array of resource_defs
 *
 * @param[in]	autocorrect	-	Whether to autocorrect (when possible) erroneous
 * 								resource flags/type combinations.
 *
 * @retval	-1	: on error
 * @retval	-2	: on error that got auto-corrected
 * @retval	0	: otherwise.
 * @par
 *  	Format of entries in the file are:
 *	    resource name type=x flag=y
 * 	where
 *		x is "long", "float",  "size", "boolean" or  "string"
 *		y is a combination of the characters 'n' and 'q'
 * @par
 *		If routine returns -1 or -2, then "log_buffer" contains a message to
 *		be logged.
 *
 * @par MT-safe: No
 */

int
setup_resc(int autocorrect)
{
	FILE	     *nin;
	char	     *line = NULL;
	char	     buf[4096];
	char	     *token;
	int	      linenum;
	int	      err;
	char	     *val;
	char	      xchar;
	char	     *rescname;
	int	      resc_type;
	int	      resc_flag;
	int	      flag_ir = 0;
	int	      rc;
	int	      err_code = -1;
	int	      len = 0;
	resource_def *presc;
	extern char  *path_rescdef;
	static char  *invalchar = "invalid character in resource "
		"name \"%s\" on line %d of ";
	static char * invalchar_skip = "invalid character in resource name \"%s\"";

	if ((nin = fopen(path_rescdef, "r")) == NULL) {
		return 0;
	}

	for (linenum=1; pbs_fgets(&line, &len, nin); linenum++) {
		resc_flag = READ_WRITE;
		resc_type = ATR_TYPE_LONG;

		if (line[0] == '#')	/* comment */
			continue;

		/* first token is the resource name */

		token = parse_node_token(line, 1, &err, &xchar);
		if (token == NULL)
			continue;	/* blank line */
		if (err) {

			if (autocorrect) {
				if (err_code != -2) {
					 err_code = -2;
				 }
				snprintf(log_buffer, sizeof(log_buffer), invalchar_skip, token);
				fprintf(stderr, "%s\n", log_buffer);
				log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
					LOG_WARNING, msg_daemonname, log_buffer);
				continue;
			}
			else {
				snprintf(log_buffer, sizeof(log_buffer), invalchar, token, linenum);
				goto errtoken2;
			}
		}

		rc = verify_resc_name(token);
		if (rc == -1) {
			if (autocorrect) {
				 if (err_code != -2) {
					err_code = -2;
				 }
				snprintf(log_buffer, sizeof(log_buffer), "resource name \"%s\" does not "
					"start with alpha; ignoring resource.", token);
				fprintf(stderr, "%s\n", log_buffer);
				log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
					LOG_WARNING, msg_daemonname, log_buffer);
				continue;
			}
			else {
				snprintf(log_buffer, sizeof(log_buffer), "resource name \"%s\" does not "
					"start with alpha on line %d of ",
					token, linenum);
				goto errtoken2;
			}
		}
		else if (rc == -2) {
			if (autocorrect) {
				if (err_code != -2) {
					err_code = -2;
				}
				snprintf(log_buffer, sizeof(log_buffer), invalchar_skip, token);
				fprintf(stderr, "%s\n", log_buffer);
				log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER, LOG_WARNING, msg_daemonname, log_buffer);
				continue;
			}
			else {
				snprintf(log_buffer, sizeof(log_buffer), invalchar, token, linenum);
				goto errtoken2;
			}
		}

		rescname = token;

		/* now process remaining tokens (if any), 	*/
		/* they must be of the form keyword=value	*/

		while (1) {

			token = parse_node_token(NULL, 0, &err, &xchar);
			if (err) {
				if (autocorrect) {
					if (err_code != -2) {
						err_code = -2;
					}
					snprintf(log_buffer, sizeof(log_buffer), invalchar_skip, token);
					fprintf(stderr, "%s\n", log_buffer);
					log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
						LOG_WARNING, msg_daemonname, log_buffer);
					break;
				}
				else {
					goto errtoken1;
				}
			}
			if (token == NULL)
				break;

			if (xchar == '=') {

				/* have  keyword=value */

				val = parse_node_token(NULL, 0, &err, &xchar);
				if ((val==NULL) || err || (xchar=='=')) {
					if (autocorrect) {
						if (err_code != -2) {
							err_code = -2;
						}
						snprintf(log_buffer, sizeof(log_buffer), invalchar_skip, token);
						fprintf(stderr, "%s\n", log_buffer);
						log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
							LOG_WARNING, msg_daemonname, log_buffer);
						break;
					}
					else {
						goto errtoken1;
					}
				}

				if (strcmp(token, "type") == 0) {
					if (parse_resc_type(val, &resc_type) == -1) {
						if (autocorrect) {
							if (err_code != -2) {
								err_code = -2;
							}
							snprintf(log_buffer, sizeof(log_buffer), "invalid resource type %s", val);
							fprintf(stderr, "%s\n", log_buffer);
							log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
								LOG_WARNING, msg_daemonname, log_buffer);
							break;
						}
						else {
							goto errtoken1;
						}
					}
				}
				else if (strcmp(token, "flag") == 0) {
					if (parse_resc_flags(val, &flag_ir, &resc_flag) == -1) {
						if (autocorrect) {
							if (err_code != -2) {
								err_code = -2;
							}
							snprintf(log_buffer, sizeof(log_buffer), "Invalid resource flag %s", val);
							fprintf(stderr, "%s\n", log_buffer);
							log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
								LOG_WARNING, msg_daemonname, log_buffer);
							break;
						}
						else {
							goto errtoken1;
						}
					}
				}
				else {
					if (autocorrect) {
						if (err_code != -2) {
							err_code = -2;
						}
						snprintf(log_buffer, sizeof(log_buffer), "Unrecognized token %s; skipping", token);
						fprintf(stderr, "%s\n", log_buffer);
						log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
							LOG_WARNING, msg_daemonname, log_buffer);
						break;
					}
					else {
						goto errtoken1;
					}
				}
			}
			else {
				if (autocorrect) {
					if (err_code != -2) {
						err_code = -2;
					}
					snprintf(log_buffer, sizeof(log_buffer), "Unrecognized token %s; skipping", token);
					fprintf(stderr, "%s\n", log_buffer);
					log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
						LOG_WARNING, msg_daemonname, log_buffer);
					break;
				}
				else {
					goto errtoken1;
				}
			}
		}

		rc = verify_resc_type_and_flags(resc_type, &flag_ir, &resc_flag, rescname, buf, sizeof(buf), autocorrect);
		if (rc != 0) {
			fprintf(stderr, "%s\n", buf);
			log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
				LOG_WARNING, msg_daemonname, buf);
			/* with autocorrect enabled a return code of -2 would be returned on error */
			if (rc == -1) {
				goto errtoken3;
			}
		}
		/* create resource definition */

		presc = find_resc_def(svr_resc_def, rescname, svr_resc_size);
		if (presc != NULL) {
			if (resc_type == presc->rs_type) {
				resc_flag &= ( ATR_DFLAG_RASSN |
					ATR_DFLAG_ANASSN |
					ATR_DFLAG_FNASSN |
					ATR_DFLAG_CVTSLT |
					READ_WRITE );
				presc->rs_flags &= ~( ATR_DFLAG_RASSN |
						ATR_DFLAG_ANASSN |
						ATR_DFLAG_FNASSN |
						ATR_DFLAG_CVTSLT |
						READ_WRITE );
				presc->rs_flags |= resc_flag;
#ifndef PBS_MOM
			} else {
				sprintf(log_buffer,
					"Erroneous to define duplicate "
					"resource \"%s\" with differing type "
					"specification, ignoring new definition",
						rescname);
				fprintf(stderr, "%s\n", log_buffer);
				log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_SERVER,
					LOG_WARNING, msg_daemonname, log_buffer);
#endif
			}
		} else {
			err = expand_resc_array(rescname, resc_type, resc_flag);
			if (err == -1) {
				(void)strcpy(log_buffer, "error allocating memory in setup_resc");
				goto errtoken3;
			}
		}
	}

	free(line);
	fclose(nin);
	return 0;

errtoken1:
	sprintf(log_buffer,"token \"%s\" in error on line %d of ",
		token, linenum);
errtoken2:
	strcat(log_buffer, path_rescdef);
errtoken3:
	free(line);
	fclose(nin);
	return err_code;
}

#ifndef PBS_MOM
/**
 * @brief
 * 		Update the global resource summation array that tracks the resources
 * 		that need to be accumulated across chunks
 *
 * @see svr_resc_sum
 */
void
update_resc_sum(void)
{
	resource_def *prdef;
	struct resc_sum *tmp_resc_sum;
	int i = 0;

	for (prdef = svr_resc_def; prdef; prdef = prdef->rs_next) {
		if (prdef->rs_flags & (ATR_DFLAG_RASSN|ATR_DFLAG_ANASSN|ATR_DFLAG_FNASSN))
			i++;
	}

	/* allocating i+1 for the NULL terminator */
	tmp_resc_sum = (struct resc_sum *)calloc((size_t) (i + 1), sizeof(struct resc_sum));
	if (tmp_resc_sum == NULL) {
		log_err(-1, "setup_resc", "unable to malloc for svr_resc_sum");
		return;
	}

	if (svr_resc_sum != NULL)
		free(svr_resc_sum);

	svr_resc_sum = tmp_resc_sum;

	for (i=0, prdef = svr_resc_def; prdef; prdef = prdef->rs_next) {
		if (prdef->rs_flags & (ATR_DFLAG_RASSN|ATR_DFLAG_ANASSN|ATR_DFLAG_FNASSN)) {
			svr_resc_sum[i].rs_def = prdef;
			svr_resc_sum[i].rs_prs = NULL;
			(void)memset((char *)&svr_resc_sum[i].rs_attr, 0, sizeof(struct attribute));
			svr_resc_sum[i].rs_attr.at_type = prdef->rs_type;
			svr_resc_sum[i].rs_set = 0;
			i++;
		}
	}
	svr_resc_sum[i].rs_def = NULL;
}
#endif
