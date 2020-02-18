/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*! \file */

#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>

#include <isc/app.h>
#include <isc/boolean.h>
#include <isc/event.h>

#include <string.h>
#include <isc/task.h>
#include <isc/time.h>
#include <isc/util.h>

/*%
 * For BIND9 internal applications built with threads, we use a single app
 * context and let multiple worker, I/O, timer threads do actual jobs.
 * For other cases (including BIND9 built without threads) an app context acts
 * as an event loop dispatching various events.
 */
#include "../timer_p.h"
#include "../task_p.h"
#include "socket_p.h"

/*
 * The application context of this module.  This implementation actually
 * doesn't use it. (This may change in the future).
 */

typedef struct isc_appctx {
	isc_eventlist_t		on_run;
	isc_boolean_t		shutdown_requested;
	isc_boolean_t		running;

	/*!
	 * We assume that 'want_shutdown' can be read and written atomically.
	 */
	isc_boolean_t		want_shutdown;
	/*
	 * We assume that 'want_reload' can be read and written atomically.
	 */
	isc_boolean_t		want_reload;

	isc_boolean_t		blocked;

	isc_taskmgr_t		*taskmgr;
	isc_socketmgr_t		*socketmgr;
	isc_timermgr_t		*timermgr;
} isc_appctx_t;

static isc_appctx_t isc_g_appctx;
static isc_boolean_t is_running = ISC_FALSE;

static isc_result_t isc_app_ctxonrun(isc_appctx_t *ctx, isc_task_t *task,
    isc_taskaction_t action, void *arg);

static void
reload_action(int arg) {
	UNUSED(arg);
	isc_g_appctx.want_reload = ISC_TRUE;
}

static isc_result_t
handle_signal(int sig, void (*handler)(int)) {
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;

	if (sigfillset(&sa.sa_mask) != 0 ||
	    sigaction(sig, &sa, NULL) < 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "handle_signal() %d setup: %s", sig,
				 strerror(errno));
		return (ISC_R_UNEXPECTED);
	}

	return (ISC_R_SUCCESS);
}

static isc_result_t
isc_app_ctxstart(isc_appctx_t *ctx) {
	isc_result_t result;
	int presult;
	sigset_t sset;

	/*
	 * Start an ISC library application.
	 */

	ISC_LIST_INIT(ctx->on_run);

	ctx->shutdown_requested = ISC_FALSE;
	ctx->running = ISC_FALSE;
	ctx->want_shutdown = ISC_FALSE;
	ctx->want_reload = ISC_FALSE;
	ctx->blocked = ISC_FALSE;

	/*
	 * Always ignore SIGPIPE.
	 */
	result = handle_signal(SIGPIPE, SIG_IGN);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	/*
	 * On Solaris 2, delivery of a signal whose action is SIG_IGN
	 * will not cause sigwait() to return. We may have inherited
	 * unexpected actions for SIGHUP, SIGINT, and SIGTERM from our parent
	 * process (e.g, Solaris cron).  Set an action of SIG_DFL to make
	 * sure sigwait() works as expected.  Only do this for SIGTERM and
	 * SIGINT if we don't have sigwait(), since a different handler is
	 * installed above.
	 */
	result = handle_signal(SIGHUP, SIG_DFL);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = handle_signal(SIGTERM, SIG_DFL);
	if (result != ISC_R_SUCCESS)
		goto cleanup;
	result = handle_signal(SIGINT, SIG_DFL);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	/*
	 * Unblock SIGHUP, SIGINT, SIGTERM.
	 *
	 * If we're not using threads, we need to make sure that SIGHUP,
	 * SIGINT and SIGTERM are not inherited as blocked from the parent
	 * process.
	 */
	if (sigemptyset(&sset) != 0 ||
	    sigaddset(&sset, SIGHUP) != 0 ||
	    sigaddset(&sset, SIGINT) != 0 ||
	    sigaddset(&sset, SIGTERM) != 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_app_start() sigsetops: %s",
				 strerror(errno));
		result = ISC_R_UNEXPECTED;
		goto cleanup;
	}
	presult = sigprocmask(SIG_UNBLOCK, &sset, NULL);
	if (presult != 0) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_app_start() sigprocmask: %s",
				 strerror(errno));
		result = ISC_R_UNEXPECTED;
		goto cleanup;
	}

	return (ISC_R_SUCCESS);

 cleanup:
	return (result);
}

isc_result_t
isc_app_start(void) {
	/* The remaining members will be initialized in ctxstart() */

	return (isc_app_ctxstart((isc_appctx_t *)&isc_g_appctx));
}

isc_result_t
isc_app_onrun(isc_task_t *task, isc_taskaction_t action,
	      void *arg)
{
	return (isc_app_ctxonrun((isc_appctx_t *)&isc_g_appctx,
				  task, action, arg));
}

isc_result_t
isc_app_ctxonrun(isc_appctx_t *ctx, isc_task_t *task,
		  isc_taskaction_t action, void *arg)
{
	isc_event_t *event;
	isc_task_t *cloned_task = NULL;
	isc_result_t result;

	if (ctx->running) {
		result = ISC_R_ALREADYRUNNING;
		goto unlock;
	}

	/*
	 * Note that we store the task to which we're going to send the event
	 * in the event's "sender" field.
	 */
	isc_task_attach(task, &cloned_task);
	event = isc_event_allocate(cloned_task, ISC_APPEVENT_SHUTDOWN,
				   action, arg, sizeof(*event));
	if (event == NULL) {
		isc_task_detach(&cloned_task);
		result = ISC_R_NOMEMORY;
		goto unlock;
	}

	ISC_LIST_APPEND(ctx->on_run, event, ev_link);

	result = ISC_R_SUCCESS;

 unlock:
	return (result);
}

/*!
 * Event loop for nonthreaded programs.
 */
static isc_result_t
evloop(isc_appctx_t *ctx) {
	isc_result_t result;

	while (!ctx->want_shutdown) {
		int n;
		struct timespec when, now;
		struct timeval tv, *tvp;
		isc_socketwait_t *swait;
		isc_boolean_t readytasks;
		isc_boolean_t call_timer_dispatch = ISC_FALSE;

		/*
		 * Check the reload (or suspend) case first for exiting the
		 * loop as fast as possible in case:
		 *   - the direct call to isc_taskmgr_dispatch() in
		 *     isc_app_ctxrun() completes all the tasks so far,
		 *   - there is thus currently no active task, and
		 *   - there is a timer event
		 */
		if (ctx->want_reload) {
			ctx->want_reload = ISC_FALSE;
			return (ISC_R_RELOAD);
		}

		readytasks = isc_taskmgr_ready(ctx->taskmgr);
		if (readytasks) {
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			tvp = &tv;
			call_timer_dispatch = ISC_TRUE;
		} else {
			result = isc_timermgr_nextevent(ctx->timermgr, &when);
			if (result != ISC_R_SUCCESS)
				tvp = NULL;
			else {
				uint64_t us;

				clock_gettime(CLOCK_REALTIME, &now);
				us = isc_time_microdiff(&when, &now);
				if (us == 0)
					call_timer_dispatch = ISC_TRUE;
				tv.tv_sec = us / 1000000;
				tv.tv_usec = us % 1000000;
				tvp = &tv;
			}
		}

		swait = NULL;
		n = isc_socketmgr_waitevents(ctx->socketmgr, tvp, &swait);

		if (n == 0 || call_timer_dispatch) {
			/*
			 * We call isc_timermgr_dispatch() only when
			 * necessary, in order to reduce overhead.  If the
			 * select() call indicates a timeout, we need the
			 * dispatch.  Even if not, if we set the 0-timeout
			 * for the select() call, we need to check the timer
			 * events.  In the 'readytasks' case, there may be no
			 * timeout event actually, but there is no other way
			 * to reduce the overhead.
			 * Note that we do not have to worry about the case
			 * where a new timer is inserted during the select()
			 * call, since this loop only runs in the non-thread
			 * mode.
			 */
			isc_timermgr_dispatch(ctx->timermgr);
		}
		if (n > 0)
			(void)isc_socketmgr_dispatch(ctx->socketmgr, swait);
		(void)isc_taskmgr_dispatch(ctx->taskmgr);
	}
	return (ISC_R_SUCCESS);
}

static isc_result_t
isc_app_ctxrun(isc_appctx_t *ctx) {
	int result;
	isc_event_t *event, *next_event;
	isc_task_t *task;

	if (!ctx->running) {
		ctx->running = ISC_TRUE;

		/*
		 * Post any on-run events (in FIFO order).
		 */
		for (event = ISC_LIST_HEAD(ctx->on_run);
		     event != NULL;
		     event = next_event) {
			next_event = ISC_LIST_NEXT(event, ev_link);
			ISC_LIST_UNLINK(ctx->on_run, event, ev_link);
			task = event->ev_sender;
			event->ev_sender = NULL;
			isc_task_sendanddetach(&task, &event);
		}

	}

	if (ctx == &isc_g_appctx) {
		result = handle_signal(SIGHUP, reload_action);
		if (result != ISC_R_SUCCESS)
			return (ISC_R_SUCCESS);
	}

	(void) isc_taskmgr_dispatch(ctx->taskmgr);
	result = evloop(ctx);
	return (result);
}

isc_result_t
isc_app_run(void) {
	return (isc_app_ctxrun((isc_appctx_t *)&isc_g_appctx));
}

static isc_result_t
isc_app_ctxshutdown(isc_appctx_t *ctx) {
	isc_boolean_t want_kill = ISC_TRUE;

	REQUIRE(ctx->running);

	if (ctx->shutdown_requested)
		want_kill = ISC_FALSE;
	else
		ctx->shutdown_requested = ISC_TRUE;

	if (want_kill) {
		if (ctx != &isc_g_appctx)
			/* BIND9 internal, but using multiple contexts */
			ctx->want_shutdown = ISC_TRUE;
		else {
			ctx->want_shutdown = ISC_TRUE;
		}
	}

	return (ISC_R_SUCCESS);
}

isc_boolean_t
isc_app_isrunning() {
	return (is_running);
}

isc_result_t
isc_app_shutdown(void) {
	return (isc_app_ctxshutdown((isc_appctx_t *)&isc_g_appctx));
}

void
isc_app_block(void) {
	REQUIRE(isc_g_appctx.running);
	REQUIRE(!isc_g_appctx.blocked);

	isc_g_appctx.blocked = ISC_TRUE;
}

void
isc_app_unblock(void) {

	REQUIRE(isc_g_appctx.running);
	REQUIRE(isc_g_appctx.blocked);

	isc_g_appctx.blocked = ISC_FALSE;

}
