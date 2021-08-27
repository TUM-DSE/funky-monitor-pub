.PHONY: all
all: primary daemon

SCHED_CFLAGS  := -Wall -Werror -std=c99 -O2 -g
SCHED_LDFLAGS := -lpthread

primary: primary.c
	$(CC) $(SCHED_CFLAGS) -o $@ $< $(SCHED_LDFLAGS)

daemon: daemon.c
	$(CC) $(SCHED_CFLAGS) -o $@ $< 

.PHONY: clean

clean:
	rm -f primary daemon
