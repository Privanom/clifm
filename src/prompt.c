/* prompt.c -- functions controlling the appearance and behaviour of the prompt */

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

#include <stdio.h>
#include <string.h>
#if !defined(__HAIKU__) && !defined(__OpenBSD__)
# include <wordexp.h>
#endif
#include <readline/readline.h>

#include "aux.h"
#include "exec.h"
#include "history.h"
#include "init.h"
#include "listing.h"
#include "misc.h"
#include "navigation.h"
#include "prompt.h"
#include "sanitize.h"

#ifndef _NO_TRASH
# include "trash.h"
#else
# include <time.h>
#endif

#define CTLESC '\001'
#define CTLNUL '\177'

#define __WS_STR_LEN sizeof(int) + 6 + (MAX_COLOR + 2) * 2

#define ROOT_IND "\001\x1b[1;31m\002R\001\x1b[0m\002"
#define ROOT_IND_NO_COLOR "\001\x1b[1m\002R\001\x1b[0m\002"
#define ROOT_IND_SIZE 17
#define STEALTH_IND "S\001\x1b[0m\002"
#define STEALTH_IND_SIZE MAX_COLOR + 7 + 1

#define EMERGENCY_PROMPT_MSG "Error decoding prompt line. Using an \
emergency prompt"
#define EMERGENCY_PROMPT "\001\x1b[0m\002> "
#define EMERGENCY_PROMPT_LEN 8

/* Flag macros to generate files statistic string for the prompt */
#define STATS_DIR      0
#define STATS_REG      1
#define STATS_EXE      2
#define STATS_HIDDEN   3
#define STATS_SUID     4
#define STATS_SGID     5
#define STATS_FIFO     6
#define STATS_SOCK     7
#define STATS_BLK      8
#define STATS_CHR      9
#define STATS_CAP      10
#define STATS_LNK      11
#define STATS_BROKEN_L 12 /* Broken link */
#define STATS_MULTI_L  13 /* Multi-link */
#define STATS_OTHER_W  14 /* Other writable */
#define STATS_STICKY   15
#define STATS_EXTENDED 16 /* Extended attributes (acl) */
#define STATS_UNKNOWN  17
#define STATS_UNSTAT   18

/* Size of the indicator for msgs, trash, and sel */
#define N_IND MAX_COLOR + 1 + sizeof(size_t) + 6 + 1 + 13
/* Color + 1 letter + plus unsigned integer + RL_NC size + nul char */

static inline char *
gen_time(const int c)
{
	char *temp = (char *)NULL;
	time_t rawtime = time(NULL);
	struct tm tm;
	localtime_r(&rawtime, &tm);

	if (c == 't') {
		char time[9] = "";
		strftime(time, sizeof(time), "%H:%M:%S", &tm);
		temp = savestring(time, sizeof(time));
	} else if (c == 'T') {
		char time[9] = "";
		strftime(time, sizeof(time), "%I:%M:%S", &tm);
		temp = savestring(time, sizeof(time));
	} else if (c == 'A') {
		char time[6] = "";
		strftime(time, sizeof(time), "%H:%M", &tm);
		temp = savestring(time, sizeof(time));
	} else if (c == '@') {
		char time[12] = "";
		strftime(time, sizeof(time), "%I:%M:%S %p", &tm);
		temp = savestring(time, sizeof(time));
	} else { /* c == 'd' */
		char time[12] = "";
		strftime(time, sizeof(time), "%a %b %d", &tm);
		temp = savestring(time, sizeof(time));
	}

	return temp;
}

static inline char *
get_dir_basename(char *_path)
{
	char *temp = (char *)NULL,
		 *ret = (char *)NULL;

	/* If not root dir (/), get last path component */
	if (!(*_path == '/' && !*(_path + 1)))
		ret = strrchr(_path, '/');

	if (!ret)
		temp = savestring(_path, strlen(_path));
	else
		temp = savestring(ret + 1, strlen(ret + 1));

	return temp;
}

static inline char *
reduce_path(char *_path)
{
	char *temp = (char *)NULL;

	if (strlen(_path) > (size_t)max_path) {
		char *ret = strrchr(_path, '/');
		if (!ret)
			temp = savestring(_path, strlen(_path));
		else
			temp = savestring(ret + 1, strlen(ret + 1));
	} else {
		temp = savestring(_path, strlen(_path));
	}

	return temp;
}

static inline char *
gen_pwd(int c)
{
	char *temp = (char *)NULL;
	/* Reduce HOME to "~" */
	int free_tmp_path = 0;
	char *tmp_path = (char *)NULL;

	if (strncmp(workspaces[cur_ws].path, user.home, user.home_len) == 0)
		tmp_path = home_tilde(workspaces[cur_ws].path);

	if (!tmp_path)
		tmp_path = workspaces[cur_ws].path;
	else
		free_tmp_path = 1;

	if (c == 'W') {
		temp = get_dir_basename(tmp_path);
	} else if (c == 'p') {
		temp = reduce_path(tmp_path);
	} else { /* If c == 'w' */
		temp = savestring(tmp_path, strlen(tmp_path));
	}

	if (free_tmp_path)
		free(tmp_path);

	return temp;
}

static inline char *
gen_workspace(void)
{
	char *temp = (char *)NULL;
	char s[__WS_STR_LEN];
	char *cl = (char *)NULL;

	switch(cur_ws + 1) {
	case 1: cl = ws1_c; break;
	case 2: cl = ws2_c; break;
	case 3: cl = ws3_c; break;
	case 4: cl = ws4_c; break;
	case 5: cl = ws5_c; break;
	case 6: cl = ws6_c; break;
	case 7: cl = ws7_c; break;
	case 8: cl = ws8_c; break;
	default: break;
	}
	snprintf(s, __WS_STR_LEN, "%s%d\001%s\002", cl, cur_ws + 1, df_c);
	temp = savestring(s, strlen(s));

	return temp;
}

static inline char *
gen_exit_status(void)
{
	size_t code_len = (size_t)DIGINUM(exit_code);

	char *temp = (char *)xnmalloc(code_len + 12 + MAX_COLOR, sizeof(char));
	sprintf(temp, "\001%s\002%d\001%s\002",
			(exit_code == 0) ? (colorize ? _GREEN : "")
			: (colorize ? _RED : ""), exit_code, df_c);

	return temp;
}

static inline char *
gen_escape_char(char **line, int *c)
{
	(*line)++;
	*c = 0;
	/* 27 (dec) == 033 (octal) == 0x1b (hex) == \e */
	char *temp = (char *)xnmalloc(2, sizeof(char));
	*temp = '\033';
	temp[1] = '\0';

	return temp;
}

static inline char *
gen_octal(char **line, int *c)
{
	char octal_string[4];
	int n;

	xstrsncpy(octal_string, *line, 3);
	octal_string[3] = '\0';

	n = read_octal(octal_string);
	char *temp = (char *)xnmalloc(3, sizeof(char));

	if (n == CTLESC || n == CTLNUL) {
		*line += 3;
		temp[0] = CTLESC;
		temp[1] = (char)n;
		temp[2] = '\0';
	} else if (n == -1) {
		temp[0] = '\\';
		temp[1] = '\0';
	} else {
		*line += 3;
		temp[0] = (char)n;
		temp[1] = '\0';
	}

	*c = 0;

	return temp;
}

static inline char *
gen_profile(void)
{
	char *temp = (char *)NULL;

	if (!alt_profile)
		temp = savestring("default", 7);
	else
		temp = savestring(alt_profile, strlen(alt_profile));

	return temp;
}

static inline char *
gen_user_name(void)
{
	char *temp = (char *)NULL;

	if (!user.name)
		temp = savestring("?", 1);
	else
		temp = savestring(user.name, strlen(user.name));

	return temp;
}

static inline char *
gen_hostname(const int c)
{
	char *temp = savestring(hostname, strlen(hostname));
	if (c != 'h')
		return temp;

	char *ret = strchr(temp, '.');
	if (ret)
		*ret = '\0';

	return temp;
}

static inline char *
gen_user_flag(void)
{
	char *temp = (char *)xnmalloc(2, sizeof(char));

	if ((flags & ROOT_USR))
		*temp = '#';
	else
		*temp = '$';

	temp[1] = '\0';
	return temp;
}

static inline char *
gen_mode(void)
{
	char *temp = (char *)xnmalloc(2, sizeof(char));
	if (light_mode) {
		*temp = 'L';
		temp[1] = '\0';
	} else {
		*temp = '\0';
	}

	return temp;
}

static inline char*
gen_misc(const int c)
{
	char *temp = (char *)xnmalloc(2, sizeof(char));

	if (c == 'n')
		*temp = '\n';
	else if (c == 'r')
		*temp = '\r';
	else
		*temp = '\a';

	temp[1] = '\0';

	return temp;
}

static inline char *
gen_non_print_sequence(const int c)
{
	char *temp = (char *)xnmalloc(2, sizeof(char));
	*temp = (c == '[') ? RL_PROMPT_START_IGNORE
			: RL_PROMPT_END_IGNORE;
	temp[1] = '\0';

	return temp;
}

static inline char *
gen_shell_name(void)
{
	char *p = (char *)NULL,
		 *shell_name = strrchr(user.shell, '/');

	if (shell_name && *(shell_name + 1))
		p = shell_name + 1;
	else
		p = user.shell;

	return savestring(p, strlen(p));
}

static inline void
add_string(char **tmp, const int c, char **line, char **res, size_t *len)
{
	if (!*tmp)
		return;

	if (c)
		(*line)++;

	*len += strlen(*tmp);

	if (!*res) {
		*res = (char *)xnmalloc(*len + 1, sizeof(char));
		*(*res) = '\0';
	} else {
		*res = (char *)xrealloc(*res, (*len + 1) * sizeof(char));
	}

	strcat(*res, *tmp);
	free(*tmp);
}

static inline void
reset_ifs(const char *value)
{
	if (value)
		setenv("IFS", value, 1);
	else
		unsetenv("IFS");
}

static inline void
substitute_cmd(char **line, char **res, size_t *len)
{
	int tmp = strcntchr(*line, ')');
	if (tmp == -1) return; /* No ending bracket */

	char *tmp_str = (char *)xnmalloc(strlen(*line) + 2, sizeof(char));
	sprintf(tmp_str, "$%s", *line);

	tmp_str[tmp + 2] = '\0';
	*line += tmp + 1;

	const char *old_value = getenv("IFS");
	setenv("IFS", "", 1);

	wordexp_t wordbuf;
	if (wordexp(tmp_str, &wordbuf, 0) != EXIT_SUCCESS) {
		free(tmp_str);
		reset_ifs(old_value);
		return;
	}
	reset_ifs(old_value);
	free(tmp_str);

	if (wordbuf.we_wordc) {
		for (size_t j = 0; j < wordbuf.we_wordc; j++) {
			*len += strlen(wordbuf.we_wordv[j]);
			if (!*res) {
				*res = (char *)xnmalloc(*len + 2, sizeof(char));
				*(*res) = '\0';
			} else {
				*res = (char *)xrealloc(*res, (*len + 2) * sizeof(char));
			}
			strcat(*res, wordbuf.we_wordv[j]);
		}
	}

	wordfree(&wordbuf);
	return;
}

static inline char *
gen_emergency_prompt(void)
{
	static int f = 0;
	if (f == 0) {
		f = 1;
		fprintf(stderr, _("%s: %s\n"), PROGRAM_NAME, EMERGENCY_PROMPT_MSG);
	}
	char *_prompt = savestring(EMERGENCY_PROMPT, EMERGENCY_PROMPT_LEN);
	return _prompt;
}

static inline char *
gen_stats_str(int flag)
{
	size_t val = 0;

	switch(flag) {
	case STATS_DIR: val = stats.dir; break;
	case STATS_REG: val = stats.reg; break;
	case STATS_EXE: val = stats.exec; break;
	case STATS_HIDDEN: val = stats.hidden; break;
	case STATS_SUID: val = stats.suid; break;
	case STATS_SGID: val = stats.sgid; break;
	case STATS_FIFO: val = stats.fifo; break;
	case STATS_SOCK: val = stats.socket; break;
	case STATS_BLK: val = stats.block_dev; break;
	case STATS_CHR: val = stats.char_dev; break;
	case STATS_CAP: val = stats.caps; break;
	case STATS_LNK: val = stats.link; break;
	case STATS_BROKEN_L: val = stats.broken_link; break;
	case STATS_MULTI_L: val = stats.multi_link; break;
	case STATS_OTHER_W: val = stats.other_writable; break;
	case STATS_STICKY: val = stats.sticky; break;
	case STATS_EXTENDED: val = stats.extended; break;
	case STATS_UNKNOWN: val = stats.unknown; break;
	case STATS_UNSTAT: val = stats.unstat; break;
	default: break;
	}

	char *p = (char *)NULL;
	if (val != 0) {
		p = (char *)xnmalloc(sizeof(size_t) + 1, sizeof(char));
		sprintf(p, "%zu", val);
	} else {
		p = (char *)xnmalloc(2, sizeof(char));
		*p = '-';
		p[1] = '\0';
	}

	return p;
}

/*
static inline void
write_result(char **res, size_t *len, const int c)
{
	*res = (char *)xrealloc(*res, (*len + 2) * sizeof(char));
	*res[*len] = (char)c;
	(*len)++;
	*res[*len] = '\0';
} */

/* Decode the prompt string (encoded_prompt global variable) taken from
 * the configuration file. Based on the decode_prompt_string function
 * found in an old bash release (1.14.7). */
char *
decode_prompt(char *line)
{
	if (!line)
		return (char *)NULL;

	char *temp = (char *)NULL, *result = (char *)NULL;
	size_t result_len = 0;
	int c;

	while ((c = *line++)) {
		/* We have an escape char */
		if (c == '\\') {
			/* Now move on to the next char */
			c = *line;
			switch (c) {
			/* Files statistics */
			case 'D': temp = gen_stats_str(STATS_DIR); goto ADD_STRING;
			case 'R': temp = gen_stats_str(STATS_REG); goto ADD_STRING;
			case '.': temp = gen_stats_str(STATS_HIDDEN); goto ADD_STRING;
			case 'X': temp = gen_stats_str(STATS_EXE); goto ADD_STRING;
			case 'U': temp = gen_stats_str(STATS_SUID); goto ADD_STRING;
			case 'G': temp = gen_stats_str(STATS_SGID); goto ADD_STRING;
			case 'F': temp = gen_stats_str(STATS_FIFO); goto ADD_STRING;
			case 'K': temp = gen_stats_str(STATS_SOCK); goto ADD_STRING;
			case 'B': temp = gen_stats_str(STATS_BLK); goto ADD_STRING;
			case 'C': temp = gen_stats_str(STATS_CHR); goto ADD_STRING;
			case 'x': temp = gen_stats_str(STATS_CAP); goto ADD_STRING;
			case 'L': temp = gen_stats_str(STATS_LNK); goto ADD_STRING;
			case 'o': temp = gen_stats_str(STATS_BROKEN_L); goto ADD_STRING;
			case 'M': temp = gen_stats_str(STATS_MULTI_L); goto ADD_STRING;
			case 'E': temp = gen_stats_str(STATS_EXTENDED); goto ADD_STRING;
			case 'O': temp = gen_stats_str(STATS_OTHER_W); goto ADD_STRING;
			case '*': temp = gen_stats_str(STATS_STICKY); goto ADD_STRING;
			case '?': temp = gen_stats_str(STATS_UNKNOWN); goto ADD_STRING;
			case '!': temp = gen_stats_str(STATS_UNSTAT); goto ADD_STRING;

			case 'z': /* Exit status of last executed command */
				temp = gen_exit_status(); goto ADD_STRING;

			case 'e': /* Escape char */
				temp = gen_escape_char(&line, &c); goto ADD_STRING;

			case '0': /* fallthrough */ /* Octal char */
			case '1': /* fallthrough */
			case '2': /* fallthrough */
			case '3': /* fallthrough */
			case '4': /* fallthrough */
			case '5': /* fallthrough */
			case '6': /* fallthrough */
			case '7':
				temp = gen_octal(&line, &c); goto ADD_STRING;

			case 'c': /* Program name */
				temp = savestring(PNL, strlen(PNL)); goto ADD_STRING;

			case 'P': /* Current profile name */
				temp = gen_profile(); goto ADD_STRING;

			case 't': /* fallthrough */ /* Time: 24-hour HH:MM:SS format */
			case 'T': /* fallthrough */ /* 12-hour HH:MM:SS format */
			case 'A': /* fallthrough */ /* 24-hour HH:MM format */
			case '@': /* fallthrough */ /* 12-hour HH:MM:SS am/pm format */
			case 'd': /* Date: abrev_weak_day, abrev_month_day month_num */
				temp = gen_time(c); goto ADD_STRING;

			case 'u': /* User name */
				temp = gen_user_name(); goto ADD_STRING;

			case 'h': /* fallthrough */ /* Hostname up to first '.' */
			case 'H': /* Full hostname */
				temp = gen_hostname(c); goto ADD_STRING;

			case 's': /* Shell name (after last slash)*/
				if (!user.shell) { line++; break; }
				temp = gen_shell_name(); goto ADD_STRING;

			case 'S': /* Current workspace */
				temp = gen_workspace(); goto ADD_STRING;

			case 'l': /* Current mode */
				temp = gen_mode(); goto ADD_STRING;

			case 'p': /* fallthrough */ /* Abbreviated if longer than PathMax */
			case 'w': /* fallthrough */ /* Full PWD */
			case 'W': /* Short PWD */
				if (!workspaces[cur_ws].path) {	line++;	break; }
				temp = gen_pwd(c); goto ADD_STRING;

			case '$': /* '$' or '#' for normal and root user */
				temp = gen_user_flag();	goto ADD_STRING;

			case 'a': /* fallthrough */ /* Bell character */
			case 'r': /* fallthrough */ /* Carriage return */
			case 'n': /* fallthrough */ /* New line char */
				temp = gen_misc(c); goto ADD_STRING;

			case '[': /* fallthrough */ /* Begin a sequence of non-printing characters */
			case ']': /* End the sequence */
				temp = gen_non_print_sequence(c); goto ADD_STRING;

			case '\\': /* Literal backslash */
				temp = savestring("\\", 1); goto ADD_STRING;

			default:
				temp = savestring("\\ ", 2);
				temp[1] = (char)c;

ADD_STRING:
				add_string(&temp, c, &line, &result, &result_len);
				break;
			}
		}

		/* If not escape code, check for command substitution, and if not,
		 * just add whatever char is there */
		else {
			/* Remove non-escaped quotes */
			if (c == '\'' || c == '"')
				continue;

#if !defined(__HAIKU__) && !defined(__OpenBSD__)
			/* Command substitution */
			if (c == '$' && *line == '(') {
				substitute_cmd(&line, &result, &result_len);
				continue;
			}
#endif /* __HAIKU__ && __OpenBSD__ */

/*			write_result(&result, &result_len, c); */
			result = (char *)xrealloc(result, (result_len + 2) * sizeof(char));
			result[result_len] = (char)c;
			result_len++;
			result[result_len] = '\0';
		}
	}

	/* Remove trailing new line char, if any */
	if (result && result[result_len - 1] == '\n')
		result[result_len - 1] = '\0';

	/* Emergency prompt, just in case something went wrong */
	if (!result)
		result = gen_emergency_prompt();

	return result;
}

/* Make sure CWD exists; if not, go up to the parent, and so on */
static inline void
check_cwd(void)
{
	while (xchdir(workspaces[cur_ws].path, SET_TITLE) != EXIT_SUCCESS) {
		char *ret = strrchr(workspaces[cur_ws].path, '/');
		if (ret && ret != workspaces[cur_ws].path)
			*ret = '\0';
		else
			break;
	}
}

/* Remove all final slash(es) from path, if any */
static inline void
trim_final_slashes(void)
{
	size_t path_len = strlen(workspaces[cur_ws].path), i;

	for (i = path_len - 1; workspaces[cur_ws].path[i] && i > 0; i--) {
		if (workspaces[cur_ws].path[i] != '/')
			break;
		else
			workspaces[cur_ws].path[i] = '\0';
	}
}

static inline void
print_welcome_msg(void)
{
	if (welcome_message) {
		printf("%s%s > %s\n%s%s\n", wc_c, PROGRAM_NAME, _(PROGRAM_DESC),
				df_c, _(HELP_MESSAGE));
		welcome_message = 0;
	}
}

static inline void
_print_tips(void)
{
	if (tips == 0)
		return;

	static int first_run = 1;
	if (first_run) {
		print_tips(0);
		first_run = 0;
	}
}

static inline void
run_prompt_cmds(void)
{
	if (ext_cmd_ok == 0 || prompt_cmds_n == 0)
		return;

	size_t i;
	for (i = 0; i < prompt_cmds_n; i++) {
		if (xargs.secure_cmds == 0
		|| sanitize_cmd(prompt_cmds[i], SNT_PROMPT) == EXIT_SUCCESS)
			launch_execle(prompt_cmds[i]);
	}
}

#ifndef _NO_TRASH
static inline void
update_trash_indicator(void)
{
	if (trash_ok) {
		trash_n = count_dir(trash_files_dir, NO_CPOP);
		if (trash_n <= 2)
			trash_n = 0;
	}
}
#endif

static inline void
setenv_prompt(void)
{
	if (prompt_style != CUSTOM_PROMPT_STYLE)
		return;

	/* Set environment variables with CliFM state information
	 * (sel files, trash, stealth mode, messages) to be handled by
	 * the prompt itself */
	char t[32];
	sprintf(t, "%d", (int)sel_n);
	setenv("CLIFM_STAT_SEL", t, 1);
#ifndef _NO_TRASH
	sprintf(t, "%d", (int)trash_n);
	setenv("CLIFM_STAT_TRASH", t, 1);
#endif
	sprintf(t, "%d", (msgs_n && pmsg) ? (int)msgs_n : 0);
	setenv("CLIFM_STAT_MSG", t, 1);
	sprintf(t, "%d", cur_ws + 1);
	setenv("CLIFM_STAT_WS", t, 1);
	sprintf(t, "%d", exit_code);
	setenv("CLIFM_STAT_EXIT", t, 1);
	setenv("CLIFM_STAT_ROOT", (flags & ROOT_USR) ? "1" : "0", 1);
	setenv("CLIFM_STAT_STEALTH", (xargs.stealth_mode == 1) ? "1" : "0", 1);
}

static inline size_t
set_prompt_length(size_t decoded_prompt_len)
{
	size_t len = 0;

	if (prompt_style == DEF_PROMPT_STYLE) {
		len = (size_t)(decoded_prompt_len
		+ (xargs.stealth_mode == 1 ? STEALTH_IND_SIZE : 0)
		+ ((flags & ROOT_USR) ? ROOT_IND_SIZE : 0)
		+ (sel_n ? N_IND : 0)
		+ (trash_n ? N_IND : 0)
		+ ((msgs_n && pmsg) ? N_IND : 0)
		+ 6 + sizeof(tx_c) + 1 + 2);
	} else {
		len = (size_t)(decoded_prompt_len + 6 + sizeof(tx_c) + 1);
	}

	return len;
}

static inline char *
construct_prompt(const char *decoded_prompt)
{
	/* Construct indicators: MSGS, SEL, and TRASH */

	/* Messages are categorized in three groups: errors, warnings, and
	 * notices. The kind of message should be specified by the function
	 * printing the message itself via a global enum: pmsg, with the
	 * following values: NOMSG, ERROR, WARNING, and NOTICE. */
	char msg_ind[N_IND];
	*msg_ind = '\0';

	if (msgs_n) {
		/* Errors take precedence over warnings, and warnings over
		 * notices. That is to say, if there is an error message AND a
		 * warning message, the prompt will always display the error
		 * message sign: a red 'E'. */
		switch (pmsg) {
		case NOMSG:	break;
		case ERROR:	snprintf(msg_ind, N_IND, "%sE%zu%s", em_c, msgs_n, RL_NC); break;
		case WARNING: snprintf(msg_ind, N_IND, "%sW%zu%s", wm_c, msgs_n, RL_NC); break;
		case NOTICE: snprintf(msg_ind, N_IND, "%sN%zu%s", nm_c, msgs_n, RL_NC); break;
		default: break;
		}
	}

	char trash_ind[N_IND];
	*trash_ind = '\0';
	if (trash_n > 2)
		snprintf(trash_ind, N_IND, "%sT%zu%s", ti_c, (size_t)trash_n - 2, RL_NC);

	char sel_ind[N_IND];
	*sel_ind = '\0';
	if (sel_n > 0)
		snprintf(sel_ind, N_IND, "%s*%zu%s", li_c, sel_n, RL_NC);

	size_t prompt_len = set_prompt_length(strlen(decoded_prompt));
	char *the_prompt = (char *)xnmalloc(prompt_len, sizeof(char));

	if (prompt_style == DEF_PROMPT_STYLE) {
		snprintf(the_prompt, prompt_len,
			"%s%s%s%s%s%s%s%s\001%s\002",
			(flags & ROOT_USR) ? (colorize ? ROOT_IND : ROOT_IND_NO_COLOR) : "",
			(msgs_n && pmsg) ? msg_ind : "",
			(xargs.stealth_mode == 1) ? si_c : "",
			(xargs.stealth_mode == 1) ? STEALTH_IND : "",
			(trash_n) ? trash_ind : "",
			(sel_n) ? sel_ind : "",
			decoded_prompt, RL_NC, tx_c);
	} else {
		snprintf(the_prompt, prompt_len, "%s%s\001%s\002", decoded_prompt,
			RL_NC, tx_c);
	}

	return the_prompt;
}

static inline void
initialize_prompt_data(void)
{
	check_cwd();
	trim_final_slashes();
	print_welcome_msg();
	_print_tips();

	/* Set foreground color to default */
	fputs(df_c, stdout);
	fflush(stdout);

	run_prompt_cmds();
#ifndef _NO_TRASH
	update_trash_indicator();
#endif
	get_sel_files();
	setenv_prompt();

	args_n = 0;
	curhistindex = current_hist_n;

	/* Print error messages */
	if (print_msg && msgs_n) {
		fputs(messages[msgs_n - 1], stderr);
		print_msg = 0; /* Print messages only once */
	}
}

static inline void
log_and_record(char *input)
{
	/* Keep a literal copy of the last entered command to compose the
	 * commands log, if needed and enabled */
	if (logs_enabled) {
		free(last_cmd);
		last_cmd = savestring(input, strlen(input));
	}

	/* Do not record empty lines, exit, history commands, consecutively
	 * equal inputs, or lines starting with space */
	if (record_cmd(input))
		add_to_cmdhist(input);
}

/* Print the prompt and return the string entered by the user (to be
 * parsed later by parse_input_str()) */
char *
prompt(void)
{
	initialize_prompt_data();

	/* Generate the prompt string using the prompt line in the config
	 * file (stored in encoded_prompt at startup) */
	char *decoded_prompt = decode_prompt(encoded_prompt);
	char *the_prompt = construct_prompt(decoded_prompt);
	free(decoded_prompt);

	/* Print the prompt and get user input */
	char *input = (char *)NULL;
	input = readline(the_prompt);
	free(the_prompt);

	if (!input || !*input) {
		free(input);
		return (char *)NULL;
	}

	log_and_record(input);

	return input;
}
