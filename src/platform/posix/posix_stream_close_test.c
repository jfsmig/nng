//
// Copyright 2026 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <sys/socket.h>
#include <unistd.h>

#include <nuts.h>

// Operations started on a stream after it has been closed must fail right
// away.  If they waited on the closed stream instead, they would also
// outlive it once the stream is freed.
static void
check_io_after_close(nng_stream *s)
{
	nng_aio *aio;
	nng_iov  iov;
	char     buf[4];

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 1000);
	iov.iov_buf = buf;
	iov.iov_len = sizeof(buf);
	NUTS_PASS(nng_aio_set_iov(aio, 1, &iov));

	nng_stream_close(s);
	nng_stream_recv(s, aio);
	nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECLOSED);
	nng_stream_send(s, aio);
	nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECLOSED);

	nng_stream_recv(s, aio);
	nng_stream_free(s);
	nng_aio_wait(aio);
	NUTS_FAIL(nng_aio_result(aio), NNG_ECLOSED);

	nng_aio_free(aio);
}

// connect_streams dials the listener at url and accepts the connection.
static void
connect_streams(
    nng_stream_listener *l, const char *url, nng_stream **dp, nng_stream **ap)
{
	nng_stream_dialer *d;
	nng_aio           *daio;
	nng_aio           *laio;

	NUTS_PASS(nng_aio_alloc(&daio, NULL, NULL));
	NUTS_PASS(nng_aio_alloc(&laio, NULL, NULL));
	nng_aio_set_timeout(daio, 5000);
	nng_aio_set_timeout(laio, 5000);

	NUTS_PASS(nng_stream_dialer_alloc(&d, url));
	nng_stream_dialer_dial(d, daio);
	nng_stream_listener_accept(l, laio);
	nng_aio_wait(daio);
	nng_aio_wait(laio);
	NUTS_PASS(nng_aio_result(daio));
	NUTS_PASS(nng_aio_result(laio));
	*dp = nng_aio_get_output(daio, 0);
	*ap = nng_aio_get_output(laio, 0);

	nng_stream_dialer_free(d);
	nng_aio_free(daio);
	nng_aio_free(laio);
}

void
test_tcp_io_after_close(void)
{
	nng_stream_listener *l;
	nng_stream          *dialed;
	nng_stream          *accepted;
	char                 url[64];
	int                  port;

	NUTS_PASS(nng_stream_listener_alloc(&l, "tcp://127.0.0.1"));
	NUTS_PASS(nng_stream_listener_listen(l));
	NUTS_PASS(nng_stream_listener_get_int(l, NNG_OPT_BOUND_PORT, &port));
	(void) snprintf(url, sizeof(url), "tcp://127.0.0.1:%d", port);
	connect_streams(l, url, &dialed, &accepted);

	check_io_after_close(dialed);

	nng_stream_free(accepted);
	nng_stream_listener_free(l);
}

void
test_ipc_io_after_close(void)
{
	nng_stream_listener *l;
	nng_stream          *dialed;
	nng_stream          *accepted;
	char                *addr;

	NUTS_ADDR(addr, "ipc");
	NUTS_PASS(nng_stream_listener_alloc(&l, addr));
	NUTS_PASS(nng_stream_listener_listen(l));
	connect_streams(l, addr, &dialed, &accepted);

	check_io_after_close(dialed);

	nng_stream_free(accepted);
	nng_stream_listener_free(l);
}

void
test_sockfd_io_after_close(void)
{
	nng_stream_listener *l;
	nng_aio             *aio;
	int                  fds[2];

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 5000);
	NUTS_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	NUTS_PASS(nng_stream_listener_alloc(&l, "socket://"));
	NUTS_PASS(nng_stream_listener_listen(l));
	NUTS_PASS(nng_stream_listener_set_int(l, NNG_OPT_SOCKET_FD, fds[0]));
	nng_stream_listener_accept(l, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));

	check_io_after_close(nng_aio_get_output(aio, 0));

	nng_stream_listener_free(l);
	nng_aio_free(aio);
	(void) close(fds[1]);
}

NUTS_TESTS = {
	{ "tcp stream io after close", test_tcp_io_after_close },
	{ "ipc stream io after close", test_ipc_io_after_close },
	{ "socket stream io after close", test_sockfd_io_after_close },
	{ NULL, NULL },
};
