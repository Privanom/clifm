#!/bin/sh

# Directory change plugin for CliFM

# Find and change directory using find and fzf
# Author: Docbroke
# License: GPL3

if ! type fzf > /dev/null 2>&1; then
	printf "%s" "CliFM: fzf: Command not found\n" >&2
	exit 1
fi

# Source our plugins helper
if [ -z "$CLIFM_PLUGINS_HELPER" ] || ! [ -f "$CLIFM_PLUGINS_HELPER" ]; then
	printf "CliFM: Unable to find plugins-helper file\n" >&2
	exit 1
fi
# shellcheck source=/dev/null
. "$CLIFM_PLUGINS_HELPER"

# shellcheck disable=SC2154
DIR="$(find / -type d -print0 2> /dev/null | \
fzf --read0 --prompt "$fzf_prompt" \
--reverse --height 15 --header "Fuzzy directory changer" \
--bind "tab:accept" --info=inline \
--color="$(get_fzf_colors)")"

if [ -n "$DIR" ]; then
    printf "%s\n" "$DIR" > "$CLIFM_BUS"
fi

exit 0
