/* exec.h */

/*
 * This file is part of CliFM
 * 
 * Copyright (C) 2016-2022, L. Abramovich <johndoe.arch@outlook.com>
 * All rights reserved.

 * CliFM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * CliFM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
*/

#ifndef EXEC_H
#define EXEC_H

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <signal.h>
#endif

int exec_cmd(char **comm);
void exec_chained_cmds(char *cmd);
void exec_profile(void);
int launch_execve(char **cmd, int bg, int xflags);
int launch_execle(const char *cmd);
int run_and_refresh(char **comm);

#endif /* EXEC_H */
