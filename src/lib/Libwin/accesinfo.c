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
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "portability.h"

#include <windows.h>
#include "win.h"
#include "log.h"
#include <aclapi.h>
#include "pbs_ifl.h"
#include "pbs_internal.h"

static char *print_mask(int mask);
/**
 * @brief
 *	Basically, create a dacl allowing only access to Administrators-type groups
 * 	This also will add the 'owner_sid' to this permission list giving it
 * 	Full Control
 *
 * @param[in]  user - the username to assign ownership
 * @param[in]  mask - the file permission mask to assign
 * @param[in]  owner_sid - session sid
 *
 * @return	ACL*
 * @retval	pointer to ACL list	success
 * @retval	NULL			error
 *
 */

static ACL *
create_secure_dacl(char *user, ACCESS_MASK mask, SID *owner_sid)
{

	DWORD rids[1] = {0};
	gid_t grp[_MAX_GROUPS] = {0};
	int i = 0;
	int k = 0;
	int cbAcl = 0;
	ACL *ndacl = NULL;

	rids[0] = DOMAIN_ALIAS_RID_ADMINS;
	k = getgids(getlogin(), grp, rids);
	
	if ((k < _MAX_GROUPS) && (owner_sid != NULL)) {
		grp[k] = sid_dup(owner_sid);
		if (grp[k] == NULL) {
			log_event(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "failed to copy owner_sid");
			return NULL;
		}
		k++;
	}

	if (user != NULL && mask != 0) {
		SID *sid = getgrpsid(user);
		if (sid == NULL)
			sid = getusersid(user);
		if (sid) {
			if (k == _MAX_GROUPS) {
				grp[k-1] = sid;
			} else {
				grp[k] = sid;
				k++;
			}
		} else {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "failed to get group and user sid for %s", user);
		}
	} 

	cbAcl = sizeof(ACL);
	for (i = 0 ; i < k; i++) {
		// subtract ACE.SidStart from the size
		int cbAce = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD);
		// add this ACE's SID length
		cbAce += GetLengthSid(grp[i]);
		// add the length of each ACE to the total ACL length
		cbAcl += cbAce;
	}

	ndacl = (ACL *)malloc(cbAcl);
	if (ndacl == NULL) {
		log_errf(errno, __func__, "failed to malloc %d bytes", cbAcl);
		return NULL;
	}
	if (InitializeAcl(ndacl, cbAcl, ACL_REVISION) == 0) {
		log_err(-1, __func__, "failed in InitializeAcl");
	}

	for (i=0; i < k; i++) {
		char *name = getgrpname_full(grp[i]);

		if (name == NULL)
			name = getusername(grp[i]);

		if (name == NULL)
			continue;

		if (user != NULL && mask != 0 && i == (k-1)) {
			if (AddAccessAllowedAce(ndacl, ACL_REVISION, mask | 0x00100000, grp[i]) == 0) {
				log_errf(-1, __func__, "AddAccessAllowedAce failed to add %s to %s", print_mask(mask | 0x00100000), name);
			}

		} else {
			if (AddAccessAllowedAce(ndacl, ACL_REVISION,
				READS_MASK | WRITES_MASK | STANDARD_RIGHTS_ALL, grp[i]) == 0) {
				log_errf(-1, __func__, "failed to add mask in AddAccessAllowedAce for %s", name);
			}
		}
		(void)free(name);
		LocalFree(grp[i]);
	}
	return (ndacl);
}
/**
 *	@brief Set the ownership, permissions, and ACLs for a file or directory.
 *
 *	A file is secured by setting owner of the file to current user, group to current
 *	group, and the DACLS to ALL access to any admin type groups. If user and mask is set,
 *	then this is added to path's DACL.
 *
 *	  @param[in]  path - the target file/directory
 *	  @param[in]  user - the username to assign ownership
 *	  @param[in]  mask - the file permission mask to assign
 *
 *	@return int
 *	@retval 1 on success
 *	@retval 0 on failure
 *
 */
int
secure_file(char *path, char *user, ACCESS_MASK mask)
{
	SECURITY_DESCRIPTOR	sd;
	SID	*usid = NULL;
	SID	*gsid;
	ACL	*pdacl;
	struct  stat sbuf;
	SECURITY_INFORMATION	si = 0;
	char	*gname = NULL;

	if (path == NULL || *path == '\0') {
		log_eventf(PBSEVENT_ERROR PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "invalid path");
		return (0);
	}

	if (lstat(path, &sbuf) == -1) {
		log_eventf(PBSEVENT_ERROR PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "Path %d doesn't exists", path);
		return (0);	/* ignore non-existent files! */
	}

	if (!has_privilege(SE_RESTORE_NAME)) {
		if (ena_privilege(SE_RESTORE_NAME) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to enable privilege: SE_RESTORE_NAME");
		}
	}

	if (!has_privilege(SE_TAKE_OWNERSHIP_NAME)) {
		if (ena_privilege(SE_TAKE_OWNERSHIP_NAME) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to enable privilege: SE_TAKE_OWNERSHIP_NAME");
		}
	}

	if (!has_privilege(SE_SECURITY_NAME)) {
		if (ena_privilege(SE_SECURITY_NAME) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to enable privilege: SE_SECURITY_NAME");
		}
	}

	if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) == 0) {
		log_err(-1, __func__, "failed in InitializeSecurityDescriptor");
	}

	/* make PBS service account as the owner */
	usid = create_administrators_sid();

	if (usid == NULL) {
		usid   = getusersid(getlogin());
	}

	if (usid) {
		if (SetSecurityDescriptorOwner(&sd, usid, FALSE) == 0) {
			log_err(-1, __func__, "failed in SetSecurityDescriptorOwner");
			LocalFree(usid);
			return (0);
		}
		si |= OWNER_SECURITY_INFORMATION;

		/* to reset perms, trick is to set ownership of file first */
		if (SetFileSecurity(path, si, &sd) == 0) {
			log_errf(-1, __func__, "failed in SetFileSecurity for %s", path);
			LocalFree(usid);
			return (0);
		}
		if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)==0 ) {
			log_err(-1, __func__, "failed in InitializeSecurityDescriptor for usid");
		}
		si = 0;

	}

	/* can't use gsid=getgid() since gsid here must be LocalFree()d */
	if ((gname=getdefgrpname(getlogin()))) {
		gsid = getgrpsid(gname);
		(void)free(gname);
	} else {
		gsid = NULL;
	}

	if (gsid) {
		if (SetSecurityDescriptorGroup(&sd, gsid, FALSE) == 0) {
			log_err(-1, __func__, "failed in SetSecurityDescriptorGroup");
			if (usid) LocalFree(usid);
			LocalFree(gsid);
			return (0);
		}
		si |= GROUP_SECURITY_INFORMATION;

	}

	pdacl = create_secure_dacl(user, mask, usid);
	if (pdacl == NULL) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "failed to create secure dacl for file %s", path);
		if (usid) LocalFree(usid);
		if (gsid) LocalFree(gsid);
		return (0);
	}

	if (SetSecurityDescriptorDacl(&sd, TRUE, pdacl, TRUE) == 0) {
		log_err(-1, __func__, "failed in SetSecurityDescriptorDacl");
		if (usid) LocalFree(usid);
		if (gsid) LocalFree(gsid);
		(void)free(pdacl);
		return (0);
	}
	si |= DACL_SECURITY_INFORMATION;

	if (SetFileSecurity(path, si, &sd) == 0) {
		log_errf(-1, __func__, "failed in SetFileSecurity for %s", path);
		if (usid) LocalFree(usid);
		if (gsid) LocalFree(gsid);
		(void)free(pdacl);
		return (0);
	}

	if (usid) LocalFree(usid);
	if (gsid) LocalFree(gsid);
	(void)free(pdacl);

	/* Even though permissions have been set on the file, it can be   */
	/* overriden if a file attribute was given say a                  */
	/* FILE_ATTRIBUTE_READONLY flag previously outside of PBS. Any    */
	/* writes to the file would still fail even if Administrators     */
	/* have been given write permission.                              */
	/* The following call is to clear any special attributes that     */
	/* may have gotten set outside of PBS, negating PBS' permission   */
	/* change.							  */
	if (SetFileAttributes(path, FILE_ATTRIBUTE_NORMAL) == 0) {
		log_errf(-1, __func__, "failed in SetFileAttributes for %s", path);
	}

	return (1);

}

/**
 * @brief
 * 	Like create_secure_dacl() but for 2 users.
 *
 * @param[in]  user - the username to assign ownership
 * @param[in]  mask - the file permission mask to assign
 * @param[in]  user2 - the username to assign ownership
 * @param[in]  mask2 - the file permission mask to assign
 * @param[in]  owner_sid -  session id
 *
 * @return      ACL*
 * @retval      pointer to ACL list     success
 * @retval      NULL                    error
 *
 */

static ACL *
create_secure_dacl2(char *user, ACCESS_MASK mask, char *user2, ACCESS_MASK mask2, SID *owner_sid)
{
	DWORD rids[1] = {0};
	gid_t grp[_MAX_GROUPS] = {0};
	int i = 0;
	int k = 0;
	int cbAcl = 0;
	ACL *ndacl = NULL;
	SID *sid = NULL;

	rids[0] = DOMAIN_ALIAS_RID_ADMINS;
	k = getgids(getlogin(), grp, rids);

	if ((k < _MAX_GROUPS) && (owner_sid != NULL)) {
		grp[k] = sid_dup(owner_sid);
		if (grp[k] == NULL) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "failed to copy owner sid");
			return NULL;
		}
		k++;
	}

	if (user != NULL && mask != 0) {
		sid = getgrpsid(user);
		if (sid == NULL) {
			sid = getusersid(user);
		}

		if (sid) {
			if (k == _MAX_GROUPS) {
				grp[k-2] = sid;
			} else {
				grp[k] = sid;
				k++;
			}
		} else {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "failed to get group and user sid for user: %s", user);
		}
	}
	if (user2 != NULL && mask2 != 0) {
		sid = getgrpsid(user2);
		if (sid == NULL)
			sid = getusersid(user2);
		if (sid) {
			if (k == _MAX_GROUPS) {
				grp[k-1] = sid;
			} else {
				grp[k] = sid;
				k++;
			}
		} else {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "failed to get group and user sid for user2: %s", user2);
		}
	}

	cbAcl = sizeof(ACL);
	for (i = 0 ; i < k; i++) {
		// subtract ACE.SidStart from the size
		int cbAce = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD);
		// add this ACE's SID length
		cbAce += GetLengthSid(grp[i]);
		// add the length of each ACE to the total ACL length
		cbAcl += cbAce;
	}

	ndacl = (ACL *)malloc(cbAcl);
	if (ndacl == NULL) {
		log_errf(errno, __func__, "failed to malloc %d bytes", cbAcl);
		return NULL;
	}
	if (InitializeAcl(ndacl, cbAcl, ACL_REVISION) == 0) {
		log_err(-1, __func__, "failed in InitializeAcl");
	}

	for (i=0; i < k; i++) {
		char *name = getgrpname_full(grp[i]);

		if (name == NULL)
			name = getusername(grp[i]);

		if (name == NULL)
			continue;

		if (user != NULL && mask != 0 && i == (k-2)) {
			if (AddAccessAllowedAceEx(ndacl, ACL_REVISION, CONTAINER_INHERIT_ACE|OBJECT_INHERIT_ACE, mask | 0x00100000, grp[i]) == 0) {
				log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__,
					"failed in AddAccessAllowedAceEx for user: %s, mask: %s, name: %s", user, print_mask(mask | 0x00100000), name);
			}

		} else if (user2 != NULL && mask2 != 0 && i == (k-1)) {
			if (AddAccessAllowedAceEx(ndacl, ACL_REVISION, CONTAINER_INHERIT_ACE|OBJECT_INHERIT_ACE, mask2 | 0x00100000, grp[i]) == 0) {
				log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__,
					"failed in AddAccessAllowedAceEx for user2: %s, mask: %s, name: %s", user2, print_mask(mask2 | 0x00100000), name);
			}
		} else {
			if (AddAccessAllowedAceEx(ndacl, ACL_REVISION, CONTAINER_INHERIT_ACE|OBJECT_INHERIT_ACE,
					READS_MASK | WRITES_MASK | STANDARD_RIGHTS_ALL, grp[i]) == 0) {
				log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__,
					"failed in AddAccessAllowedAceEx for name: %s and mask: READS_MASK | WRITES_MASK | STANDARD_RIGHTS_ALL", name);
			}
		}
		(void)free(name);
		LocalFree(grp[i]);
	}

	return (ndacl);
}

/**
 * @brief
 *	Like secure_file() except we have 2 other user permissions to add to
 * 	path
 *
 * @param[in]  user - the username to assign ownership
 * @param[in]  mask - the file permission mask to assign
 * @param[in]  user2 - the username to assign ownership
 * @param[in]  mask2 - the file permission mask to assign
 * @param[in]  owner_sid -  session id
 *
 * @return 	int
 * @retval 	1 	on success
 * @retval 	0 	on failure
 *
 */

int
secure_file2(char *path, char *user, ACCESS_MASK mask, char *user2, ACCESS_MASK mask2)
{
	SECURITY_DESCRIPTOR	sd;
	SID	*usid = NULL;
	SID	*gsid;
	ACL	*pdacl;
	struct  stat sbuf;
	SECURITY_INFORMATION	si = 0;
	char	*gname = NULL;

	if (path == NULL) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "Invalid path");
		return (0);
	}

	if (lstat(path, &sbuf) == -1) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "path %s doesn't exists", path);
		return (0);	/* ignore non-existent files! */
	}

	if (!has_privilege(SE_RESTORE_NAME)) {
		if (ena_privilege(SE_RESTORE_NAME) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to enable privilege: SE_RESTORE_NAME");
		}
	}

	if (!has_privilege(SE_TAKE_OWNERSHIP_NAME)) {
		if (ena_privilege(SE_TAKE_OWNERSHIP_NAME) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to enable privilege: SE_TAKE_OWNERSHIP_NAME");
		}
	}

	if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) == 0) {
		log_err(-1, __func__, "failed in InitializeSecurityDescriptor");
	}

	/* make PBS service account as the owner */
	usid = create_administrators_sid();

	if (usid == NULL)
		usid = getusersid(getlogin());

	if (usid) {
		if (SetSecurityDescriptorOwner(&sd, usid, FALSE) == 0) {
			log_errf(-1, __func__, "failed in SetSecurityDescriptorOwner for %s", path);
			LocalFree(usid);
			return (0);
		}
		si |= OWNER_SECURITY_INFORMATION;

		/* trick with setting perms, set ownership first! */
		if (SetFileSecurity(path, si, &sd) == 0) {
			log_errf(-1, __func__, "failed in SetFileSecurity for %s", path);
			LocalFree(usid);
			return (0);
		}

		if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) == 0) {
			log_err(-1, __func__, "failed in InitializeSecurityDescriptor for usid");
		}
		si = 0;
	}

	/* can't use gsid=getgid() since gsid here must be LocalFree()d */
	if ((gname=getdefgrpname(getlogin()))) {
		gsid = getgrpsid(gname);
		(void)free(gname);
	} else {
		gsid = NULL;
	}
	if (gsid) {
		if (SetSecurityDescriptorGroup(&sd, gsid, FALSE) == 0) {
			log_errf(-1, __func__, "failed in SetSecurityDescriptorGroup for %s", path);
			if (usid) LocalFree(usid);
			LocalFree(gsid);
			return (0);
		}
		si |= GROUP_SECURITY_INFORMATION;

	}

	pdacl = create_secure_dacl2(user, mask, user2, mask2, usid);
	if (pdacl == NULL) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "failed to create secure dacl for file %s", path);
		if (usid) LocalFree(usid);
		if (gsid) LocalFree(gsid);
		return (0);
	}

	if (SetSecurityDescriptorDacl(&sd, TRUE, pdacl, TRUE) == 0) {
		log_errf(-1, __func__, "failed in SetSecurityDescriptorDacl for %s", path);
		if (usid) LocalFree(usid);
		if (gsid) LocalFree(gsid);
		(void)free(pdacl);
		return (0);
	}
	si |= DACL_SECURITY_INFORMATION;

	if (SetFileSecurity(path, si, &sd) == 0) {
		log_errf(-1, __func__, "failed in SetFileSecurity for %s", path);
		if (usid) LocalFree(usid);
		if (gsid) LocalFree(gsid);
		(void)free(pdacl);
		return (0);
	}

	if (usid) LocalFree(usid);
	if (gsid) LocalFree(gsid);
	(void)free(pdacl);

	/* Even though permissions have been set on the file, it can be   */
	/* overriden if a file attribute was given say a                  */
	/* FILE_ATTRIBUTE_READONLY flag previously outside of PBS. Any    */
	/* writes to the file would still fail even if Administrators     */
	/* have been given write permission.                              */
	/* The following call is to clear any special attributes that     */
	/* may have gotten set outside of PBS, negating PBS' permission   */
	/* change.							  */
	if (SetFileAttributes(path, FILE_ATTRIBUTE_NORMAL) == 0) {
		log_errf(-1, __func__, "failed in SetFileAttributes for %s", path);
	}

	return (1);

}

/**
 * @brief
 *	initialize the accessinfo
 *
 * @param[out] acc - pointer to access info
 * @param[in]  len - length
 *
 */
void
accessinfo_init(struct accessinfo *acc, int len)
{
	int	i;

	for (i=0; i < len; i++) {
		acc[i].group = NULL;
		acc[i].mask = 0;
	}
}

/**
 * @brief
 * 	adds accessinfo.
 *
 * @param[in] acc - pointer accessinfo
 * @param[in] len - length
 * @param[in] group - group name
 * @param[in] mask - the file permission mask to assign
 *
 * @return	int
 * @retval	1 	if an entry is added;
 * @retval	0 	if an entry is updated;
 * @retval	-1 	if no entry was added;
 *
 */
int
accessinfo_add(struct accessinfo *acc, int len, char *group, int mask)
{
	int	i;

	for (i=0; i < len; i++) {
		if (acc[i].group == NULL) {
			acc[i].group = group;
			acc[i].mask = mask;
			return (1);
		} else if (strcmp(acc[i].group, group) == 0) {
			acc[i].mask |= mask;
			return (0);
		}
	}
	return (-1);
}

/**
 * @brief
 *	checks mask value of acc
 *
 * @param[in] acc - pointer to access info
 * @param[in] len - length of access info
 *
 * @return	int
 * @retval	1 	if acc has all zeros for mask;
 * @retval	0	otherwise
 *
 */
int
accessinfo_mask_allzero(struct accessinfo *acc, int len)
{
	int	i;

	for (i=0; i < len; i++) {
		if (acc[i].group && (acc[i].mask != 0)) {
			return (0);
		}
	}
	return (1);
}

/**
 * @brief
 *      frees access info entry from list.
 *
 * @param[in] acc - pointer to access info
 * @param[in] len - length of access info
 *
 */
void
accessinfo_free(struct accessinfo *acc, int len)
{
	int	i;

	for (i=0;i < len; i++) {
		if (acc[i].group) {
			(void)free(acc[i].group);
		}
	}
	if (acc)
		(void)free(acc);
}

/**
 * @brief
 *	prints mask values
 *
 * @param[in] mask - mask val
 *
 * @return	string
 * @retval	mask val	success
 * @retval	NULL		error
 *
 */
static char *
print_mask(int mask)
{
	static char buf[LOG_BUF_SIZE];

	buf[0] = '\0';
	if (mask & FILE_READ_DATA)
		strcat(buf, "FILE_READ_DATA|");
	if (mask & FILE_LIST_DIRECTORY)
		strcat(buf, "FILE_LIST_DIRECTORY|");
	if (mask & FILE_WRITE_DATA)
		strcat(buf, "FILE_WRITE_DATA|");
	if (mask & FILE_ADD_FILE)
		strcat(buf, "FILE_ADD_FILE|");
	if (mask & FILE_APPEND_DATA)
		strcat(buf, "FILE_APPEND_DATA|");
	if (mask & FILE_ADD_SUBDIRECTORY)
		strcat(buf, "FILE_ADD_SUBDIRECTORY|");
	if (mask & FILE_READ_EA)
		strcat(buf, "FILE_READ_EA|");
	if (mask & FILE_WRITE_EA)
		strcat(buf, "FILE_WRITE_EA|");
	if (mask & FILE_EXECUTE)
		strcat(buf, "FILE_EXECUTE|");
	if (mask & FILE_TRAVERSE)
		strcat(buf, "FILE_TRAVERSE|");
	if (mask & FILE_DELETE_CHILD)
		strcat(buf, "FILE_DELETE_CHILD|");
	if (mask & FILE_READ_ATTRIBUTES)
		strcat(buf, "FILE_READ_ATTRIBUTES|");
	if (mask & FILE_WRITE_ATTRIBUTES)
		strcat(buf, "FILE_READ_ATTRIBUTES|");

	return (buf);

}

/**
 * @brief
 *	RETURNS THE ACCESS INFO.
 *
 * @param[in] acc - pointer to access info
 * @param[in] len - length of info
 *
 * @return	string
 * @retval	access info	success
 * @retval	EMPTY STRING	error
 *
 */
char *
accessinfo_values(struct accessinfo *acc, int len)
{
	int i;
	static char buf[512];
	static char msg[LOG_BUF_SIZE];

	strcpy(msg, "");
	for (i=0; i < len; i++) {
		if (acc[i].group != NULL) {
			sprintf(buf, "acc[%d]=(grp=%s,mask=%s) ", i,
				(acc[i].group?acc[i].group:"null"),
				print_mask(acc[i].mask));
			if ((strlen(msg) + strlen(buf)) < 4095)
				strcat(msg, buf);
		}
	}
	return (msg);
}

/**
 * @brief
 * 	A bug is in PBS-windows interaction partucularly when windows is shutdown
 *	and the file's perms get corrupted. This usually happens in the
 *	middle of a move <fname.new> <fname> operation. This function fixes this
 *	problem allowing <fname.*> to be accessible to the current process.
 *
 * @param[in] fname - filename
 *
 */
void
fix_perms(char *fname)
{
	char fname2[MAXPATHLEN+1];

	secure_file(fname, "Administrators",
		READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
	sprintf(fname2, "%s.new", fname);
	secure_file(fname2, "Administrators",
		READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
}

/**
 * @brief
 *	The same as fix_perms() except the two file names are explicitly given
 *
 * @param[in] fname1 - filename
 * @param[in] fname2 - filename
 *
 */
void
fix_perms2(char *fname1, char *fname2)
{
	secure_file(fname1, "Administrators",
		READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
	secure_file(fname2, "Administrators",
		READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED);
}

/**
 * @brief
 * 	perm_granted_admin_and_name: returns 0 if permission mask in 'disallow' has
 *	been granted to only admin-type of accounts and 'owner'. 'owner' can be
 *	set to NULL. Otherwise, it will return the values EPERM or EACCESS with
 *	error message returned in 'errmsg'.
 */
int
perm_granted_admin_and_owner(char *path, int disallow, char *owner, char *errmsg)
{
	int	rc;
	DWORD	i, j;
	SID	*powner = NULL;
	char	*powner_s = NULL;

	ACL	   *pdacl = NULL;
	SECURITY_DESCRIPTOR *psd = NULL;
	ACL_SIZE_INFORMATION	sizeInfo;
	ACCESS_ALLOWED_ACE	*pace;
	int			violate = 0;
	struct	accessinfo	*allowed = NULL;
	struct  accessinfo	*denied = NULL;
	int			mask;
	char			*name;
	SID			*esid = getusersid("Everyone");
	DWORD ret;

	ret = GetNamedSecurityInfo(path, SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
		&powner, 0, &pdacl, 0, &psd);
	if (ret != ERROR_SUCCESS) {
		rc = ret;
		sprintf(errmsg, "GetNameSecurityInfo on file %s failed with errno %lu", path, ret);
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, errmsg);
		goto chkerr;
	}

	/* is owner SID a privilege user? */
	powner_s = getusername(powner);
	if (powner_s == NULL)
		powner_s = getgrpname(powner);

	if (powner_s != NULL) {
		if ( !sidIsAdminPrivilege(powner) || \
			( (owner != NULL) && strcmp(owner, powner_s) != 0 )  ) {
			rc = EPERM;
			sprintf(errmsg, "File %s not owned by user %s or an admin-type user!",
				path, owner);
			log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, errmsg);
			goto chkerr;
		}
	}

	/* make sure perm mask granted only to privilege groups or the
	 special 'owner' */

	if (GetAclInformation(pdacl, &sizeInfo, sizeof(sizeInfo),
		AclSizeInformation) == 0) {
		log_err(-1, __func__, "failed in GetAclInformation");
	}

	allowed = (struct accessinfo *)malloc(sizeof(struct accessinfo)*sizeInfo.AceCount);
	denied = (struct accessinfo *)malloc(sizeof(struct accessinfo)*sizeInfo.AceCount);

	if (allowed == NULL || denied == NULL) {
		rc = errno;
		strcpy(errmsg, "malloc of allowed or denied struct failed!");
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, errmsg);
		goto chkerr;
	}

	accessinfo_init(allowed, sizeInfo.AceCount);
	accessinfo_init(denied, sizeInfo.AceCount);

	for (i=0; i < sizeInfo.AceCount; i++) {

		if (GetAce(pdacl, i, (void **)&pace) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "failed in GetAce with errno %lu", GetLstError());
		}
		name = getgrpname_full((SID *)&pace->SidStart);
		if (name == NULL)
			name = getusername((SID *)&pace->SidStart);

		/* later, we need to also check denied access which */
		/* overrides allowed access. */
		if ( pace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&\
			 (mask=(pace->Mask & disallow)) && \
			 ( EqualSid((SID *)&pace->SidStart, esid) || \
			sid2rid((SID *)&pace->SidStart) != \
					 SECURITY_CREATOR_OWNER_RID ) && \
			(!sidIsAdminPrivilege( (SID *)&pace->SidStart)) || \
   			((owner != NULL) && strcmp(name, owner) != 0) ) {
			(void)accessinfo_add(allowed, sizeInfo.AceCount,
				name, (mask & 0xFFFF));
		} else if (pace->Header.AceType == ACCESS_DENIED_ACE_TYPE) {
			(void)accessinfo_add(denied, sizeInfo.AceCount,
				name, (pace->Mask & 0xFFFF));
		} else {
			if (name) {
				(void)free(name);
			}
		}
	}
	/* validate */
	for (i=0; i < sizeInfo.AceCount; i++) {
		for (j=0; j < sizeInfo.AceCount; j++) {
			if( allowed[i].group == NULL || \
						denied[j].group == NULL )
				continue;

			if (strcmp(allowed[i].group, denied[j].group) == 0)
				allowed[i].mask &= ~denied[j].mask;
			/* unset the denied mask from allowed */
		}
	}


	if (!accessinfo_mask_allzero(allowed, sizeInfo.AceCount)) {
		rc = EACCES;
		sprintf(errmsg, "File %s has following disallowed perm masks: ",
			path);
		strcat(errmsg, accessinfo_values(allowed, sizeInfo.AceCount));
		log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, errmsg);
		goto chkerr;
	}

	rc = 0;

chkerr:
	if (powner_s)(void)free(powner_s);
	/* NOTE: powner and pdacl hang off of psd, so a free of psd would */
	/* automatically free them */
	if (psd) LocalFree(psd);
	if (allowed) accessinfo_free(allowed, sizeInfo.AceCount);
	if (denied) accessinfo_free(denied, sizeInfo.AceCount);
	if (esid) LocalFree(esid);

	return (rc);
}

/**
 * @brief
 *      create read directory in given path,
 *
 * @param[in] path - path where directory to be created
 */

static void
create_dir_everyone_read(char *path)
{
	if (CreateDirectory(path, 0) != 0) {
		log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG,PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__, 
			"created %s for everyone to read", path);
	} else {
		if (GetLastError() != ERROR_ALREADY_EXISTS) {
			log_errf(-1, __func__, "Unable to create new directory %s", path);
		}
	}
	log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG,PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
		"securing %s for read access by Everyone", path);
	if (secure_file2(path,
		"Administrators", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
		"\\Everyone", READS_MASK | READ_CONTROL) == 0) {
		log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG,PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__, 
			"Failed to secure %s", path);
	}
}

/**
 * @brief
 *	create read/write directory in given path,
 *
 * @param[in] path - path where directory to be created
 */
static void
create_dir_everyone_readwrite(char *path)
{
	if (CreateDirectory(path, 0) != 0) {
		log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
			"created %s for everyone to read/write", path);
	} else {
		if (GetLastError()  != ERROR_ALREADY_EXISTS) {
			log_errf(-1, __func__, "Unable to create new directory %s", path);
		}
	}
	log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
		"securing %s for read/write access by Everyone", path);
	if (secure_file2(path,
		"Administrators", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
		"\\Everyone", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED) == 0) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "Failed to secure %s", path);
	}
}

/**
 *
 *	@brief Recursively change permissions to everyone read in a directory tree.
 *
 *	  @param[in]  path - the target file/directory
 *
 *	@return void
 *
 */
static void
make_dir_files_everyone_read(char *path)
{
	DIR	*dir;
	struct	dirent *pdirent;
	char	dirfile[MAXPATHLEN+1];
	struct stat sb;
	int	isdir = 1;

	if (path == NULL || *path == '\0')
		return;
	/* Secure the item that path refers to. */
	if (secure_file2(path, "Administrators", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
		"\\Everyone", READS_MASK|READ_CONTROL) == 0) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "Unable to secure %s", path);
	}

	log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
		"securing %s for read access by Everyone", path);
	/* If the item is not a directory, we are done. */
	if (stat(path, &sb) == -1) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "\"%s\" does not exist", path);
		return;
	}
	if (!S_ISDIR(sb.st_mode)) {
		/* regular file */
		return;
	}

	dir = opendir(path);
	if (dir == NULL) {
		log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "opendir failed for %s", path);
		return;
	}
	/* Recurse into the directory. */
	while (errno = 0, (pdirent = readdir(dir)) != NULL) {
		/* Ignore the "." and ".." entries. */
		if (pdirent->d_name[0] == '.') {
			if (pdirent->d_name[1] == '\0')
				continue;
			if (pdirent->d_name[1] == '.' && pdirent->d_name[2] == '\0')
				continue;
		}
		/* If we will exceed the maximum path length, skip this item. */
		if (strlen(path) + strlen(pdirent->d_name) + 1 >= MAXPATHLEN)
			continue;
		sprintf(dirfile, "%s/%s", path, pdirent->d_name);
		make_dir_files_everyone_read(dirfile);
	}
	if (errno != 0 && errno != ENOENT) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "readdir error; %s", path);
	}
	(void)closedir(dir);
}

/**
 *
 *	@brief Recursively change permissions for administrators group
 *         and Service account in a directory tree.
 *
 *	@param[in]  path - the target file/directory
 *
 *	@return void
 *
 */
void
make_dir_files_service_account_read(char *path)
{
	DIR	*dir;
	struct	dirent *pdirent;
	char    dirfile[MAXPATHLEN+1];
	char    *username = NULL;

	username = getlogin_full();
	if (username == "" || username == NULL) {
		if (secure_file(path, "Administrators",
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to secure file %s", path);
		}
	} else {
		if (secure_file2(path, "Administrators",
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
			username, READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to secure file %s for Administrators and %s", path, username);
		}
	}

	dir = opendir(path);
	if (dir == NULL) {
		return;
	}

	while (errno = 0, (pdirent = readdir(dir)) != NULL) {
		if (strcmp(pdirent->d_name, ".") == 0 ||
			strcmp(pdirent->d_name, "..") == 0)
			continue;

		sprintf(dirfile, "%s/%s", path, pdirent->d_name);

		if (secure_file2(dirfile, "Administrators",
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
			username, READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED) == 0) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to secure file %s", dirfile);
		}
#ifdef DEBUG
		printf("securing file %s: full access to admin and %s \n", dirfile, username);
#endif

	}
#ifdef DEBUG
	if (errno != 0 && errno != ENOENT)
		printf("readdir error; %s\n", path);
#endif
	(void)closedir(dir);
}

/**
 *
 *	@brief Create a Directory. Grant ownership to "Administrators" group.
 *         Recursively grant full access to administrators group
 *         and Service account in the directory tree.
 *
 *	@param[in]  path - the target directory
 *
 *	@return void
 *
 */
static void
create_dir_admin_service_account_full_access(char *path)
{
	if (CreateDirectory(path, 0) != 0) {
		log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
			"created %s for service account and admin-type access", path);
	} else {
		if (GetLastError() != ERROR_ALREADY_EXISTS) {
			log_err(-1, __func__, "Unable to create new directory");
		}
	}
	log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
		"securing %s for access to service account and administrators", path);
	make_dir_files_service_account_read(path);
}

/**
 *
 * @brief
 *	Secures all the files' permissions (and recreate directories)
 *	that are related to pbs_mom service.
 *
 */
void
secure_mom_files(void)
{
	DIR *dir;
	char	path[MAXPATHLEN+1];
	HANDLE	hfile;
	char    *username = NULL;

	if (pbs_conf.pbs_home_path == NULL) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "no home_path!");
		return;
	}
        username = getlogin_full();

	sprintf(path, "%s/mom_priv", pbs_conf.pbs_home_path);
	create_dir_everyone_read(path);

	dir = opendir(path);

	if (dir != NULL) {
		struct dirent *pdirent;
		char fpath[MAXPATHLEN+1];

		while (errno = 0,
			(pdirent = readdir(dir)) != NULL) {
			char *p;
			if (p = strrchr(pdirent->d_name, '.')) {
				int baselen = strlen(p)-4;
				if (baselen < 0)
					continue;
				if (strcmpi(p+baselen, ".bat") == 0) {
					sprintf(fpath, "%s/%s", path, pdirent->d_name);
					log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
						"securing file %s", fpath);
					if (secure_file2(fpath, "Administrators",
						READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
						"\\Everyone", READS_MASK|READ_CONTROL) == 0) {
						log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to secure file %s", fpath);
					}
				}
			}
		}
		if (errno != 0 && errno != ENOENT) {
			log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "readdir error; %s", path);
		}
		(void)closedir(dir);
	}

	sprintf(path, "%s/mom_priv/config", pbs_conf.pbs_home_path);

	hfile = CreateFile(path, GENERIC_WRITE, FILE_SHARE_WRITE, 0,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

	if (hfile != INVALID_HANDLE_VALUE) {
		log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
			"created file %s", path);
		CloseHandle(hfile);

	} else {
		log_errf(-1, __func__, "failed in CreateFile for %s", path);
	}
	log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
		"securing %s for admin-only access", path);
	if (secure_file2(path, "Administrators",
			READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
			username, READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED) == 0) {
		log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to secure file %s", path);
	}

	sprintf(path, "%s/mom_logs", pbs_conf.pbs_home_path);
	create_dir_everyone_read(path);

	sprintf(path, "%s/mom_priv/jobs", pbs_conf.pbs_home_path);
	create_dir_everyone_read(path);

	sprintf(path, "%s/mom_priv/hooks", pbs_conf.pbs_home_path);
	create_dir_admin_service_account_full_access(path);

	sprintf(path, "%s/mom_priv/hooks/tmp", pbs_conf.pbs_home_path);
	create_dir_admin_service_account_full_access(path);
}

/**
 * @brief
 *	Secures all the files' permissions in home_path to full control
 *	for administrators group and to read for everyone group.
 *
 *  @return void
 */
void
secure_misc_files()
{
	char	path[MAXPATHLEN+1];
	HANDLE	hfile;

	if (pbs_conf.pbs_home_path == NULL) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "no home_path!");
		return;
	}

	if (secure_file2(pbs_conf.pbs_home_path,
			"Administrators", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
			"\\Everyone", READS_MASK | READ_CONTROL) == 0) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to secure file %s", pbs_conf.pbs_home_path);
	}
	log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
		"securing %s for read access by Everyone", pbs_conf.pbs_home_path );

	sprintf(path, "%s/spool", pbs_conf.pbs_home_path);
	create_dir_everyone_readwrite(path);

	sprintf(path, "%s/undelivered", pbs_conf.pbs_home_path);
	create_dir_everyone_readwrite(path);

	sprintf(path, "%s/pbs_environment", pbs_conf.pbs_home_path);

	hfile = CreateFile(path, GENERIC_WRITE, FILE_SHARE_WRITE, 0,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

	if (hfile != INVALID_HANDLE_VALUE) {
		log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG,PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
			"created file %s", path);
		CloseHandle(hfile);
	} else {
		log_errf(-1, __func__, "failed in CreateFile for %s", path);
	}
	log_eventf(PBSEVENT_SYSTEM | PBSEVENT_ADMIN | PBSEVENT_FORCE| PBSEVENT_DEBUG, PBS_EVENTCLASS_FILE, LOG_DEBUG, __func__,
		"securing file %s: full access to admin and read to Everyone", path);
	if (secure_file2(path,
			"Administrators", READS_MASK|WRITES_MASK|STANDARD_RIGHTS_REQUIRED,
			"\\Everyone", READS_MASK|READ_CONTROL) == 0) {
		log_eventf(PBSEVENT_DEBUG3, PBS_EVENTCLASS_SERVER, LOG_DEBUG, __func__, "Unable to secure file %s", path);
	}

	sprintf(path, "%s/auxiliary", pbs_conf.pbs_home_path);
	create_dir_everyone_read(path);

	sprintf(path, "%s/checkpoint", pbs_conf.pbs_home_path);
	create_dir_admin_service_account_full_access(path);

}

/**
 *
 *  @brief Secures all the files' permissions in exec_path to full control
 *         for administrators group and to read for everyone group.
 *
 *  @return void
 */
void
secure_exec_files()
{
	char	path[MAXPATHLEN+1];

	if (pbs_conf.pbs_exec_path == NULL) {
		log_eventf(PBSEVENT_ERROR, PBS_EVENTCLASS_SERVER, LOG_ERR, __func__, "no exec_path!");
		return;
	}

	sprintf(path, "%s/bin", pbs_conf.pbs_exec_path);
	make_dir_files_everyone_read(path);

	sprintf(path, "%s/include", pbs_conf.pbs_exec_path);
	make_dir_files_everyone_read(path);

	sprintf(path, "%s/lib", pbs_conf.pbs_exec_path);
	make_dir_files_everyone_read(path);

	sprintf(path, "%s/sbin", pbs_conf.pbs_exec_path);
	make_dir_files_everyone_read(path);

	sprintf(path, "%s/unsupported", pbs_conf.pbs_exec_path);
	make_dir_files_everyone_read(path);

	sprintf(path, "%s/python", pbs_conf.pbs_exec_path);
	make_dir_files_everyone_read(path);

	sprintf(path, "%s/etc", pbs_conf.pbs_exec_path);
	make_dir_files_everyone_read(path);

}
