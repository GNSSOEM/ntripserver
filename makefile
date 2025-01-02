#!/usr/bin/make
# $Id: makefile,v 1.9 2009/02/10 12:20:09 stoecker Exp $

BINDIR = /usr/local/bin

ifdef windir
CC   = gcc
OPTS = -Wall -W -DWINDOWSVERSION
LIBS = -lwsock32
else
OPTS = -Wall -W -DCLOCK_MONOTONIC_RAW
endif

ntripserver: ntripserver.c
	$(CC) $(OPTS) $? -O3 -DNDEBUG -o $@ $(LIBS)

debug: ntripserver.c
	$(CC) $(OPTS) $? -g -o ntripserver $(LIBS)

clean:
	$(RM) -f ntripserver core

archive:
	zip -9 ntripserver.zip makefile ntripserver.c README startntripserver.sh

tgzarchive:
	tar -cvzf ntripserver.tgz makefile ntripserver.c README startntripserver.sh

install: ntripserver
	cp ntripserver $(BINDIR)
