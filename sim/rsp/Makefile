# The RSP reference model and its task runner (rsp.h, rsp_task.c).
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
rsp_task: rsp_task.c rsp.c rsp.h rsp_time.c rsp_time.h
	$(CC) $(CFLAGS) rsp_task.c rsp.c rsp_time.c -o $@
clean:
	rm -f rsp_task
.PHONY: clean
