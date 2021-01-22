/*
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "extern.h"
#include "pathnames.h"
/**
 * @file	util.c
 */

#ifdef        USELOG
extern int    use_neterr;
#endif        /* USELOG */

/**
 * @brief
 *	extract and return the file name from input string
 *
 * @param[in] cp - argument
 *
 * @return	string
 * @retval	filename	success
 * @retval	0		error
 */
char *
colon(char *cp)
{
	if (*cp == ':')		/* Leading colon is part of file name. */
		return (0);

	for (; *cp; ++cp) {
		if (*cp == ':')
			return (cp);
		if (*cp == '/')
			return (0);
	}
	return (0);
}

/**
 * @breif
 *	verify the input directory.
 *
 * @param[in] cp - directory path
 *
 * @return Void
 */
void
verifydir(char *cp)
{
	struct stat stb;

	if (!stat(cp, &stb)) {
		if (S_ISDIR(stb.st_mode))
			return;
		errno = ENOTDIR;
	}
	run_err("%s: %s", cp, strerror(errno));
	exit(1);
}

/**
 * @brief
 *	validate input user name.
 *
 * @param[in] cp0 - user name
 */
int
okname(char *cp0)
{
	int c;
	char *cp;

	cp = cp0;
	do {
		c = *cp;
		if (c & 0200)
			goto bad;
		if (!isalpha(c) && !isdigit(c) && c != '_' && c != '-' && c != '.')
			goto bad;
	} while (*++cp);
	return (1);

	bad:	warnx("%s: invalid user name", cp0);
	return (0);
}

/**
 * @brief
 */
int
susystem(char *s,
	uid_t userid, /* used in unix */
	char *username) /* used in windows */
{

#ifdef WIN32
	HANDLE 	hUser;
	int	rc;

	STARTUPINFO             si = { 0 };
	PROCESS_INFORMATION     pi = { 0 };
	int             flags = CREATE_DEFAULT_ERROR_MODE|CREATE_NEW_CONSOLE|
		CREATE_NEW_PROCESS_GROUP;

	if ((hUser=LogonUserNoPass(username)) == INVALID_HANDLE_VALUE)
		return (1);

	rc = CreateProcessAsUser(hUser, NULL, s, NULL, NULL, TRUE, flags,
		NULL, NULL, &si, &pi);

	if (rc == 0)
		errno = GetLastError();


	if (errno != 0) {
		rc = errno + 10000;     /* error on fork (100xx), retry */
	} else {
		if (WaitForSingleObject(pi.hProcess,
			INFINITE) != WAIT_OBJECT_0)
			errno = GetLastError();
		else if (!GetExitCodeProcess(pi.hProcess, &rc))
			errno = GetLastError();
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);

		if (errno != 0)
			rc = (20000+errno); /* 200xx is error on wait */
	}

	CloseHandle(hUser);
	return (rc);
#else
	int status;
	pid_t pid;

	pid = fork();
	switch (pid) {
		case -1:
			return (127);

		case 0:
			if (setuid(userid) == -1)
				_exit(126);
			execl(_PATH_BSHELL, "sh", "-c", s, NULL);
			_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		status = -1;
	return (status);

#endif
}

/**
 * @brief
 *	reallocate memory in BUF structure based on
 *	given size of file <fd> in given block size <blksize>
 *
 * @param[in]	bp - pointer to allocated BUF struct which will be reallocated
 * @param[in]	fd - fd of file to get file size
 * @param[in]	blksize - block size to get buffer size in BUF struct
 *
 * @return	BUF*
 * @retval	pointer to reallocated BUF struct
 * @par	NOTE:
 *	This pointer can be same as <bp> if size is 0 or less than
 *	already allocated buffer size in <bp>
 */
BUF *
allocbuf(BUF *bp, int fd, int blksize)
{
	struct stat stb = {0};
	int size = 0;

	if (fstat(fd, &stb) < 0) {
		run_err("fstat: %s", strerror(errno));
		return (0);
	}

#ifdef WIN32
	size = 0;
#else
	size = (((int)stb.st_blksize + blksize - 1) / blksize) * blksize;
#endif

	if (size == 0)
		size = blksize;
	if (bp->cnt >= size)
		return (bp);
	if (bp->buf) {
		char *tbuf;
		tbuf = realloc(bp->buf, size);
		if (tbuf == NULL)
			free(bp->buf);
		bp->buf = tbuf;
	} else
		bp->buf = malloc(size);
	if (bp->buf == NULL) {
		bp->cnt = 0;
		run_err("%s", strerror(errno));
		return (0);
	}
	bp->cnt = size;
	return (bp);
}

/**
 * @brief
 *	log warning msg.
 *
 * @param[in] signo -  signal num
 */
void
lostconn(int signo)
{
	if (!iamremote)
		warnx("lost connection");
#ifdef        USELOG
	use_neterr++;
#endif        /* USELOG */

	exit(1);
}
