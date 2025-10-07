# SPDX-License-Identifier: GPL-2.0-only
# ################################################################
# Copyright (C) 2025, LunarKernel Project.
# All rights reserved.
#
# This software is licensed under the terms of the GNU General Public
# License version 2, as published by the Free Software Foundation, and
# may be copied, distributed, and modified under those terms.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
# ################################################################

GCOV_PROFILE := y
obj-$(CONFIG_LUNAR_SCHED_EXT) += lunar_bsp_ext_sched.o

ccflags-y += -I$(src)/include
lunar_bsp_ext_sched-y := \
        cpufreq_lse.o \
		lse_cfs.o \
        lse_main.o \
		lse_sched_cluster.o \
		lse_sysctl.o \
		lse_task_struct_ext.o \
		lse_util_track.o
