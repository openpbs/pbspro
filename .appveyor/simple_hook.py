# -*- coding: utf-8 -*-
# Copyright (C) 1994-2020 Altair Engineering, Inc.
# For more information, contact Altair at www.altair.com.
#
# This file is part of the PBS Professional ("PBS Pro") software.
#
# Open Source License Information:
#
# PBS Pro is free software. You can redistribute it and/or modify it under the
# terms of the GNU Affero General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option) any
# later version.
#
# PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.
# See the GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Commercial License Information:
#
# For a copy of the commercial license terms and conditions,
# go to: (http://www.pbspro.com/UserArea/agreement.html)
# or contact the Altair Legal Department.
#
# Altair’s dual-license business model allows companies, individuals, and
# organizations to create proprietary derivative works of PBS Pro and
# distribute them - whether embedded or bundled with other software -
# under a commercial license agreement.
#
# Use of Altair’s trademarks, including but not limited to "PBS™",
# "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
# trademark licensing policies.

import pbs

e = pbs.event()
if e.type == pbs.RESVSUB:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is resvsub")
elif e.type == pbs.RESV_END:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is resv_end")
elif e.type == pbs.QUEUEJOB:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is queuejob")
elif e.type == pbs.MODIFYJOB:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is modifyjob")
elif e.type == pbs.MOVEJOB:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is movejob")
elif e.type == pbs.RUNJOB:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is runjob")
elif e.type == pbs.PERIODIC:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is periodic")
elif e.type == pbs.EXECJOB_BEGIN:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is execjob_begin")
elif e.type == pbs.EXECJOB_PROLOGUE:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is execjob_prologue")
elif e.type == pbs.EXECJOB_LAUNCH:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is execjob_launch")
elif e.type == pbs.EXECJOB_ATTACH:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is execjob_attach")
elif e.type == pbs.EXECJOB_PRETERM:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is execjob_preterm")
elif e.type == pbs.EXECJOB_EPILOGUE:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is execjob_epilogue")
elif e.type == pbs.EXECJOB_END:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is execjob_end")
elif e.type == pbs.EXECHOST_STARTUP:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is exechost_startup")
elif e.type == pbs.EXECHOST_PERIODIC:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is exechost_periodic")
else:
    pbs.logmsg(pbs.LOG_DEBUG, "hook event type is unknown")
e.accept()
