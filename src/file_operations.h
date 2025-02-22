/* file_operations.h */

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

#ifndef FILE_OPERATIONS_H
#define FILE_OPERATIONS_H

/* Macros for open_function */
#define OPEN_BLK 0
#define OPEN_CHR 1
#define OPEN_SOCK 2
#define OPEN_FIFO 3
#define OPEN_UNK 4

/* file_operations.c */
int batch_link(char **args);
char *export(char **filenames, int open);
int bulk_rename(char **args);
int remove_file(char **args);
int copy_function(char **comm);
int edit_link(char *link);
int open_function(char **cmd);
int xchmod(const char *file, mode_t mode);
int create_file(char **cmd);
int dup_file(char **cmd);
int open_file(char *file);
void clear_selbox(void);

#endif /* FILE_OPERATIONS_H */
