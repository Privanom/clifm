/* exec.c -- functions controlling the execution of programs */

/*
 * This file is part of CliFM
 * 
 * Copyright (C) 2016-2021, L. Abramovich <johndoe.arch@outlook.com>
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
#ifdef __OpenBSD__
#include <sys/dirent.h>
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <readline/readline.h>
#include <limits.h>

#include "actions.h"
#ifndef _NO_ARCHIVING
#include "archives.h"
#endif
#include "aux.h"
#include "bookmarks.h"
#include "checks.h"
#include "colors.h"
#include "config.h"
#include "exec.h"
#include "file_operations.h"
#include "history.h"
#include "init.h"
#include "jump.h"
#include "keybinds.h"
#include "listing.h"
#include "mime.h"
#include "misc.h"
#include "navigation.h"
#include "profiles.h"
#include "properties.h"
#include "readline.h"
#include "remotes.h"
#include "search.h"
#include "selection.h"
#include "sort.h"
#include "strings.h"
#ifndef _NO_TRASH
#include "trash.h"
#endif
#include "messages.h"
#include "media.h"
#ifndef _NO_BLEACH
#include "name_cleaner.h"
#endif

char **_comm = (char **)NULL;

static char *
get_new_name(void)
{
	char *input = (char *)NULL;

	rl_nohist = 1;

	char m[NAME_MAX];
	snprintf(m, NAME_MAX, "Enter new name ('Ctrl-x' to quit)\n"
		"\001%s\002>\001%s\002 ", mi_c, tx_c);

	while (!input && _xrename) {
		input = readline(m);
		if (!input)
			continue;
		if (!*input || *input == ' ') {
			free(input);
			input = (char *)NULL;
			continue;
		}
	}

	rl_nohist = 0;
	return input;
}

/* Run a command via execle() and refresh the screen in case of success */
int
run_and_refresh(char **cmd)
{
	if (!cmd)
		return EXIT_FAILURE;

	log_function(cmd);

	size_t i, total_len = 0;
	for (i = 0; i <= args_n; i++)
		total_len += strlen(cmd[i]);

	char *tmp_cmd = (char *)NULL;
	tmp_cmd = (char *)xcalloc(total_len + (i + 1) + 2, sizeof(char));

	for (i = 0; i <= args_n; i++) {
		strcat(tmp_cmd, cmd[i]);
		strcat(tmp_cmd, " ");
	}

	/* If cp, and no destiny was provided, append '.' to copy source
	 * into CWD */
	if (!cmd[2] && cmd[1] && *cmd[0] == 'c' && *(cmd[0] + 1) == 'p'
	&& *(cmd[0] + 2) == ' ') {
		char *p = strrchr(cmd[1], '/');
		if (p && *(p + 1)) {
			*p = '\0';
			if (strcmp(cmd[1], ws[cur_ws].path) != 0)
				strcat(tmp_cmd, ".");
			*p = '/';
		}
	}

	if (xrename) {
		/* If we have a number here, it was not expanded by parse_input_str,
		 * and thereby, we have an invalid ELN */
		if (is_number(cmd[1])) {
			fprintf(stderr, "%s: %s: Invalid ELN\n", PROGRAM_NAME, cmd[1]);
			free(tmp_cmd);
			xrename = 0;
			return EXIT_FAILURE;
		}
		_xrename = 1;
		char *new_name = get_new_name();
		_xrename = 0;
		if (!new_name) {
			free(tmp_cmd);
			return EXIT_SUCCESS;
		}
		char *enn = (char *)NULL;
		if (!strchr(new_name, '\\')) {
			enn = escape_str(new_name);
			if (!enn) {
				free(tmp_cmd);
				fprintf(stderr, "%s: %s: Error escaping string\n", PROGRAM_NAME, new_name);
				return EXIT_FAILURE;
			}
		}
		tmp_cmd = (char *)xrealloc(tmp_cmd,
				(total_len + (i + 1) + 1 + strlen(enn ? enn : new_name))
				* sizeof(char));
		strcat(tmp_cmd, enn ? enn : new_name);

		free(new_name);
		free(enn);
	}

	int ret = launch_execle(tmp_cmd);
	free(tmp_cmd);

	if (ret != EXIT_SUCCESS)
		return EXIT_FAILURE;
	/* Error messages will be printed by launch_execve() itself */

	/* If 'rm sel' and command is successful, deselect everything */
	if (is_sel && *cmd[0] == 'r' && cmd[0][1] == 'm' && (!cmd[0][2]
	|| cmd[0][2] == ' ')) {
		int j = (int)sel_n;
		while (--j >= 0)
			free(sel_elements[j]);
		sel_n = 0;
		save_sel();
	}

#ifdef __HAIKU__
	if (autols && cmd[1] && strcmp(cmd[1], "--help") != 0
	&& strcmp(cmd[1], "--version") != 0) {
		free_dirlist();
		list_dir();
	}
#endif

	return EXIT_SUCCESS;
}

static int
run_in_foreground(pid_t pid)
{
	int status = 0;

	/* The parent process calls waitpid() on the child */
	if (waitpid(pid, &status, 0) > 0) {
		if (WIFEXITED(status) && !WEXITSTATUS(status)) {
			/* The program terminated normally and executed successfully
			 * (WEXITSTATUS(status) == 0) */
			return EXIT_SUCCESS;
		} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
			/* Program terminated normally, but returned a
			 * non-zero status. Error codes should be printed by the
			 * program itself */
			return WEXITSTATUS(status);
		} else {
			/* The program didn't terminate normally. In this case too,
			 * error codes should be printed by the program */
			return EXCRASHERR;
		}
	} else {
		/* waitpid() failed */
		fprintf(stderr, "%s: waitpid: %s\n", PROGRAM_NAME,
		    strerror(errno));
		return errno;
	}

	return EXIT_FAILURE; /* Never reached */
}

static void
run_in_background(pid_t pid)
{
	int status = 0;
	/* Keep it in the background */
	waitpid(pid, &status, WNOHANG); /* or: kill(pid, SIGCONT); */
}

/* Execute a command using the system shell (/bin/sh), which takes care
 * of special functions such as pipes and stream redirection, and special
 * chars like wildcards, quotes, and escape sequences. Use only when the
 * shell is needed; otherwise, launch_execve() should be used instead. */
int
launch_execle(const char *cmd)
{
	if (!cmd || !*cmd)
		return EXNULLERR;

	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTSTP, SIG_DFL);

	int ret = system(cmd);

	set_signals_to_ignore();

	if (WIFEXITED(ret) && !WEXITSTATUS(ret))
		return EXIT_SUCCESS;
	if (WIFEXITED(ret) && WEXITSTATUS(ret))
		return WEXITSTATUS(ret);
	return EXCRASHERR;
/*
	// Reenable SIGCHLD, in case it was disabled. Otherwise, waitpid won't
	// be able to catch error codes coming from the child
	signal(SIGCHLD, SIG_DFL);

	int status;
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: fork: %s\n", PROGRAM_NAME, strerror(errno));
		return EXFORKERR;
	} else if (pid == 0) {
		// Reenable signals only for the child, in case they were
		// disabled for the parent
		signal(SIGHUP, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);

		// Get shell base name
		char *name = strrchr(user.shell, '/');

		execl(user.shell, name ? name + 1 : user.shell, "-c", cmd, NULL);
		fprintf(stderr, "%s: %s: execle: %s\n", PROGRAM_NAME, user.shell,
		    strerror(errno));
		_exit(errno);
	}
	// Get command status
	else {
		// The parent process calls waitpid() on the child
		if (waitpid(pid, &status, 0) > 0) {
			if (WIFEXITED(status) && !WEXITSTATUS(status)) {
				// The program terminated normally and executed
				// successfully
				return EXIT_SUCCESS;
			} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
				// Either "command not found" (WEXITSTATUS(status) == 127),
				// "permission denied" (not executable) (WEXITSTATUS(status) ==
				// 126) or the program terminated normally, but returned a
				// non-zero status. These exit codes will be handled by the
				// system shell itself, since we're using here execle()
				return WEXITSTATUS(status);
			} else {
				// The program didn't terminate normally
				return EXCRASHERR;
			}
		} else {
			// Waitpid() failed
			fprintf(stderr, "%s: waitpid: %s\n", PROGRAM_NAME,
			    strerror(errno));
			return errno;
		}
	}

	// Never reached
	return EXIT_FAILURE; */
}

/* Execute a command and return the corresponding exit status. The exit
 * status could be: zero, if everything went fine, or a non-zero value
 * in case of error. The function takes as first arguement an array of
 * strings containing the command name to be executed and its arguments
 * (cmd), an integer (bg) specifying if the command should be
 * backgrounded (1) or not (0), and a flag to control file descriptors */
int
launch_execve(char **cmd, int bg, int xflags)
{
	if (!cmd)
		return EXNULLERR;

	/* Reenable SIGCHLD, in case it was disabled. Otherwise, waitpid
	 * won't be able to catch error codes coming from the child. */
	signal(SIGCHLD, SIG_DFL);

	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: fork: %s\n", PROGRAM_NAME, strerror(errno));
		return errno;
	} else if (pid == 0) {
		if (!bg) {
			/* If the program runs in the foreground, reenable signals
			 * only for the child, in case they were disabled for the
			 * parent */
			signal(SIGHUP, SIG_DFL);
			signal(SIGINT, SIG_DFL);
			signal(SIGQUIT, SIG_DFL);
			signal(SIGTERM, SIG_DFL);
		}

		if (xflags) {
			int fd = open("/dev/null", O_WRONLY, 0200);

			if (xflags & E_NOSTDIN)
				dup2(fd, STDIN_FILENO);

			if (xflags & E_NOSTDOUT)
				dup2(fd, STDOUT_FILENO);

			if (xflags & E_NOSTDERR)
				dup2(fd, STDERR_FILENO);

			close(fd);
		}

		execvp(cmd[0], cmd);
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, cmd[0],
		    strerror(errno));
		_exit(errno);
	}

	/* Get command status (pid > 0) */
	else {
		if (bg) {
			run_in_background(pid);
			return EXIT_SUCCESS;
		} else {
			return run_in_foreground(pid);
		}
	}

	/* Never reached */
	return EXIT_FAILURE;
}

static int
run_shell_cmd(char **comm)
{
	/* LOG EXTERNAL COMMANDS
	* 'no_log' will be true when running profile or prompt commands */
	if (!no_log)
		log_function(comm);

	/* PREVENT UNGRACEFUL EXIT */
	/* Prevent the user from killing the program via the 'kill',
	 * 'pkill' or 'killall' commands, from within CliFM itself.
	 * Otherwise, the program will be forcefully terminated without
	 * freeing allocated memory */
	if ((*comm[0] == 'k' || *comm[0] == 'p') && (strcmp(comm[0], "kill") == 0
	|| strcmp(comm[0], "killall") == 0 || strcmp(comm[0], "pkill") == 0)) {
		size_t i;
		for (i = 1; i <= args_n; i++) {
			if ((strcmp(comm[0], "kill") == 0 && atoi(comm[i]) == (int)own_pid)
			|| ((strcmp(comm[0], "killall") == 0 || strcmp(comm[0], "pkill") == 0)
			&& strcmp(comm[i], argv_bk[0]) == 0)) {
				fprintf(stderr, _("%s: To gracefully quit enter 'quit'\n"),
						PROGRAM_NAME);
				return EXIT_FAILURE;
			}
		}
	}

	/* CHECK WHETHER SHELL COMMANDS ARE ALLOWED */
	if (!ext_cmd_ok) {
		fprintf(stderr, _("%s: External commands are not allowed. "
				  "Run 'ext on' to enable them.\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	if (*comm[0] == *argv_bk[0] && strcmp(comm[0], argv_bk[0]) == 0) {
		fprintf(stderr, "%s: Nested instances are not allowed\n",
		    PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	/* Little export implementation. What it lacks? Command substitution */
	if (*comm[0] == 'e' && strcmp(comm[0], "export") == 0 && comm[1]) {
		char *p = strchr(comm[1], '=');
		if (p && *(p + 1)) {
			*p = '\0';
			int ret = setenv(comm[1], p + 1, 1);
			if (ret == -1) {
				fprintf(stderr, "%s: %s\n", PROGRAM_NAME, strerror(errno));
				exit_code = EXIT_FAILURE;
			} else {
				exit_code = EXIT_SUCCESS;
			}
			*p = '=';
			return exit_code;
		}
	}

	/* By making precede the command by a colon or a semicolon, the
	 * user can BYPASS CliFM parsing, expansions, and checks to be
	 * executed DIRECTLY by the system shell (execle) */
	char *first = comm[0]; 
	if (*comm[0] == ':' || *comm[0] == ';')
		first++;

	/* #### RUN THE SHELL COMMAND #### */

	/* Store the command and each argument into a single array to be
	 * executed by execle() using the system shell (/bin/sh -c) */
	char *cmd = (char *)NULL;
	size_t len = strlen(first) + 3;
	cmd = (char *)xnmalloc(len + (bg_proc ? 2 : 0), sizeof(char));
	strcpy(cmd, first);
	cmd[len - 3] = ' ';
	cmd[len - 2] = '\0';

	size_t i;
	for (i = 1; comm[i]; i++) {
		/* Dest string (cmd) is NULL terminated, just as the source
		 * string (comm[i]) */
		if (i > 1) {
			cmd[len - 3] = ' ';
			cmd[len - 2] = '\0';
		}
		len += strlen(comm[i]) + 1;
		/* LEN holds the previous size of the buffer, plus space, the
		 * ampersand character, and the new src string. The buffer is
		 * thus big enough */
		cmd = (char *)xrealloc(cmd, (len + 3 + (bg_proc ? 2 : 0))
				* sizeof(char));
		strcat(cmd, comm[i]);
	}

	/* Append final ampersand if backgrounded */
	if (bg_proc) {
		cmd[len - 3] = ' ';
		cmd[len - 2] = '&';
		cmd[len - 1] = '\0';
	} else {
		cmd[len - 3] = '\0';
	}

	int exit_status = launch_execle(cmd);
	free(cmd);

	/* Reload the list of available commands in PATH for TAB completion.
	 * Why? If this list is not updated, whenever some new program is
	 * installed, renamed, or removed from some of the paths in PATH
	 * while in CliFM, this latter needs to be restarted in order
	 * to be able to recognize the new program for TAB completion */
	int j;
	if (bin_commands) {
		j = (int)path_progsn;
		while (--j >= 0)
			free(bin_commands[j]);
		free(bin_commands);
		bin_commands = (char **)NULL;
	}

	if (paths) {
		j = (int)path_n;
		while (--j >= 0)
			free(paths[j]);
	}

	path_n = (size_t)get_path_env();
	get_path_programs();

	return exit_status;
}

static int
set_max_files(char **args)
{
	if (!args[1]) { /* Inform about the current value */
		if (max_files == -1)
			puts(_("Max files: unset"));
		else
			printf(_("Max files: %d\n"), max_files);
		return EXIT_SUCCESS;
	}

	if (*args[1] == '-' && strcmp(args[1], "--help") == 0) {
		puts(_(MF_USAGE));
		return EXIT_SUCCESS;
	}

	if (*args[1] == 'u' && strcmp(args[1], "unset") == 0) {
		max_files = -1;
		puts(_("Max files: unset"));
		return EXIT_SUCCESS;
	}

	if (*args[1] == '0' && !args[1][1]) {
		max_files = 0;
		printf(_("Max files set to %d\n"), max_files);
		return EXIT_SUCCESS;
	}

	long inum = strtol(args[1], NULL, 10);
	if (inum == LONG_MAX || inum == LONG_MIN || inum <= 0) {
		fprintf(stderr, _("%s: %s: Invalid number\n"), PROGRAM_NAME, args[1]);
		return (exit_code = EXIT_FAILURE);
	}

	max_files = (int)inum;
	printf(_("Max files set to %d\n"), max_files);
	return EXIT_SUCCESS;
}


/* Take the command entered by the user, already splitted into substrings
 * by parse_input_str(), and call the corresponding function. Return zero
 * in case of success and one in case of error */
int
exec_cmd(char **comm)
{
	fputs(df_c, stdout);
	/* Exit flag. exit_code is zero (sucess) by default. In case of error
	 * in any of the functions below, it will be set to one (failure).
	 * This will be the value returned by this function. Used by the \z
	 * escape code in the prompt to print the exit status of the last
	 * executed command */
	int old_exit_code = exit_code;
	exit_code = EXIT_SUCCESS;

	if (*comm[0] == '#' && access(comm[0], F_OK) != 0)
		return exit_code;

				/* ##########################
				 * #     	AUTOJUMP	    #
				 * ########################## */

/*	if (autojump) {
		exit_code = run_autojump(comm);
		if (exit_code != -1)
			return exit_code;
	} */

	/* Warn when using the ',' keyword and there's no pinned file */
	int k = (int)args_n + 1;
	while (--k >= 0) {
		if (*comm[k] == ',' && !comm[k][1]) {
			fprintf(stderr, _("%s: No pinned file\n"), PROGRAM_NAME);
			return (exit_code = EXIT_FAILURE);
		}
	}

	/* User defined actions */
	if (actions_n) {
		int i = (int)actions_n;
		while (--i >= 0) {
			if (*comm[0] == *usr_actions[i].name
			&& strcmp(comm[0], usr_actions[i].name) == 0)
				return (exit_code = run_action(usr_actions[i].value, comm));
		}
	}

	/* User defined variables */
	if (flags & IS_USRVAR_DEF) {
		flags &= ~IS_USRVAR_DEF;
		return (exit_code = create_usr_var(comm[0]));
	}

	if (comm[0][0] == ';' || comm[0][0] == ':') {
		if (!comm[0][1]) {
			/* If just ":" or ";", launch the default shell */
			char *cmd[] = {user.shell, NULL};
			if (launch_execve(cmd, FOREGROUND, E_NOFLAG) != EXIT_SUCCESS)
				exit_code = EXIT_FAILURE;
			return exit_code;
		} else if (comm[0][1] == ';' || comm[0][1] == ':') {
		/* If double semi colon or colon (or ";:" or ":;") */
			fprintf(stderr, _("%s: '%s': Syntax error\n"), PROGRAM_NAME, comm[0]);
			return (exit_code = EXIT_FAILURE);
		}
	}

				/* ###############################
				 * #    AUTOCD & AUTO-OPEN (1)   #
				 * ############################### */

	char *deq_str = (char *)NULL;
	if (autocd || auto_open) {
		/* Expand tilde */
		if (*comm[0] == '~') {
			char *exp_path = tilde_expand(comm[0]);
			if (exp_path) {
				comm[0] = (char *)xrealloc(comm[0], (strlen(exp_path) + 1) * sizeof(char));
				strcpy(comm[0], exp_path);
				free(exp_path);
			}
		}

		/* Deescape the string (only if file name) */
		if (strchr(comm[0], '\\')) {
			deq_str = dequote_str(comm[0], 0);
/*			if (deq_str) {
				if (access(deq_str, F_OK) == 0)
					strcpy(comm[0], deq_str);
				free(deq_str);
			} */
		}
	}

	/* Only autocd or auto-open here if not absolute path and if there
	 * is no second argument or if second argument is "&" */
	if (*comm[0] != '/' && (autocd || auto_open) && (!comm[1]
	|| (*comm[1] == '&' && !comm[1][1]))) {
		char *tmp = deq_str ? deq_str : comm[0];
		size_t tmp_len = strlen(tmp);
		if (tmp[tmp_len - 1] == '/')
			tmp[tmp_len - 1] = '\0';

		if (autocd && cdpath_n && !comm[1]
		&& cd_function(comm[0], CD_NO_PRINT_ERROR) == EXIT_SUCCESS) {
			free(deq_str);
			return EXIT_SUCCESS;
		}

		int i = (int)files;
		while (--i >= 0) {
			if (*tmp != *file_info[i].name)
				continue;

			if (strcmp(tmp, file_info[i].name) != 0)
				continue;

			free(deq_str);
			deq_str = (char *)NULL;

			if (autocd && (file_info[i].type == DT_DIR || file_info[i].dir == 1))
				return (exit_code = cd_function(comm[0], CD_PRINT_ERROR));

			if (auto_open && (file_info[i].type == DT_REG
			|| file_info[i].type == DT_LNK)) {
				char *cmd[] = {"open", comm[0],
				    comm[1] ? comm[1] : NULL, NULL};
				return (exit_code = open_function(cmd));
			} else {
				break;
			}
		}
	}

	free(deq_str);

	/* The more often a function is used, the more on top should it be
	 * in this if...else..if chain. It will be found faster this way. */

	/* ####################################################
	 * #                 BUILTIN COMMANDS                 #
	 * ####################################################*/

	/*          ############### CD ##################     */
	if (*comm[0] == 'c' && comm[0][1] == 'd' && !comm[0][2]) {
		if (!comm[1])
			exit_code = cd_function(NULL, CD_PRINT_ERROR);
		else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0)
			puts(_(CD_USAGE));
		else
			exit_code = cd_function(comm[1], CD_PRINT_ERROR);
		return exit_code;
	}

	/*         ############### OPEN ##################     */
	else if (*comm[0] == 'o' && (!comm[0][1] || strcmp(comm[0], "open") == 0)) {
		if (!comm[1]) {
			puts(_(OPEN_USAGE));
			exit_code = EXIT_FAILURE;
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(OPEN_USAGE));
		} else {
			exit_code = open_function(comm);
		}
		return exit_code;
	}

	else if (*comm[0] == 'b' && comm[0][1] == 'd' && !comm[0][2])
		return (exit_code = backdir(comm[1] ? comm[1] : NULL));

	/*      ############### OPEN WITH ##################     */
	else if (*comm[0] == 'o' && comm[0][1] == 'w' && !comm[0][2]) {
#ifndef _NO_LIRA
		if (comm[1]) {
			if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
				puts(_(OW_USAGE));
				return EXIT_SUCCESS;
			}
			return mime_open_with(comm[1], comm[2] ? comm + 2 : NULL);
		}
		puts(_(OW_USAGE));
		return EXIT_SUCCESS;
#else
		fprintf(stderr, "%s: %s\n", PROGRAM_NAME, _(NOT_AVAILABLE));
		return EXIT_FAILURE;
#endif
	}

	/*   ############## DIRECTORY JUMPER ##################     */
	else if (*comm[0] == 'j' && (!comm[0][1] || ((comm[0][1] == 'c'
	|| comm[0][1] == 'p' || comm[0][1] == 'e' || comm[0][1] == 'o'
	|| comm[0][1] == 'l') && !comm[0][2])))
		return (exit_code = dirjump(comm, NO_SUG_JUMP));

	/*       ############### REFRESH ##################     */
	else if (*comm[0] == 'r' && ((comm[0][1] == 'f' && !comm[0][2])
	|| strcmp(comm[0], "refresh") == 0)) {
		if (autols) {
			free_dirlist();
			list_dir();
		}
		return exit_code = old_exit_code;
	}

	/*     ############### BOOKMARKS ##################     */
	else if (*comm[0] == 'b' && ((comm[0][1] == 'm' && !comm[0][2])
	|| strcmp(comm[0], "bookmarks") == 0)) {
		if (comm[1] && strcmp(comm[1], "--help") == 0) {
			puts(_(BOOKMARKS_USAGE));
			return EXIT_SUCCESS;
		}
		/* Disable keyboard shortcuts. Otherwise, the function will
		 * still be waiting for input while the screen have been taken
		 * by another function */
		kbind_busy = 1;
		/* Disable TAB completion while in Bookmarks */
		rl_attempted_completion_function = NULL;
		exit_code = bookmarks_function(comm);
		/* Reenable TAB completion */
		rl_attempted_completion_function = my_rl_completion;
		/* Reenable keyboard shortcuts */
		kbind_busy = 0;
		return exit_code;
	}

	/*       ############### BACK AND FORTH ##################     */
	else if (*comm[0] == 'b' && (!comm[0][1] || strcmp(comm[0], "back") == 0))
		return (exit_code = back_function(comm));

	else if (*comm[0] == 'f' && (!comm[0][1] || strcmp(comm[0], "forth") == 0))
		return (exit_code = forth_function(comm));

	else if ((*comm[0] == 'b' && comm[0][1] == 'h' && !comm[0][2])
	|| (*comm[0] == 'f' && comm[0][1] == 'h' && !comm[0][2])) {
		print_dirhist();
		return EXIT_SUCCESS;
	}


	/*     ################# NEW FILE ##################     */
	else if (*comm[0] == 'n' && (!comm[0][1] || strcmp(comm[0], "new") == 0))
		exit_code = create_file(comm);

	/*     ############### DUPLICATE FILE ##################     */
	else if (*comm[0] == 'd' && (!comm[0][1] || strcmp(comm[0], "dup") == 0)) {
		if (!comm[1] || (*comm[1] == '-' && strcmp(comm[1], "--help") == 0)) {
			puts(DUP_USAGE);
			return EXIT_SUCCESS;
		}
		exit_code = dup_file(comm[1], comm[2] ? comm[2] : NULL);
	}

#ifdef __HAIKU__
	else if ((*comm[0] == 'c' || *comm[0] == 'r' || *comm[0] == 'm'
	|| *comm[0] == 't' || *comm[0] == 'u' || *comm[0] == 'l')
	&& (strcmp(comm[0], "cp") == 0 || strcmp(comm[0], "rm") == 0
	|| strcmp(comm[0], "mkdir") == 0 || strcmp(comm[0], "unlink") == 0
	|| strcmp(comm[0], "touch") == 0 || strcmp(comm[0], "ln") == 0
	|| strcmp(comm[0], "chmod") == 0))
		return (exit_code = run_and_refresh(comm));
#endif

	/*     ############### COPY AND MOVE ##################     */
	else if ((*comm[0] == 'c' && (!comm[0][1] || (comm[0][1] == 'p'
	&& !comm[0][2])))

		|| (*comm[0] == 'm' && (!comm[0][1] || (comm[0][1] == 'v'
		&& !comm[0][2])))

		|| (*comm[0] == 'v' && (!comm[0][1] || (comm[0][1] == 'v'
		&& !comm[0][2])))

		|| (*comm[0] == 'p' && strcmp(comm[0], "paste") == 0)) {

		if (((*comm[0] == 'c' || *comm[0] == 'v') && !comm[0][1])
		|| (*comm[0] == 'v' && comm[0][1] == 'v' && !comm[0][2])
		|| strcmp(comm[0], "paste") == 0) {

			if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
				if (*comm[0] == 'v' && comm[0][1] == 'v' && !comm[0][2])
					puts(_(VV_USAGE));
				else
					puts(_(WRAPPERS_USAGE));
				return EXIT_SUCCESS;
			}

			if (*comm[0] == 'v' && comm[0][1] == 'v' && !comm[0][2])
				copy_n_rename = 1;

			comm[0] = (char *)xrealloc(comm[0], 12 * sizeof(char));
			if (!copy_n_rename) {
				if (cp_cmd == CP_CP)
					strcpy(comm[0], "cp -iRp");
				else if (cp_cmd == CP_ADVCP)
					strcpy(comm[0], "advcp -giRp");
				else
					strcpy(comm[0], "wcp");
			} else {
				strcpy(comm[0], "cp");
			}
		} else if (*comm[0] == 'm' && !comm[0][1]) {
			if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
				puts(_(WRAPPERS_USAGE));
				return EXIT_SUCCESS;
			}
			if (!sel_is_last && comm[1] && !comm[2])
				xrename = 1;

			comm[0] = (char *)xrealloc(comm[0], 10 * sizeof(char));
			if (mv_cmd == MV_MV)
				strcpy(comm[0], "mv -i");
			else
				strcpy(comm[0], "advmv -gi");
		}

		kbind_busy = 1;
		exit_code = copy_function(comm);
		kbind_busy = 0;
	}

	/*         ############### TRASH ##################     */
	else if (*comm[0] == 't' && (!comm[0][1] || strcmp(comm[0], "tr") == 0
	|| strcmp(comm[0], "trash") == 0)) {
#ifndef _NO_TRASH
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(TRASH_USAGE));
			return EXIT_SUCCESS;
		}

		exit_code = trash_function(comm);

		if (is_sel) { /* If 'tr sel', deselect everything */
			int i = (int)sel_n;
			while (--i >= 0)
				free(sel_elements[i]);
			sel_n = 0;
			if (save_sel() != 0)
				exit_code = EXIT_FAILURE;
		}
#else
		fprintf(stderr, _("%s: trash: %s\n"), PROGRAM_NAME, _(NOT_AVAILABLE));
		return EXIT_FAILURE;
#endif /* !_NO_TRASH */
	}
		
	else if (*comm[0] == 'u' && (!comm[0][1] || strcmp(comm[0], "undel") == 0
	|| strcmp(comm[0], "untrash") == 0)) {
#ifndef _NO_TRASH
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(UNTRASH_USAGE));
			return EXIT_SUCCESS;
		}

		kbind_busy = 1;
		rl_attempted_completion_function = NULL;
		exit_code = untrash_function(comm);
		rl_attempted_completion_function = my_rl_completion;
		kbind_busy = 0;
#else
		fprintf(stderr, _("%s: trash: %s\n"), PROGRAM_NAME, _(NOT_AVAILABLE));
		return EXIT_FAILURE;
#endif /* !_NO_TRASH */
	}

	/*     ############### SELECTION ##################     */
	else if (*comm[0] == 's' && (!comm[0][1] || strcmp(comm[0], "sel") == 0))
		return (exit_code = sel_function(comm));

	else if (*comm[0] == 's' && (strcmp(comm[0], "sb") == 0
	|| strcmp(comm[0], "selbox") == 0)) {
		show_sel_files();
		return EXIT_SUCCESS;
	}

	else if (*comm[0] == 'd' && (strcmp(comm[0], "ds") == 0
	|| strcmp(comm[0], "desel") == 0)) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(DESEL_USAGE));
			return EXIT_SUCCESS;
		}

		kbind_busy = 1;
		rl_attempted_completion_function = NULL;
		exit_code = deselect(comm);
		rl_attempted_completion_function = my_rl_completion;
		kbind_busy = 0;
		return exit_code;
	}

	/*  ############# SOME SHELL CMD WRAPPERS ###############  */

	else if ((*comm[0] == 'r' || *comm[0] == 'm' || *comm[0] == 'l')
	&& (strcmp(comm[0], "r") == 0 || strcmp(comm[0], "l") == 0
	|| strcmp(comm[0], "md") == 0 || strcmp(comm[0], "le") == 0)) {
		/* This help is only for c, l, le, m, r, t, and md commands */
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(WRAPPERS_USAGE));
			return EXIT_SUCCESS;
		}

		if (*comm[0] == 'l' && !comm[0][1]) {
			comm[0] = (char *)xrealloc(comm[0], 7 * sizeof(char));
			strcpy(comm[0], "ln -sn");

			/* Make sure the symlink source is always an absolute path */
			if (comm[1] && *comm[1] != '/' && *comm[1] != '~') {
				size_t len = strlen(comm[1]);
				char *tmp = (char *)xnmalloc(len + 1, sizeof(char));
				xstrsncpy(tmp, comm[1], len);
				comm[1] = (char *)xrealloc(comm[1], (len
							+ strlen(ws[cur_ws].path) + 2) * sizeof(char));
				sprintf(comm[1], "%s/%s", ws[cur_ws].path, tmp);
				free(tmp);
			}
		} else if (*comm[0] == 'r' && !comm[0][1]) {
			exit_code = remove_file(comm);
			goto CHECK_EVENTS;
		} else if (*comm[0] == 'm' && comm[0][1] == 'd' && !comm[0][2]) {
			comm[0] = (char *)xrealloc(comm[0], 9 * sizeof(char));
			strcpy(comm[0], "mkdir -p");
		}

		if (*comm[0] == 'l' && comm[0][1] == 'e' && !comm[0][2]) {
			if (!comm[1]) {
				fprintf(stderr, "%s\n", _(LE_USAGE));
				return (exit_code = EXIT_FAILURE);
			}
			exit_code = edit_link(comm[1]);
			goto CHECK_EVENTS;
		} else if (*comm[0] == 'l' && comm[0][1] == 'n' && !comm[0][2]) {
			if (comm[1] && (strcmp(comm[1], "edit") == 0
			|| strcmp(comm[1], "e") == 0)) {
				if (!comm[2]) {
					fprintf(stderr, "%s\n", _(LE_USAGE));
					return (exit_code = EXIT_FAILURE);
				}
				exit_code = edit_link(comm[2]);
				goto CHECK_EVENTS;
			}
		}

		kbind_busy = 1;
		exit_code = run_and_refresh(comm);
		kbind_busy = 0;
	}

	/*    ############### TOGGLE EXEC ##################     */
	else if (*comm[0] == 't' && comm[0][1] == 'e' && !comm[0][2]) {
		if (!comm[1] || (*comm[1] == '-' && strcmp(comm[1], "--help") == 0)) {
			puts(_(TE_USAGE));
			return EXIT_SUCCESS;
		}

		size_t j;
		for (j = 1; comm[j]; j++) {
			struct stat attr;
			if (strchr(comm[j], '\\')) {
				char *tmp = dequote_str(comm[j], 0);
				if (tmp) {
					strcpy(comm[j], tmp);
					free(tmp);
				}
			}

			if (lstat(comm[j], &attr) == -1) {
				fprintf(stderr, "stat: %s: %s\n", comm[j], strerror(errno));
				exit_code = EXIT_FAILURE;
				continue;
			}

			if (xchmod(comm[j], attr.st_mode) == -1)
				exit_code = EXIT_FAILURE;
		}

		if (exit_code == EXIT_SUCCESS)
			printf(_("%s: Toggled executable bit on %zu file(s)\n"),
			    PROGRAM_NAME, args_n);
	}

	/*    ############### PINNED FILE ##################     */
	else if (*comm[0] == 'p' && strcmp(comm[0], "pin") == 0) {
		if (comm[1]) {
			if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0)
				puts(PIN_USAGE);
			else
				exit_code = pin_directory(comm[1]);
		} else {
			if (pinned_dir)
				printf(_("pinned file: %s\n"), pinned_dir);
			else
				puts(_("No pinned file"));
		}
		return exit_code;
	}

	else if (*comm[0] == 'u' && strcmp(comm[0], "unpin") == 0)
		return (exit_code = unpin_dir());

	/*    ############### PROPERTIES ##################     */
	else if (*comm[0] == 'p' && (!comm[0][1] || strcmp(comm[0], "pr") == 0
	|| strcmp(comm[0], "pp") == 0 || strcmp(comm[0], "prop") == 0)) {
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(PROP_USAGE));
			exit_code = EXIT_FAILURE;
			return EXIT_FAILURE;
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(PROP_USAGE));
			return EXIT_SUCCESS;
		}

		return (exit_code = properties_function(comm));
	}

	/*     ############### SEARCH ##################     */
	else if (*comm[0] == '/' && !strchr(comm[0], '\\')
	&& access(comm[0], F_OK) != 0) {
		/* If not absolute path */
		/* Try first globbing, and if no result, try regex */
		if (search_glob(comm, (comm[0][1] == '!') ? 1 : 0) == EXIT_FAILURE)
			exit_code = search_regex(comm, (comm[0][1] == '!') ? 1 : 0,
					case_sens_search ? 1 : 0);
		else
			exit_code = EXIT_SUCCESS;
		return exit_code;
	}

	/*      ############## HISTORY ##################     */
	else if (*comm[0] == '!' && comm[0][1] != ' ' && comm[0][1] != '\t'
	&& comm[0][1] != '\n' && comm[0][1] != '=' && comm[0][1] != '(')
		exit_code = run_history_cmd(comm[0] + 1);

	/*    ############### BATCH LINK ##################     */
	else if (*comm[0] == 'b' && comm[0][1] == 'l' && !comm[0][2])
		exit_code = batch_link(comm);

	/*    ############### BULK RENAME ##################     */
	else if (*comm[0] == 'b' && ((comm[0][1] == 'r' && !comm[0][2])
	|| strcmp(comm[0], "bulk") == 0)) {
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(BULK_USAGE));
			return (exit_code = EXIT_FAILURE);
		}

		if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(BULK_USAGE));
			return EXIT_SUCCESS;
		}
		exit_code = bulk_rename(comm);
	}

	/*      ################ SORT ##################     */
	else if (*comm[0] == 's' && ((comm[0][1] == 't' && !comm[0][2])
	|| strcmp(comm[0], "sort") == 0)) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(SORT_USAGE));
			return EXIT_SUCCESS;
		}
		return (exit_code = sort_function(comm));
	}

	/*    ########## FILE NAMES CLEANER ############## */
	else if (*comm[0] == 'b' && ((comm[0][1] == 'b' && !comm[0][2])
	|| strcmp(comm[0], "bleach") == 0)) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(BLEACH_USAGE));
			return EXIT_SUCCESS;
		}
#ifndef _NO_BLEACH
		exit_code = bleach_files(comm);
#else
		fprintf(stderr, _("%s: bleach: %s\n"), PROGRAM_NAME, NOT_AVAILABLE);
		return EXIT_FAILURE;
#endif
	}

	/*   ################ ARCHIVER ##################     */
	else if (*comm[0] == 'a' && ((comm[0][1] == 'c' || comm[0][1] == 'd')
	&& !comm[0][2])) {
#ifndef _NO_ARCHIVING
		if (!comm[1] || (*comm[1] == '-' && strcmp(comm[1], "--help") == 0)) {
			puts(_(ARCHIVE_USAGE));
			return EXIT_SUCCESS;
		}

		if (comm[0][1] == 'c')
			exit_code = archiver(comm, 'c');
		else
			exit_code = archiver(comm, 'd');
#else
		fprintf(stderr, _("%s: archiving: %s\n"), PROGRAM_NAME, _(NOT_AVAILABLE));
		return EXIT_FAILURE;
#endif
	}

	/* ##################################################
	 * #                 MINOR FUNCTIONS                #
	 * ##################################################*/

	else if (*comm[0] == 'w' && comm[0][1] == 's' && !comm[0][2])
		return (exit_code = workspaces(comm[1] ? comm[1] : NULL));

	else if (*comm[0] == 'f' && ((comm[0][1] == 't' && !comm[0][2])
	|| strcmp(comm[0], "filter") == 0))
		return (exit_code = filter_function(comm[1]));

	else if (*comm[0] == 'c' && ((comm[0][1] == 'l' && !comm[0][2])
	|| strcmp(comm[0], "columns") == 0)) {
		if (!comm[1] || (*comm[1] == '-' && strcmp(comm[1], "--help") == 0)) {
			puts(_(COLUMNS_USAGE));
			return EXIT_SUCCESS;
		} else if (*comm[1] == 'o' && comm[1][1] == 'n' && !comm[1][2]) {
			columned = 1;
			if (autols) {
				free_dirlist();
				exit_code = list_dir();
			}
		} else if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
			columned = 0;
			if (autols) {
				free_dirlist();
				exit_code = list_dir();
			}
		} else {
			fprintf(stderr, "%s\n", _(COLUMNS_USAGE));
			exit_code = EXIT_FAILURE;
			return EXIT_FAILURE;
		}
		return exit_code;
	}
	else if (*comm[0] == 'i' && strcmp(comm[0], "icons") == 0) {
#ifndef _NO_ICONS
		if (!comm[1] || (*comm[1] == '-' && strcmp(comm[1], "--help") == 0)) {
			puts(_(ICONS_USAGE));
		} else if (*comm[1] == 'o' && comm[1][1] == 'n' && !comm[1][2]) {
			icons = 1;
			if (autols) {
				free_dirlist();
				exit_code = list_dir();
			}
		} else if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
			icons = 0;
			if (autols) {
				free_dirlist();
				exit_code = list_dir();
			}
		} else {
			fprintf(stderr, "%s\n", _(ICONS_USAGE));
			exit_code = EXIT_FAILURE;
			return EXIT_FAILURE;
		}

		return EXIT_SUCCESS;
#else
		fprintf(stderr, _("%s: icons: %s\n"), PROGRAM_NAME, _(NOT_AVAILABLE));
		return EXIT_SUCCESS;
#endif /* _NO_ICONS */
	}

	else if (*comm[0] == 'c' && ((comm[0][1] == 's' && !comm[0][2])
	|| strcmp(comm[0], "colorschemes") == 0))
		return (exit_code = cschemes_function(comm));

	else if (*comm[0] == 'k' && ((comm[0][1] == 'b' && !comm[0][2])
	|| strcmp(comm[0], "keybinds") == 0))
		return (exit_code = kbinds_function(comm));

	else if (*comm[0] == 'e' && strcmp(comm[0], "exp") == 0) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(EXPORT_USAGE));
			return EXIT_SUCCESS;
		}

		char *ret = export(comm, 1);
		if (ret) {
			printf("Files exported to: %s\n", ret);
			free(ret);
			return EXIT_SUCCESS;
		}

		return (exit_code = EXIT_FAILURE);
	}

	else if (*comm[0] == 'o' && strcmp(comm[0], "opener") == 0) {
		if (!comm[1]) {
			printf("opener: %s\n", (opener) ? opener : "lira (built-in)");
			return EXIT_SUCCESS;
		}
		if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(OPENER_USAGE));
			return EXIT_SUCCESS;
		}
		if (opener) {
			free(opener);
			opener = (char *)NULL;
		}
		if (strcmp(comm[1], "default") != 0 && strcmp(comm[1], "lira") != 0) {
			opener = (char *)xnmalloc(strlen(comm[1]) + 1, sizeof(char));
			strcpy(opener, comm[1]);
		}
		printf(_("opener: Opener set to '%s'\n"), (opener) ? opener
								   : "lira (built-in)");
		return EXIT_SUCCESS;
	}

	/* #### TIPS #### */
	else if (*comm[0] == 't' && strcmp(comm[0], "tips") == 0) {
		print_tips(1);
		return EXIT_SUCCESS;
	}

	/* #### ACTIONS #### */
	else if (*comm[0] == 'a' && strcmp(comm[0], "actions") == 0) {
		if (!comm[1]) {
			if (actions_n) {
				size_t i;
				for (i = 0; i < actions_n; i++)
					printf("%s %s->%s %s\n", usr_actions[i].name,
					    mi_c, df_c, usr_actions[i].value);
			} else {
				puts(_("actions: No actions defined. Use the 'actions "
				       "edit' command to add some"));
			}
		} else if (strcmp(comm[1], "edit") == 0) {
			return (exit_code = edit_actions());
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(ACTIONS_USAGE));
		} else {
			fprintf(stderr, "%s\n", _(ACTIONS_USAGE));
			exit_code = EXIT_FAILURE;
			return EXIT_FAILURE;
		}
		return exit_code;
	}

	/* #### LIGHT MODE #### */
	else if (*comm[0] == 'l' && comm[0][1] == 'm' && !comm[0][2]) {
		if (comm[1]) {
			if (*comm[1] == 'o' && strcmp(comm[1], "on") == 0) {
				light_mode = 1;
				puts(_("Light mode is on"));
			} else if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
				light_mode = 0;
				puts(_("Light mode is off"));
			} else {
				puts(_(LM_USAGE));
				exit_code = EXIT_FAILURE;
			}
		} else {
			fprintf(stderr, "%s\n", _(LM_USAGE));
			exit_code = EXIT_FAILURE;
		}
		return exit_code;
	}

	/*    ############### RELOAD ##################     */
	else if (*comm[0] == 'r' && ((comm[0][1] == 'l' && !comm[0][2])
	|| strcmp(comm[0], "reload") == 0)) {
		exit_code = reload_config();
		welcome_message = 0;
		if (autols) {
			free_dirlist();
			if (list_dir() != EXIT_SUCCESS)
				exit_code = EXIT_FAILURE;
		}
		return exit_code;
	}

	/* #### NEW INSTANCE #### */
	else if ((*comm[0] == 'x' || *comm[0] == 'X') && !comm[0][1]) {
		if (comm[1]) {
			if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
				puts(_(X_USAGE));
				return EXIT_SUCCESS;
			} else if (*comm[0] == 'x') {
				exit_code = new_instance(comm[1], 0);
			} else { /* Run as root */
				exit_code = new_instance(comm[1], 1);
			}
		} else {
		/* Run new instance in CWD */
			if (*comm[0] == 'x')
				exit_code = new_instance(ws[cur_ws].path, 0);
			else
				exit_code = new_instance(ws[cur_ws].path, 1);
		}

		return exit_code;
	}

	/* #### NET #### */
	else if (*comm[0] == 'n' && (strcmp(comm[0], "net") == 0))
		return (exit_code = remotes_function(comm));

	/* #### MIME #### */
	else if (*comm[0] == 'm' && ((comm[0][1] == 'm' && !comm[0][2])
	|| strcmp(comm[0], "mime") == 0)) {
#ifndef _NO_LIRA
		return (exit_code = mime_open(comm));
#else
		fprintf(stderr, _("%s: Lira: %s\n"), PROGRAM_NAME, _(NOT_AVAILABLE));
		return EXIT_FAILURE;
#endif
	}

	else if (*comm[0] == 'l' && comm[0][1] == 's' && !comm[0][2] && !autols) {
		free_dirlist();
		exit_code = list_dir();
		if (get_sel_files() != EXIT_SUCCESS)
			exit_code = EXIT_FAILURE;
		return exit_code;
	}

	/* #### PROFILE #### */
	else if (*comm[0] == 'p' && ((comm[0][1] == 'f' && !comm[0][2]) || strcmp(comm[0], "prof") == 0 || strcmp(comm[0], "profile") == 0))
		return (exit_code = profile_function(comm));

	/* #### MOUNTPOINTS #### */
	else if (*comm[0] == 'm' && ((comm[0][1] == 'p' && !comm[0][2]) || strcmp(comm[0], "mountpoints") == 0)) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(MOUNPOINTS_USAGE));
			return EXIT_SUCCESS;
		} else {
			kbind_busy = 1;
			rl_attempted_completion_function = NULL;
			exit_code = media_menu(MEDIA_LIST);
			rl_attempted_completion_function = my_rl_completion;
			kbind_busy = 0;
			return exit_code;
		}
	}

	/* #### MEDIA #### */
	else if (*comm[0] == 'm' && strcmp(comm[0], "media") == 0) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(MEDIA_USAGE));
			return EXIT_SUCCESS;
		} else {
			kbind_busy = 1;
			rl_attempted_completion_function = NULL;
			exit_code = media_menu(MEDIA_MOUNT);
			rl_attempted_completion_function = my_rl_completion;
			kbind_busy = 0;
			return exit_code;
		}
	}

	/* #### MAX FILES #### */
	else if (*comm[0] == 'm' && comm[0][1] == 'f' && !comm[0][2])
		return set_max_files(comm);

	/* #### EXT #### */
	else if (*comm[0] == 'e' && comm[0][1] == 'x' && comm[0][2] == 't'
	&& !comm[0][3]) {
		if (!comm[1]) {
			puts(_(EXT_USAGE));
			return (exit_code = EXIT_FAILURE);
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(EXT_USAGE));
		} else {
			if (*comm[1] == 's' && strcmp(comm[1], "status") == 0) {
				printf(_("%s: External commands %s\n"), PROGRAM_NAME,
				    (ext_cmd_ok) ? _("enabled") : _("disabled"));
			} else if (*comm[1] == 'o' && strcmp(comm[1], "on") == 0) {
				ext_cmd_ok = 1;
				printf(_("%s: External commands enabled\n"), PROGRAM_NAME);
			} else if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
				ext_cmd_ok = 0;
				printf(_("%s: External commands disabled\n"), PROGRAM_NAME);
			} else {
				fprintf(stderr, "%s\n", _(EXT_USAGE));
				exit_code = EXIT_FAILURE;
			}
		}
		return exit_code;
	}

	/* #### PAGER #### */
	else if (*comm[0] == 'p' && ((comm[0][1] == 'g' && !comm[0][2])
	|| strcmp(comm[0], "pager") == 0)) {
		if (!comm[1]) {
			puts(_(PAGER_USAGE));
			return (exit_code = EXIT_FAILURE);
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(PAGER_USAGE));
			return EXIT_SUCCESS;
		} else {
			if (*comm[1] == 's' && strcmp(comm[1], "status") == 0) {
				printf(_("%s: Pager %s\n"), PROGRAM_NAME,
				    (pager) ? _("enabled") : _("disabled"));
			} else if (*comm[1] == 'o' && strcmp(comm[1], "on") == 0) {
				pager = 1;
				printf(_("%s: Pager enabled\n"), PROGRAM_NAME);
			} else if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
				pager = 0;
				printf(_("%s: Pager disabled\n"), PROGRAM_NAME);
			} else {
				fprintf(stderr, "%s\n", _(PAGER_USAGE));
				exit_code = EXIT_FAILURE;
			}
		}
		return exit_code;
	}

	/* #### FILES COUNTER #### */
	else if (*comm[0] == 'f' && ((comm[0][1] == 'c' && !comm[0][2])
	|| strcmp(comm[0], "filescounter") == 0)) {
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(FC_USAGE));
			return (exit_code = EXIT_FAILURE);
		}

		if (*comm[1] == 'o' && strcmp(comm[1], "on") == 0) {
			files_counter = 1;
			puts(_("Filescounter is enabled"));
			return EXIT_SUCCESS;
		}

		if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
			files_counter = 0;
			puts(_("Filescounter is disabled"));
			return EXIT_SUCCESS;
		}

		if (*comm[1] == 's' && strcmp(comm[1], "status") == 0) {
			if (files_counter)
				puts(_("Filescounter is enabled"));
			else
				puts(_("Filescounter is disabled"));
			return EXIT_SUCCESS;
		} else {
			fprintf(stderr, "%s\n", _(FC_USAGE));
			return (exit_code = EXIT_FAILURE);
		}
	}

	/* #### UNICODE #### */
	else if (*comm[0] == 'u' && ((comm[0][1] == 'c' && !comm[0][2])
	|| strcmp(comm[0], "unicode") == 0)) {
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(UNICODE_USAGE));
			return (exit_code = EXIT_FAILURE);
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(UNICODE_USAGE));
			return EXIT_SUCCESS;
		} else {
			if (*comm[1] == 's' && strcmp(comm[1], "status") == 0) {
				printf(_("%s: Unicode %s\n"), PROGRAM_NAME,
				    (unicode) ? _("enabled") : _("disabled"));
			} else if (*comm[1] == 'o' && strcmp(comm[1], "on") == 0) {
				unicode = 1;
				printf(_("%s: Unicode enabled\n"), PROGRAM_NAME);
			} else if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
				unicode = 0;
				printf(_("%s: Unicode disabled\n"), PROGRAM_NAME);
			} else {
				fprintf(stderr, "%s\n", _(UNICODE_USAGE));
				exit_code = EXIT_FAILURE;
			}
		}
		return exit_code;
	}

	/* #### FOLDERS FIRST #### */
	else if (*comm[0] == 'f' && ((comm[0][1] == 'f' && !comm[0][2])
	|| strcmp(comm[0], "folders-first") == 0)) {
		if (autols == 0)
			return EXIT_SUCCESS;
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(FF_USAGE));
			return (exit_code = EXIT_FAILURE);
		}
		if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(FF_USAGE));
			return EXIT_SUCCESS;
		}

		int status = list_folders_first;
		if (*comm[1] == 's' && strcmp(comm[1], "status") == 0) {
			printf(_("%s: Folders first %s\n"), PROGRAM_NAME,
			    (list_folders_first) ? _("enabled") : _("disabled"));
		}  else if (*comm[1] == 'o' && strcmp(comm[1], "on") == 0) {
			list_folders_first = 1;
		} else if (*comm[1] == 'o' && strcmp(comm[1], "off") == 0) {
			list_folders_first = 0;
		} else {
			fprintf(stderr, "%s\n", _(FF_USAGE));
			return (exit_code = EXIT_FAILURE);
		}

		if (list_folders_first != status) {
			if (autols) {
				free_dirlist();
				exit_code = list_dir();
			}
		}
		return exit_code;
	}

	/* #### LOG #### */
	else if (*comm[0] == 'l' && strcmp(comm[0], "log") == 0) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(LOG_USAGE));
			return EXIT_SUCCESS;
		}

		/* I make this check here, and not in the function itself,
		 * because this function is also called by other instances of
		 * the program where no message should be printed */
		if (!config_ok) {
			fprintf(stderr, _("%s: Log function disabled\n"), PROGRAM_NAME);
			return (exit_code = EXIT_FAILURE);
		}

		return (exit_code = log_function(comm));
	}

	/* #### MESSAGES #### */
	else if (*comm[0] == 'm' && (strcmp(comm[0], "msg") == 0
	|| strcmp(comm[0], "messages") == 0)) {
		if (comm[1] && strcmp(comm[1], "--help") == 0) {
			puts(_(MSG_USAGE));
			return EXIT_SUCCESS;
		}

		if (comm[1] && strcmp(comm[1], "clear") == 0) {
			if (!msgs_n) {
				printf(_("%s: There are no messages\n"), PROGRAM_NAME);
				return EXIT_SUCCESS;
			}

			size_t i;
			for (i = 0; i < (size_t)msgs_n; i++)
				free(messages[i]);

			msgs_n = 0;
			pmsg = NOMSG;
		} else {
			if (msgs_n) {
				size_t i;
				for (i = 0; i < (size_t)msgs_n; i++)
					printf("%s", messages[i]);
			} else {
				printf(_("%s: There are no messages\n"), PROGRAM_NAME);
			}
		}
		return exit_code;
	}

	/* #### ALIASES #### */
	else if (*comm[0] == 'a' && strcmp(comm[0], "alias") == 0) {
		if (comm[1]) {
			if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
				puts(_(ALIAS_USAGE));
				return EXIT_SUCCESS;
			} else if (*comm[1] == 'i' && strcmp(comm[1], "import") == 0) {
				if (!comm[2]) {
					fprintf(stderr, "%s\n", _(ALIAS_USAGE));
					return (exit_code = EXIT_FAILURE);
				}
				return (exit_code = alias_import(comm[2]));
			}
		}

		if (aliases_n) {
			size_t i;
			for (i = 0; i < aliases_n; i++)
				printf("%s %s->%s %s\n", aliases[i].name, mi_c, df_c, aliases[i].cmd);
		} else {
			printf("%s: No aliases found\n", PROGRAM_NAME);
		}
		return EXIT_SUCCESS;
	}

	/* #### SHELL #### */
/*	else if (*comm[0] == 's' && strcmp(comm[0], "shell") == 0) {
		if (!comm[1]) {
			if (user.shell)
				printf("%s: shell: %s\n", PROGRAM_NAME, user.shell);
			else
				printf(_("%s: shell: unknown\n"), PROGRAM_NAME);
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(SHELL_USAGE));
			return EXIT_SUCCESS;
		} else {
			return (exit_code = set_shell(comm[1]));
		}
	} */

	/* #### EDIT #### */
	else if (*comm[0] == 'e' && strcmp(comm[0], "edit") == 0)
		return (exit_code = edit_function(comm));

	/* #### HISTORY #### */
	else if (*comm[0] == 'h' && strcmp(comm[0], "history") == 0)
		return (exit_code = history_function(comm));

	/* #### HIDDEN FILES #### */
	else if (*comm[0] == 'h' && ((comm[0][1] == 'f' && !comm[0][2])
	|| strcmp(comm[0], "hidden") == 0)) {
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(HF_USAGE));
			return (exit_code = EXIT_FAILURE);
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			/* The same message is in hidden_function(), and printed
			 * whenever an invalid argument is entered */
			puts(_(HF_USAGE));
			return EXIT_SUCCESS;
		} else {
			return (exit_code = hidden_function(comm));
		}
	}

	/* #### AUTOCD #### */
	else if (*comm[0] == 'a' && (strcmp(comm[0], "acd") == 0
	|| strcmp(comm[0], "autocd") == 0)) {
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(AUTOCD_USAGE));
			return (exit_code = EXIT_FAILURE);
		}

		if (strcmp(comm[1], "on") == 0) {
			autocd = 1;
			printf(_("%s: autocd is enabled\n"), PROGRAM_NAME);
		} else if (strcmp(comm[1], "off") == 0) {
			autocd = 0;
			printf(_("%s: autocd is disabled\n"), PROGRAM_NAME);
		} else if (strcmp(comm[1], "status") == 0) {
			if (autocd)
				printf(_("%s: autocd is enabled\n"), PROGRAM_NAME);
			else
				printf(_("%s: autocd is disabled\n"), PROGRAM_NAME);
		} else if (*comm[1] == '-' && strcmp(comm[1], "--help") == 0) {
			puts(_(AUTOCD_USAGE));
		} else {
			fprintf(stderr, "%s\n", _(AUTOCD_USAGE));
			return (exit_code = EXIT_FAILURE);
		}
		return EXIT_SUCCESS;
	}

	/* #### AUTO-OPEN #### */
	else if (*comm[0] == 'a' && ((comm[0][1] == 'o' && !comm[0][2])
	|| strcmp(comm[0], "auto-open") == 0)) {
		if (!comm[1]) {
			fprintf(stderr, "%s\n", _(AUTO_OPEN_USAGE));
			return (exit_code = EXIT_FAILURE);
		}

		if (strcmp(comm[1], "on") == 0) {
			auto_open = 1;
			printf(_("%s: auto-open is enabled\n"), PROGRAM_NAME);
		} else if (strcmp(comm[1], "off") == 0) {
			auto_open = 0;
			printf(_("%s: auto-open is disabled\n"), PROGRAM_NAME);
		} else if (strcmp(comm[1], "status") == 0) {
			if (auto_open)
				printf(_("%s: auto-open is enabled\n"), PROGRAM_NAME);
			else
				printf(_("%s: auto-open is disabled\n"), PROGRAM_NAME);
		} else if (strcmp(comm[1], "--help") == 0) {
			puts(_(AUTO_OPEN_USAGE));
		} else {
			fprintf(stderr, "%s\n", _(AUTO_OPEN_USAGE));
			return (exit_code = EXIT_FAILURE);
		}
		return EXIT_SUCCESS;
	}

	/* #### COMMANDS #### */
	else if (*comm[0] == 'c' && (strcmp(comm[0], "cmd") == 0
	|| strcmp(comm[0], "commands") == 0))
		return (exit_code = list_commands());

	/* #### AND THESE ONES TOO #### */
	/* These functions just print stuff, so that the value of exit_code
	 * is always zero, that is to say, success */
	else if (strcmp(comm[0], "path") == 0 || strcmp(comm[0], "cwd") == 0) {
		printf("%s\n", ws[cur_ws].path);
		return EXIT_SUCCESS;
	}

	else if ((*comm[0] == '?' && !comm[0][1]) || strcmp(comm[0], "help") == 0) {
		quick_help();
		return EXIT_SUCCESS;
	}

	else if (*comm[0] == 'c' && ((comm[0][1] == 'c' && !comm[0][2])
	|| strcmp(comm[0], "colors") == 0)) {
		if (comm[1] && *comm[1] == '-' && strcmp(comm[1], "--help") == 0)
			puts(_(COLORS_USAGE));
		else
			color_codes();
		return EXIT_SUCCESS;
	}

	else if (*comm[0] == 'v' && (strcmp(comm[0], "ver") == 0
	|| strcmp(comm[0], "version") == 0)) {
		version_function();
		return EXIT_SUCCESS;
	}

	else if (*comm[0] == 'f' && comm[0][1] == 's' && !comm[0][2]) {
		free_software();
		return EXIT_SUCCESS;
	}

	else if (*comm[0] == 'b' && strcmp(comm[0], "bonus") == 0) {
		bonus_function();
		return EXIT_SUCCESS;
	}

	else if (*comm[0] == 's' && strcmp(comm[0], "splash") == 0) {
		splash();
		return EXIT_SUCCESS;
	}

	/* #### QUIT #### */
	else if ((*comm[0] == 'q' && (!comm[0][1] || strcmp(comm[0], "quit") == 0))
	|| (*comm[0] == 'e' && strcmp(comm[0], "exit") == 0)
	|| (*comm[0] == 'Q' && !comm[0][1])) {
		/* Free everything and exit */
		if (*comm[0] == 'Q')
			cd_on_quit = 1;
		int i = (int)args_n + 1;
		while (--i >= 0)
			free(comm[i]);
		free(comm);
		exit(exit_code);
	}

	else {
				/* ###############################
				 * #     AUTOCD & AUTO-OPEN (2)   #
				 * ############################### */

		char *tmp = (char *)xnmalloc(strlen(comm[0]) + 1, sizeof(char));
		strcpy(tmp, comm[0]);
		if (strchr(tmp, '\\')) {
			char *dstr = dequote_str(tmp, 0);
			if (dstr) {
				strcpy(tmp, dstr);
				free(dstr);
			}
		}

		if (autocd && cdpath_n && !comm[1]) {
			exit_code = cd_function(tmp, CD_NO_PRINT_ERROR);
			if (exit_code == EXIT_SUCCESS) {
				free(tmp);
				return EXIT_SUCCESS;
			}
		}

		struct stat attr;
		if (stat(tmp, &attr) == 0) {
			if ((attr.st_mode & S_IFMT) == S_IFDIR) {
				if (autocd)
					exit_code = cd_function(tmp, CD_PRINT_ERROR);
				else
					fprintf(stderr, _("%s: %s: Is a directory\n"),
							PROGRAM_NAME, tmp);
				free(tmp);
				return exit_code;
			} else if (auto_open && (attr.st_mode & S_IFMT) == S_IFREG) {
				if (!(attr.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
					char *cmd[] = {"open", tmp, (args_n >= 1) ? comm[1]
						: NULL, (args_n >= 2) ? comm[2] : NULL, NULL};
					args_n++;
					exit_code = open_function(cmd);
					args_n--;
					free(tmp);
					return exit_code;
				}
			}
		}

		free(tmp);

	/* ####################################################
	 * #                EXTERNAL/SHELL COMMANDS           #
	 * ####################################################*/
		if ((exit_code = run_shell_cmd(comm)) == EXIT_FAILURE)
			return EXIT_FAILURE;
	}

CHECK_EVENTS:
	if (!autols)
		return exit_code;

#ifdef LINUX_INOTIFY
	if (watch)
		read_inotify();
#elif defined(BSD_KQUEUE)
	if (watch && event_fd >= 0)
		read_kqueue();
#endif

	return exit_code;
}

/* Execute chained commands (cmd1;cmd2 and/or cmd1 && cmd2). The function
 * is called by parse_input_str() if some non-quoted double ampersand or
 * semicolon is found in the input string AND at least one of these
 * chained commands is internal */
void
exec_chained_cmds(char *cmd)
{
	if (!cmd)
		return;

	size_t i = 0, cmd_len = strlen(cmd);
	for (i = 0; i < cmd_len; i++) {
		char *str = (char *)NULL;
		size_t len = 0, cond_exec = 0;

		/* Get command */
		str = (char *)xcalloc(strlen(cmd) + 1, sizeof(char));
		while (cmd[i] && cmd[i] != '&' && cmd[i] != ';')
			str[len++] = cmd[i++];

		if (!*str) {
			free(str);
			continue;
		}

		/* Should we execute conditionally? */
		if (cmd[i] == '&')
			cond_exec = 1;

		/* Execute the command */
		char **tmp_cmd = parse_input_str(str);
		free(str);

		if (!tmp_cmd)
			continue;
		
		int error_code = 0;
		size_t j;
		char **alias_cmd = check_for_alias(tmp_cmd);
		if (alias_cmd) {
			if (exec_cmd(alias_cmd) != 0)
				error_code = 1;
			for (j = 0; alias_cmd[j]; j++)
				free(alias_cmd[j]);
			free(alias_cmd);
		} else {
			if (exec_cmd(tmp_cmd) != 0)
				error_code = 1;
			for (j = 0; j <= args_n; j++)
				free(tmp_cmd[j]);
			free(tmp_cmd);
		}
		/* Do not continue if the execution was condtional and
		 * the previous command failed */
		if (cond_exec && error_code)
			break;
	}
}

void
exec_profile(void)
{
	if (!config_ok || !profile_file)
		return;

	FILE *fp = fopen(profile_file, "r");
	if (!fp)
		return;

	size_t line_size = 0;
	char *line = (char *)NULL;
	ssize_t line_len = 0;

	while ((line_len = getline(&line, &line_size, fp)) > 0) {
		/* Skip empty and commented lines */
		if (!*line || *line == '\n' || *line == '#')
			continue;

		/* Remove trailing new line char */
		if (line[line_len - 1] == '\n')
			line[line_len - 1] = '\0';

		if (strchr(line, '=') && !_ISDIGIT(*line)) {
			create_usr_var(line);
		} else if (strlen(line) != 0) {
		/* Parse line and execute it */
			args_n = 0;

			char **cmds = parse_input_str(line);

			if (cmds) {
				no_log = 1;
				exec_cmd(cmds);
				no_log = 0;
				int i = (int)args_n + 1;
				while (--i >= 0)
					free(cmds[i]);
				free(cmds);
				cmds = (char **)NULL;
			}
			args_n = 0;
		}
	}

	free(line);
	fclose(fp);
}
