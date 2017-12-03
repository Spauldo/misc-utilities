j/*
 * strip_comments.c
 *
 * Reads a C source file and outputs the code without comments.
 *
 * Do note that the ucontext.h functionality has been obsoleted due to the
 * use of '...' in the function prototypes being obsoleted by the C99
 * standard.  This will not compile with -std=c99 or higher, but should
 * compile fine with -std=gnu99.
 *
 * Copyright (c) 2017 Jeff Spaulding <sarnet@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ucontext.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

/* This is not the best way to do this, and is not portable to some systems
 * such as OpenBSD.  But it should be fast.
 *
 * Implemented like this because I wanted to play with coroutines, which only
 * works in standard C in a very hackish manner. */

/* Coroutine Identifier */
enum coroutine_id {
	CR_MAIN       = 0,
	CR_IN_CODE    = 1,
	CR_IN_COMMENT = 2
};

/* Size of a coroutine stack */
const size_t CR_STACK_SIZE = 8192;

/* Number to times to attempt a write before giving up */
const int MAX_ATTEMPTS = 20;

/* Yeah, there are globals.  I'm doing coroutines, this isn't exactly
 * "structured programming." */
char *progname;       /* Pointer to argv[0] */
char *input;          /* Beginning of the mmapped input file */
int ifd, ofd;         /* Input and output file descriptors */
off_t input_size;     /* Size of the input file */
volatile size_t mark; /* Offset to begin next output from */
volatile size_t pos;  /* Current file position */
ucontext_t uc[3];     /* Contexts for coroutines */

/* Prototypes */
void print_help();
void output_state();
void comment_state();
void write_bytes();


int
main(int argc, char **argv)
{
	int opt;
	char *infilename, *outfilename;
	struct stat sb;
	int i;

	progname = argv[0];
	ofd = STDOUT_FILENO;
	infilename = outfilename = NULL;
	mark = pos = 0;

	/* Argument Parsing */
	while ((opt = getopt(argc, argv, "i:o:")) != -1) {
		switch (opt) {
		case 'i':
			infilename = optarg;
			break;
		case 'o':
			outfilename = optarg;
			break;
		default:
			print_help();
			return EXIT_FAILURE;
		}
	}

	if (infilename == NULL) {
		print_help();
		return EXIT_FAILURE;
	}

	/* Open files as necessary */
	if (outfilename) {
		open(outfilename, O_CREAT, O_WRONLY, O_TRUNC);

		if (ofd == -1)
			err(EXIT_FAILURE, "%s", outfilename);
	}

	ifd = open(infilename, O_RDONLY);

	if (ifd == -1)
		err(EXIT_FAILURE, "%s", infilename);


	/* Get input file size */
	if (fstat(ifd, &sb) == -1) {
		err(EXIT_FAILURE, "%s", infilename);
	}

	input_size = sb.st_size;

	/* mmap input file */
	input = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE,
			    ifd, 0);

	if (input == MAP_FAILED) {
		err(EXIT_FAILURE, "%s", infilename);
	}

	/* Set up contexts
	 * We are using mmapped memory for the coroutine stacks.
	 * uc[CR_MAIN] does not require setup. */
	for (i = 1; i < 3; i++) {
		getcontext(&uc[i]);
		uc[i].uc_stack.ss_size = CR_STACK_SIZE;
		uc[i].uc_stack.ss_flags = 0;
		uc[i].uc_link = &uc[CR_MAIN];
		uc[i].uc_stack.ss_sp =
			mmap(NULL, CR_STACK_SIZE,
			     PROT_READ | PROT_WRITE | PROT_EXEC,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (uc[i].uc_stack.ss_sp == MAP_FAILED) {
			err(EXIT_FAILURE, NULL);
		}
	}

	makecontext(&uc[CR_IN_CODE], output_state, 0);
	makecontext(&uc[CR_IN_COMMENT], comment_state, 0);

	/* Start the IN_CODE coroutine */
	swapcontext(&uc[CR_MAIN], &uc[CR_IN_CODE]);

	/* Unmap file and memory, close files, and exit */
	munmap(input, sb.st_size);
	munmap(uc[1].uc_stack.ss_sp, uc[1].uc_stack.ss_size);
	munmap(uc[2].uc_stack.ss_sp, uc[2].uc_stack.ss_size);

	close(ifd);
	if (outfilename)
		close(ofd);

	return EXIT_SUCCESS;
}

/* Outputs a usage message */
void
print_help()
{
	fprintf(stderr, "Usage:\n    %s -i <file> [-o <file>]\n", progname);
}

/* Coroutine for outputting code */
void
output_state()
{
	ssize_t bytes_written;

	while (pos < input_size) {
		if (input[pos] == '\n') {

			write_bytes();

		} else if (input[pos] == '/' &&
			   input[pos + 1] == '*') {

			if (mark < pos)
				write_bytes();

			swapcontext(&uc[CR_IN_CODE], &uc[CR_IN_COMMENT]);
		}

		pos++;
	}

	if (mark < pos)
		write_bytes();
}

/* Coroutine for skipping comments */
void
comment_state()
{
	while (pos < input_size) {
		if (input[pos] == '*' &&
		    input[pos + 1] == '/') {
			mark = pos = pos + 2;
			swapcontext(&uc[CR_IN_COMMENT], &uc[CR_IN_CODE]);
		} else {
			mark = pos = pos + 1;
		}
	}
}

/* Output code from mark to pos, updating mark */
void
write_bytes()
{
	ssize_t bytes_written = 0;
	int attempts = 0;

	while (bytes_written < pos - mark) {
		if (bytes_written == -1)
			err(EXIT_FAILURE, NULL);

		bytes_written = write(ofd, input + mark, pos - mark);
		mark += bytes_written;
		attempts++;

		if (attempts > MAX_ATTEMPTS)
			errx(EXIT_FAILURE, "Gave up writing output, aborting.");
	}
}
