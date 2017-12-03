CC=cc
CFLAGS=-O2
EXECS=strip_comments

.PHONY: clean all

all: $(EXECS)

clean:
	rm -f $(EXECS)

strip_comments: strip_comments.c
	$(CC) $(CFLAGS) -o strip_comments strip_comments.c
