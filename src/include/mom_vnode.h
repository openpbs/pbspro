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


#include	"job.h"
#include	"server_limits.h"
#include	"attribute.h"
#include	"resource.h"
#include	"pbs_nodes.h"


/*
 *	A mom that manages its own lists of CPUs needs to provide a function
 *	that frees these CPUs when the job terminates.  If non-NULL, this
 *	function pointer will be called from mom_deljob().
 */
extern void	(*free_job_CPUs)(job *);

/*
 *	These are interfaces to functions that manipulate CPU states for moms
 *	that manage their own CPU lists.  The cpuindex_*() functions are used
 *	when referring to a CPU by its relative position on a given mom_vninfo_t
 *	CPU list, while the cpunum_*() functions deal with physical CPU numbers.
 *
 *	get_cpubits() and get_membits() initialize memory bitmasks used to
 *	represent the CPUs (resp. memory boards) discovered while parsing
 *	vnode definitions files.
 */
extern void	cpuindex_free(mom_vninfo_t *, unsigned int);
extern void	cpuindex_inuse(mom_vninfo_t *, unsigned int, job *);
extern void	cpunum_outofservice(unsigned int);
extern void	cpu_raresync(void);
