.PHONY: all
all: primary daemon

SCHED_CFLAGS  := -Wall -Werror -O2 -g
SCHED_LDFLAGS := -lpthread

primary: primary.c
	$(CC) $(SCHED_CFLAGS) -std=c99 -o $@ $< $(SCHED_LDFLAGS)

daemon: daemon.c
	$(CC) $(SCHED_CFLAGS) -o $@ $< 

.PHONY: clean

clean:
	rm -f primary daemon
