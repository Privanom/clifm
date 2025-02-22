/* file_operations.c -- control multiple file operations */

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

#include "helpers.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <readline/readline.h>
#include <limits.h>
#include <fcntl.h>

#ifndef _NO_ARCHIVING
#include "archives.h"
#endif
#include "aux.h"
#include "checks.h"
#include "colors.h"
#include "exec.h"
#include "file_operations.h"
#include "history.h"
#include "listing.h"
#include "mime.h"
#include "misc.h"
#include "navigation.h"
#include "readline.h"
#include "selection.h"
#include "messages.h"

#include "config.h"

void
clear_selbox(void)
{
	size_t i;
	for (i = 0; i < sel_n; i++)
		free(sel_elements[i]);
	sel_n = 0;
	save_sel();	
}

static inline int
run_mime(char *file)
{
	char *p = rl_line_buffer;
	if ( (*p == 'i' && (strncmp(p, "import", 6) == 0
	|| strncmp(p, "info", 4) == 0))
	|| (*p == 'o' && (p[1] == ' ' || strncmp(p, "open", 4) == 0)) ) {
		char *cmd[] = {"mm", "open", file, NULL};
		return mime_open(cmd);
	}

	char *cmd[] = {"mm", file, NULL};
	return mime_open(cmd);
}

/* Open a file via OPENER, if set, or via LIRA. If not compiled with
 * Lira support, fallback to open (Haiku), or xdg-open. Returns zero
 * on success and one on failure */
int
open_file(char *file)
{
	if (!file || !*file)
		return EXIT_FAILURE;

	int exit_status = EXIT_SUCCESS;

	if (opener) {
		if (*opener == 'g' && strcmp(opener, "gio") == 0) {
			char *cmd[] = {"gio", "open", file, NULL};
			if (launch_execve(cmd, FOREGROUND, E_NOSTDERR) != EXIT_SUCCESS)
				exit_status = EXIT_FAILURE;
		} else {
			char *cmd[] = {opener, file, NULL};
			if (launch_execve(cmd, FOREGROUND, E_NOSTDERR) != EXIT_SUCCESS)
				exit_status = EXIT_FAILURE;
		}
	} else {
#ifndef _NO_LIRA
		exit_status = run_mime(file);
/*		char *p = rl_line_buffer;
		if (*p == 'i' && (strncmp(p, "import", 6) == 0
		|| strncmp(p, "info", 4) == 0)) {
			char *cmd[] = {"mm", "open", file, NULL};
			return exit_status = mime_open(cmd);
		}
		if (*p == 'o' && (p[1] == ' ' || strncmp(p, "open", 4) == 0)) {
			char *cmd[] = {"mm", "open", file, NULL};
			return exit_status = mime_open(cmd);
		}
		char *cmd[] = {"mm", file, NULL};
		exit_status = mime_open(cmd); */
#else
		/* Fallback to (xdg-)open */
#ifdef __HAIKU__
		char *cmd[] = {"open", file, NULL};
#else
		char *cmd[] = {"xdg-open", file, NULL};
#endif /* __HAIKU__ */
		if (launch_execve(cmd, FOREGROUND, E_NOSTDERR) != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
#endif /* _NO_LIRA */
	}

	return exit_status;
}

/* Toggle executable bit on file */
int
xchmod(const char *file, mode_t mode)
{
	/* Set or unset S_IXUSR, S_IXGRP, and S_IXOTH */
	(0100 & mode) ? (mode &= (mode_t)~0111) : (mode |= 0111);

	log_function(NULL);

	int fd = open(file, O_WRONLY);
	if (fd == -1) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, file, strerror(errno));
		return EXIT_FAILURE;
	}

	if (fchmod(fd, mode) == -1) {
		close(fd);
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, file, strerror(errno));
		return EXIT_FAILURE;
	}

	close(fd);

	return EXIT_SUCCESS;
}

int
dup_file(char **cmd)
{
	if (!cmd[1] || IS_HELP(cmd[1])) {
		puts(_(DUP_USAGE));
		return EXIT_SUCCESS;
	}

	log_function(NULL);

	char *rsync_path = get_cmd_path("rsync");
	int exit_status =  EXIT_SUCCESS;

	size_t i;
	for (i = 1; cmd[i]; i++) {
		if (!cmd[i] || !*cmd[i])
			continue;
		char *source = cmd[i];
		if (strchr(source, '\\')) {
			char *deq_str = dequote_str(source, 0);
			if (!deq_str) {
				fprintf(stderr, "%s: %s: Error dequoting file name\n",
					PROGRAM_NAME, source);
				continue;
			}
			strcpy(source, deq_str);
			free(deq_str);
		}

		// Use source as destiny file name: source.copy, and, if already
		// exists, source.copy-n, where N is an integer greater than zero
		size_t source_len = strlen(source);
		if (strcmp(source, "/") != 0 && source[source_len - 1] == '/')
			source[source_len - 1] = '\0';

		char *tmp = strrchr(source, '/');
		char *source_name;

		if (tmp && *(tmp + 1))
			source_name = tmp + 1;
		else
			source_name = source;

		char tmp_dest[PATH_MAX];
		snprintf(tmp_dest, PATH_MAX - 1, "%s.copy", source_name);
		char bk[PATH_MAX + 11];
		xstrsncpy(bk, tmp_dest, PATH_MAX);
		struct stat attr;
		int suffix = 1;
		while (stat(bk, &attr) == EXIT_SUCCESS) {
			snprintf(bk, sizeof(bk), "%s-%d", tmp_dest, suffix);
			suffix++;
		}
		char *dest = savestring(bk, strlen(bk));

		if (rsync_path) {
			char *_cmd[] = {"rsync", "-aczvAXHS", "--progress", source, dest, NULL};
			if (launch_execve(_cmd, FOREGROUND, E_NOFLAG) != EXIT_SUCCESS)
				exit_status = EXIT_FAILURE;
		} else {
			char *_cmd[] = {"cp", "-a", source, dest, NULL};
			if (launch_execve(_cmd, FOREGROUND, E_NOFLAG) != EXIT_SUCCESS)
				exit_status = EXIT_FAILURE;
		}

		free(dest);
	}

	free(rsync_path);
	return exit_status;
} 

int
create_file(char **cmd)
{
	if (cmd[1] && IS_HELP(cmd[1])) {
		puts(_(NEW_USAGE));
		return EXIT_FAILURE;
	}

	log_function(NULL);

	int exit_status = EXIT_SUCCESS;
#ifdef __HAIKU__
	int file_in_cwd = 0;
#endif
	int free_cmd = 0;

	/* If no argument provided, ask the user for a filename */
	if (!cmd[1]) {
		char *filename = (char *)NULL;
		while (!filename) {
			puts(_("End filename with a slash to create a directory"));
			filename = rl_no_hist(_("Filename ('q' to quit): "));

			if (!filename)
				continue;

			if (!*filename) {
				free(filename);
				filename = (char *)NULL;
				continue;
			}
		}

		if (*filename == 'q' && !filename[1]) {
			free(filename);
			return EXIT_SUCCESS;
		}

		/* Once we have the filename, reconstruct the cmd array */
		char **tmp_cmd = (char **)xnmalloc(args_n + 3, sizeof(char *));
		tmp_cmd[0] = (char *)xnmalloc(2, sizeof(char));
		*tmp_cmd[0] = 'n';
		tmp_cmd[0][1] = '\0';
		tmp_cmd[1] = (char *)xnmalloc(strlen(filename) + 1, sizeof(char));
		strcpy(tmp_cmd[1], filename);
		tmp_cmd[2] = (char *)NULL;
		cmd = tmp_cmd;
		free_cmd = 1;
		free(filename);
	}

	/* Properly format filenames */
	size_t i;
	for (i = 1; cmd[i]; i++) {
		if (strchr(cmd[i], '\\')) {
			char *deq_str = dequote_str(cmd[i], 0);
			if (!deq_str) {
				_err('w', PRINT_PROMPT, _("%s: %s: Error dequoting filename\n"),
					PROGRAM_NAME, cmd[i]);
				continue;
			}

			strcpy(cmd[i], deq_str);
			free(deq_str);
		}

		if (*cmd[i] == '~') {
			char *exp_path = tilde_expand(cmd[i]);
			if (exp_path) {
				cmd[i] = (char *)xrealloc(cmd[i], (strlen(exp_path) + 1)
											* sizeof(char));
				strcpy(cmd[i], exp_path);
				free(exp_path);
			}
		}

		/* If the file already exists, create it as file.new */
		struct stat a;
		if (lstat(cmd[i], &a) == 0) {
			int dir = 0;
			char old_name[PATH_MAX];
			strcpy(old_name, cmd[i]); 

			size_t len = strlen(cmd[i]);
			if (cmd[i][len - 1] == '/') {
				cmd[i][len - 1] = '\0';
				dir = 1;
			}

			cmd[i] = (char *)xrealloc(cmd[i], (len + 5) * sizeof(char));
			if (dir)
				strcat(cmd[i], ".new/");
			else
				strcat(cmd[i], ".new");

			_err(0, PRINT_PROMPT, _("%s: %s: File already exists. "
			"Trying with '%s' instead\n"), PROGRAM_NAME, old_name, cmd[i]);
		}

#ifdef __HAIKU__
		/* If at least one filename lacks a slash (or it is the only and
		 * last char, in which case we have a directory in CWD), we are
		 * creating a file in CWD, and thereby we need to update the screen */
		char *ret = strrchr(cmd[i], '/');
		if (!ret || !*(ret + 1))
			file_in_cwd = 1;
#endif
	}

	/* Construct commands */
	size_t files_num = i - 1;

	char **nfiles = (char **)xnmalloc(files_num + 2, sizeof(char *));
	char **ndirs = (char **)xnmalloc(files_num + 3, sizeof(char *));

	/* Let's use 'touch' for files and 'mkdir -p' for dirs */
	nfiles[0] = (char *)xnmalloc(6, sizeof(char));
	strcpy(nfiles[0], "touch");

	ndirs[0] = (char *)xnmalloc(6, sizeof(char));
	strcpy(ndirs[0], "mkdir");

	ndirs[1] = (char *)xnmalloc(3, sizeof(char));
	ndirs[1][0] = '-';
	ndirs[1][1] = 'p';
	ndirs[1][2] = '\0';

	size_t cnfiles = 1, cndirs = 2;

	for (i = 1; cmd[i]; i++) {
		size_t cmd_len = strlen(cmd[i]);
		/* Filenames ending with a slash are taken as dir names */
		if (cmd[i][cmd_len - 1] == '/') {
			ndirs[cndirs] = cmd[i];
			cndirs++;
		} else {
			nfiles[cnfiles] = cmd[i];
			cnfiles++;
		}
	}

	ndirs[cndirs] = (char *)NULL;
	nfiles[cnfiles] = (char *)NULL;

	/* Execute commands */
	if (cnfiles > 1) {
		if (launch_execve(nfiles, FOREGROUND, 0) != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
	}

	if (cndirs > 2) {
		if (launch_execve(ndirs, FOREGROUND, 0) != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
	}

	free(nfiles[0]);
	free(ndirs[0]);
	free(ndirs[1]);
	free(nfiles);
	free(ndirs);
	if (free_cmd) {
		for (i = 0; cmd[i]; i++)
			free(cmd[i]);
		free(cmd);
	}

#ifdef __HAIKU__
	if (exit_status == EXIT_SUCCESS && autols && file_in_cwd) {
		free_dirlist();
		if (list_dir() != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
	}
#endif

	return exit_status;
}

int
open_function(char **cmd)
{
	if (!cmd)
		return EXIT_FAILURE;

	if (!cmd[1] || IS_HELP(cmd[1])) {
		puts(_(OPEN_USAGE));
		return EXIT_SUCCESS;
	}

	if (*cmd[0] == 'o' && (!cmd[0][1] || strcmp(cmd[0], "open") == 0)) {
		if (strchr(cmd[1], '\\')) {
			char *deq_path = dequote_str(cmd[1], 0);
			if (!deq_path) {
				fprintf(stderr, _("%s: %s: Error dequoting filename\n"),
					PROGRAM_NAME, cmd[1]);
				return EXIT_FAILURE;
			}

			strcpy(cmd[1], deq_path);
			free(deq_path);
		}
	}

	char *file = cmd[1];

	/* Check file existence */
	struct stat attr;
	if (lstat(file, &attr) == -1) {
		fprintf(stderr, "%s: open: %s: %s\n", PROGRAM_NAME, cmd[1],
		    strerror(errno));
		return EXIT_FAILURE;
	}

	/* Check file type: only directories, symlinks, and regular files
	 * will be opened */

	char no_open_file = 1;
	char *file_type = (char *)NULL;
	char *types[] = {
		"block device",
		"character device",
		"socket",
		"FIFO/pipe",
		"unknown file type",
		NULL};

	switch ((attr.st_mode & S_IFMT)) {
		/* Store file type to compose and print the error message, if
		 * necessary */
	case S_IFBLK: file_type = types[OPEN_BLK]; break;
	case S_IFCHR: file_type = types[OPEN_CHR]; break;
	case S_IFSOCK: file_type = types[OPEN_SOCK]; break;
	case S_IFIFO: file_type = types[OPEN_FIFO]; break;
	case S_IFDIR: return cd_function(file, CD_PRINT_ERROR);
	case S_IFLNK: {
		int ret = get_link_ref(file);
		if (ret == -1) {
			fprintf(stderr, _("%s: %s: Broken symbolic link\n"),
					PROGRAM_NAME, file);
			return EXIT_FAILURE;
		} else if (ret == S_IFDIR) {
			return cd_function(file, CD_PRINT_ERROR);
		} else if (ret != S_IFREG) {
			switch (ret) {
			case S_IFBLK: file_type = types[OPEN_BLK]; break;
			case S_IFCHR: file_type = types[OPEN_CHR]; break;
			case S_IFSOCK: file_type = types[OPEN_SOCK]; break;
			case S_IFIFO: file_type = types[OPEN_FIFO]; break;
			default: file_type = types[OPEN_UNK]; break;
			}
		}
		}
		/* fallthrough */
	case S_IFREG:
/*#ifndef _NO_ARCHIVING
		// If an archive/compressed file, call archiver()
		if (is_compressed(file, 1) == 0) {
			char *tmp_cmd[] = {"ad", file, NULL};
			return archiver(tmp_cmd, 'd');
		}
#endif */
		no_open_file = 0;
		break;

	default:
		file_type = types[OPEN_UNK];
		break;
	}

	/* If neither directory nor regular file nor symlink (to directory
	 * or regular file), print the corresponding error message and
	 * exit */
	if (no_open_file) {
		fprintf(stderr, _("%s: %s (%s): Cannot open file\nTry "
			"'APPLICATION FILENAME'\n"), PROGRAM_NAME, cmd[1], file_type);
		return EXIT_FAILURE;
	}

	/* At this point we know that the file to be openend is either a regular
	 * file or a symlink to a regular file. So, just open the file */
	if (!cmd[2] || (*cmd[2] == '&' && !cmd[2][1])) {
		int ret = open_file(file);
		if (!opener && ret == EXIT_FAILURE) {
			fputs("Add a new entry to the mimelist file ('mime "
			      "edit' or F6) or run 'open FILE APPLICATION'\n", stderr);
			return EXIT_FAILURE;
		}
		return ret;
	}

	/* If some application was specified to open the file */
	char *tmp_cmd[] = {cmd[2], file, NULL};
	int ret = launch_execve(tmp_cmd, bg_proc ? BACKGROUND : FOREGROUND, E_NOSTDERR);
	if (ret != EXIT_SUCCESS)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

/* Relink symlink to new path */
int
edit_link(char *link)
{
	if (!link || !*link)
		return EXIT_FAILURE;

	log_function(NULL);

	/* Dequote the file name, if necessary */
	if (strchr(link, '\\')) {
		char *tmp = dequote_str(link, 0);
		if (!tmp) {
			fprintf(stderr, _("%s: %s: Error dequoting file\n"),
			    PROGRAM_NAME, link);
			return EXIT_FAILURE;
		}

		strcpy(link, tmp);
		free(tmp);
	}

	size_t len = strlen(link);
	if (link[len - 1] == '/')
		link[len - 1] = '\0';

	/* Check we have a valid symbolic link */
	struct stat attr;
	if (lstat(link, &attr) == -1) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, link,
		    strerror(errno));
		return EXIT_FAILURE;
	}

	if (!S_ISLNK(attr.st_mode)) {
		fprintf(stderr, _("%s: %s: Not a symbolic link\n"),
		    PROGRAM_NAME, link);
		return EXIT_FAILURE;
	}

	/* Get file pointed to by symlink and report to the user */
	char *real_path = realpath(link, NULL);
	if (!real_path) {
		printf(_("%s%s%s currently pointing to nowhere (broken link)\n"),
		    or_c, link, df_c);
	} else {
		printf(_("%s%s%s currently pointing to "), ln_c, link, df_c);
		colors_list(real_path, NO_ELN, NO_PAD, PRINT_NEWLINE);
		free(real_path);
		real_path = (char *)NULL;
	}

	char *new_path = (char *)NULL;
	/* Enable autocd and auto-open (in case they are not already
	 * enabled) to allow TAB completion for ELN's */
	int autocd_status = autocd, auto_open_status = auto_open;
	autocd = auto_open = 1;

	while (!new_path) {
		new_path = rl_no_hist(_("New path ('q' to quit): "));
		if (!new_path)
			continue;
		if (!*new_path) {
			free(new_path);
			new_path = (char *)NULL;
			continue;
		}

		if (*new_path == 'q' && !new_path[1]) {
			free(new_path);
			return EXIT_SUCCESS;
		}
	}

	/* Set autocd and auto-open to their original values */
	autocd = autocd_status;
	auto_open = auto_open_status;

	/* If an ELN, replace by the corresponding file name */
	if (is_number(new_path)) {
		int a = atoi(new_path);
		if (a > 0 && a <= (int)files) {
			--a;
			if (file_info[a].name)
				new_path = savestring(file_info[a].name, strlen(file_info[a].name));
		} else {
			fprintf(stderr, _("%s: %s: Invalid ELN\n"), PROGRAM_NAME, new_path);
			free(new_path);
			return EXIT_FAILURE;
		}
	}

	/* Remove terminating space. TAB completion puts a final space
	 * after file names */
	size_t path_len = strlen(new_path);
	if (new_path[path_len - 1] == ' ')
		new_path[path_len - 1] = '\0';

	/* Dequote new path, if needed */
	if (strchr(new_path, '\\')) {
		char *tmp = dequote_str(new_path, 0);
		if (!tmp) {
			fprintf(stderr, _("%s: %s: Error dequoting file\n"),
			    PROGRAM_NAME, new_path);
			free(new_path);
			return EXIT_FAILURE;
		}

		strcpy(new_path, tmp);
		free(tmp);
	}

	/* Check new_path existence and warn the user if it does not
	 * exist */
	if (lstat(new_path, &attr) == -1) {
		printf("'%s': %s\n", new_path, strerror(errno));
		char *answer = (char *)NULL;
		while (!answer) {
			answer = rl_no_hist(_("Relink as a broken symbolic link? [y/n] "));
			if (!answer)
				continue;
			if (!*answer) {
				free(answer);
				answer = (char *)NULL;
				continue;
			}

			if (*answer != 'y' && *answer != 'n') {
				free(answer);
				answer = (char *)NULL;
				continue;
			}

			if (answer[1]) {
				free(answer);
				answer = (char *)NULL;
				continue;
			}

			if (*answer == 'y') {
				free(answer);
				break;
			} else {
				free(answer);
				free(new_path);
				return EXIT_SUCCESS;
			}
		}
	}

	/* Finally, relink the symlink to new_path */
	char *cmd[] = {"ln", "-sfn", new_path, link, NULL};
	if (launch_execve(cmd, FOREGROUND, E_NOFLAG) != EXIT_SUCCESS) {
		free(new_path);
		return EXIT_FAILURE;
	}

	real_path = realpath(link, NULL);
	printf(_("%s%s%s successfully relinked to "), real_path ? ln_c
			: or_c, link, df_c);
	colors_list(new_path, NO_ELN, NO_PAD, PRINT_NEWLINE);
	free(new_path);
	if (real_path)
		free(real_path);

	return EXIT_SUCCESS;
}

int
copy_function(char **args)
{
	log_function(NULL);

	if (*args[0] == 'm' && args[1]) {
		size_t len = strlen(args[1]);
		if (args[1][len - 1] == '/')
			args[1][len - 1] = '\0';
	}

	if (!is_sel)
		return run_and_refresh(args);

	size_t n = 0;
	char **tcmd = (char **)xnmalloc(2 + args_n + 2, sizeof(char *));
	char *p = strchr(args[0], ' ');
	if (p && *(p + 1)) {
		*p = '\0';
		p++;
		tcmd[0] = savestring(args[0], strlen(args[0]));
		tcmd[1] = savestring(p, strlen(p));
		n += 2;
	} else {
		tcmd[0] = savestring(args[0], strlen(args[0]));
		n++;
	}

	size_t i;
	for (i = 1; args[i]; i++) {
		p = dequote_str(args[i], 0);
		if (!p)
			continue;
		tcmd[n] = savestring(p, strlen(p));
		free(p);
		n++;
	}

	if (sel_is_last) {
		tcmd[n] = savestring(".", 1);
		n++;
	}

	tcmd[n] = (char *)NULL;

	int ret = launch_execve(tcmd, FOREGROUND, E_NOFLAG);

	for (i = 0; tcmd[i]; i++)
		free(tcmd[i]);
	free(tcmd);

	if (ret != EXIT_SUCCESS)
		return EXIT_FAILURE;

	if (copy_n_rename) { /* vv */
		char **tmp = (char **)xnmalloc(sel_n + 3, sizeof(char *));
		tmp[0] = savestring("br", 2);

		size_t j;
		for (j = 0; j < sel_n; j++) {
			size_t arg_len = strlen(sel_elements[j]);

			if (sel_elements[j][arg_len - 1] == '/')
				sel_elements[j][arg_len - 1] = '\0';

			if (*args[args_n] == '~') {
				char *exp_dest = tilde_expand(args[args_n]);
				args[args_n] = xrealloc(args[args_n],
				    (strlen(exp_dest) + 1) * sizeof(char));
				strcpy(args[args_n], exp_dest);
				free(exp_dest);
			}

			size_t dest_len = strlen(args[args_n]);
			if (args[args_n][dest_len - 1] == '/')
				args[args_n][dest_len - 1] = '\0';

			char dest[PATH_MAX];
			strcpy(dest, (sel_is_last || strcmp(args[args_n], ".") == 0)
					 ? workspaces[cur_ws].path : args[args_n]);

			char *ret_val = strrchr(sel_elements[j], '/');
			char *tmp_str = (char *)xnmalloc(strlen(dest)
					+ strlen(ret_val + 1) + 2, sizeof(char));

			sprintf(tmp_str, "%s/%s", dest, ret_val + 1);

			tmp[j + 1] = savestring(tmp_str, strlen(tmp_str));
			free(tmp_str);
		}

		tmp[j + 1] = (char *)NULL;
		bulk_rename(tmp);

		for (i = 0; tmp[i]; i++)
			free(tmp[i]);
		free(tmp);
		copy_n_rename = 0;
		return EXIT_SUCCESS;
	}

	/* If 'mv sel' and command is successful deselect everything,
	 * since sel files are note there anymore */
	if (*args[0] == 'm' && args[0][1] == 'v'
	&& (!args[0][2] || args[0][2] == ' '))
		clear_selbox();

#ifdef __HAIKU__
	if (autols) {
		free_dirlist();
		list_dir();
	}
#endif

	return EXIT_SUCCESS;
}

int
remove_file(char **args)
{
	int cwd = 0, exit_status = EXIT_SUCCESS;

	log_function(NULL);

	char **rm_cmd = (char **)xnmalloc(args_n + 4, sizeof(char *));
	int i, j = 3, dirs = 0;

	for (i = 1; args[i]; i++) {
		/* Check if at least one file is in the current directory. If not,
		 * there is no need to refresh the screen */
		if (!cwd) {
			char *ret = strchr(args[i], '/');
			/* If there's no slash, or if slash is the last char and
			 * the file is not root "/", we have a file in CWD */
			if (!ret || (!*(ret + 1) && ret != args[i]))
				cwd = 1;
		}

		char *tmp = (char *)NULL;
		if (strchr(args[i], '\\')) {
			tmp = dequote_str(args[i], 0);
			if (tmp) {
				/* Start storing file names in 3: 0 is for 'rm', and 1
				 * and 2 for parameters, including end of parameters (--) */
				rm_cmd[j] = savestring(tmp, strlen(tmp));
				j++;
				free(tmp);
			} else {
				fprintf(stderr, "%s: %s: Error dequoting file name\n",
				    PROGRAM_NAME, args[i]);
				continue;
			}
		} else {
			rm_cmd[j] = savestring(args[i], strlen(args[i]));
			j++;
		}

		struct stat attr;
		if (!dirs && lstat(rm_cmd[j - 1], &attr) != -1 && S_ISDIR(attr.st_mode))
			dirs = 1;
	}

	rm_cmd[j] = (char *)NULL;

	rm_cmd[0] = savestring("rm", 2);
	if (dirs)
#if defined(__NetBSD__) || defined(__OpenBSD__)
		rm_cmd[1] = savestring("-r", 2);
#else
		rm_cmd[1] = savestring("-dIr", 4);
#endif
	else
#if defined(__NetBSD__) || defined(__OpenBSD__)
		rm_cmd[1] = savestring("-f", 2);
#else
		rm_cmd[1] = savestring("-I", 2);
#endif
	rm_cmd[2] = savestring("--", 2);

	if (launch_execve(rm_cmd, FOREGROUND, E_NOFLAG) != EXIT_SUCCESS)
		exit_status = EXIT_FAILURE;
#ifdef __HAIKU__
	else {
		if (cwd && autols && strcmp(args[1], "--help") != 0
		&& strcmp(args[1], "--version") != 0) {
			free_dirlist();
			exit_status = list_dir();
		}
	}
#endif

	if (is_sel && exit_status == EXIT_SUCCESS)
		clear_selbox();

	for (i = 0; rm_cmd[i]; i++)
		free(rm_cmd[i]);
	free(rm_cmd);
	return exit_status;
}

/* Rename a bulk of files (ARGS) at once. Takes files to be renamed
 * as arguments, and returns zero on success and one on error. The
 * procedude is quite simple: file names to be renamed are copied into
 * a temporary file, which is opened via the mime function and shown
 * to the user to modify it. Once the file names have been modified and
 * saved, modifications are printed on the screen and the user is
 * asked whether to perform the actual bulk renaming (via mv) or not.
 * I took this bulk rename method, just because it is quite simple and
 * KISS, from the fff filemanager. So, thanks fff! BTW, this method
 * is also implemented by ranger and nnn */
int
bulk_rename(char **args)
{
	if (!args[1])
		return EXIT_FAILURE;

	log_function(NULL);

	int exit_status = EXIT_SUCCESS;

	char bulk_file[PATH_MAX];
	if (xargs.stealth_mode == 1)
		snprintf(bulk_file, PATH_MAX - 1, "%s/%s", P_tmpdir, TMP_FILENAME);
	else
		snprintf(bulk_file, PATH_MAX - 1, "%s/%s", tmp_dir, TMP_FILENAME);

	int fd = mkstemp(bulk_file);
	if (fd == -1) {
		_err('e', PRINT_PROMPT, "bulk: %s: %s\n", bulk_file, strerror(errno));
		return EXIT_FAILURE;
	}

	size_t i, arg_total = 0;
	FILE *fp = (FILE *)NULL;

#ifdef __HAIKU__
	fp = fopen(bulk_file, "w");
	if (!fp) {
		_err('e', PRINT_PROMPT, "bulk: %s: %s\n", bulk_file, strerror(errno));
		return EXIT_FAILURE;
	}
#endif

#define BULK_MESSAGE "# Edit the file names, save, and quit the editor\n\
# Just quit the editor to cancel the operation\n\n"

#ifndef __HAIKU__
	dprintf(fd, BULK_MESSAGE);
#else
	fprintf(fp, BULK_MESSAGE);
#endif

	/* Copy all files to be renamed to the bulk file */
	for (i = 1; args[i]; i++) {
		/* Dequote file name, if necessary */
		if (strchr(args[i], '\\')) {
			char *deq_file = dequote_str(args[i], 0);
			if (!deq_file) {
				fprintf(stderr, _("bulk: %s: Error dequoting "
						"file name\n"), args[i]);
				continue;
			}
			strcpy(args[i], deq_file);
			free(deq_file);
		}
#ifndef __HAIKU__
		dprintf(fd, "%s\n", args[i]);
#else
		fprintf(fp, "%s\n", args[i]);
#endif
	}
#ifdef __HAIKU__
	fclose(fp);
#endif
	arg_total = i;
	close(fd);

	fp = open_fstream_r(bulk_file, &fd);
	if (!fp) {
		_err('e', PRINT_PROMPT, "bulk: '%s': %s\n", bulk_file, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Store the last modification time of the bulk file. This time
	 * will be later compared to the modification time of the same
	 * file after shown to the user */
	struct stat attr;
	fstat(fd, &attr);
	time_t mtime_bfr = (time_t)attr.st_mtime;

	/* Open the bulk file */
	open_in_foreground = 1;
	exit_status = open_file(bulk_file);
	open_in_foreground = 0;
	if (exit_status != EXIT_SUCCESS) {
		fprintf(stderr, _("bulk: %s\n"), strerror(errno));
		if (unlinkat(fd, bulk_file, 0) == -1) {
			_err('e', PRINT_PROMPT, "%s: '%s': %s\n", PROGRAM_NAME,
			    bulk_file, strerror(errno));
		}
		close_fstream(fp, fd);
		return EXIT_FAILURE;
	}

	close_fstream(fp, fd);
	fp = open_fstream_r(bulk_file, &fd);
	if (!fp) {
		_err('e', PRINT_PROMPT, "bulk: '%s': %s\n", bulk_file, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Compare the new modification time to the stored one: if they
	 * match, nothing was modified */
	fstat(fd, &attr);
	if (mtime_bfr == (time_t)attr.st_mtime) {
		puts(_("bulk: Nothing to do"));
		if (unlinkat(fd, bulk_file, 0) == -1) {
			_err('e', PRINT_PROMPT, "%s: '%s': %s\n", PROGRAM_NAME,
			    bulk_file, strerror(errno));
			exit_status = EXIT_FAILURE;
		}
		close_fstream(fp, fd);
		return exit_status;
	}

	/* Make sure there are as many lines in the bulk file as files
	 * to be renamed */
	size_t file_total = 1;
	char tmp_line[256];
	while (fgets(tmp_line, (int)sizeof(tmp_line), fp)) {
		if (!*tmp_line || *tmp_line == '\n' || *tmp_line == '#')
			continue;
		file_total++;
	}

	if (arg_total != file_total) {
		fputs(_("bulk: Line mismatch in rename file\n"), stderr);
		if (unlinkat(fd, bulk_file, 0) == -1)
			_err('e', PRINT_PROMPT, "%s: '%s': %s\n", PROGRAM_NAME,
			    bulk_file, strerror(errno));
		close_fstream(fp, fd);
		return EXIT_FAILURE;
	}

	/* Go back to the beginning of the bulk file, again */
	fseek(fp, 0L, SEEK_SET);

	size_t line_size = 0;
	char *line = (char *)NULL;
	ssize_t line_len = 0;
	int modified = 0;

	i = 1;
	/* Print what would be done */
	while ((line_len = getline(&line, &line_size, fp)) > 0) {
		if (!*line || *line == '\n' || *line == '#')
			continue;
		if (line[line_len - 1] == '\n')
			line[line_len - 1] = '\0';

		if (args[i] && strcmp(args[i], line) != 0) {
			printf("%s %s->%s %s\n", args[i], mi_c, df_c, line);
			modified++;
		}

		i++;
	}

	/* If no file name was modified */
	if (!modified) {
		puts(_("bulk: Nothing to do"));
		if (unlinkat(fd, bulk_file, 0) == -1) {
			_err('e', PRINT_PROMPT, "%s: '%s': %s\n", PROGRAM_NAME,
			    bulk_file, strerror(errno));
			exit_status = EXIT_FAILURE;
		}
		free(line);
		close_fstream(fp, fd);
		return exit_status;
	}

	/* Ask the user for confirmation */
	char *answer = (char *)NULL;
	while (!answer) {
		answer = rl_no_hist(_("Continue? [y/N] "));
		if (answer && *answer && strlen(answer) > 1) {
			free(answer);
			answer = (char *)NULL;
			continue;
		}

		if (!answer) {
			free(line);
			close_fstream(fp, fd);
			return EXIT_SUCCESS;
		}

		switch (*answer) {
		case 'y': /* fallthrough */
		case 'Y': break;

		case 'n': /* fallthrough */
		case 'N': /* fallthrough */
		case '\0':
			free(answer);
			free(line);
			close_fstream(fp, fd);
			return EXIT_SUCCESS;

		default:
			free(answer);
			answer = (char *)NULL;
			break;
		}
	}

	free(answer);

	/* Once again */
	fseek(fp, 0L, SEEK_SET);

	i = 1;

	/* Rename each file */
	while ((line_len = getline(&line, &line_size, fp)) > 0) {
		if (!*line || *line == '\n' || *line == '#')
			continue;

		if (!args[i]) {
			i++;
			continue;
		}

		if (line[line_len - 1] == '\n')
			line[line_len - 1] = '\0';
		if (args[i] && strcmp(args[i], line) != 0) {
			if (renameat(AT_FDCWD, args[i], AT_FDCWD, line) == -1)
				exit_status = EXIT_FAILURE;
		}

		i++;
	}

	free(line);

	if (unlinkat(fd, bulk_file, 0) == -1) {
		_err('e', PRINT_PROMPT, "%s: '%s': %s\n", PROGRAM_NAME,
		    bulk_file, strerror(errno));
		exit_status = EXIT_FAILURE;
	}
	close_fstream(fp, fd);

#ifdef __HAIKU__
	if (autols) {
		free_dirlist();
		if (list_dir() != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
	}
#endif

	return exit_status;
}

/* Export files in CWD (if FILENAMES is NULL), or files in FILENAMES,
 * into a temporary file. Return the address of this empt file if
 * success (it must be freed) or NULL in case of error */
char *export(char **filenames, int open)
{
	char *tmp_file = (char *)xnmalloc(strlen(tmp_dir) + 14, sizeof(char));
	sprintf(tmp_file, "%s/%s", tmp_dir, TMP_FILENAME);

	int fd = mkstemp(tmp_file);
	if (fd == -1) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, tmp_file, strerror(errno));
		free(tmp_file);
		return (char *)NULL;
	}
	
	size_t i;
#ifdef __HAIKU__
	FILE *fp = fopen(tmp_file, "w");
	if (!fp) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, tmp_file, strerror(errno));
		free(tmp_file);
		return (char *)NULL;
	}
#endif

	/* If no argument, export files in CWD */
	if (!filenames[1]) {
		for (i = 0; file_info[i].name; i++)
#ifndef __HAIKU__
			dprintf(fd, "%s\n", file_info[i].name);
#else
			fprintf(fp, "%s\n", file_info[i].name);
#endif
	} else {
		for (i = 1; filenames[i]; i++) {
			if (*filenames[i] == '.' && (!filenames[i][1] || (filenames[i][1] == '.' && !filenames[i][2])))
				continue;
#ifndef __HAIKU__
			dprintf(fd, "%s\n", filenames[i]);
#else
			fprintf(fp, "%s\n", filenames[i]);
#endif
		}
	}
#ifdef __HAIKU__
	fclose(fp);
#endif
	close(fd);

	if (!open)
		return tmp_file;

	int ret = open_file(tmp_file);
	if (ret == EXIT_SUCCESS) {
		return tmp_file;
	} else {
		free(tmp_file);
		return (char *)NULL;
	}
}

int
batch_link(char **args)
{
	if (!args)
		return EXIT_FAILURE;

	if (!args[1] || IS_HELP(args[1])) {
		puts(_(BL_USAGE));
		return EXIT_SUCCESS;
	}

	log_function(NULL);

	puts("Suffix defaults to '.link'");
	char *suffix = rl_no_hist(_("Enter links suffix ('q' to quit): "));

	if (suffix && *suffix == 'q' && !*(suffix + 1)) {
		free(suffix);
		return EXIT_SUCCESS;
	}

	size_t i;
	int exit_status = EXIT_SUCCESS;
	char tmp[NAME_MAX];

	for (i = 1; args[i]; i++) {
		char *linkname = (char *)NULL;

		if (!suffix || !*suffix) {
			snprintf(tmp, NAME_MAX, "%s.link", args[i]);
			linkname = tmp;
		} else {
			snprintf(tmp, NAME_MAX, "%s%s", args[i], suffix);
			linkname = tmp;
		}

		char *ptr = strrchr(linkname, '/');
		if (symlinkat(args[i], AT_FDCWD, ptr ? ++ptr : linkname) == -1) {
			exit_status = EXIT_FAILURE;
			fprintf(stderr, _("%s: %s: Cannot create symlink: %s\n"),
			    PROGRAM_NAME, ptr ? ptr : linkname, strerror(errno));
		}
	}

#ifdef __HAIKU__
	if (exit_status == EXIT_SUCCESS && autols) {
		free_dirlist();

		if (list_dir() != EXIT_SUCCESS)
			exit_status = EXIT_FAILURE;
	}
#endif

	free(suffix);
	return exit_status;
}
