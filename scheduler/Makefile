.PHONY: all
all: primary daemon rest

SCHED_CFLAGS  := -Wall -Werror -O2 -g
SCHED_LDFLAGS := -lpthread

primary: primary.c
	$(CC) $(SCHED_CFLAGS) -std=c99 -o $@ $< $(SCHED_LDFLAGS)

daemon: daemon.c
	$(CC) $(SCHED_CFLAGS) -o $@ $< 

rest: rest.go
	go build rest.go

.PHONY: clean

clean:
	rm -f primary daemon rest
