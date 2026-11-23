//
// Copyright 2026 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "../testing/nuts.h"
#include "msgqueue.h"

static void
put_value(nni_msgq *mq, uint32_t v)
{
	nng_msg *msg;

	NUTS_PASS(nng_msg_alloc(&msg, 0));
	NUTS_PASS(nng_msg_append_u32(msg, v));
	NUTS_PASS(nni_msgq_tryput(mq, msg));
}

static uint32_t
get_value(nni_msgq *mq, nng_aio *aio)
{
	nng_msg *msg;
	uint32_t v;

	nni_msgq_aio_get(mq, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	msg = nng_aio_get_msg(aio);
	NUTS_PASS(nng_msg_trim_u32(msg, &v));
	nng_msg_free(msg);
	return (v);
}

// Shrinking a queue below the number of messages it holds drops the
// oldest ones, and has to follow the ring around its end to find them.
static void
test_msgq_shrink_across_end(void)
{
	nni_msgq *mq;
	nng_aio  *aio;
	uint32_t  v = 0;

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 1000);

	// A queue of capacity 4 has a ring of 6 slots.  Pass 5 messages
	// through, so that the next one goes into the last slot.
	NUTS_PASS(nni_msgq_init(&mq, 4));
	for (int i = 0; i < 5; i++) {
		put_value(mq, v);
		NUTS_TRUE(get_value(mq, aio) == v);
		v++;
	}

	// Fill the queue, wrapping around the end of the ring, then shrink
	// it.  Only the newest message is kept.
	for (int i = 0; i < 4; i++) {
		put_value(mq, v++);
	}
	NUTS_PASS(nni_msgq_resize(mq, 0));
	NUTS_TRUE(nni_msgq_cap(mq) == 0);
	NUTS_TRUE(get_value(mq, aio) == v - 1);

	nni_msgq_fini(mq);
	nng_aio_free(aio);
}

NUTS_TESTS = {
	{ "msgq shrink across the end", test_msgq_shrink_across_end },
	{ NULL, NULL },
};
