/*
 * Copyright (c) 2022 Willemijn Coene
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
# include <IOKit/serial/ioss.h>
#endif

#define nitems(arr)	(sizeof(arr) / sizeof((arr)[0]))

int		 verbose = 0;
speed_t		 baudrate = 115200;
const char	*ttydev = NULL;

int		 ttyfd = 0;
struct termios	 termios, otermios;
sig_atomic_t	 interrupt = 0;

#define LOG(l, ...)	do {                                                \
	if (verbose >= (l))                                                 \
		fprintf(stderr, __VA_ARGS__);                               \
} while (0)

void
usage(void)
{
	fprintf(stderr, "Usage: blctl [-v] -d dev [-b baudrate]\n");
	exit(1);
}

void
sigint(int signo)
{
	interrupt = 1;
}

void
dump(FILE *stream, const unsigned char *buf, size_t len)
{
	for (size_t row = 0; row < len; row += 16) {
		fprintf(stream, "%04zx", row);

		for (size_t col = row; col < row + 16; col++) {
			if (col % 8 == 0)
				fprintf(stream, " ");

			if (col < len)
				fprintf(stream, " %02x", *(buf + col));
			else
				fprintf(stream, "   ");
		}

		fprintf(stream, "  |");

		for (size_t col = row; col < row + 16 && col < len; col++) {
			unsigned char ch = *(buf + col);
			fprintf(stream, "%c", isprint(ch) ? ch : '.');
		}

		fprintf(stream, "|\n");
	}
}

bool
canget(void)
{
	int avail = 0;

	if (ioctl(ttyfd, FIONREAD, &avail) == -1)
		err(1, "FIONREAD");

	return avail != 0;
}

unsigned char
get(void)
{
	unsigned char ch;

	if (poll(&(struct pollfd) {
		    .fd = ttyfd,
		    .events = POLLIN
	    }, 1, -1) == -1)
		err(1, "poll(\"%s\")", ttydev);

	switch (read(ttyfd, &ch, sizeof(ch))) {
	case -1:
		err(1, "read(\"%s\")", ttydev);

	case 0:
		errx(1, "short read");
	}

	if (isprint(ch))
		LOG(3, "get: %02x ('%c')\n", ch, ch);
	else
		LOG(3, "get: %02x\n", ch);

	return ch;
}

void
getbuf(unsigned char *buf, size_t len)
{
	size_t pos = 0;
	ssize_t done;

	while (pos < len) {
		if (poll(&(struct pollfd) {
			    .fd = ttyfd,
			    .events = POLLIN
		    }, 1, -1) == -1)
			err(1, "poll(\"%s\")", ttydev);

		done = read(ttyfd, buf + pos, len - pos);

		if (done == -1 && errno == EAGAIN)
			continue;

		if (done == -1)
			err(1, "read(\"%s\")", ttydev);
		else
			pos += done;
	}

	if (verbose >= 3) {
		fprintf(stderr, "get:\n");
		dump(stderr, buf, len);
	}
}

void
put(unsigned char ch)
{
	if (poll(&(struct pollfd) {
		    .fd = ttyfd,
		    .events = POLLOUT
	    }, 1, -1) == -1)
		err(1, "poll(\"%s\")", ttydev);

	if (isprint(ch))
		LOG(3, "put: %02x ('%c')\n", ch, ch);
	else
		LOG(3, "put: %02x\n", ch);

	switch (write(ttyfd, &ch, sizeof(ch))) {
	case -1:
		err(1, "write(\"%s\")", ttydev);

	case 0:
		errx(1, "short write");
	}
}

void
putbuf(const unsigned char *buf, size_t len)
{
	size_t pos = 0;
	ssize_t done;

	if (verbose >= 3) {
		fprintf(stderr, "put:\n");
		dump(stderr, buf, len);
	}

	while (pos < len) {
		if (poll(&(struct pollfd) {
			    .fd = ttyfd,
			    .events = POLLOUT
		    }, 1, -1) == -1)
			err(1, "poll(\"%s\")", ttydev);

		done = write(ttyfd, buf + pos, len - pos);

		if (done == -1 && errno == EAGAIN)
			continue;

		if (done == -1)
			err(1, "write(\"%s\")", ttydev);
		else
			pos += done;
	}
}

bool
blsync(void)
{
	LOG(1, "syncing...");

	for (int i = 0; i < baudrate * 5 /* ms */ / 10000; i++)
		put(0x55);

	usleep(20 /* ms */ * 1000);

	/* bflb-mcu-tool sends this right after the sync header */
	putbuf((unsigned char[]) {
		0x50,				/* command */
		0x00,				/* reserved */
		0x08, 0x00,			/* length */
		0x38, 0xf0, 0x00, 0x20,		/* data */
		0x00, 0x00, 0x00, 0x18
	}, 12);

	if (canget() && get() == 'O' && get() == 'K') {
		unsigned char buf[3];

		/* get additional pending input (from above command?) */
		getbuf(buf, nitems(buf));

		LOG(1, "ok\n");
		return true;
	}

	LOG(1, "failed, retrying\n");
	return false;
}

void
blinfo(void)
{
	unsigned char *buf;
	size_t len;

	putbuf((unsigned char[]) {
		0x10,			/* command */
		0x00,			/* reserved */
		0x00, 0x00		/* length */
	}, 4);

	if (get() != 'O' || get() != 'K')
		errx(1, "info failed");

	len = get();
	len += (size_t) get() << 8;
	buf = malloc(len);
	if (buf == NULL)
		err(1, "malloc");

	getbuf(buf, len);
	dump(stdout, buf, len);
}

void
cleanup(void)
{
	if (interrupt)
		LOG(1, "\ninterrupted\n");

	if (tcsetattr(ttyfd, TCSANOW, &otermios) == -1)
		err(1, "tcsetattr(\"%s\")", ttydev);
	close(ttyfd);

	LOG(1, "closed %s\n", ttydev);
}

int
main(int argc, char *const *argv)
{
	while (1) {
		switch (getopt(argc, argv, "hvd:b:")) {
		case 'h':
			usage();
			break;

		case 'v':
			verbose++;
			continue;

		case 'd':
			ttydev = optarg;
			continue;

		case 'b':
			baudrate = strtoul(optarg, NULL, 10);
			continue;

		default:
			break;
		}

		break;
	}

	if (ttydev == NULL)
		usage();

	signal(SIGINT, sigint);
	setbuf(stderr, NULL);

	ttyfd = open(ttydev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (ttyfd == -1)
		err(1, "open(\"%s\")", ttydev);

	if (tcgetattr(ttyfd, &termios) == -1)
		err(1, "tcgetattr(\"%s\")", ttydev);
	otermios = termios;

	cfmakeraw(&termios);
	termios.c_iflag &= ~(IXON | IXOFF);
	termios.c_cflag = CS8 | (termios.c_cflag & ~CSIZE);
	termios.c_cflag |= CREAD | CLOCAL;
	termios.c_cflag &= ~(PARENB | CCTS_OFLOW | CRTS_IFLOW);

#if !defined(__APPLE__)
	cfsetspeed(&termios, baudrate);
#endif

	if (tcsetattr(ttyfd, TCSANOW, &termios) == -1)
		err(1, "tcsetattr(\"%s\")", ttydev);

#if defined(__APPLE__)
	if (ioctl(ttyfd, IOSSIOSPEED, &baudrate) == -1)
		err(1, "IOSSIOSPEED");
#endif

	atexit(cleanup);

	LOG(1, "opened %s (%lu,8,N,1)\n", ttydev, (unsigned long) baudrate);

	while (!blsync()) {
		if (interrupt)
			return 1;

		sleep(3);	/* timeout (2s) + some */
	}

	blinfo();

	return 0;
}
