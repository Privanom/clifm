/* misc.c -- functions that do not fit in any other file */

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
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__NetBSD__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

/*
#if defined(__HAIKU__)
#include <private/libs/compat/freebsd_network/compat/sys/mount.h>
#include <private/libs/compat/freebsd_network/compat/sys/sysctl.h>
#endif */
#include <time.h>
#include <unistd.h>
#include <readline/readline.h>
#ifdef LINUX_INOTIFY
#include <sys/inotify.h>
#endif

/*
#ifdef __linux__
#include <libudev.h>
#endif */

#include "aux.h"
#include "bookmarks.h"
#include "checks.h"
#include "exec.h"
#include "history.h"
#include "init.h"
#include "jump.h"
#include "listing.h"
#include "navigation.h"
#include "readline.h"
#include "strings.h"
#include "remotes.h"
#include "messages.h"

#ifdef LINUX_INOTIFY
void
reset_inotify(void)
{
	watch = 0;

	if (inotify_wd >= 0) {
		inotify_rm_watch(inotify_fd, inotify_wd);
		inotify_wd = -1;
	}

	inotify_wd = inotify_add_watch(inotify_fd, workspaces[cur_ws].path,
				INOTIFY_MASK);
	if (inotify_wd > 0)
		watch = 1;
}

void
read_inotify(void)
{
	if (inotify_fd == UNSET)
		return;

	int i;
	struct inotify_event *event;
	char inotify_buf[EVENT_BUF_LEN];

	memset((void *)inotify_buf, '\0', EVENT_BUF_LEN);
	i = (int)read(inotify_fd, inotify_buf, EVENT_BUF_LEN);

	if (i <= 0) {
#ifdef INOTIFY_DEBUG
		puts("INOTIFY_RETURN");
#endif
		return;
	}

	int ignore_event = 0, refresh = 0;
	for (char *ptr = inotify_buf;
	ptr + ((struct inotify_event *)ptr)->len < inotify_buf + i;
	ptr += sizeof(struct inotify_event) + event->len) {
		event = (struct inotify_event *)ptr;

#ifdef INOTIFY_DEBUG
		printf("%s (%u): ", *event->name ? event->name : NULL, event->len);
#endif

		if (!event->wd) {
#ifdef INOTIFY_DEBUG
			puts("INOTIFY_BREAK");
#endif
			break;
		}

		if (event->mask & IN_CREATE) {
#ifdef INOTIFY_DEBUG
			puts("IN_CREATE");
#endif
			struct stat a;
			if (event->len && lstat(event->name, &a) != 0) {
				/* The file was created, but doesn't exist anymore */
				ignore_event = 1;
			}
		}

		/* A file was renamed */
		if (event->mask & IN_MOVED_TO) {
			int j = (int)files;
			while (--j >= 0) {
				if (*file_info[j].name == *event->name
				&& strcmp(file_info[j].name, event->name) == 0)
					break;
			}
			if (j < 0)
				ignore_event = 0;
			else {
				/* If destiny file name is already in the files list,
				 * ignore this event */
				ignore_event = 1;
			}
		}

		if (event->mask & IN_DELETE) {
#ifdef INOTIFY_DEBUG
			puts("IN_DELETE");
#endif
			struct stat a;
			if (event->len && lstat(event->name, &a) == 0)
				/* The file was removed, but is still there (recreated) */
				ignore_event = 1;
		}

#ifdef INOTIFY_DEBUG
		if (event->mask & IN_DELETE_SELF)
			puts("IN_DELETE_SELF");
		if (event->mask & IN_MOVE_SELF)
			puts("IN_MOVE_SELF");
		if (event->mask & IN_MOVED_FROM)
			puts("IN_MOVED_FROM");
		if (event->mask & IN_MOVED_TO)
			puts("IN_MOVED_TO");
		if (event->mask & IN_IGNORED)
			puts("IN_IGNORED");
#endif

		if (!ignore_event && (event->mask & INOTIFY_MASK))
			refresh = 1;
	}

	if (refresh) {
#ifdef INOTIFY_DEBUG
		puts("INOTIFY_REFRESH");
#endif
		free_dirlist();
		list_dir();
	} else {
#ifdef INOTIFY_DEBUG
		puts("INOTIFY_RESET");
#endif
		/* Reset the inotify watch list */
		reset_inotify();
	}

	return;
}
#elif defined(BSD_KQUEUE)
void
read_kqueue(void)
{
	struct kevent event_data[NUM_EVENT_SLOTS];
	memset((void *)event_data, '\0', sizeof(struct kevent)
			* NUM_EVENT_SLOTS);

	int i, refresh = 0;
	int count = kevent(kq, NULL, 0, event_data, 4096, &timeout);

	for (i = 0; i < count; i++) {
/*		if (event_data[i].fflags & NOTE_DELETE)
			puts("NOTE_DELETE");
		if (event_data[i].fflags & NOTE_WRITE)
			puts("NOTE_WRITE");
		if (event_data[i].fflags & NOTE_EXTEND)
			puts("NOTE_EXTEND");
		if (event_data[i].fflags & NOTE_ATTRIB)
			puts("NOTE_ATTRIB");
		if (event_data[i].fflags & NOTE_LINK)
			puts("NOTE_LINK");
		if (event_data[i].fflags & NOTE_RENAME)
			puts("NOTE_RENAME");
		if (event_data[i].fflags & NOTE_REVOKE)
			puts("NOTE_REVOKE"); */

		if (event_data[i].fflags & KQUEUE_FFLAGS) {
			refresh = 1;
			break;
		}
	}

	if (refresh) {
		free_dirlist();
		if (list_dir() != EXIT_SUCCESS)
			exit_code = EXIT_FAILURE;
		return;
	}

	if (event_fd >= 0) {
		close(event_fd);
		event_fd = -1;
		watch = 0;
	}
}
#endif

void
set_term_title(const char *str)
{
	char *tmp = (char *)NULL;
	tmp = home_tilde(str);

	printf("\033]2;%s - %s\007", PROGRAM_NAME, tmp ? tmp : str);
	fflush(stdout);

	free(tmp);
}

static int
unset_filter(void)
{
	if (!_filter) {
		puts(_("No filter set"));
		return EXIT_SUCCESS;
	}

	free(_filter);
	_filter = (char *)NULL;
	regfree(&regex_exp);
	puts(_("Filter unset"));
	filter_rev = 0;

	return EXIT_SUCCESS;
}

static int
compile_filter(void)
{
	if (regcomp(&regex_exp, _filter, REG_NOSUB | REG_EXTENDED) != EXIT_SUCCESS) {
		fprintf(stderr, _("%s: '%s': Invalid regular expression\n"),
		    PROGRAM_NAME, _filter);
		free(_filter);
		_filter = (char *)NULL;
		regfree(&regex_exp);
	} else {
		puts(_("New filter successfully set"));
	}

	return EXIT_SUCCESS;
}

int
filter_function(char *arg)
{
	if (!arg) {
		printf(_("Current filter: %c%s\n"), filter_rev ? '!' : 0,
				_filter ? _filter : "none");
		return EXIT_SUCCESS;
	}

	if (IS_HELP(arg)) {
		puts(_(FILTER_USAGE));
		return EXIT_SUCCESS;
	}

	if (*arg == 'u' && strcmp(arg, "unset") == 0)
		return unset_filter();

	if (_filter)
		free(_filter);

	regfree(&regex_exp);

	if (*arg == '!') {
		filter_rev = 1;
		arg++;
	} else {
		filter_rev = 0;
	}

	char *p = arg;
	if (*arg == '\'' || *arg == '"')
		p = remove_quotes(arg);

	_filter = savestring(p, strlen(p));

	return compile_filter();
}

/* Print either all tips (if ALL == 1) or just a random one (ALL == 0) */
void
print_tips(int all)
{
	const char *TIPS[] = {
		"Try the autocd and auto-open functions: run 'FILE' instead "
		"of 'open FILE' or 'cd FILE'",
		"Add a new entry to the mimelist file with 'mm edit' or F6",
		"Do not forget to take a look at the manpage",
		"Need more speed? Try the light mode (Alt-y)",
		"The Selection Box is shared among different instances of CliFM",
		"Select files here and there with the 's' command",
		"Use wildcards and regular expressions to select files: "
		"'s *.c' or 's .*\\.c$'",
		"ELN's and the 'sel' keyword work for shell commands as well: "
		"'file 1 sel'",
		"Press TAB to automatically expand an ELN: 's 2' -> TAB -> "
		"'s FILENAME'",
		"Easily copy everything in CWD into another directory: 's * "
		"&& c sel ELN/DIR'",
		"Use ranges (ELN-ELN) to easily move multiple files: 'm 3-12 "
		"ELN/DIR'",
		"Trash files with a simple 't ELN'",
		"Get mime information for a file: 'mm info ELN'",
		"If too many files are listed, try enabling the pager ('pg on')",
		"Once in the pager, go backwards pressing the keyboard shortcut "
		"provided by your terminal emulator",
		"Once in the pager, press 'q' to stop it",
		"Press 'Alt-l' to switch to long view mode",
		"Search for files using the slash command: '/*.png'",
		"The search function allows regular expressions: '/^c'",
		"Add a new bookmark by just entering 'bm a ELN/FILE'",
		"Use c, l, m, md, and r instead of cp, ln, mv, mkdir, and rm",
		"Access a remote file system using the 'net' command",
		"Manage default associated applications with the 'mime' command",
		"Go back and forth in the directory history with 'Alt-j' and 'Alt-k' "
		"or Shift-Left and Shift-Right",
		"Open a new instance of CliFM with the 'x' command: 'x ELN/DIR'",
		"Send a command directly to the system shell with ';CMD'",
		"Run the last executed command by just running '!!'",
		"Import aliases from file using 'alias import FILE'",
		"List available aliases by running 'alias'",
		"Create aliases to easily run your preferred commands",
		"Open and edit the configuration file with 'edit'",
		"Find a description for each CliFM command by running 'cmd'",
		"Print the currently used color codes list by entering 'cc'",
		"Press 'Alt-i' or 'Alt-.' to toggle hidden files on/off",
		"List mountpoints by pressing 'Alt-m'",
		"Disallow the use of shell commands with the -x option: 'clifm -x'",
		"Go to the root directory by just pressing 'Alt-r'",
		"Go to the home directory by just pressing 'Alt-e'",
		"Press 'F8' to open and edit the current color scheme",
		"Press 'F9' to open and edit the keybindings file",
		"Press 'F10' to open and edit the configuration file",
		"Press 'F11' to open and edit the bookmarks file",
		"Set the starting path: 'clifm PATH'",
		"Use the 'o' command to open files and directories: '12'",
		"Bypass the resource opener specifying an application: '12 "
		"leafpad'",
		"Open a file and send it to the background running '24&'",
		"Create a custom prompt editing the configuration file",
		"Customize color codes via 'cs edit' command (F6)",
		"Open the bookmarks manager by just pressing 'Alt-b'",
		"Chain commands using ; and &&: 's 2 7-10; r sel'",
		"Add emojis to the prompt by copying them to the Prompt line "
		"in the configuration file",
		"Create a new profile running 'pf add PROFILE' or 'clifm -P "
		"PROFILE'",
		"Switch profiles using 'pf set PROFILE'",
		"Delete a profile using 'pf del PROFILE'",
		"Copy selected files into CWD by just running 'v sel' or "
		"pressing Ctrl-Alt-v",
		"Use 'p ELN' to print file properties for ELN",
		"Deselect all selected files by pressing 'Alt-d'",
		"Select all files in CWD by pressing 'Alt-a'",
		"Jump to the Selection Box by pressing 'Alt-s'",
		"Restore trashed files using the 'u' command",
		"Empty the trash bin running 't clear'",
		"Press Alt-f to toggle list-folders-first on/off",
		"Use the 'fc' command to disable the files counter",
		"Take a look at the splash screen with the 'splash' command",
		"Have some fun trying the 'bonus' command",
		"Launch the default system shell in CWD using ':' or ';'",
		"Use 'Alt-z' and 'Alt-x' to switch sorting methods",
		"Reverse sorting order using the 'rev' option: 'st rev'",
		"Compress and decompress files using the 'ac' and 'ad' "
		"commands respectively",
		"Rename multiple files at once with the bulk rename function: "
		"'br *.txt'",
		"Need no more tips? Disable this feature in the configuration "
		"file",
		"Need root privileges? Launch a new instance of CLifM as root "
		"running the 'X' command",
		"Create custom commands and features using the 'actions' command",
		"Create a fresh configuration file by running 'edit reset'",
		"Use 'ln edit' (or 'le') to edit symbolic links",
		"Change default keyboard shortcuts by editing the keybindings file (F9)",
		"Keep in sight previous and next visited directories enabling the "
		"DirhistMap option in the configuration file",
		"Leave no traces at all running in stealth mode (-S)",
		"Pin a file via the 'pin' command and then use it with the "
		"period keyword (,). Ex: 'pin DIR' and then 'cd ,'",
		"Switch between color schemes using the 'cs' command",
		"Try the 'j' command to quickly navigate through visited "
		"directories",
		"Switch workspaces by pressing Alt-[1-4]",
		"Use the 'ws' command to list available workspaces",
		"Take a look at available plugins using the 'actions' command",
		"Space is not needed: enter 'p12' instead of 'p 12'",
		"When searching or selecting files, use the exclamation mark "
		"to reverse the meaning of a pattern",
		"Enable the TrashAsRm option to prevent accidental deletions",
		"Don't like ELN's? Disable them using the -e option",
		"Use the 'n' command to create multiple files and/or directories",
		"Customize your prompt by adding prompt commands via the 'edit' "
		"command (F10)",
		"Need git integration? Consult the manpage",
		"Accept a given suggestion by pressing the Right arrow key",
		"Accept only the first suggested word by pressing Alt-f or Alt-Right",
		"Enter 'c sel' to copy selected files into the current directory",
		"Take a look at available plugins via the 'actions' command",
		"Enable the FZF mode for a better TAB completion experience",
		"Press Alt-q to delete the last typed word",
		"Check ELN ranges by pressing TAB",
		"Operate on specific selected files by typing 'sel' and then TAB",
		"Use the 'ow' command to open a file with an specific application",
		"Use the 'mf' command to limit the amount of files listed on the screen",
		"Set a maximum file name length for listed files via the MaxFilenameLen "
		"option in the configuration file (F10)",
		"Use the 'm' command to interactively rename a file",
		"Set options on a per directory basis via the autocommands function",
		"Clean up non-ASCII file names using the 'bleach' command",
		"Running in an untrusted environment? Try the --secure-env and "
		"--secure-cmds options",
		"Get help for any internal command via the -h or --help parameters",
		NULL};

	size_t tipsn = (sizeof(TIPS) / sizeof(TIPS[0])) - 1;

	if (all == 1) {
		size_t i;
		for (i = 0; i < tipsn; i++)
			printf("%sTIP %zu%s: %s\n", colorize ? BOLD : "", i,
				colorize ? NC : "", TIPS[i]);

		return;
	}

	srand((unsigned int)time(NULL));
	printf("%sTIP%s: %s\n", colorize ? BOLD : "", colorize ? NC : "",
		TIPS[rand() % (int)tipsn]);
}

/* Open DIR in a new instance of the program (using TERM, set in the config
 * file, as terminal emulator) */
int
new_instance(char *dir, int sudo)
{
#if defined(__HAIKU__)
	UNUSED(dir); UNUSED(sudo);
	fprintf(stderr, _("%s: This function is not available on Haiku\n"),
			PROGRAM_NAME);
	return EXIT_FAILURE;
#elif defined(__OpenBSD__)
	UNUSED(dir); UNUSED(sudo);
	fprintf(stderr, _("%s: This function is not available on OpenBSD\n"),
			PROGRAM_NAME);
	return EXIT_FAILURE;
#else
	if (!term) {
		fprintf(stderr, _("%s: Default terminal not set. Use the "
				"configuration file to set one\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	if (!(flags & GUI)) {
		fprintf(stderr, _("%s: Function only available for graphical "
				"environments\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	/* Get absolute path of executable name of itself */
#if defined(__linux__)
	char *self = realpath("/proc/self/exe", NULL);

	if (!self) {
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	const int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
	char *self = malloc(PATH_MAX);
	size_t len = PATH_MAX;

	if (!self || sysctl(mib, 4, self, &len, NULL, 0) == -1) {
#endif
		fprintf(stderr, "%s: %s\n", PROGRAM_NAME, strerror(errno));
		return EXIT_FAILURE;
	}

	if (!dir) {
		free(self);
		return EXIT_FAILURE;
	}

	char *_sudo = (char *)NULL;
	if (sudo) {
		_sudo = get_sudo_path();
		if (!_sudo) {
			free(self);
			return EXIT_FAILURE;
		}
	}

	char *deq_dir = dequote_str(dir, 0);

	if (!deq_dir) {
		fprintf(stderr, _("%s: %s: Error dequoting file name\n"),
		    PROGRAM_NAME, dir);
		free(self);
		return EXIT_FAILURE;
	}

	struct stat attr;

	if (stat(deq_dir, &attr) == -1) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, deq_dir, strerror(errno));
		free(self);
		free(deq_dir);
		return EXIT_FAILURE;
	}

	if (!S_ISDIR(attr.st_mode)) {
		fprintf(stderr, _("%s: %s: Not a directory\n"), PROGRAM_NAME, deq_dir);
		free(self);
		free(deq_dir);
		return EXIT_FAILURE;
	}

	char *path_dir = (char *)NULL;

	if (*deq_dir != '/') {
		path_dir = (char *)xnmalloc(strlen(workspaces[cur_ws].path)
					+ strlen(deq_dir) + 2, sizeof(char));
		sprintf(path_dir, "%s/%s", workspaces[cur_ws].path, deq_dir);
		free(deq_dir);
	} else {
		path_dir = deq_dir;
	}

	char **tmp_term = (char **)NULL,
		 **tmp_cmd = (char **)NULL;

	if (strcntchr(term, ' ') != -1) {
		tmp_term = get_substr(term, ' ');

		if (tmp_term) {
			int i;
			for (i = 0; tmp_term[i]; i++);

			int num = i;
			tmp_cmd = (char **)xrealloc(tmp_cmd, ((size_t)i + (sudo ? 4 : 3))
													* sizeof(char *));
			for (i = 0; tmp_term[i]; i++) {
				tmp_cmd[i] = savestring(tmp_term[i], strlen(tmp_term[i]));
				free(tmp_term[i]);
			}
			free(tmp_term);

			i = num - 1;
			int plus = 1;

			if (sudo) {
				tmp_cmd[i + plus] = (char *)xnmalloc(strlen(self) + 1,
				    sizeof(char));
				strcpy(tmp_cmd[i + plus], _sudo);
				plus++;
			}

			tmp_cmd[i + plus] = (char *)xnmalloc(strlen(self) + 1, sizeof(char));
			strcpy(tmp_cmd[i + plus], self);
			plus++;
			tmp_cmd[i + plus] = (char *)xnmalloc(strlen(path_dir) + 1, sizeof(char));
			strcpy(tmp_cmd[i + plus], path_dir);
			plus++;
			tmp_cmd[i + plus] = (char *)NULL;
		}
	}

	int ret = -1;

	if (tmp_cmd) {
		ret = launch_execve(tmp_cmd, BACKGROUND, E_NOFLAG);
		size_t i;
		for (i = 0; tmp_cmd[i]; i++)
			free(tmp_cmd[i]);
		free(tmp_cmd);
	} else {
		fprintf(stderr, _("%s: No option specified for '%s'\n"
				"Trying '%s -e %s %s'\n"), PROGRAM_NAME, term,
				term, self, workspaces[cur_ws].path);
		if (sudo) {
			char *cmd[] = {term, "-e", _sudo, self, path_dir, NULL};
			ret = launch_execve(cmd, BACKGROUND, E_NOFLAG);
		} else {
			char *cmd[] = {term, "-e", self, path_dir, NULL};
			ret = launch_execve(cmd, BACKGROUND, E_NOFLAG);
		}
	}

	free(_sudo);
	free(path_dir);
	free(self);

	if (ret != EXIT_SUCCESS)
		fprintf(stderr, _("%s: Error lauching new instance\n"), PROGRAM_NAME);

	return ret;
#endif /* !__HAIKU__ */
// cppcheck-suppress syntaxError
}

int
alias_import(char *file)
{
	if (xargs.stealth_mode == 1) {
		printf("%s: alias: %s\n", PROGRAM_NAME, STEALTH_DISABLED);
		return EXIT_SUCCESS;
	}

	if (!file)
		return EXIT_FAILURE;

	char rfile[PATH_MAX] = "";
	rfile[0] = '\0';

	if (*file == '~') {
		char *file_exp = tilde_expand(file);
		if (!file_exp) {
			fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, file, strerror(errno));
			return EXIT_FAILURE;
		}

		if (realpath(file_exp, rfile) == NULL) {
			fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, file_exp,
					strerror(errno));
			free(file_exp);
			return EXIT_FAILURE;
		}
		free(file_exp);
	} else {
		if (realpath(file, rfile) == NULL) {
			fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, file, strerror(errno));
			return EXIT_FAILURE;
		}
	}

	if (rfile[0] == '\0') {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, file, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Open the file to import aliases from */
	int fd;
	FILE *fp = open_fstream_r(rfile, &fd);
	if (!fp) {
		fprintf(stderr, "b%s: '%s': %s\n", PROGRAM_NAME, rfile, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Open CliFM's config file as well */
	FILE *config_fp = fopen(config_file, "a");
	if (!config_fp) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, config_file,
		    strerror(errno));
		close_fstream(fp, fd);
		return EXIT_FAILURE;
	}

	size_t line_size = 0, i;
	char *line = (char *)NULL;
	size_t alias_found = 0, alias_imported = 0;
	int first = 1;

	while (getline(&line, &line_size, fp) > 0) {
		if (*line == 'a' && strncmp(line, "alias ", 6) == 0) {
			alias_found++;

			/* If alias name conflicts with some internal command,
			 * skip it */
			char *alias_name = strbtw(line, ' ', '=');
			if (!alias_name)
				continue;

			if (is_internal_c(alias_name)) {
				fprintf(stderr, _("%s: Alias conflicts with "
						"internal command\n"), alias_name);
				free(alias_name);
				continue;
			}

			char *p = line + 6; /* p points now to the beginning of the
			alias name (because "alias " == 6) */

			char *tmp = strchr(p, '=');
			if (!tmp) {
				free(alias_name);
				continue;
			}
			if (!*(++tmp)) {
				free(alias_name);
				continue;
			}
			if (*tmp != '\'' && *tmp != '"') {
				free(alias_name);
				continue;
			}

			*(tmp - 1) = '\0';
			/* If alias already exists, skip it too */
			int exists = 0;

			for (i = 0; i < aliases_n; i++) {
				if (*p == *aliases[i].name && strcmp(aliases[i].name, p) == 0) {
					exists = 1;
					break;
				}
			}

			*(tmp - 1) = '=';

			if (!exists) {
				if (first) {
					first = 0;
					fputs("\n\n", config_fp);
				}

				alias_imported++;

				/* Write the new alias into CliFM's config file */
				fputs(line, config_fp);
			} else {
				fprintf(stderr, _("%s: Alias already exists\n"),
				    alias_name);
			}

			free(alias_name);
		}
	}

	free(line);
	close_fstream(fp, fd);
	fclose(config_fp);

	/* No alias was found in FILE */
	if (alias_found == 0) {
		fprintf(stderr, _("%s: %s: No alias found\n"), PROGRAM_NAME,
		    rfile);
		return EXIT_FAILURE;
	}

	/* Aliases were found in FILE, but none was imported (either because
	 * they conflicted with internal commands or the alias already
	 * existed) */
	else {
		if (alias_imported == 0) {
			fprintf(stderr, _("%s: No alias imported\n"), PROGRAM_NAME);
			return EXIT_FAILURE;
		}
	}

	/* If some alias was found and imported, print the corresponding
	 * message and update the aliases array */
	if (alias_imported > 1) {
		printf(_("%s: %zu aliases were successfully imported\n"),
				PROGRAM_NAME, alias_imported);
	} else {
		printf(_("%s: 1 alias was successfully imported\n"), PROGRAM_NAME);
	}

	/* Add new aliases to the internal list of aliases */
	get_aliases();

	/* Add new aliases to the commands list for TAB completion */
	if (bin_commands) {
		for (i = 0; bin_commands[i]; i++)
			free(bin_commands[i]);
		free(bin_commands);
		bin_commands = (char **)NULL;
	}

	get_path_programs();
	return EXIT_SUCCESS;
}

/* Store last visited directory for the restore last path and the
 * cd on quit functions. Current workspace/path will be marked with an
 * asterisk. It will be read at startup by get_last_path */
void
save_last_path(void)
{
	if (!config_ok || !config_dir || !config_dir_gral)
		return;

	char *last_dir = (char *)xnmalloc(config_dir_len + 7, sizeof(char));
	sprintf(last_dir, "%s/.last", config_dir);

	FILE *last_fp = fopen(last_dir, "w");
	if (!last_fp) {
		fprintf(stderr, _("%s: Error saving last visited directory\n"), PROGRAM_NAME);
		free(last_dir);
		return;
	}

	size_t i;
	for (i = 0; i < MAX_WS; i++) {
		if (workspaces[i].path) {
			if ((size_t)cur_ws == i)
				fprintf(last_fp, "*%zu:%s\n", i, workspaces[i].path);
			else
				fprintf(last_fp, "%zu:%s\n", i, workspaces[i].path);
		}
	}

	fclose(last_fp);

	char *last_dir_tmp = xnmalloc(strlen(config_dir_gral) + 7, sizeof(char *));
	sprintf(last_dir_tmp, "%s/.last", config_dir_gral);

	if (cd_on_quit == 1) {
		char *cmd[] = {"cp", "-p", last_dir, last_dir_tmp, NULL};
		launch_execve(cmd, FOREGROUND, E_NOFLAG);
	} else { /* If not cd on quit, remove the file */
		char *cmd[] = {"rm", "-f", last_dir_tmp, NULL};
		launch_execve(cmd, FOREGROUND, E_NOFLAG);
	}

	free(last_dir_tmp);
	free(last_dir);
}

char *
parse_usrvar_value(const char *str, const char c)
{
	if (c == '\0' || !str)
		return (char *)NULL;

	/* Get whatever comes after c */
	char *tmp = strchr(str, c);
	if (!tmp || !*(++tmp))
		return (char *)NULL;

	/* Remove leading quotes */
	if (*tmp == '"' || *tmp == '\'')
		tmp++;

	/* Remove trailing spaces, tabs, new line chars, and quotes */
	size_t tmp_len = strlen(tmp), i;

	for (i = tmp_len - 1; tmp[i] && i > 0; i--) {
		if (tmp[i] != ' ' && tmp[i] != '\t' && tmp[i] != '"' && tmp[i] != '\''
		&& tmp[i] != '\n')
			break;
		else
			tmp[i] = '\0';
	}

	if (!*tmp)
		return (char *)NULL;

	char *buf = savestring(tmp, strlen(tmp));
	return buf;
}

int
create_usr_var(char *str)
{
	if (!str || !*str)
		return EXIT_FAILURE;

	char *p = strchr(str, '=');
	if (!p || p == str)
		return EXIT_FAILURE;

	*p = '\0';
	char *name = (char *)xnmalloc((size_t)(p - str + 1), sizeof(char));
	strcpy(name, str);
	*p = '=';

	char *value = parse_usrvar_value(str, '=');

	if (!value) {
		free(name);
		fprintf(stderr, _("%s: Error getting variable value\n"),
		    PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	usr_var = xrealloc(usr_var, (size_t)(usrvar_n + 2) * sizeof(struct usrvar_t));
	usr_var[usrvar_n].name = savestring(name, strlen(name));
	usr_var[usrvar_n].value = savestring(value, strlen(value));
	usrvar_n++;

	usr_var[usrvar_n].name = (char *)NULL;
	usr_var[usrvar_n].value = (char *)NULL;

	free(name);
	free(value);
	return EXIT_SUCCESS;
}

/* Custom POSIX implementation of GNU asprintf() modified to log program
 * messages. MSG_TYPE is one of: 'e', 'w', 'n', or zero (meaning this
 * latter that no message mark (E, W, or N) will be added to the prompt).
 * PROMPT tells whether to print the message immediately before the
 * prompt or rather in place. Based on littlstar's xasprintf
 * implementation:
 * https://github.com/littlstar/asprintf.c/blob/master/asprintf.c*/
__attribute__((__format__(__printf__, 3, 0)))
/* We use __attribute__ here to silence clang warning: "format string is
 * not a string literal" */
int
_err(int msg_type, int prompt, const char *format, ...)
{
	va_list arglist, tmp_list;

	va_start(arglist, format);
	va_copy(tmp_list, arglist);
	int size = vsnprintf((char *)NULL, 0, format, tmp_list);
	va_end(tmp_list);

	if (size < 0) {
		va_end(arglist);
		return EXIT_FAILURE;
	}

	char *buf = (char *)xnmalloc((size_t)size + 1, sizeof(char));

	vsprintf(buf, format, arglist);
	va_end(arglist);

	/* If the new message is the same as the last message, skip it */
	if (msgs_n && strcmp(messages[msgs_n - 1], buf) == 0) {
		free(buf);
		return EXIT_SUCCESS;
	}

	if (buf) {
		if (msg_type) {
			switch (msg_type) {
			case 'e': pmsg = ERROR; break;
			case 'w': pmsg = WARNING; break;
			case 'n': pmsg = NOTICE; break;
			default: pmsg = NOMSG;
			}
		}

		log_msg(buf, (prompt == 1) ? PRINT_PROMPT : NOPRINT_PROMPT);
		free(buf);
		return EXIT_SUCCESS;
	}

	return EXIT_FAILURE;
}

/* Set STR as the program current shell */
/*int
set_shell(char *str)
{
	if (!str || !*str)
		return EXIT_FAILURE;

	// IF no slash in STR, check PATH env variable for a file named STR
	// and get its full path
	char *full_path = (char *)NULL;

	if (strcntchr(str, '/') == -1)
		full_path = get_cmd_path(str);

	char *tmp = (char *)NULL;

	if (full_path)
		tmp = full_path;
	else
		tmp = str;

	if (access(tmp, X_OK) == -1) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, tmp, strerror(errno));
		return EXIT_FAILURE;
	}

	if (user.shell)
		free(user.shell);

	user.shell = savestring(tmp, strlen(tmp));
	printf(_("Successfully set '%s' as %s default shell\n"), user.shell,
	    PROGRAM_NAME);

	if (full_path)
		free(full_path);
	return EXIT_SUCCESS;
} */

int
free_remotes(int exit)
{
	if (exit)
		autounmount_remotes();

	size_t i;
	for (i = 0; i < remotes_n; i++) {
		free(remotes[i].name);
		free(remotes[i].desc);
		free(remotes[i].mountpoint);
		free(remotes[i].mount_cmd);
		free(remotes[i].unmount_cmd);		
	}
	free(remotes);
	remotes_n = 0;

	return EXIT_SUCCESS;
}

/* This function is called by atexit() to clear whatever is there at exit
 * time and avoid thus memory leaks */
void
free_stuff(void)
{
	int i = 0;

#ifdef LINUX_INOTIFY
	/* Shutdown inotify */
	if (inotify_wd >= 0)
		inotify_rm_watch(inotify_fd, inotify_wd);
	if (inotify_fd != UNSET)
		close(inotify_fd);
#elif defined(BSD_KQUEUE)
	if (event_fd >= 0)
		close(event_fd);
	if (kq != UNSET)
		close(kq);
#endif

	i = (int)autocmds_n;
	while (--i >= 0) {
		free(autocmds[i].pattern);
		free(autocmds[i].cmd);
	}
	free(autocmds);

	free_remotes(1);

	if (xargs.stealth_mode != 1)
		save_jumpdb();

	save_dirhist();

	if (restore_last_path || cd_on_quit)
		save_last_path();

	free(alt_profile);
	free_bookmarks();
	free(encoded_prompt);
	free_dirlist();
	free(opener);
	free(wprompt_str);
	free(fzftab_options);

	if (stdin_tmp_dir) {
		char *rm_cmd[] = {"rm", "-rd", "--", stdin_tmp_dir, NULL};
		launch_execve(rm_cmd, FOREGROUND, E_NOFLAG);
		free(stdin_tmp_dir);
	}

	i = (int)cschemes_n;
	while (i-- > 0)
		free(color_schemes[i]);
	free(color_schemes);
	free(usr_cscheme);

	if (jump_db) {
		i = (int)jump_n;
		while (--i >= 0)
			free(jump_db[i].path);
		free(jump_db);
	}

	if (pinned_dir)
		free(pinned_dir);

	if (_filter) {
		regfree(&regex_exp);
		free(_filter);
	}

	if (eln_as_file_n)
		free(eln_as_file);

	if (ext_colors_n)
		free(ext_colors_len);


	if (profile_names) {
		for (i = 0; profile_names[i]; i++)
			free(profile_names[i]);
		free(profile_names);
	}

	if (sel_n > 0) {
		i = (int)sel_n;
		while (--i >= 0)
			free(sel_elements[i]);
		free(sel_elements);
	}
	free(sel_devino);

	if (bin_commands) {
		i = (int)path_progsn;
		while (--i >= 0)
			free(bin_commands[i]);
		free(bin_commands);
	}

	if (paths) {
		i = (int)path_n;
		while (--i >= 0)
			free(paths[i]);
		free(paths);
	}

	if (cdpaths) {
		i = (int)cdpath_n;
		while (--i >= 0)
			free(cdpaths[i]);
		free(cdpaths);
	}

	if (history) {
		i = (int)current_hist_n;
		while (--i >= 0)
			free(history[i]);
		free(history);
	}

	if (argv_bk) {
		i = argc_bk;
		while (--i >= 0)
			free(argv_bk[i]);
		free(argv_bk);
	}

	if (dirhist_total_index) {
		i = (int)dirhist_total_index;
		while (--i >= 0)
			free(old_pwd[i]);
		free(old_pwd);
	}

	i = (int)aliases_n;
	while (--i >= 0) {
		free(aliases[i].name);
		free(aliases[i].cmd);
	}
	free(aliases);

	i = (int)kbinds_n;
	while (--i >= 0) {
		free(kbinds[i].function);
		free(kbinds[i].key);
	}
	free(kbinds);

	i = (int)usrvar_n;
	while (--i >= 0) {
		free(usr_var[i].name);
		free(usr_var[i].value);
	}
	free(usr_var);

	i = (int)actions_n;
	while (--i >= 0) {
		free(usr_actions[i].name);
		free(usr_actions[i].value);
	}
	free(usr_actions);

	i = (int)prompt_cmds_n;
	while (--i >= 0)
		free(prompt_cmds[i]);
	free(prompt_cmds);

	if (msgs_n) {
		i = (int)msgs_n;
		while (--i >= 0)
			free(messages[i]);
		free(messages);
	}

	if (ext_colors_n) {
		i = (int)ext_colors_n;
		while (--i >= 0)
			free(ext_colors[i]);
		free(ext_colors);
	}

	if (workspaces && workspaces[0].path) {
		i = MAX_WS;
		while (--i >= 0)
			if (workspaces[i].path)
				free(workspaces[i].path);
		free(workspaces);
	}

	free(actions_file);
	free(bm_file);
	free(data_dir);
	free(colors_dir);
	free(config_dir_gral);
	free(config_dir);
	free(config_file);
	free(dirhist_file);
	free(hist_file);
	free(kbinds_file);
	free(log_file);
	free(mime_file);
	free(msg_log_file);
	free(plugins_dir);
	free(profile_file);
	free(remotes_file);

#ifndef _NO_SUGGESTIONS
	free(suggestion_buf);
	free(suggestion_strategy);
#endif

	free(sel_file);
	free(tmp_dir);
	free(user.name);
	free(user.home);
	free(user.shell);
#ifndef _NO_TRASH
	free(trash_dir);
	free(trash_files_dir);
	free(trash_info_dir);
#endif

	/* Restore the color of the running terminal */
	fputs("\x1b[0;39;49m", stdout);
}

void
set_signals_to_ignore(void)
{
	/* signal(SIGINT, signal_handler); C-c */
	signal(SIGINT, SIG_IGN);  /* C-c */
	signal(SIGQUIT, SIG_IGN); /* C-\ */
	signal(SIGTSTP, SIG_IGN); /* C-z */
	/* signal(SIGTERM, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN); */
}

void
handle_stdin()
{
	/* If files are passed via stdin, we need to disable restore
	 * last path in order to correctly understand relative paths */
	restore_last_path = 0;

	/* Max input size: 512 * (512 * 1024)
	 * 512 chunks of 524288 bytes (512KiB) each
	 * == (65535 * PATH_MAX)
	 * == 262MiB of data ((65535 * PATH_MAX) / 1024) */

	size_t chunk = 512 * 1024,
		   chunks_n = 1,
		   total_len = 0,
		   max_chunks = 512;

	ssize_t input_len = 0;

	/* Initial buffer allocation == 1 chunk */
	char *buf = (char *)xnmalloc(chunk, sizeof(char));

	while (chunks_n < max_chunks) {
		input_len = read(STDIN_FILENO, buf + total_len, chunk);

		/* Error */
		if (input_len < 0) {
			free(buf);
			return;
		}

		/* Nothing else to be read */
		if (input_len == 0)
			break;

		total_len += (size_t)input_len;
		chunks_n++;

		/* Append a new chunk of memory to the buffer */
		buf = (char *)xrealloc(buf, (chunks_n + 1) * chunk);
	}

	if (total_len == 0)
		goto FREE_N_EXIT;

	/* Null terminate the input buffer */
	buf[total_len] = '\0';

	/* Create tmp dir to store links to files */
	char *rand_ext = gen_rand_str(6);
	if (!rand_ext)
		goto FREE_N_EXIT;

	if (tmp_dir) {
		stdin_tmp_dir = (char *)xnmalloc(strlen(tmp_dir) + 14, sizeof(char));
		sprintf(stdin_tmp_dir, "%s/.clifm%s", tmp_dir, rand_ext);
	} else {
		stdin_tmp_dir = (char *)xnmalloc(P_tmpdir_len + 14, sizeof(char));
		sprintf(stdin_tmp_dir, "%s/.clifm%s", P_tmpdir, rand_ext);
	}

	free(rand_ext);

	char *cmd[] = {"mkdir", "-p", stdin_tmp_dir, NULL};
	if (launch_execve(cmd, FOREGROUND, E_NOFLAG) != EXIT_SUCCESS)
		goto FREE_N_EXIT;

	/* Get CWD: we need it to prepend it to relative paths */
	char *cwd = (char *)NULL;
	cwd = getcwd(NULL, 0);
	if (!cwd)
		goto FREE_N_EXIT;

	/* Get substrings from buf */
	char *p = buf, *q = buf;

	while (*p) {
		if (!*p || *p == '\n') {
			*p = '\0';

			/* Create symlinks (in tmp dir) to each valid file in
			 * the buffer */

			/* If file does not exist */
			struct stat attr;
			if (lstat(q, &attr) == -1)
				continue;

			/* Construct source and destiny files */
			char *tmp_file = strrchr(q, '/');
			if (!tmp_file || !*(++tmp_file))
				tmp_file = q;

			char source[PATH_MAX];
			if (*q != '/' || !q[1])
				snprintf(source, PATH_MAX, "%s/%s", cwd, q);
			else
				xstrsncpy(source, q, PATH_MAX);

			char dest[PATH_MAX + 1];
			snprintf(dest, PATH_MAX, "%s/%s", stdin_tmp_dir, tmp_file);

			if (symlink(source, dest) == -1)
				_err('w', PRINT_PROMPT, "ln: '%s': %s\n", q, strerror(errno));

			q = p + 1;
		}

		p++;
	}

	/* chdir to tmp dir and update path var */
	if (xchdir(stdin_tmp_dir, SET_TITLE) == -1) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, stdin_tmp_dir,
		    strerror(errno));

		char *rm_cmd[] = {"rm", "-drf", stdin_tmp_dir, NULL};
		launch_execve(rm_cmd, FOREGROUND, E_NOFLAG);

		free(cwd);
		goto FREE_N_EXIT;
	}

	free(cwd);

	if (workspaces[cur_ws].path)
		free(workspaces[cur_ws].path);

	workspaces[cur_ws].path = savestring(stdin_tmp_dir, strlen(stdin_tmp_dir));
	goto FREE_N_EXIT;

FREE_N_EXIT:
	free(buf);

	/* Go back to tty */
	dup2(STDOUT_FILENO, STDIN_FILENO);

	if (autols) {
		free_dirlist();
		list_dir();
		add_to_dirhist(workspaces[cur_ws].path);
	}

	return;
}

/* Save pinned in a file */
static int
save_pinned_dir(void)
{
	if (!pinned_dir || !config_ok)
		return EXIT_FAILURE;

	char *pin_file = (char *)xnmalloc(config_dir_len + 7, sizeof(char));
	sprintf(pin_file, "%s/.pin", config_dir);

	FILE *fp = fopen(pin_file, "w");
	if (!fp) {
		fprintf(stderr, _("%s: Error storing pinned directory\n"), PROGRAM_NAME);
	} else {
		fprintf(fp, "%s", pinned_dir);
		fclose(fp);
	}

	free(pin_file);

	return EXIT_SUCCESS;
}

int
pin_directory(char *dir)
{
	if (!dir || !*dir)
		return EXIT_FAILURE;

	struct stat attr;
	if (lstat(dir, &attr) == -1) {
		fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, dir, strerror(errno));
		return EXIT_FAILURE;
	}

	if (pinned_dir) {
		free(pinned_dir);
		pinned_dir = (char *)NULL;
	}

	/* If absolute path */
	if (*dir == '/') {
		pinned_dir = savestring(dir, strlen(dir));
	} else { /* If relative path */
		if (strcmp(workspaces[cur_ws].path, "/") == 0) {
			pinned_dir = (char *)xnmalloc(strlen(dir) + 2, sizeof(char));
			sprintf(pinned_dir, "/%s", dir);
		} else {
			pinned_dir = (char *)xnmalloc(strlen(dir)
						+ strlen(workspaces[cur_ws].path) + 2, sizeof(char));
			sprintf(pinned_dir, "%s/%s", workspaces[cur_ws].path, dir);
		}
	}

	if (xargs.stealth_mode == 1 || save_pinned_dir() == EXIT_SUCCESS)
		goto END;

	free(pinned_dir);
	pinned_dir = (char *)NULL;
	return EXIT_FAILURE;

END:
	printf(_("%s: Succesfully pinned '%s'\n"), PROGRAM_NAME, dir);
	return EXIT_SUCCESS;
}

int
unpin_dir(void)
{
	if (!pinned_dir) {
		printf(_("%s: No pinned file\n"), PROGRAM_NAME);
		return EXIT_SUCCESS;
	}

	if (config_dir && xargs.stealth_mode != 1) {
		int cmd_error = 0;
		char *pin_file = (char *)xnmalloc(config_dir_len + 7, sizeof(char));
		sprintf(pin_file, "%s/.pin", config_dir);
		if (unlink(pin_file) == -1) {
			fprintf(stderr, "%s: %s: %s\n", PROGRAM_NAME, pin_file,
			    strerror(errno));
			cmd_error = 1;
		}

		free(pin_file);
		if (cmd_error)
			return EXIT_FAILURE;
	}

	printf(_("Succesfully unpinned %s\n"), pinned_dir);

	free(pinned_dir);
	pinned_dir = (char *)NULL;
	return EXIT_SUCCESS;
}

int
hidden_function(char **comm)
{
	int exit_status = EXIT_SUCCESS;

	if (strcmp(comm[1], "status") == 0) {
		printf(_("%s: Hidden files %s\n"), PROGRAM_NAME,
		    (show_hidden) ? _("enabled") : _("disabled"));
	} else if (strcmp(comm[1], "off") == 0) {
		if (show_hidden == 1) {
			show_hidden = 0;

			if (autols) {
				free_dirlist();
				exit_status = list_dir();
			}
		}
	} else if (strcmp(comm[1], "on") == 0) {
		if (show_hidden == 0) {
			show_hidden = 1;

			if (autols) {
				free_dirlist();
				exit_status = list_dir();
			}
		}
	} else {
		fprintf(stderr, "%s\n", _(HF_USAGE));
	}

	return exit_status;
}

/* Instead of recreating here the commands description, just jump to the
 * corresponding section in the manpage */
int
list_commands(void)
{
	char cmd[PATH_MAX];
	snprintf(cmd, PATH_MAX - 1, "export PAGER=\"less -p '^[0-9]+\\.[[:space:]]COMMANDS'\"; man %s\n",
			PNL);
	if (launch_execle(cmd) != EXIT_SUCCESS)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

/* Retrieve pager path, first from PAGER, then try less(1), and finally
 * more(1). If none is found returns NULL */
static char *
get_pager(void)
{
	char *_pager = (char *)NULL;

	char *p = getenv("PAGER");
	if (p)
		_pager = savestring(p, strlen(p));
	else {
		p = get_cmd_path("less");
		if (p) {
			_pager = savestring(p, strlen(p));
			free(p);
		} else {
			p = get_cmd_path("more");
			if (p) {
				_pager = savestring(p, strlen(p));
				free(p);
			}
		}
	}

	return _pager;
}

int
quick_help(void)
{
#ifdef __HAIKU__
	printf("%s                                %s\n\n%s",
		ASCII_LOGO, PROGRAM_NAME, QUICK_HELP);
	puts(_("\nNOTE: Some keybindings on Haiku might differ. Take a look "
		"at your current keybindings via the 'kb' command"));
	return EXIT_SUCCESS;
#else
	char *_pager = get_pager();
	if (!*_pager) {
		fprintf(stderr, _("%s: Unable to find any pager\n"), PROGRAM_NAME);
		return EXIT_FAILURE;
	}

	char tmp_file[PATH_MAX];
	if (xargs.stealth_mode == 1)
		snprintf(tmp_file, PATH_MAX - 1, "%s/%s", P_tmpdir, TMP_FILENAME);
	else
		snprintf(tmp_file, PATH_MAX - 1, "%s/%s", tmp_dir, TMP_FILENAME);

	int fd = mkstemp(tmp_file);
	if (fd == -1) {
		fprintf(stderr, "%s: Error creating temporary file\n", PROGRAM_NAME);
		free(_pager);
		return EXIT_FAILURE;
	}

	FILE *fp;
	fp = open_fstream_w(tmp_file, &fd);
	if (!fp) {
		fprintf(stderr, "%s: fopen: %s: %s\n", PROGRAM_NAME, tmp_file, strerror(errno));
		free(_pager);
		return EXIT_FAILURE;
	}

	dprintf(fd, "%s                                %s\n\n%s",
			ASCII_LOGO, PROGRAM_NAME, QUICK_HELP);

	int ret;
	if (*_pager == 'l' && strcmp(_pager, "less") == 0) {
		char *cmd[] = {_pager, "-FIRX", tmp_file, NULL};
		ret = launch_execve(cmd, FOREGROUND, E_NOFLAG);
	} else {
		char *cmd[] = {_pager, tmp_file, NULL};
		ret = launch_execve(cmd, FOREGROUND, E_NOFLAG);
	}
	unlink(tmp_file);

	close_fstream(fp, fd);
	free(_pager);

	if (ret != EXIT_SUCCESS)
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
#endif
}

void
help_function(void)
{
	fputs("\x1b[0m", stdout);

	printf("%s\n", ASCII_LOGO);

	printf(_("%s %s (%s), by %s\n"), PROGRAM_NAME, VERSION, DATE, AUTHOR);

	printf("\nUSAGE: %s %s\n%s%s", PNL, GRAL_USAGE, _(SHORT_OPTIONS),
			_(LONG_OPTIONS));

	puts(_(CLIFM_COMMANDS));
	puts(_(CLIFM_KEYBOARD_SHORTCUTS));
	puts(_(HELP_END_NOTE));
}

void
free_software(void)
{
	puts(_(FREE_SOFTWARE));
}

void
version_function(void)
{
	printf(_("%s %s (%s), by %s\nContact: %s\nWebsite: "
		 "%s\nLicense: %s\n"), PROGRAM_NAME, VERSION, DATE,
	    AUTHOR, CONTACT, WEBSITE, LICENSE);
}

void
splash(void)
{
	printf("\n%s%s\n\n%s%s\t\t       %s%s\n           %s\n",
		colorize ? D_CYAN : "", ASCII_LOGO_BIG, df_c,
		BOLD, df_c, PROGRAM_NAME, _(PROGRAM_DESC));

	if (splash_screen) {
		printf(_("\n            Press any key to continue... "));
		xgetchar();
		putchar('\n');
	} else {
		putchar('\n');
	}
}

void
bonus_function(void)
{
	char *phrases[] = {
	    "\"Vamos Boca Juniors Carajo!\" (La mitad + 1)",
	    "\"Hey! Look behind you! A three-headed monkey! (G. Threepweed)",
	    "\"Free as in free speech, not as in free beer\" (R. M. S)",
	    "\"Nothing great has been made in the world without passion\" (G. W. F. Hegel)",
	    "\"Simplicity is the ultimate sophistication\" (Leo Da Vinci)",
	    "\"Yo vendí semillas de alambre de púa, al contado, y me lo agradecieron\" (Marquitos, 9 Reinas)",
	    "\"I'm so happy, because today I've found my friends, they're in my head\" (K. D. Cobain)",
	    "\"The best code is written with the delete key (Someone, somewhere, sometime)",
	    "\"I'm selling these fine leather jackets (Indy)",
	    "\"I pray to God to make me free of God\" (Meister Eckhart)",
	    "¡Truco y quiero retruco mierda!",
	    "The only truth is that there is no truth",
	    "\"This is a lie\" (The liar paradox)",
	    "\"There are two ways to write error-free programs; only the third one works\" (Alan J. Perlis)",
	    "The man who sold the world was later sold by the big G",
	    "A programmer is always one year older than herself",
	    "A smartphone is anything but smart",
	    "And he did it: he killed the one who killed him",
	    ">++('>",
	    ":(){:|:&};:",
	    "Keep it simple, stupid",
	    "If ain't broken, brake it",
	    "\"I only know that I know nothing\" (Socrates)",
	    "(Learned) Ignorance is the true outcome of wisdom (Nicholas "
	    "of Cusa)",
	    "True intelligence is about questions, not about answers",
	    "Humanity is just an arrow released towards God",
	    "Buzz is right: infinity is our only and ultimate goal",
	    "That stain will never ever be erased (La 12)",
	    "\"A work of art is never finished, but adandoned\" (J. L. Guerrero)",
	    "At the beginning, software was hardware; but today hardware is "
	    "being absorbed by software",
	    "\"What you're referring to as Linux, is in fact, GNU/Linux\" (RMS)",
	    "\"Given enough eyeballs, all bugs are shallow.\" (Linus's law)",
	    "\"We're gonna need a bigger boat.\" (Caleb)",
	    "\"Ein Verletzter, Alarm, Alarm!\"",
	    NULL};

	size_t num = (sizeof(phrases) / sizeof(phrases[0])) - 1;
	srand((unsigned int)time(NULL));
	puts(phrases[rand() % (int)num]);
}
