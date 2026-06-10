CC      := gcc
BINARY  := ultrahttp

SRCDIR  := src
INCDIR  := include

SRCS    := $(SRCDIR)/mem.c \
           $(SRCDIR)/conn.c \
           $(SRCDIR)/http.c \
           $(SRCDIR)/worker.c \
           $(SRCDIR)/server.c \
           $(SRCDIR)/main.c

WARNS   := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-aliasing=2 \
           -Wcast-align -Wundef -Wmissing-prototypes -Wno-unused-parameter \
           -Werror

BASE_CFLAGS := -std=c11 -I$(INCDIR) $(WARNS)

REL_CFLAGS  := -O3 -march=native -mtune=native -fno-plt \
               -fomit-frame-pointer -funroll-loops -fstrict-aliasing \
               -fvisibility=hidden
REL_LDFLAGS := -O3 -flto -fno-plt -Wl,--strip-all -Wl,-z,now -Wl,-z,relro

DBG_CFLAGS  := -O0 -g3 -fsanitize=address,undefined
DBG_LDFLAGS := -fsanitize=address,undefined

PGO1_CFLAGS := -O2 -fprofile-generate=pgo-data
PGO1_LDFLAGS:= -fprofile-generate=pgo-data

PGO2_CFLAGS := -O3 -march=native -fno-plt \
               -fprofile-use=pgo-data -fprofile-correction
PGO2_LDFLAGS:= -flto -fprofile-use=pgo-data

EPOLL_DEFS  := -DBACKEND_EPOLL=1 -DBACKEND_URING=0
URING_DEFS  := -DBACKEND_EPOLL=0 -DBACKEND_URING=1

EPOLL_SRCS  := $(SRCS) $(SRCDIR)/epoll_backend.c
URING_SRCS  := $(SRCS) $(SRCDIR)/uring_backend.c

LIBS        := -lpthread
URING_LIBS  := -lpthread -luring

.PHONY: all release debug uring uring-debug pgo-instrument pgo-use clean

all: release

release: $(EPOLL_SRCS)
	$(CC) $(BASE_CFLAGS) $(REL_CFLAGS) $(EPOLL_DEFS) \
	    $(EPOLL_SRCS) $(LIBS) $(REL_LDFLAGS) -o $(BINARY)

debug: $(EPOLL_SRCS)
	$(CC) $(BASE_CFLAGS) $(DBG_CFLAGS) $(EPOLL_DEFS) \
	    $(EPOLL_SRCS) $(LIBS) $(DBG_LDFLAGS) -o $(BINARY)-debug

uring: $(URING_SRCS)
	$(CC) $(BASE_CFLAGS) $(REL_CFLAGS) $(URING_DEFS) \
	    $(URING_SRCS) $(URING_LIBS) $(REL_LDFLAGS) -o $(BINARY)-uring

uring-debug: $(URING_SRCS)
	$(CC) $(BASE_CFLAGS) $(DBG_CFLAGS) $(URING_DEFS) \
	    $(URING_SRCS) $(URING_LIBS) $(DBG_LDFLAGS) -o $(BINARY)-uring-debug

pgo-instrument: $(EPOLL_SRCS)
	mkdir -p pgo-data
	$(CC) $(BASE_CFLAGS) $(PGO1_CFLAGS) $(EPOLL_DEFS) \
	    $(EPOLL_SRCS) $(LIBS) $(PGO1_LDFLAGS) -o $(BINARY)-pgo-inst

pgo-use: $(EPOLL_SRCS)
	$(CC) $(BASE_CFLAGS) $(PGO2_CFLAGS) $(EPOLL_DEFS) \
	    $(EPOLL_SRCS) $(LIBS) $(PGO2_LDFLAGS) -o $(BINARY)-pgo

clean:
	rm -f $(BINARY) $(BINARY)-debug $(BINARY)-uring \
	      $(BINARY)-uring-debug $(BINARY)-pgo-inst $(BINARY)-pgo
	rm -rf pgo-data
