                  ###################################
                  #   Configuration file for Lira   #
                  #     CliFM's resource opener     #
                  ###################################

# Commented and blank lines are omitted

# The below settings cover the most common filetypes
# It is recommended to edit this file keeping only applications you need
# to speed up the opening process

# The file is read top to bottom and left to right; the first existent
# application found will be used

# Applications defined here are NOT desktop files, but commands (arguments
# could be used as well)

# Use 'X' to specify a GUI environment and '!X' for non-GUI environments,
# like the kernel built-in console or a remote SSH session.

# Use 'N' to match file names instead of MIME types.

# Regular expressions are allowed for both file types and file names.

# Use the %f placeholder to specify the position of the file name to be
# opened in the command. Example:
# 'mpv %f --terminal=no' -> 'mpv FILE --terminal=no' 
# If %f is not specified, the file name will be added to the end of the
# command. Ex: 'mpv --terminal=no' -> 'mpv --terminal=no FILE'

# Running the opening application in the background:
# For GUI applications:
#    APP %f &
# For terminal applications:
#    TERM -e APP %f &
# Replace 'TERM' and 'APP' by the corresponding values. The -e option
# might vary depending on the terminal emulator used (TERM)

# Environment variables could be used as well. Example:
# X:text/plain=$TERM -e $EDITOR %f &;$VISUAL;nano;vi

########################
#      File Names      #
########################

# Match a full file name
#X:N:some_filename=cmd

# Match all file names starting with 'str'
#X:N:^str.*:cmd

########################
#    File extensions   #
########################

X:N:.*\.djvu$=djview;zathura;evince;atril
X:N:.*\.epub$=mupdf;zathura;ebook-viewer
X:N:.*\.mobi$=ebook-viewer
X:N:.*\.(cbr|cbz)$=zathura
X:N:.*\.cfm$=$EDITOR;$VISUAL;nano;nvim;vim;vis;vi;mg;emacs;ed;micro;kak;mili;leafpad;mousepad;featherpad;gedit;kate;pluma
!X:N:.*\.cfm$=$EDITOR;$VISUAL;nano;nvim;vim;vis;vi;mg;emacs;ed;micro;kak

##################
#   MIME types   #
##################

# Directories - only for the open-with command (ow)
# In graphical environment directories will be opened in a new window
X:inode/directory=xterm -e clifm %f &;xterm -e vifm %f &;pcmanfm %f &;thunar %f &;xterm -e ncdu %f &
!X:inode/directory=vifm;ranger;nnn;ncdu

# Web content
X:^text/html$=$BROWSER;surf;vimprobable;vimprobable2;qutebrowser;dwb;jumanji;luakit;uzbl;uzbl-tabbed;uzbl-browser;uzbl-core;iceweasel;midori;opera;firefox;seamonkey;chromium-browser;chromium;google-chrome;epiphany;konqueror;elinks;links2;links;lynx;w3m
!X:^text/html$=$BROWSER;elinks;links2;links;lynx;w3m

# Text
#X:^text/x-(c|shellscript|perl|script.python|makefile|fortran|java-source|javascript|pascal)$=geany
X:(^text/.*|application/json|inode/x-empty)=$EDITOR;$VISUAL;nano;nvim;vim;vis;vi;mg;emacs;ed;micro;kak;dte;mili;leafpad;mousepad;featherpad;nedit;kate;gedit;pluma;io.elementary.code;liri-text;xed;atom;nota;gobby;kwrite;xedit
!X:(^text/.*|application/json|inode/x-empty)=$EDITOR;$VISUAL;nano;nvim;vim;vis;vi;mg;emacs;ed;micro;kak;dte

# Office documents
X:^application/.*(open|office)document.*=libreoffice;soffice;ooffice

# Archives
X:^application/(zip|gzip|zstd|x-7z-compressed|x-xz|x-bzip*|x-tar|x-iso9660-image)=ad;xarchiver %f &;lxqt-archiver %f &;ark %f &
!X:^application/(zip|gzip|zstd|x-7z-compressed|x-xz|x-bzip*|x-tar|x-iso9660-image)=ad

# PDF
X:.*/pdf$=mupdf;llpp;zathura;mupdf-x11;apvlv;xpdf;evince;atril;okular;epdfview;qpdfview

# Images
X:^image/gif$=animate;pqiv;sxiv -a;nsxiv -a
X:^image/.*=fim;feh;display;sxiv;nsxiv;pqiv;gpicview;qview;qimgv;inkscape;mirage;ristretto;eog;eom;xviewer;viewnior;nomacs;geeqie;gwenview;gthumb;gimp
!X:^image/*=fim;img2txt;cacaview;fbi;fbv

# Video and audio
X:^video/.*=ffplay;mplayer;mplayer2;mpv;vlc;gmplayer;smplayer;celluloid;qmplayer2;haruna;totem
X:^audio/.*=ffplay -nodisp -autoexit;mplayer;mplayer2;mpv;vlc;gmplayer;smplayer;totem

# Fonts
X:^font/.*=fontforge;fontpreview

# Torrent:
X:application/x-bittorrent=rtorrent;transimission-gtk;transmission-qt;deluge-gtk;ktorrent

# Fallback to another resource opener as last resource
.*=xdg-open;mimeo;mimeopen -n;whippet -m;open;linopen;
