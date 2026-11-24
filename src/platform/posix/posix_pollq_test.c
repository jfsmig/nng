//
// Copyright 2026 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nuts.h>

// select() cannot watch descriptors at or above FD_SETSIZE, so the select
// poller must refuse them.  The other pollers must accept them like any
// other descriptor.

// The open-file limit the tests need, to open a few descriptors at and
// above FD_SETSIZE.
#define ABOVE_SELECT_LIMIT (FD_SETSIZE + 16)

// allow_above_select_limit raises the soft open-file limit to
// ABOVE_SELECT_LIMIT if it is lower.  It returns false when the hard limit
// does not allow that.  It runs no check, so a test may still skip after
// calling it.
static bool
allow_above_select_limit(void)
{
	struct rlimit limit;

	if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
		return (false);
	}
	if ((limit.rlim_cur == RLIM_INFINITY) ||
	    (limit.rlim_cur >= ABOVE_SELECT_LIMIT)) {
		return (true);
	}
	if ((limit.rlim_max != RLIM_INFINITY) &&
	    (limit.rlim_max < ABOVE_SELECT_LIMIT)) {
		return (false);
	}
	limit.rlim_cur = ABOVE_SELECT_LIMIT;
	return (setrlimit(RLIMIT_NOFILE, &limit) == 0);
}

// move_above_select_limit returns a duplicate of fd numbered at least
// FD_SETSIZE and closes fd.  allow_above_select_limit must have succeeded.
static int
move_above_select_limit(int fd)
{
	int high;

	NUTS_ASSERT((high = fcntl(fd, F_DUPFD, FD_SETSIZE)) >= FD_SETSIZE);
	(void) close(fd);
	return (high);
}

#ifdef NNG_POLLQ_SELECT
static bool
fd_is_open(int fd)
{
	return ((fcntl(fd, F_GETFD) >= 0) || (errno != EBADF));
}
#endif

#ifdef NNG_TRANSPORT_FDC
void
test_pollq_socket_fd_above_select_limit(void)
{
	nng_stream_listener *l;
	nng_aio             *aio;
	int                  fds[2];
	int                  high;

	if (!allow_above_select_limit()) {
		NUTS_SKIP("cannot open descriptors above FD_SETSIZE");
		return;
	}
	NUTS_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	high = move_above_select_limit(fds[0]);

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 5000);
	NUTS_PASS(nng_stream_listener_alloc(&l, "socket://"));
	NUTS_PASS(nng_stream_listener_listen(l));
	NUTS_PASS(nng_stream_listener_set_int(l, NNG_OPT_SOCKET_FD, high));
	nng_stream_listener_accept(l, aio);
	nng_aio_wait(aio);

#ifdef NNG_POLLQ_SELECT
	NUTS_FAIL(nng_aio_result(aio), NNG_ENOFILES);
	NUTS_TRUE(!fd_is_open(high));
#else
	nng_stream *s;
	nng_iov     iov;
	char        buf[4];

	NUTS_PASS(nng_aio_result(aio));
	s = nng_aio_get_output(aio, 0);

	// Post the receive before the data exists, so that it has to wait
	// for the poller to report the descriptor readable.
	iov.iov_buf = buf;
	iov.iov_len = sizeof(buf);
	NUTS_PASS(nng_aio_set_iov(aio, 1, &iov));
	nng_stream_recv(s, aio);
	nng_msleep(50);
	NUTS_TRUE(write(fds[1], "ping", 4) == 4);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_aio_count(aio) == 4);
	NUTS_TRUE(memcmp(buf, "ping", 4) == 0);
	nng_stream_free(s);
#endif

	nng_stream_listener_free(l);
	nng_aio_free(aio);
	(void) close(fds[1]);
}
#endif // NNG_TRANSPORT_FDC

void
test_pollq_tcp_listen_fd_above_select_limit(void)
{
	nng_stream_listener *l;
	struct sockaddr_in   sin;
	int                  lfd;
	int                  high;

	if (!allow_above_select_limit()) {
		NUTS_SKIP("cannot open descriptors above FD_SETSIZE");
		return;
	}
	NUTS_ASSERT((lfd = socket(AF_INET, SOCK_STREAM, 0)) >= 0);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family      = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	NUTS_ASSERT(bind(lfd, (struct sockaddr *) &sin, sizeof(sin)) == 0);
	NUTS_ASSERT(listen(lfd, 8) == 0);
	high = move_above_select_limit(lfd);

	NUTS_PASS(nng_stream_listener_alloc(&l, "tcp://127.0.0.1"));

#ifdef NNG_POLLQ_SELECT
	struct sockaddr_in refused;
	socklen_t          len = sizeof(refused);
	int                port;

	NUTS_ASSERT(getsockname(high, (void *) &refused, &len) == 0);

	// A rejected descriptor still belongs to the caller.
	NUTS_FAIL(nng_stream_listener_set_int(l, NNG_OPT_LISTEN_FD, high),
	    NNG_ENOFILES);
	NUTS_TRUE(fd_is_open(high));

	// The listener keeps the address of its URL, so it can still listen
	// there instead.
	NUTS_PASS(nng_stream_listener_listen(l));
	NUTS_PASS(nng_stream_listener_get_int(l, NNG_OPT_BOUND_PORT, &port));
	NUTS_TRUE(port != ntohs(refused.sin_port));
	nng_stream_listener_free(l);
	(void) close(high);
#else
	nng_stream_dialer *d;
	nng_aio           *daio;
	nng_aio           *laio;
	int                port;
	char               url[64];

	NUTS_PASS(nng_stream_listener_set_int(l, NNG_OPT_LISTEN_FD, high));
	NUTS_PASS(nng_stream_listener_get_int(l, NNG_OPT_BOUND_PORT, &port));
	(void) snprintf(url, sizeof(url), "tcp://127.0.0.1:%d", port);

	NUTS_PASS(nng_aio_alloc(&daio, NULL, NULL));
	NUTS_PASS(nng_aio_alloc(&laio, NULL, NULL));
	nng_aio_set_timeout(daio, 5000);
	nng_aio_set_timeout(laio, 5000);
	NUTS_PASS(nng_stream_dialer_alloc(&d, url));

	// The accept is posted first, so it has to wait for the poller to
	// report the listening descriptor readable.
	nng_stream_listener_accept(l, laio);
	nng_msleep(50);
	nng_stream_dialer_dial(d, daio);
	nng_aio_wait(laio);
	nng_aio_wait(daio);
	NUTS_PASS(nng_aio_result(laio));
	NUTS_PASS(nng_aio_result(daio));

	nng_stream_free(nng_aio_get_output(laio, 0));
	nng_stream_free(nng_aio_get_output(daio, 0));
	nng_stream_dialer_free(d);
	nng_aio_free(daio);
	nng_aio_free(laio);
	nng_stream_listener_free(l);
#endif
}

void
test_pollq_ipc_listen_fd_above_select_limit(void)
{
	nng_stream_listener *l;
	struct sockaddr_un   sun;
	char                 path[64];
	char                 url[96];
	int                  lfd;
	int                  high;

	if (!allow_above_select_limit()) {
		NUTS_SKIP("cannot open descriptors above FD_SETSIZE");
		return;
	}
	(void) snprintf(
	    path, sizeof(path), "/tmp/nng_pollq_test_%d", (int) getpid());
	(void) snprintf(url, sizeof(url), "ipc://%s.url", path);
	(void) unlink(path);

	NUTS_ASSERT((lfd = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	(void) snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", path);
	NUTS_ASSERT(bind(lfd, (struct sockaddr *) &sun, sizeof(sun)) == 0);
	NUTS_ASSERT(listen(lfd, 8) == 0);
	high = move_above_select_limit(lfd);

	NUTS_PASS(nng_stream_listener_alloc(&l, url));
#ifdef NNG_POLLQ_SELECT
	struct stat st;

	// A rejected descriptor still belongs to the caller.
	NUTS_FAIL(nng_stream_listener_set_int(l, NNG_OPT_LISTEN_FD, high),
	    NNG_ENOFILES);
	NUTS_TRUE(fd_is_open(high));

	// The listener keeps the path of its URL, so it can still listen
	// there instead, and leaves the caller's socket alone.
	NUTS_PASS(nng_stream_listener_listen(l));
	NUTS_TRUE(stat(path, &st) == 0);
#else
	NUTS_PASS(nng_stream_listener_set_int(l, NNG_OPT_LISTEN_FD, high));
#endif
	nng_stream_listener_free(l);
	(void) close(high);
	(void) unlink(path);
}

// The select poller watches its own waker pipe with select() too, so the
// pipe must also be below FD_SETSIZE.  When the library is initialized
// while every lower descriptor is in use, the initialization must fail.
void
test_pollq_waker_above_select_limit(void)
{
	int     fillers[FD_SETSIZE];
	int     nfill = 0;
	int     fd;
	nng_err rv;

	if (!allow_above_select_limit()) {
		NUTS_SKIP("cannot open descriptors above FD_SETSIZE");
		return;
	}
	nng_fini();

	// Take every free descriptor below FD_SETSIZE.
	NUTS_ASSERT((fd = open("/dev/null", O_RDONLY)) >= 0);
	NUTS_ASSERT(fd < FD_SETSIZE);
	fillers[nfill++] = fd;
	while ((fd = fcntl(fillers[0], F_DUPFD, 0)) < FD_SETSIZE) {
		NUTS_ASSERT(fd >= 0);
		fillers[nfill++] = fd;
	}
	(void) close(fd);

	rv = nng_init(NULL);
	for (int i = 0; i < nfill; i++) {
		(void) close(fillers[i]);
	}
#ifdef NNG_POLLQ_SELECT
	NUTS_FAIL(rv, NNG_ENOFILES);
#else
	NUTS_PASS(rv);
#endif
	// Leave the library initialized, as the test found it.
	if (rv == NNG_OK) {
		nng_fini();
	}
	NUTS_PASS(nng_init(NULL));
}

// A listener that never listened has no descriptor registered with the
// poller.  Closing it must not touch descriptor 0, which belongs to the
// application.
void
test_pollq_close_unregistered_listener(void)
{
	const char *urls[] = { "tcp://127.0.0.1:0",
		"ipc:///tmp/nng_pollq_unregistered_test" };
	int         fds[2];
	int         saved;
	char        c = 0;
	ssize_t     n_peer;
	ssize_t     n_recv;
	ssize_t     n_send;

	// Put one end of a socket pair on descriptor 0, as an inetd or
	// socket-activated service would have.
	NUTS_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	NUTS_ASSERT((saved = dup(0)) >= 0);
	NUTS_ASSERT(dup2(fds[0], 0) == 0);
	(void) close(fds[0]);

	for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
		nng_stream_listener *l;

		NUTS_PASS(nng_stream_listener_alloc(&l, urls[i]));
		nng_stream_listener_close(l);
		nng_stream_listener_free(l);
	}

	// Descriptor 0 must still work both ways.  Restore it before
	// checking anything.
	n_peer = send(fds[1], "x", 1, MSG_NOSIGNAL);
	n_recv = recv(0, &c, 1, MSG_DONTWAIT);
	n_send = send(0, "y", 1, MSG_NOSIGNAL);

	(void) dup2(saved, 0);
	(void) close(saved);
	(void) close(fds[1]);

	NUTS_TRUE(n_peer == 1);
	NUTS_TRUE(n_recv == 1);
	NUTS_TRUE(c == 'x');
	NUTS_TRUE(n_send == 1);
}

NUTS_TESTS = {
#ifdef NNG_TRANSPORT_FDC
	{ "pollq socket fd above select limit",
	    test_pollq_socket_fd_above_select_limit },
#endif
	{ "pollq tcp listen fd above select limit",
	    test_pollq_tcp_listen_fd_above_select_limit },
	{ "pollq ipc listen fd above select limit",
	    test_pollq_ipc_listen_fd_above_select_limit },
	{ "pollq waker above select limit",
	    test_pollq_waker_above_select_limit },
	{ "pollq close unregistered listener",
	    test_pollq_close_unregistered_listener },
	{ NULL, NULL },
};
