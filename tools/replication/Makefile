CC       = gcc
INCLUDES =
OBJS		= common.o
#CFLAGS   = -Wall -O2 -g -std=c99 -Werror ${INCLUDES}
CFLAGS   = -Wall -O0 -g -std=c99 -Werror ${INCLUDES}
LDLIBS   = -pthread -lnuma
LDFLAGS  = ${OBJS}

TARGETS  = makefile.dep stresstest0 stresstest1 stresstest2 stresstest3 stresstest4 stresstest5 stresstest6 stresstest7 stresstest8 stresstest9 stresstest10

KV = $(shell uname -r)

ifeq ($(KV), 3.2.1-replication+)
CFLAGS   += -DLEGACY_MADV_REP
endif


all: $(TARGETS) tags

makefile.dep : *.[Cch] ${OBJS}
		    for i in *.[Cc]; do gcc $(CFLAGS) -MM "$${i}"; done > $@
-include makefile.dep

clean:
		rm -f *.o $(TARGETS)

tags: *.[Cch]
	ctags --totals *.c *.h

.PHONY: all clean
