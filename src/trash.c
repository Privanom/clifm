/* trash.c -- functions to manage the trash system */

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

#ifndef _NO_TRASH

#include "helpers.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aux.h"
#include "checks.h"
#include "colors.h"
#include "exec.h"
#include "misc.h"
#include "navigation.h"
#include "readline.h"
#include "sort.h"
#include "trash.h"

/* Recursively check directory permissions (write and execute). Returns
 * zero if OK, and one if at least one subdirectory does not have
 * write/execute permissions */
static int
recur_perm_check(const char *dirname)
{
	DIR *dir;
	struct dirent *ent;
#if !defined(_DIRENT_HAVE_D_TYPE)
	struct stat attr;
#endif

	if (!(dir = opendir(dirname)))
		return EXIT_FAILURE;

	while ((ent = readdir(dir)) != NULL) {
#if !defined(_DIRENT_HAVE_D_TYPE)
		if (lstat(ent->d_name, &attr) == -1)
			continue;
		if (S_ISDIR(attr.st_mode)) {
#else
		if (ent->d_type == DT_DIR) {
#endif
			char dirpath[PATH_MAX] = "";

			if (*ent->d_name == '.' && (!ent->d_name[1]
			|| (ent->d_name[1] == '.' && !ent->d_name[2])))
				continue;

			snprintf(dirpath, PATH_MAX, "%s/%s", dirname, ent->d_name);

			if (access(dirpath, W_OK | X_OK) != 0) {
				/* recur_perm_error_flag needs to be a global variable.
				  * Otherwise, since this function calls itself
				  * recursivelly, the flag would be reset upon every
				  * new call, without preserving the error code, which
				  * is what the flag is aimed to do. On the other side,
				  * if I use a local static variable for this flag, it
				  * will never drop the error value, and all subsequent
				  * calls to the function will allways return error
				  * (even if there's no actual error) */
				recur_perm_error_flag = 1;
				fprintf(stderr, _("%s: Permission denied\n"), dirpath);
			}

			recur_perm_check(dirpath);
		}
	}

	closedir(dir);

	if (recur_perm_error_flag)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

/* Check whether the current user has enough permissions (write, execute)
 * to modify the contents of the parent directory of 'file'. 'file' needs
 * to be an absolute path. Returns zero if yes and one if no. Useful to
 * know if a file can be removed from or copied into the parent. In case
 * FILE is a directory, the function checks all its subdirectories for
 * appropriate permissions, including the immutable bit */
static int
wx_parent_check(char *file)
{
	struct stat attr;
	int exit_status = -1, ret = -1;
	size_t file_len = strlen(file);

	if (file[file_len - 1] == '/')
		file[file_len - 1] = '\0';

	if (lstat(file, &attr) == -1) {
		fprintf(stderr, _("%s: No such file or directory\n"), file);
		return EXIT_FAILURE;
	}

	char *parent = strbfrlst(file, '/');
	if (!parent) {
		/* strbfrlst() will return NULL if file's parent is root (/),
		 * simply because in this case there's nothing before the last
		 * slash. So, check if file's parent dir is root */
		if (file[0] == '/' && strcntchr(file + 1, '/') == -1) {
			parent = (char *)xnmalloc(2, sizeof(char));
			parent[0] = '/';
			parent[1] = '\0';
		} else {
			fprintf(stderr, _("%s: %s: Error getting parent directory\n"),
					PROGRAM_NAME, file);
			return EXIT_FAILURE;
		}
	}

	switch (attr.st_mode & S_IFMT) {
	case S_IFDIR:
		ret = check_immutable_bit(file);

		if (ret == -1) {
			/* Error message is printed by check_immutable_bit() itself */
			exit_status = EXIT_FAILURE;
		} else if (ret == 1) {
			fprintf(stderr, _("%s: Directory is immutable\n"), file);
			exit_status = EXIT_FAILURE;
		} else if (access(parent, W_OK | X_OK) == 0) {
		/* Check the parent for appropriate permissions */
			int files_n = count_dir(parent, NO_CPOP);

			if (files_n > 2) {
				/* I manually check here subdir because recur_perm_check()
				 * will only check the contents of subdir, but not subdir
				 * itself */
				/* If the parent is ok and not empty, check subdir */
				if (access(file, W_OK | X_OK) == 0) {
					/* If subdir is ok and not empty, recusivelly check
					 * subdir */
					files_n = count_dir(file, NO_CPOP);

					if (files_n > 2) {
						/* Reset the recur_perm_check() error flag. See
						 * the note in the function block. */
						recur_perm_error_flag = 0;

						if (recur_perm_check(file) == 0) {
							exit_status = EXIT_SUCCESS;
						} else {
							/* recur_perm_check itself will print the
							 * error messages */
							exit_status = EXIT_FAILURE;
						}
					} else { /* Subdir is ok and empty */
						exit_status = EXIT_SUCCESS;
					}
				} else { /* No permission for subdir */
					fprintf(stderr, _("%s: Permission denied\n"),
					    file);
					exit_status = EXIT_FAILURE;
				}
			} else {
				exit_status = EXIT_SUCCESS;
			}
		} else { /* No permission for parent */
			fprintf(stderr, _("%s: Permission denied\n"), parent);
			exit_status = EXIT_FAILURE;
		}
		break;

	case S_IFREG:
		ret = check_immutable_bit(file);

		if (ret == -1) {
			/* Error message is printed by check_immutable_bit()
			 * itself */
			exit_status = EXIT_FAILURE;
		} else if (ret == 1) {
			fprintf(stderr, _("%s: File is immutable\n"), file);
			exit_status = EXIT_FAILURE;
		} else {
			if (parent) {
				if (access(parent, W_OK | X_OK) == 0) {
					exit_status = EXIT_SUCCESS;
				} else {
					fprintf(stderr, _("%s: Permission denied\n"), parent);
					exit_status = EXIT_FAILURE;
				}
			}
		}

		break;

	case S_IFSOCK:
	case S_IFIFO:
	case S_IFLNK:
		/* Symlinks, sockets and pipes do not support immutable bit */
		if (parent) {
			if (access(parent, W_OK | X_OK) == 0) {
				exit_status = EXIT_SUCCESS;
			} else {
				fprintf(stderr, _("%s: Permission denied\n"), parent);
				exit_status = EXIT_FAILURE;
			}
		}
		break;

	/* DO NOT TRASH BLOCK AND CHAR DEVICES */
	default:
		fprintf(stderr, _("%s: trash: %s (%s): Unsupported file type\n"),
		    PROGRAM_NAME, file, S_ISBLK(attr.st_mode) ? "Block device"
		    : (S_ISCHR(attr.st_mode) ? "Character device"
		    : "Unknown file type"));
		exit_status = EXIT_FAILURE;
		break;
	}

	if (parent)
		free(parent);

	return exit_status;
}

static int
trash_clear(void)
{
	struct dirent **trash_files = (struct dirent **)NULL;
	int files_n = -1, exit_status = EXIT_SUCCESS;

	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: trash: '%s': %s\n", PROGRAM_NAME,
		    trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	files_n = scandir(trash_files_dir, &trash_files, skip_files, xalphasort);

	if (!files_n) {
		puts(_("trash: There are no trashed files"));
		return EXIT_SUCCESS;
	}

	size_t i;
	for (i = 0; i < (size_t)files_n; i++) {
		size_t info_file_len = strlen(trash_files[i]->d_name) + 11;
		char *info_file = (char *)xnmalloc(info_file_len, sizeof(char));
		sprintf(info_file, "%s.trashinfo", trash_files[i]->d_name);

		char *file1 = (char *)NULL;
		file1 = (char *)xnmalloc(strlen(trash_files_dir) +
					strlen(trash_files[i]->d_name) + 2, sizeof(char));

		sprintf(file1, "%s/%s", trash_files_dir, trash_files[i]->d_name);

		char *file2 = (char *)NULL;
		file2 = (char *)xnmalloc(strlen(trash_info_dir) +
					strlen(info_file) + 2, sizeof(char));
		sprintf(file2, "%s/%s", trash_info_dir, info_file);

		char *tmp_cmd[] = {"rm", "-r", file1, file2, NULL};
		int ret = launch_execve(tmp_cmd, FOREGROUND, E_NOFLAG);

		free(file1);
		free(file2);

		if (ret != EXIT_SUCCESS) {
			fprintf(stderr, _("%s: trash: %s: Error removing "
				"trashed file\n"), PROGRAM_NAME, trash_files[i]->d_name);
			exit_status = EXIT_FAILURE;
			/* If there is at least one error, return error */
		}

		free(info_file);
		free(trash_files[i]);
	}

	free(trash_files);

	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: trash: '%s': %s\n", PROGRAM_NAME,
		    workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	return exit_status;
}

static int
trash_element(const char *suffix, struct tm *tm, char *file)
{
	/* Check file's existence */
	struct stat file_attrib;

	if (lstat(file, &file_attrib) == -1) {
		fprintf(stderr, "%s: trash: %s: %s\n", PROGRAM_NAME, file,
		    strerror(errno));
		return EXIT_FAILURE;
	}

	/* Check whether the user has enough permissions to remove file */
	/* If relative path */
	char full_path[PATH_MAX] = "";

	if (*file != '/') {
		/* Construct absolute path for file */
		snprintf(full_path, PATH_MAX, "%s/%s", workspaces[cur_ws].path, file);
		if (wx_parent_check(full_path) != 0)
			return EXIT_FAILURE;
	} else {
		if (wx_parent_check(file) != 0)
			/* If absolute path */
			return EXIT_FAILURE;
	}

	int ret = -1;

	/* Create the trashed file name: orig_filename.suffix, where SUFFIX is
	 * current date and time */
	char *filename = (char *)NULL;
	if (*file != '/') /* If relative path */
		filename = straftlst(full_path, '/');
	else /* If absolute path */
		filename = straftlst(file, '/');

	if (!filename) {
		fprintf(stderr, _("%s: trash: %s: Error getting file name\n"),
		    PROGRAM_NAME, file);
		return EXIT_FAILURE;
	}
	/* If the length of the trashed file name (orig_filename.suffix) is
	 * longer than NAME_MAX (255), trim the original filename, so that
	 * (original_filename_len + 1 (dot) + suffix_len) won't be longer
	 * than NAME_MAX */
	size_t filename_len = strlen(filename), suffix_len = strlen(suffix);
	int size = (int)(filename_len + suffix_len + 11) - NAME_MAX;
	/* len = filename.suffix.trashinfo */

	if (size > 0) {
		/* If SIZE is a positive value, that is, the trashed file name
		 * exceeds NAME_MAX by SIZE bytes, reduce the original file name
		 * SIZE bytes. Terminate the original file name (FILENAME) with
		 * a tilde (~), to let the user know it is trimmed */
		filename[filename_len - (size_t)size - 1] = '~';
		filename[filename_len - (size_t)size] = '\0';
	}

	/* 2 = dot + null byte */
	size_t file_suffix_len = filename_len + suffix_len + 2;
	char *file_suffix = (char *)xnmalloc(file_suffix_len, sizeof(char));
	/* No need for memset. sprintf adds the terminating null byte by
	 * itself */
	sprintf(file_suffix, "%s.%s", filename, suffix);

	/* Copy the original file into the trash files directory */
	char *dest = (char *)NULL;
	dest = (char *)xnmalloc(strlen(trash_files_dir) + strlen(file_suffix) + 2,
							sizeof(char));
	sprintf(dest, "%s/%s", trash_files_dir, file_suffix);

	char *tmp_cmd[] = {"cp", "-a", file, dest, NULL};

	free(filename);

	ret = launch_execve(tmp_cmd, FOREGROUND, E_NOFLAG);
	free(dest);
	dest = (char *)NULL;

	if (ret != EXIT_SUCCESS) {
		fprintf(stderr, _("%s: trash: %s: Failed copying file to "
			"Trash\n"), PROGRAM_NAME, file);
		free(file_suffix);
		return EXIT_FAILURE;
	}

	/* Generate the info file */
	size_t info_file_len = strlen(trash_info_dir) + strlen(file_suffix) + 12;

	char *info_file = (char *)xnmalloc(info_file_len, sizeof(char));
	sprintf(info_file, "%s/%s.trashinfo", trash_info_dir, file_suffix);

	FILE *info_fp = fopen(info_file, "w");
	if (!info_fp) { /* If error creating the info file */
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, info_file,
		    strerror(errno));
		/* Remove the trash file */
		char *trash_file = (char *)NULL;
		trash_file = (char *)xnmalloc(strlen(trash_files_dir)
						+ strlen(file_suffix) + 2, sizeof(char));
		sprintf(trash_file, "%s/%s", trash_files_dir, file_suffix);

		char *tmp_cmd2[] = {"rm", "-r", trash_file, NULL};
		ret = launch_execve(tmp_cmd2, FOREGROUND, E_NOFLAG);

		free(trash_file);

		if (ret != EXIT_SUCCESS)
			fprintf(stderr, _("%s: trash: %s/%s: Failed removing trash "
				"file\nTry removing it manually\n"), PROGRAM_NAME,
			    trash_files_dir, file_suffix);

		free(file_suffix);
		free(info_file);

		return EXIT_FAILURE;
	}

	else { /* If info file was generated successfully */
		/* Encode path to URL format (RF 2396) */
		char *url_str = (char *)NULL;

		if (*file != '/')
			url_str = url_encode(full_path);
		else
			url_str = url_encode(file);

		if (!url_str) {
			fprintf(stderr, _("%s: trash: %s: Failed encoding path\n"),
			    PROGRAM_NAME, file);

			fclose(info_fp);
			free(info_file);
			free(file_suffix);
			return EXIT_FAILURE;
		}

		fprintf(info_fp,
		    "[Trash Info]\nPath=%s\nDeletionDate=%d-%d-%dT%d:%d:%d\n",
		    url_str, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		    tm->tm_hour, tm->tm_min, tm->tm_sec);
		fclose(info_fp);
		free(url_str);
		url_str = (char *)NULL;
	}

	/* Remove the file to be trashed */
	char *tmp_cmd3[] = {"rm", "-r", file, NULL};
	ret = launch_execve(tmp_cmd3, FOREGROUND, E_NOFLAG);

	/* If remove fails, remove trash and info files */
	if (ret != EXIT_SUCCESS) {
		fprintf(stderr, _("%s: trash: %s: Failed removing file\n"),
		    PROGRAM_NAME, file);
		char *trash_file = (char *)NULL;
		trash_file = (char *)xnmalloc(strlen(trash_files_dir)
						+ strlen(file_suffix) + 2, sizeof(char));
		sprintf(trash_file, "%s/%s", trash_files_dir, file_suffix);

		char *tmp_cmd4[] = {"rm", "-r", trash_file, info_file, NULL};
		ret = launch_execve(tmp_cmd4, FOREGROUND, E_NOFLAG);
		free(trash_file);

		if (ret != EXIT_SUCCESS) {
			fprintf(stderr, _("%s: trash: Failed removing temporary "
					"files from Trash.\nTry removing them manually\n"),
					PROGRAM_NAME);
			free(file_suffix);
			free(info_file);
			return EXIT_FAILURE;
		}
	}

	free(info_file);
	free(file_suffix);
	return EXIT_SUCCESS;
}

static int
remove_file_from_trash(char *name)
{
	char rm_file[PATH_MAX], rm_info[PATH_MAX];
	snprintf(rm_file, PATH_MAX, "%s/%s", trash_files_dir, name);
	snprintf(rm_info, PATH_MAX, "%s/%s.trashinfo", trash_info_dir, name);
	char *cmd[] = {"rm", "-r", rm_file, rm_info, NULL};
	return launch_execve(cmd, FOREGROUND, E_NOFLAG);
}

static int
remove_from_trash(char **args)
{
	int exit_status = EXIT_SUCCESS;

	/* Remove from trash files passed as parameters */
	size_t i = 2;
	if (args[2]) {
		for (; args[i]; i++) {
			char *d = (char *)NULL;
			if (strchr(args[i], '\\'))
				d = dequote_str(args[i], 0);
			if (remove_file_from_trash(d ? d : args[i]) != EXIT_SUCCESS)
				exit_status = EXIT_FAILURE;
			free(d);
		}
		return exit_status;
	}

	/* No parameters */

	/* List trashed files */
	/* Change CWD to the trash directory. Otherwise, scandir() will fail */
	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: trash: '%s': %s\n", PROGRAM_NAME,
		    trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	struct dirent **trash_files = (struct dirent **)NULL;
	int files_n = scandir(trash_files_dir, &trash_files,
					skip_files, (unicode) ? alphasort : (case_sensitive)
					? xalphasort : alphasort_insensitive);

	if (files_n) {
		printf(_("%sTrashed files%s\n\n"), BOLD, df_c);
		for (i = 0; i < (size_t)files_n; i++) {
			colors_list(trash_files[i]->d_name, (int)i + 1, NO_PAD,
			    PRINT_NEWLINE);
		}
	} else {
		puts(_("trash: No trashed files"));
		/* Restore CWD and return */
		if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
			_err(0, NOPRINT_PROMPT, "%s: trash: '%s': %s\n",
			    PROGRAM_NAME, workspaces[cur_ws].path, strerror(errno));
		}

		return EXIT_SUCCESS;
	}

	/* Restore CWD and continue */
	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: trash: '%s': %s\n", PROGRAM_NAME,
		    workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Get user input */
	printf(_("\n%sEnter 'q' to quit.\n"), df_c);
	char *line = (char *)NULL, **rm_elements = (char **)NULL;

	while (!line)
		line = rl_no_hist(_("File(s) to be removed (ex: 1 2-6, or *): "));

	rm_elements = get_substr(line, ' ');
	free(line);

	if (!rm_elements)
		return EXIT_FAILURE;

	/* Remove files */
	int ret = -1;

	/* First check for exit, wildcard, and non-number args */
	for (i = 0; rm_elements[i]; i++) {
		/* Quit */
		if (strcmp(rm_elements[i], "q") == 0) {
			size_t j;
			for (j = 0; rm_elements[j]; j++)
				free(rm_elements[j]);
			free(rm_elements);

			for (j = 0; j < (size_t)files_n; j++)
				free(trash_files[j]);
			free(trash_files);

			return exit_status;
		}

		/* Asterisk */
		else if (strcmp(rm_elements[i], "*") == 0) {
			size_t j;
			for (j = 0; j < (size_t)files_n; j++) {
				ret = remove_file_from_trash(trash_files[j]->d_name);
				if (ret != EXIT_SUCCESS) {
					fprintf(stderr, _("%s: trash: Error trashing %s\n"),
					    PROGRAM_NAME, trash_files[j]->d_name);
					exit_status = EXIT_FAILURE;
				}

				free(trash_files[j]);
			}

			free(trash_files);

			for (j = 0; rm_elements[j]; j++)
				free(rm_elements[j]);
			free(rm_elements);

			return exit_status;
		}

		else if (!is_number(rm_elements[i])) {

			fprintf(stderr, _("%s: trash: %s: Invalid ELN\n"),
			    PROGRAM_NAME, rm_elements[i]);
			exit_status = EXIT_FAILURE;

			size_t j;
			for (j = 0; rm_elements[j]; j++)
				free(rm_elements[j]);
			free(rm_elements);

			for (j = 0; j < (size_t)files_n; j++)
				free(trash_files[j]);
			free(trash_files);

			return exit_status;
		}
	}

	/* If all args are numbers, and neither 'q' nor wildcard */
	for (i = 0; rm_elements[i]; i++) {
		int rm_num = atoi(rm_elements[i]);

		if (rm_num <= 0 || rm_num > files_n) {
			fprintf(stderr, _("%s: trash: %d: Invalid ELN\n"),
			    PROGRAM_NAME, rm_num);
			free(rm_elements[i]);
			exit_status = EXIT_FAILURE;
			continue;
		}

		ret = remove_file_from_trash(trash_files[rm_num - 1]->d_name);
		if (ret != EXIT_SUCCESS) {
			fprintf(stderr, _("%s: trash: Error trashing %s\n"),
			    PROGRAM_NAME, trash_files[rm_num - 1]->d_name);
			exit_status = EXIT_FAILURE;
		}

		free(rm_elements[i]);
	}

	free(rm_elements);

	for (i = 0; i < (size_t)files_n; i++)
		free(trash_files[i]);
	free(trash_files);

	return exit_status;
}

static int
untrash_element(char *file)
{
	if (!file)
		return EXIT_FAILURE;

	char undel_file[PATH_MAX], undel_info[PATH_MAX];
	snprintf(undel_file, PATH_MAX, "%s/%s", trash_files_dir, file);
	snprintf(undel_info, PATH_MAX, "%s/%s.trashinfo", trash_info_dir,
	    file);

	FILE *info_fp;
	info_fp = fopen(undel_info, "r");
	if (!info_fp) {
		fprintf(stderr, _("%s: undel: Info file for '%s' not found. "
				"Try restoring the file manually\n"), PROGRAM_NAME, file);
		return EXIT_FAILURE;
	}

	char *orig_path = (char *)NULL;
	/* The max length for line is Path=(5) + PATH_MAX + \n(1) */
	char line[PATH_MAX + 6];
	memset(line, '\0', PATH_MAX + 6);

	while (fgets(line, (int)sizeof(line), info_fp)) {
		if (strncmp(line, "Path=", 5) == 0) {
			char *p = strchr(line, '=');
			if (!p || !*(++p))
				break;
			orig_path = savestring(p, strlen(p));
		}
	}

	fclose(info_fp);

	/* If original path is NULL or empty, return error */
	if (!orig_path)
		return EXIT_FAILURE;

	if (*orig_path == '\0') {
		free(orig_path);
		return EXIT_FAILURE;
	}

	/* Remove new line char from original path, if any */
	size_t orig_path_len = strlen(orig_path);
	if (orig_path[orig_path_len - 1] == '\n')
		orig_path[orig_path_len - 1] = '\0';

	/* Decode original path's URL format */
	char *url_decoded = url_decode(orig_path);

	if (!url_decoded) {
		fprintf(stderr, _("%s: undel: %s: Failed decoding path\n"),
		    PROGRAM_NAME, orig_path);
		free(orig_path);
		return EXIT_FAILURE;
	}

	free(orig_path);
	orig_path = (char *)NULL;

	/* Check existence and permissions of parent directory */
	char *parent = (char *)NULL;
	parent = strbfrlst(url_decoded, '/');

	if (!parent) {
		/* strbfrlst() returns NULL is file's parent is root (simply
		 * because there's nothing before last slash in this case).
		 * So, check if file's parent is root. Else returns */
		if (url_decoded[0] == '/' && strcntchr(url_decoded + 1, '/') == -1) {
			parent = (char *)xnmalloc(2, sizeof(char));
			parent[0] = '/';
			parent[1] = '\0';
		} else {
			free(url_decoded);
			return EXIT_FAILURE;
		}
	}

	if (access(parent, F_OK) != 0) {
		fprintf(stderr, _("%s: undel: %s: No such file or directory\n"),
				PROGRAM_NAME, parent);
		free(parent);
		free(url_decoded);
		return EXIT_FAILURE;
	}

	if (access(parent, X_OK | W_OK) != 0) {
		fprintf(stderr, _("%s: undel: %s: Permission denied\n"),
				PROGRAM_NAME, parent);
		free(parent);
		free(url_decoded);
		return EXIT_FAILURE;
	}

	free(parent);

	char *tmp_cmd[] = {"cp", "-a", undel_file, url_decoded, NULL};
	int ret = -1;
	ret = launch_execve(tmp_cmd, FOREGROUND, E_NOFLAG);
	free(url_decoded);

	if (ret == EXIT_SUCCESS) {
		char *tmp_cmd2[] = {"rm", "-r", undel_file, undel_info, NULL};
		ret = launch_execve(tmp_cmd2, FOREGROUND, E_NOFLAG);

		if (ret != EXIT_SUCCESS) {
			fprintf(stderr, _("%s: undel: %s: Failed removing info file\n"),
					PROGRAM_NAME, undel_info);
			return EXIT_FAILURE;
		} else {
			return EXIT_SUCCESS;
		}
	} else {
		fprintf(stderr, _("%s: undel: %s: Failed restoring trashed file\n"),
				PROGRAM_NAME, undel_file);
		return EXIT_FAILURE;
	}

	return EXIT_FAILURE; /* Never reached */
}

int
untrash_function(char **comm)
{
	if (xargs.stealth_mode == 1) {
		printf("%s: trash: %s\n", PROGRAM_NAME, STEALTH_DISABLED);
		return EXIT_SUCCESS;
	}

	if (!comm)
		return EXIT_FAILURE;

	if (!trash_ok) {
		fprintf(stderr, _("%s: Trash function disabled\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	int exit_status = EXIT_SUCCESS;

	if (comm[1] && *comm[1] != '*' && strcmp(comm[1], "a") != 0
	&& strcmp(comm[1], "all") != 0) {
		size_t j;
		for (j = 1; comm[j]; j++) {
			char *d = (char *)NULL;
			if (strchr(comm[j], '\\'))
				d = dequote_str(comm[j], 0);
			if (untrash_element(d ? d : comm[j]) != 0)
				exit_status = EXIT_FAILURE;
			free(d);
		}
		return exit_status;
	}

	/* Change CWD to the trash directory to make scandir() work */
	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: undel: '%s': %s\n", PROGRAM_NAME,
		    trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Get trashed files */
	struct dirent **trash_files = (struct dirent **)NULL;
	int trash_files_n = scandir(trash_files_dir, &trash_files,
	    skip_files, (unicode) ? alphasort : (case_sensitive) ? xalphasort
					 : alphasort_insensitive);
	if (trash_files_n <= 0) {
		puts(_("trash: No trashed files"));

		if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
			_err(0, NOPRINT_PROMPT, "%s: undel: '%s': %s\n",
			    PROGRAM_NAME, workspaces[cur_ws].path, strerror(errno));
			return EXIT_FAILURE;
		}

		return EXIT_SUCCESS;
	}

	/* if "undel all" (or "u a" or "u *") */
	if (comm[1] && (strcmp(comm[1], "*") == 0 || strcmp(comm[1], "a") == 0
	|| strcmp(comm[1], "all") == 0)) {
		size_t j;
		for (j = 0; j < (size_t)trash_files_n; j++) {
			if (untrash_element(trash_files[j]->d_name) != 0)
				exit_status = EXIT_FAILURE;
			free(trash_files[j]);
		}
		free(trash_files);

		if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
			_err(0, NOPRINT_PROMPT, "%s: undel: '%s': %s\n",
			    PROGRAM_NAME, workspaces[cur_ws].path, strerror(errno));
			return EXIT_FAILURE;
		}

		return exit_status;
	}

	/* List trashed files */
	printf(_("%sTrashed files%s\n\n"), BOLD, df_c);
	size_t i;

	for (i = 0; i < (size_t)trash_files_n; i++)
		colors_list(trash_files[i]->d_name, (int)i + 1, NO_PAD,
		    PRINT_NEWLINE);

	/* Go back to previous path */
	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: undel: '%s': %s\n", PROGRAM_NAME,
		    workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Get user input */
	printf(_("\n%sEnter 'q' to quit.\n"), df_c);
	int undel_n = 0;
	char *line = (char *)NULL, **undel_elements = (char **)NULL;
	while (!line)
		line = rl_no_hist(_("File(s) to be undeleted (ex: 1 2-6, or *): "));

	undel_elements = get_substr(line, ' ');
	free(line);
	line = (char *)NULL;
	if (undel_elements) {
		for (i = 0; undel_elements[i]; i++)
			undel_n++;
	} else {
		return EXIT_FAILURE;
	}

	/* First check for quit, *, and non-number args */
	int free_and_return = 0;

	for (i = 0; i < (size_t)undel_n; i++) {
		if (strcmp(undel_elements[i], "q") == 0) {
			free_and_return = 1;
		} else if (strcmp(undel_elements[i], "*") == 0) {
			size_t j;

			for (j = 0; j < (size_t)trash_files_n; j++)
				if (untrash_element(trash_files[j]->d_name) != 0)
					exit_status = EXIT_FAILURE;

			free_and_return = 1;
		} else if (!is_number(undel_elements[i])) {
			fprintf(stderr, _("undel: %s: Invalid ELN\n"), undel_elements[i]);
			exit_status = EXIT_FAILURE;
			free_and_return = 1;
		}
	}

	/* Free and return if any of the above conditions is true */
	if (free_and_return) {
		size_t j = 0;
		for (j = 0; j < (size_t)undel_n; j++)
			free(undel_elements[j]);
		free(undel_elements);

		for (j = 0; j < (size_t)trash_files_n; j++)
			free(trash_files[j]);
		free(trash_files);

		return exit_status;
	}

	/* Undelete trashed files */
	for (i = 0; i < (size_t)undel_n; i++) {
		int undel_num = atoi(undel_elements[i]);

		if (undel_num <= 0 || undel_num > trash_files_n) {
			fprintf(stderr, _("%s: undel: %d: Invalid ELN\n"),
					PROGRAM_NAME, undel_num);
			free(undel_elements[i]);
			continue;
		}

		/* If valid ELN */
		if (untrash_element(trash_files[undel_num - 1]->d_name) != 0)
			exit_status = EXIT_FAILURE;
		free(undel_elements[i]);
	}

	free(undel_elements);

	/* Free trashed files list */
	for (i = 0; i < (size_t)trash_files_n; i++)
		free(trash_files[i]);
	free(trash_files);

	/* If some trashed file still remains, reload the undel screen */
	trash_n = count_dir(trash_files_dir, NO_CPOP);

	if (trash_n <= 2)
		trash_n = 0;

	if (trash_n)
		untrash_function(comm);

	return exit_status;
}

/* List files currently in the trash can */
static int
list_trashed_files(void)
{
	if (xchdir(trash_files_dir, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: trash: %s: %s\n",
		    PROGRAM_NAME, trash_files_dir, strerror(errno));
		return EXIT_FAILURE;
	}

	struct dirent **trash_files = (struct dirent **)NULL;
	int files_n = scandir(trash_files_dir, &trash_files,
			skip_files, (unicode) ? alphasort : (case_sensitive)
			? xalphasort : alphasort_insensitive);

	if (files_n == -1) {
		fprintf(stderr, "%s: trash: %s\n", PROGRAM_NAME, strerror(errno));
		return EXIT_FAILURE;
	}
	if (files_n <= 0) {
		puts(_("trash: No trashed files"));
		return (-1);
	}

	size_t i;
	for (i = 0; i < (size_t)files_n; i++) {
		colors_list(trash_files[i]->d_name, (int)i + 1, NO_PAD, PRINT_NEWLINE);
		free(trash_files[i]);
	}
	free(trash_files);

	if (xchdir(workspaces[cur_ws].path, NO_TITLE) == -1) {
		_err(0, NOPRINT_PROMPT, "%s: trash: '%s': %s\n",
		    PROGRAM_NAME, workspaces[cur_ws].path, strerror(errno));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

/* Make sure we are trashing a valid file */
static int
check_trash_file(char *deq_file)
{
	char tmp_cmd[PATH_MAX];
	if (*deq_file == '/') /* If absolute path */
		strcpy(tmp_cmd, deq_file);
	else /* If relative path, add path to check against TRASH_DIR */
		snprintf(tmp_cmd, PATH_MAX, "%s/%s", workspaces[cur_ws].path, deq_file);

	/* Do not trash any of the parent directories of TRASH_DIR */
	if (strncmp(tmp_cmd, trash_dir, strlen(tmp_cmd)) == 0) {
		fprintf(stderr, _("trash: Cannot trash '%s'\n"), tmp_cmd);
		return EXIT_FAILURE;
	}

	/* Do no trash TRASH_DIR itself nor anything inside it (trashed files) */
	if (strncmp(tmp_cmd, trash_dir, strlen(trash_dir)) == 0) {
		puts(_("trash: Use 'trash del' to remove trashed files"));
		return EXIT_FAILURE;
	}

	struct stat a;
	if (lstat(deq_file, &a) == -1) {
		fprintf(stderr, _("trash: %s: %s\n"), deq_file, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Do not trash block or character devices */
	if (S_ISBLK(a.st_mode) || S_ISCHR(a.st_mode)) {
		fprintf(stderr, _("trash: %s: Cannot trash a %s device\n"), deq_file,
			S_ISCHR(a.st_mode) ? "character" : "block");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

/* Trash files passed as arguments to the trash command */
static int
trash_files_args(char **args)
{
	time_t rawtime = time(NULL);
	struct tm tm;
	localtime_r(&rawtime, &tm);
	char *suffix = gen_date_suffix(tm);
	if (!suffix)
		return EXIT_FAILURE;

	int exit_status = EXIT_SUCCESS;
	size_t i;
	for (i = 1; args[i]; i++) {
		char *deq_file = dequote_str(args[i], 0);
		/* Make sure we are trashing a valid file */
		if (check_trash_file(deq_file) == EXIT_FAILURE) {
			exit_status = EXIT_FAILURE;
			free(deq_file);
			continue;
		}

		/* Once here, everything is fine: trash the file */
		exit_status = trash_element(suffix, &tm, deq_file);
		free(deq_file);
	}

	free(suffix);
	return exit_status;
}

int
trash_function(char **args)
{
	if (xargs.stealth_mode == 1) {
		printf("%s: trash: %s\n", PROGRAM_NAME, STEALTH_DISABLED);
		return EXIT_SUCCESS;
	}

	if (!args)
		return EXIT_FAILURE;

	if (!trash_ok || !config_ok) {
		fprintf(stderr, _("%s: Trash function disabled\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	/* List trashed files ('tr' or 'tr ls') */
	if (!args[1] || (*args[1] == 'l'
	&& (strcmp(args[1], "ls") == 0 || strcmp(args[1], "list") == 0))) {
		int ret = list_trashed_files();
		if (ret == -1 || ret == EXIT_SUCCESS)
			return EXIT_SUCCESS;
		return EXIT_FAILURE;
	}

	if (*args[1] == 'd' && strcmp(args[1], "del") == 0)
		return remove_from_trash(args);

	if (*args[1] == 'c' && strcmp(args[1], "clear") == 0)
		return trash_clear();
	else
		return trash_files_args(args);
}
#else
void *__skip_me_trash;
#endif /* !_NO_TRASH */
