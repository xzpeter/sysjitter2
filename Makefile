ifndef CFLAGS
CFLAGS		= -O2 -Wall
endif

ifdef NDEBUG
CPPFLAGS	+= -DNDEBUG
endif

INCLUDE		+= -I.
LDFLAGS         += -lpthread

is_ppc		:= $(shell (uname -m || uname -p) | grep ppc)
is_x86		:= $(shell (uname -m || uname -p) | grep i.86)
is_x86_64	:= $(shell (uname -m || uname -p) | grep x86_64)

ifneq ($(is_x86),)
# Need to tell gcc we have a reasonably recent cpu to get the atomics.
CFLAGS += -march=i686
endif

sysjitter2: sysjitter2.c rt-utils.c error.c trace.c

all: sysjitter2

clean:
	rm -f *.o sysjitter2 cscope.*

cscope:
	cscope -bq *.c
