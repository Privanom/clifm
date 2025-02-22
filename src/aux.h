/* aux.h */

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

#ifndef AUX_H
#define AUX_H

#include <time.h>

/* Some memory wrapper functions */
void *xrealloc(void *ptr, size_t size);
void *xcalloc(size_t nmemb, size_t size);
void *xnmalloc(size_t nmemb, size_t size);

char xgetchar(void);
/*int *get_hex_num(const char *str); */
char *url_encode(char *str);
char *url_decode(char *str);
char from_hex(char c);
//char *from_octal(char *s);
int read_octal(char *str);
int get_link_ref(const char *link);
off_t dir_size(char *dir);
char *get_size_unit(off_t size);
char *get_cmd_path(const char *cmd);
int count_dir(const char *dir, int pop);
char *xitoa(int n);
int xatoi(const char *s);
FILE *open_fstream_r(char *name, int *fd);
FILE *open_fstream_w(char *name, int *fd);
void close_fstream(FILE *fp, int fd);
int xmkdir(char *dir, mode_t mode);
mode_t get_dt(const mode_t mode);
char *gen_date_suffix(struct tm tm);
int get_cursor_position(const int ifd, const int ofd);
//int get_term_bgcolor(const int ifd, const int ofd);
void rl_ring_bell(void);
char *normalize_path(char *src, size_t src_len);

#endif /* AUX_H */
