/*
 * xenpcid.c
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <libxenvchan.h>
#include <xenpcid.h>

#include <xenstore.h>
//#include <libxl_json.h>

#define DOM0_ID 0

#define BUFSIZE 5000
char inbuf[BUFSIZE];
char outbuf[BUFSIZE];
int insiz = 0;
int outsiz = 0;
struct libxenvchan *ctrl = 0;

static void vchan_wr(void) {
	int ret;

	if (!insiz)
		return;
	ret = libxenvchan_write(ctrl, inbuf, insiz);
	if (ret < 0) {
		fprintf(stderr, "vchan write failed\n");
		exit(1);
	}
	if (ret > 0) {
		insiz -= ret;
		memmove(inbuf, inbuf + ret, insiz);
	}
}

static void stdout_wr(void) {
	int ret;

	if (!outsiz)
		return;
	ret = write(1, outbuf, outsiz);
	if (ret < 0 && errno != EAGAIN)
		exit(1);
	if (ret > 0) {
		outsiz -= ret;
		memmove(outbuf, outbuf + ret, outsiz);
	}
}

static int set_nonblocking(int fd, int nonblocking) {
	int flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return -1;

	if (nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) == -1)
		return -1;

	return 0;
}

int main(int argc, char *argv[])
{
	int ret, rsiz, wsiz;
	int libxenvchan_fd;
	uint32_t domid;
	char *domid_str, vchan_path[100];
	struct xs_handle *xs;

	xs = xs_open(0);
	if (!xs)
		perror("XS opening ERROR");;

	domid_str = xs_read(xs, XBT_NULL, "domid", NULL);
	domid = atoi(domid_str);

	rsiz = 0;
	wsiz = 0;
	sprintf(vchan_path, XENPCID_XS_PATH, domid);
	ctrl = libxenvchan_server_init(NULL, DOM0_ID, vchan_path, rsiz, wsiz);

	if (!ctrl) {
		perror("libxenvchan_*_init");
		exit(1);
	}

	libxenvchan_fd = libxenvchan_fd_for_select(ctrl);
	for (;;) {
		fd_set rfds;
		fd_set wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		if (insiz != BUFSIZE)
			FD_SET(0, &rfds);
		if (outsiz)
			FD_SET(1, &wfds);
		FD_SET(libxenvchan_fd, &rfds);
		ret = select(libxenvchan_fd + 1, &rfds, &wfds, NULL, NULL);
		if (ret < 0) {
			perror("select");
			exit(1);
		}
		if (FD_ISSET(0, &rfds)) {
			ret = read(0, inbuf + insiz, BUFSIZE - insiz);
			if (ret < 0 && errno != EAGAIN)
				exit(1);
			if (ret == 0) {
				while (insiz) {
					vchan_wr();
					libxenvchan_wait(ctrl);
				}
				return 0;
			}
			if (ret)
				insiz += ret;
			vchan_wr();
		}
		if (FD_ISSET(libxenvchan_fd, &rfds)) {
			libxenvchan_wait(ctrl);
			vchan_wr();
		}
		if (FD_ISSET(1, &wfds))
			stdout_wr();
		while (libxenvchan_data_ready(ctrl) && outsiz < BUFSIZE) {
			ret = libxenvchan_read(ctrl, outbuf + outsiz, BUFSIZE - outsiz);
			if (ret < 0)
				exit(1);
			outsiz += ret;
			stdout_wr();
		}
		if (!libxenvchan_is_open(ctrl)) {
			if (set_nonblocking(1, 0)) {
				perror("set_nonblocking");
				exit(1);
			}
			while (outsiz)
				stdout_wr();
			return 0;
		}
	}
    return 0;
}
