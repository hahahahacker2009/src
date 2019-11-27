/*	$OpenBSD: resolver.c,v 1.79 2019/11/27 17:11:00 florian Exp $	*/

/*
 * Copyright (c) 2018 Florian Obser <florian@openbsd.org>
 * Copyright (c) 2004, 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004 Esben Norby <norby@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/time.h>

#include <net/route.h>

#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <limits.h>
#include <netdb.h>
#include <asr.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tls.h>
#include <unistd.h>

#include "libunbound/config.h"
#include "libunbound/libunbound/libworker.h"
#include "libunbound/libunbound/unbound.h"
#include "libunbound/libunbound/unbound-event.h"
#include "libunbound/sldns/sbuffer.h"
#include "libunbound/sldns/rrdef.h"
#include "libunbound/sldns/pkthdr.h"
#include "libunbound/sldns/wire2str.h"
#include "libunbound/util/regional.h"

#include <openssl/crypto.h>

#include "log.h"
#include "frontend.h"
#include "unwind.h"
#include "resolver.h"

#define	TLS_DEFAULT_CA_CERT_FILE	"/etc/ssl/cert.pem"
#define	UB_LOG_VERBOSE			4
#define	UB_LOG_BRIEF			0

#define	RESOLVER_CHECK_SEC		1
#define	RESOLVER_CHECK_MAXSEC		1024 /* ~17 minutes */

#define	TRUST_ANCHOR_RETRY_INTERVAL	8640
#define	TRUST_ANCHOR_QUERY_INTERVAL	43200

/* in libworker_event_done_cb() enum sec_status gets mapped to 0, 1 and 2 */
#define	INSECURE	0
#define	BOGUS		1
#define	SECURE		2

struct uw_resolver {
	struct event		 check_ev;
	struct event		 free_ev;
	struct ub_ctx		*ctx;
	void			*asr_ctx;
	struct timeval		 check_tv;
	int			 ref_cnt;
	int			 stop;
	enum uw_resolver_state	 state;
	enum uw_resolver_type	 type;
	int			 oppdot;
	int			 check_running;
	char			*why_bogus;
	int64_t			 histogram[nitems(histogram_limits)];
};

typedef void (*resolve_cb_t)(struct uw_resolver *, void *, int, void *, int,
    int, char *);

struct resolver_cb_data {
	resolve_cb_t		 cb;
	void			*data;
	struct uw_resolver	*res;
};

__dead void		 resolver_shutdown(void);
void			 resolver_sig_handler(int sig, short, void *);
void			 resolver_dispatch_frontend(int, short, void *);
void			 resolver_dispatch_main(int, short, void *);
int			 resolve(struct uw_resolver *, const char*, int, int,
			     void*, resolve_cb_t);
void			 resolve_done(struct uw_resolver *, void *, int, void *,
			     int, int, char *);
void			 ub_resolve_done(void *, int, void *, int, int, char *,
			     int);
void			 asr_resolve_done(struct asr_result *, void *);
void			 new_recursor(void);
void			 new_forwarders(int);
void			 new_asr_forwarders(void);
void			 new_static_forwarders(int);
void			 new_static_dot_forwarders(void);
struct uw_resolver	*create_resolver(enum uw_resolver_type, int);
void			 free_resolver(struct uw_resolver *);
void			 set_forwarders(struct uw_resolver *,
			     struct uw_forwarder_head *);
void			 set_forwarders_oppdot(struct uw_resolver *,
			     struct uw_forwarder_head *, int);
void			 resolver_check_timo(int, short, void *);
void			 resolver_free_timo(int, short, void *);
void			 check_resolver(struct uw_resolver *);
void			 check_resolver_done(struct uw_resolver *, void *, int,
			     void *, int, int, char *);
void			 schedule_recheck_all_resolvers(void);
int			 check_forwarders_changed(struct uw_forwarder_head *,
			     struct uw_forwarder_head *);
void			 replace_forwarders(struct uw_forwarder_head *,
			     struct uw_forwarder_head *);
void			 resolver_ref(struct uw_resolver *);
void			 resolver_unref(struct uw_resolver *);
struct uw_resolver	*best_resolver(void);
int			 resolver_cmp(struct uw_resolver *,
			     struct uw_resolver *);
void			 restart_resolvers(void);
void			 show_status(enum uw_resolver_type, pid_t);
void			 send_resolver_info(struct uw_resolver *, int, pid_t);
void			 send_detailed_resolver_info(struct uw_resolver *,
			     pid_t);
void			 send_resolver_histogram_info(struct uw_resolver *,
			     pid_t);
void			 trust_anchor_resolve(void);
void			 trust_anchor_timo(int, short, void *);
void			 trust_anchor_resolve_done(struct uw_resolver *, void *,
			     int, void *, int, int, char *);
void			 replace_autoconf_forwarders(struct
			     imsg_rdns_proposal *);
struct uw_forwarder	*find_forwarder(struct uw_forwarder_head *,
    			     const char *);

struct uw_conf			*resolver_conf;
struct imsgev			*iev_frontend;
struct imsgev			*iev_main;
struct uw_forwarder_head	 autoconf_forwarder_list;
struct uw_resolver		*resolvers[UW_RES_NONE];

struct event			 trust_anchor_timer;

static struct trust_anchor_head	 trust_anchors, new_trust_anchors;

struct event_base		*ev_base;

static const char * const	 as112_zones[] = {
	/* RFC1918 */
	"10.in-addr.arpa. transparent",
	"16.172.in-addr.arpa. transparent",
	"31.172.in-addr.arpa. transparent",
	"168.192.in-addr.arpa. transparent",

	/* RFC3330 */
	"0.in-addr.arpa. transparent",
	"254.169.in-addr.arpa. transparent",
	"2.0.192.in-addr.arpa. transparent",
	"100.51.198.in-addr.arpa. transparent",
	"113.0.203.in-addr.arpa. transparent",
	"255.255.255.255.in-addr.arpa. transparent",

	/* RFC6598 */
	"64.100.in-addr.arpa. transparent",
	"65.100.in-addr.arpa. transparent",
	"66.100.in-addr.arpa. transparent",
	"67.100.in-addr.arpa. transparent",
	"68.100.in-addr.arpa. transparent",
	"69.100.in-addr.arpa. transparent",
	"70.100.in-addr.arpa. transparent",
	"71.100.in-addr.arpa. transparent",
	"72.100.in-addr.arpa. transparent",
	"73.100.in-addr.arpa. transparent",
	"74.100.in-addr.arpa. transparent",
	"75.100.in-addr.arpa. transparent",
	"76.100.in-addr.arpa. transparent",
	"77.100.in-addr.arpa. transparent",
	"78.100.in-addr.arpa. transparent",
	"79.100.in-addr.arpa. transparent",
	"80.100.in-addr.arpa. transparent",
	"81.100.in-addr.arpa. transparent",
	"82.100.in-addr.arpa. transparent",
	"83.100.in-addr.arpa. transparent",
	"84.100.in-addr.arpa. transparent",
	"85.100.in-addr.arpa. transparent",
	"86.100.in-addr.arpa. transparent",
	"87.100.in-addr.arpa. transparent",
	"88.100.in-addr.arpa. transparent",
	"89.100.in-addr.arpa. transparent",
	"90.100.in-addr.arpa. transparent",
	"91.100.in-addr.arpa. transparent",
	"92.100.in-addr.arpa. transparent",
	"93.100.in-addr.arpa. transparent",
	"94.100.in-addr.arpa. transparent",
	"95.100.in-addr.arpa. transparent",
	"96.100.in-addr.arpa. transparent",
	"97.100.in-addr.arpa. transparent",
	"98.100.in-addr.arpa. transparent",
	"99.100.in-addr.arpa. transparent",
	"100.100.in-addr.arpa. transparent",
	"101.100.in-addr.arpa. transparent",
	"102.100.in-addr.arpa. transparent",
	"103.100.in-addr.arpa. transparent",
	"104.100.in-addr.arpa. transparent",
	"105.100.in-addr.arpa. transparent",
	"106.100.in-addr.arpa. transparent",
	"107.100.in-addr.arpa. transparent",
	"108.100.in-addr.arpa. transparent",
	"109.100.in-addr.arpa. transparent",
	"110.100.in-addr.arpa. transparent",
	"111.100.in-addr.arpa. transparent",
	"112.100.in-addr.arpa. transparent",
	"113.100.in-addr.arpa. transparent",
	"114.100.in-addr.arpa. transparent",
	"115.100.in-addr.arpa. transparent",
	"116.100.in-addr.arpa. transparent",
	"117.100.in-addr.arpa. transparent",
	"118.100.in-addr.arpa. transparent",
	"119.100.in-addr.arpa. transparent",
	"120.100.in-addr.arpa. transparent",
	"121.100.in-addr.arpa. transparent",
	"122.100.in-addr.arpa. transparent",
	"123.100.in-addr.arpa. transparent",
	"124.100.in-addr.arpa. transparent",
	"125.100.in-addr.arpa. transparent",
	"126.100.in-addr.arpa. transparent",
	"127.100.in-addr.arpa. transparent",

	/* RFC4291 */
	"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0."
	"ip6.arpa. transparent",

	/* RFC4193 */
	"D.F.ip6.arpa. transparent",

	/* RFC4291 */
	"8.E.F.ip6.arpa. transparent",
	"9.E.F.ip6.arpa. transparent",
	"A.E.F.ip6.arpa. transparent",
	"B.E.F.ip6.arpa. transparent",

	/* RFC3849 */
	"8.B.D.0.1.0.0.2.ip6.arpa. transparent"
};

void
resolver_sig_handler(int sig, short event, void *arg)
{
	/*
	 * Normal signal handler rules don't apply because libevent
	 * decouples for us.
	 */

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		resolver_shutdown();
	default:
		fatalx("unexpected signal");
	}
}

void
resolver(int debug, int verbose)
{
	struct event	 ev_sigint, ev_sigterm;
	struct passwd	*pw;

	resolver_conf = config_new_empty();

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);

	if ((pw = getpwnam(UNWIND_USER)) == NULL)
		fatal("getpwnam");

	uw_process = PROC_RESOLVER;
	setproctitle("%s", log_procnames[uw_process]);
	log_procinit(log_procnames[uw_process]);

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	if (unveil(TLS_DEFAULT_CA_CERT_FILE, "r") == -1)
		fatal("unveil");

	if (pledge("stdio inet dns rpath recvfd", NULL) == -1)
		fatal("pledge");

	ev_base = event_init();

	/* Setup signal handler(s). */
	signal_set(&ev_sigint, SIGINT, resolver_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, resolver_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	/* Setup pipe and event handler to the main process. */
	if ((iev_main = malloc(sizeof(struct imsgev))) == NULL)
		fatal(NULL);

	imsg_init(&iev_main->ibuf, 3);
	iev_main->handler = resolver_dispatch_main;

	/* Setup event handlers. */
	iev_main->events = EV_READ;
	event_set(&iev_main->ev, iev_main->ibuf.fd, iev_main->events,
	    iev_main->handler, iev_main);
	event_add(&iev_main->ev, NULL);

	evtimer_set(&trust_anchor_timer, trust_anchor_timo, NULL);

	new_recursor();

	TAILQ_INIT(&autoconf_forwarder_list);
	TAILQ_INIT(&trust_anchors);
	TAILQ_INIT(&new_trust_anchors);

	event_dispatch();

	resolver_shutdown();
}

__dead void
resolver_shutdown(void)
{
	log_debug("%s", __func__);

	/* Close pipes. */
	msgbuf_clear(&iev_frontend->ibuf.w);
	close(iev_frontend->ibuf.fd);
	msgbuf_clear(&iev_main->ibuf.w);
	close(iev_main->ibuf.fd);

	config_clear(resolver_conf);

	free(iev_frontend);
	free(iev_main);

	log_info("resolver exiting");
	exit(0);
}

int
resolver_imsg_compose_main(int type, pid_t pid, void *data, uint16_t datalen)
{
	return (imsg_compose_event(iev_main, type, 0, pid, -1, data, datalen));
}

int
resolver_imsg_compose_frontend(int type, pid_t pid, void *data,
    uint16_t datalen)
{
	return (imsg_compose_event(iev_frontend, type, 0, pid, -1,
	    data, datalen));
}

void
resolver_dispatch_frontend(int fd, short event, void *bula)
{
	struct imsgev			*iev = bula;
	struct imsgbuf			*ibuf;
	struct imsg			 imsg;
	struct query_imsg		*query_imsg;
	struct uw_resolver		*res;
	enum uw_resolver_type		 type;
	ssize_t				 n;
	int				 shut = 0, verbose;
	int				 update_resolvers;
	char				*ta;

	ibuf = &iev->ibuf;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("%s: imsg_get error", __func__);
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		case IMSG_CTL_LOG_VERBOSE:
			if (IMSG_DATA_SIZE(imsg) != sizeof(verbose))
				fatalx("%s: IMSG_CTL_LOG_VERBOSE wrong length: "
				    "%lu", __func__,
				    IMSG_DATA_SIZE(imsg));
			memcpy(&verbose, imsg.data, sizeof(verbose));
			update_resolvers = (log_getverbose() & OPT_VERBOSE2)
			    != (verbose & OPT_VERBOSE2);
			log_setverbose(verbose);
			if (update_resolvers)
				restart_resolvers();
			break;
		case IMSG_QUERY:
			if (IMSG_DATA_SIZE(imsg) != sizeof(*query_imsg))
				fatalx("%s: IMSG_QUERY wrong length: %lu",
				    __func__, IMSG_DATA_SIZE(imsg));
			if ((query_imsg = malloc(sizeof(*query_imsg))) ==
			    NULL) {
				log_warn("cannot allocate query");
				break;
			}
			memcpy(query_imsg, imsg.data, sizeof(*query_imsg));

			log_debug("%s: IMSG_QUERY[%llu], qname: %s, t: %d, "
			    "c: %d", __func__, query_imsg->id,
			    query_imsg->qname, query_imsg->t, query_imsg->c);

			res = best_resolver();

			if (res == NULL) {
				log_warnx("can't find working resolver");
				free(query_imsg);
				break;
			}

			log_debug("%s: choosing %s", __func__,
			    uw_resolver_type_str[res->type]);

			clock_gettime(CLOCK_MONOTONIC, &query_imsg->tp);

			if (resolve(res, query_imsg->qname, query_imsg->t,
			    query_imsg->c, query_imsg, resolve_done) != 0)
				free(query_imsg);
			break;
		case IMSG_CTL_STATUS:
			if (IMSG_DATA_SIZE(imsg) != sizeof(type))
				fatalx("%s: IMSG_CTL_STATUS wrong length: %lu",
				    __func__, IMSG_DATA_SIZE(imsg));
			memcpy(&type, imsg.data, sizeof(type));
			show_status(type, imsg.hdr.pid);
			break;
		case IMSG_NEW_TA:
			/* make sure this is a string */
			((char *)imsg.data)[IMSG_DATA_SIZE(imsg) - 1] = '\0';
			ta = imsg.data;
			add_new_ta(&new_trust_anchors, ta);
			break;
		case IMSG_NEW_TAS_ABORT:
			log_debug("%s: IMSG_NEW_TAS_ABORT", __func__);
			free_tas(&new_trust_anchors);
			break;
		case IMSG_NEW_TAS_DONE:
			log_debug("%s: IMSG_NEW_TAS_DONE", __func__);
			if (merge_tas(&new_trust_anchors, &trust_anchors)) {
				new_recursor();
				new_forwarders(0);
				new_asr_forwarders();
				new_static_forwarders(0);
				new_static_dot_forwarders();
			}
			break;
		case IMSG_NETWORK_CHANGED:
			schedule_recheck_all_resolvers();
			break;
		case IMSG_REPLACE_DNS:
			if (IMSG_DATA_SIZE(imsg) !=
			    sizeof(struct imsg_rdns_proposal))
				fatalx("%s: IMSG_ADD_DNS wrong length: %lu",
				    __func__, IMSG_DATA_SIZE(imsg));
			replace_autoconf_forwarders((struct
			    imsg_rdns_proposal *)imsg.data);
			break;
		default:
			log_debug("%s: unexpected imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

void
resolver_dispatch_main(int fd, short event, void *bula)
{
	static struct uw_conf	*nconf;
	struct imsg		 imsg;
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf;
	ssize_t			 n;
	int			 shut = 0, forwarders_changed;
	int			 dot_forwarders_changed;

	ibuf = &iev->ibuf;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* Connection closed. */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("%s: imsg_get error", __func__);
		if (n == 0)	/* No more messages. */
			break;

		switch (imsg.hdr.type) {
		case IMSG_SOCKET_IPC_FRONTEND:
			/*
			 * Setup pipe and event handler to the frontend
			 * process.
			 */
			if (iev_frontend)
				fatalx("%s: received unexpected imsg fd "
				    "to resolver", __func__);

			if ((fd = imsg.fd) == -1)
				fatalx("%s: expected to receive imsg fd to "
				   "resolver but didn't receive any", __func__);

			iev_frontend = malloc(sizeof(struct imsgev));
			if (iev_frontend == NULL)
				fatal(NULL);

			imsg_init(&iev_frontend->ibuf, fd);
			iev_frontend->handler = resolver_dispatch_frontend;
			iev_frontend->events = EV_READ;

			event_set(&iev_frontend->ev, iev_frontend->ibuf.fd,
			iev_frontend->events, iev_frontend->handler,
			    iev_frontend);
			event_add(&iev_frontend->ev, NULL);
			break;

		case IMSG_STARTUP:
			if (pledge("stdio inet dns rpath", NULL) == -1)
				fatal("pledge");
			break;
		case IMSG_RECONF_CONF:
		case IMSG_RECONF_BLOCKLIST_FILE:
		case IMSG_RECONF_FORWARDER:
		case IMSG_RECONF_DOT_FORWARDER:
			imsg_receive_config(&imsg, &nconf);
			break;
		case IMSG_RECONF_END:
			if (nconf == NULL)
				fatalx("%s: IMSG_RECONF_END without "
				    "IMSG_RECONF_CONF", __func__);
			forwarders_changed = check_forwarders_changed(
			    &resolver_conf->uw_forwarder_list,
			    &nconf->uw_forwarder_list);
			dot_forwarders_changed = check_forwarders_changed(
			    &resolver_conf->uw_dot_forwarder_list,
			    &nconf->uw_dot_forwarder_list);
			merge_config(resolver_conf, nconf);
			nconf = NULL;
			if (forwarders_changed) {
				log_debug("static forwarders changed");
				new_static_forwarders(0);
			}
			if (dot_forwarders_changed) {
				log_debug("static DoT forwarders changed");
				new_static_dot_forwarders();
			}
			break;
		default:
			log_debug("%s: unexpected imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* This pipe is dead. Remove its event handler. */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

int
resolve(struct uw_resolver *res, const char* name, int rrtype, int rrclass,
    void *mydata, resolve_cb_t cb)
{
	struct resolver_cb_data	*cb_data = NULL;
	struct asr_query	*aq = NULL;
	int			 err;

	resolver_ref(res);

	if ((cb_data = malloc(sizeof(*cb_data))) == NULL)
		goto err;
	cb_data->cb = cb;
	cb_data->data = mydata;
	cb_data->res = res;

	switch(res->type) {
	case UW_RES_ASR:
		if ((aq = res_query_async(name, rrclass, rrtype, res->asr_ctx))
		    == NULL) {
			log_warn("%s: res_query_async", __func__);
			goto err;
		}
		if (event_asr_run(aq, asr_resolve_done, cb_data) == NULL) {
			log_warn("%s: res_query_async", __func__);
			goto err;
		}
		break;
	case UW_RES_RECURSOR:
	case UW_RES_DHCP:
	case UW_RES_FORWARDER:
	case UW_RES_DOT:
		if ((err = ub_resolve_event(res->ctx, name,  rrtype, rrclass,
		    cb_data, ub_resolve_done, NULL)) != 0) {
			log_warn("%s: ub_resolve_event: err: %d, %s", __func__,
			    err, ub_strerror(err));
			goto err;
		}
		break;
	default:
		fatalx("unknown resolver type %d", res->type);
		break;
	}

	return 0;
 err:
	free(cb_data);
	free(aq);
	resolver_unref(res);
	return 1;
}

void
resolve_done(struct uw_resolver *res, void *arg, int rcode,
    void *answer_packet, int answer_len, int sec, char *why_bogus)
{
	struct query_imsg	*query_imsg;
	struct timespec		 tp, elapsed;
	int64_t			 ms;
	size_t			 i;
	char			*str;

	clock_gettime(CLOCK_MONOTONIC, &tp);

	query_imsg = (struct query_imsg *)arg;

	timespecsub(&tp, &query_imsg->tp, &elapsed);

	log_debug("elapsed: %lld.%ld", elapsed.tv_sec, elapsed.tv_nsec);

	ms = elapsed.tv_sec * 1000 + elapsed.tv_nsec / 1000000;

	for (i = 0; i < nitems(histogram_limits); i++) {
		if (ms < histogram_limits[i])
			break;
	}
	if (i == nitems(histogram_limits))
		log_debug("histogram bucket error");
	else
		res->histogram[i]++;

	log_debug("%s: ref_cnt: %d, elapsed: %lldms, "
	    "histogram: %lld - %lld", __func__, res->ref_cnt, ms,
	    histogram_limits[i], res->histogram[i]);

	log_debug("%s: rcode: %d", __func__, rcode);

	if (answer_len < LDNS_HEADER_SIZE) {
		log_warnx("bad packet: too short");
		goto servfail;
	}

	if (rcode == LDNS_RCODE_SERVFAIL) {
		if (res->stop != 1)
			check_resolver(res);
		goto servfail;
	}

	if ((str = sldns_wire2str_pkt(answer_packet, answer_len)) != NULL) {
		log_debug("%s", str);
		free(str);
	}

	query_imsg->err = 0;

	if (res->state == VALIDATING)
		query_imsg->bogus = sec == BOGUS;
	else
		query_imsg->bogus = 0;
	resolver_imsg_compose_frontend(IMSG_ANSWER_HEADER, 0, query_imsg,
	    sizeof(*query_imsg));

	/* XXX imsg overflow */
	resolver_imsg_compose_frontend(IMSG_ANSWER, 0,
	    answer_packet, answer_len);

	free(query_imsg);
	return;

servfail:
	query_imsg->err = -4; /* UB_SERVFAIL */
	resolver_imsg_compose_frontend(IMSG_ANSWER_HEADER, 0, query_imsg,
	    sizeof(*query_imsg));
	free(query_imsg);
}

void
new_recursor(void)
{
	free_resolver(resolvers[UW_RES_RECURSOR]);
	resolvers[UW_RES_RECURSOR] = NULL;

	if (TAILQ_EMPTY(&trust_anchors))
		return;

	resolvers[UW_RES_RECURSOR] = create_resolver(UW_RES_RECURSOR, 0);
	check_resolver(resolvers[UW_RES_RECURSOR]);
}

void
new_forwarders(int oppdot)
{
	free_resolver(resolvers[UW_RES_DHCP]);
	resolvers[UW_RES_DHCP] = NULL;

	if (TAILQ_EMPTY(&autoconf_forwarder_list))
		return;

	if (TAILQ_EMPTY(&trust_anchors))
		return;

	log_debug("%s: create_resolver", __func__);
	resolvers[UW_RES_DHCP] = create_resolver(UW_RES_DHCP, oppdot);

	check_resolver(resolvers[UW_RES_DHCP]);
}

void
new_asr_forwarders(void)
{
	free_resolver(resolvers[UW_RES_ASR]);
	resolvers[UW_RES_ASR] = NULL;

	if (TAILQ_EMPTY(&autoconf_forwarder_list))
		return;

	log_debug("%s: create_resolver", __func__);
	resolvers[UW_RES_ASR] = create_resolver(UW_RES_ASR, 0);

	check_resolver(resolvers[UW_RES_ASR]);
}

void
new_static_forwarders(int oppdot)
{
	free_resolver(resolvers[UW_RES_FORWARDER]);
	resolvers[UW_RES_FORWARDER] = NULL;

	if (TAILQ_EMPTY(&resolver_conf->uw_forwarder_list))
		return;

	if (TAILQ_EMPTY(&trust_anchors))
		return;

	log_debug("%s: create_resolver", __func__);
	resolvers[UW_RES_FORWARDER] = create_resolver(UW_RES_FORWARDER, oppdot);

	check_resolver(resolvers[UW_RES_FORWARDER]);
}

void
new_static_dot_forwarders(void)
{
	free_resolver(resolvers[UW_RES_DOT]);
	resolvers[UW_RES_DOT] = NULL;

	if (TAILQ_EMPTY(&resolver_conf->uw_dot_forwarder_list))
		return;

	if (TAILQ_EMPTY(&trust_anchors))
		return;

	log_debug("%s: create_resolver", __func__);
	resolvers[UW_RES_DOT] = create_resolver(UW_RES_DOT, 0);

	check_resolver(resolvers[UW_RES_DOT]);
}

static const struct {
	const char *name;
	const char *value;
} options[] = {
	{ "aggressive-nsec:", "yes" },
	{ "fast-server-permil:", "950" }
};

struct uw_resolver *
create_resolver(enum uw_resolver_type type, int oppdot)
{
	struct uw_resolver	*res;
	struct trust_anchor	*ta;
	struct uw_forwarder	*uw_forwarder;
	size_t			 i;
	int			 err;
	char			*resolv_conf = NULL, *tmp = NULL;

	if ((res = calloc(1, sizeof(*res))) == NULL) {
		log_warn("%s", __func__);
		return (NULL);
	}

	log_debug("%s: %p", __func__, res);

	res->type = type;
	res->state = UNKNOWN;
	res->check_tv.tv_sec = RESOLVER_CHECK_SEC;
	res->check_tv.tv_usec = arc4random() % 1000000; /* modulo bias is ok */

	switch (type) {
	case UW_RES_ASR:
		if (TAILQ_EMPTY(&autoconf_forwarder_list)) {
			free(res);
			return (NULL);
		}
		TAILQ_FOREACH(uw_forwarder, &autoconf_forwarder_list, entry) {
			tmp = resolv_conf;
			if (asprintf(&resolv_conf, "%snameserver %s\n", tmp ==
			    NULL ? "" : tmp, uw_forwarder->name) == -1) {
				free(tmp);
				free(res);
				log_warnx("could not create asr context");
				return (NULL);
			}
			free(tmp);
		}

		log_debug("%s: UW_RES_ASR resolv.conf: %s", __func__,
		    resolv_conf);
		if ((res->asr_ctx = asr_resolver_from_string(resolv_conf)) ==
		    NULL) {
			free(res);
			free(resolv_conf);
			log_warnx("could not create asr context");
			return (NULL);
		}
		free(resolv_conf);
		break;
	case UW_RES_RECURSOR:
	case UW_RES_DHCP:
	case UW_RES_FORWARDER:
	case UW_RES_DOT:
		if ((res->ctx = ub_ctx_create_event(ev_base)) == NULL) {
			free(res);
			log_warnx("could not create unbound context");
			return (NULL);
		}

		ub_ctx_debuglevel(res->ctx, log_getverbose() & OPT_VERBOSE2 ?
		    UB_LOG_VERBOSE : UB_LOG_BRIEF);

		TAILQ_FOREACH(ta, &trust_anchors, entry) {
			if ((err = ub_ctx_add_ta(res->ctx, ta->ta)) != 0) {
				ub_ctx_delete(res->ctx);
				free(res);
				log_warnx("error adding trust anchor: %s",
				    ub_strerror(err));
				return (NULL);
			}
		}

		for (i = 0; i < nitems(options); i++) {
			if ((err = ub_ctx_set_option(res->ctx, options[i].name,
			    options[i].value)) != 0) {
				ub_ctx_delete(res->ctx);
				free(res);
				log_warnx("error setting %s: %s: %s",
				    options[i].name, options[i].value,
				    ub_strerror(err));
				return (NULL);
			}
		}

		if (!log_getdebug()) {
			if((err = ub_ctx_set_option(res->ctx, "use-syslog:",
			    "yes")) != 0) {
				ub_ctx_delete(res->ctx);
				free(res);
				log_warnx("error setting use-syslog: yes: %s",
				    ub_strerror(err));
				return (NULL);
			}
		}

		break;
	default:
		fatalx("unknown resolver type %d", type);
		break;
	}

	evtimer_set(&res->check_ev, resolver_check_timo, res);

	switch(res->type) {
	case UW_RES_ASR:
		break;
	case UW_RES_RECURSOR:
		break;
	case UW_RES_DHCP:
		res->oppdot = oppdot;
		if (oppdot) {
			set_forwarders_oppdot(res, &autoconf_forwarder_list,
			    853);
			ub_ctx_set_option(res->ctx, "tls-cert-bundle:",
			    TLS_DEFAULT_CA_CERT_FILE);
			ub_ctx_set_tls(res->ctx, 1);
		} else {
			set_forwarders_oppdot(res, &autoconf_forwarder_list,
			    53);
		}
		break;
	case UW_RES_FORWARDER:
		res->oppdot = oppdot;
		if (oppdot) {
			set_forwarders_oppdot(res,
			    &resolver_conf->uw_forwarder_list, 853);
			ub_ctx_set_option(res->ctx, "tls-cert-bundle:",
			    TLS_DEFAULT_CA_CERT_FILE);
			ub_ctx_set_tls(res->ctx, 1);
		} else
			set_forwarders_oppdot(res,
			    &resolver_conf->uw_forwarder_list, 53);
		break;
	case UW_RES_DOT:
		set_forwarders(res, &resolver_conf->uw_dot_forwarder_list);
		ub_ctx_set_option(res->ctx, "tls-cert-bundle:",
		    TLS_DEFAULT_CA_CERT_FILE);
		ub_ctx_set_tls(res->ctx, 1);
		break;
	default:
		fatalx("unknown resolver type %d", type);
		break;
	}

	/* for the forwarder cases allow AS112 zones */
	switch(res->type) {
	case UW_RES_DHCP:
	case UW_RES_FORWARDER:
	case UW_RES_DOT:
		for (i = 0; i < nitems(as112_zones); i++) {
			if((err = ub_ctx_set_option(res->ctx, "local-zone:",
			    as112_zones[i])) != 0) {
				ub_ctx_delete(res->ctx);
				free(res);
				log_warnx("error setting local-zone: %s: %s",
				    as112_zones[i], ub_strerror(err));
				return (NULL);
			}
		}
		break;
	default:
		break;
	}

	return (res);
}

void
free_resolver(struct uw_resolver *res)
{
	if (res == NULL)
		return;

	log_debug("%s: [%p] ref_cnt: %d", __func__, res, res->ref_cnt);

	if (res->ref_cnt > 0)
		res->stop = 1;
	else {
		evtimer_del(&res->check_ev);
		ub_ctx_delete(res->ctx);
		asr_resolver_free(res->asr_ctx);
		free(res->why_bogus);
		free(res);
	}
}

void
set_forwarders(struct uw_resolver *res, struct uw_forwarder_head
    *uw_forwarder_list)
{
	struct uw_forwarder	*uw_forwarder;

	TAILQ_FOREACH(uw_forwarder, uw_forwarder_list, entry)
		ub_ctx_set_fwd(res->ctx, uw_forwarder->name);
}

void
set_forwarders_oppdot(struct uw_resolver *res, struct uw_forwarder_head
    *uw_forwarder_list, int def_port)
{
	struct uw_forwarder	*uw_forwarder;

	TAILQ_FOREACH(uw_forwarder, uw_forwarder_list, entry) {
		char name[1024];
		int port = uw_forwarder->port;
		if (port == 0)
			port = def_port;
		snprintf(name, sizeof(name), "%s@%d", uw_forwarder->name, port);
		ub_ctx_set_fwd(res->ctx, name);
	}
}

void
resolver_check_timo(int fd, short events, void *arg)
{
	check_resolver((struct uw_resolver *)arg);
}

void
resolver_free_timo(int fd, short events, void *arg)
{
	free_resolver((struct uw_resolver *)arg);
}

void
check_resolver(struct uw_resolver *resolver_to_check)
{
	struct uw_resolver		*res;

	if (resolver_to_check->check_running) {
		log_debug("%s: already checking: %s", __func__,
		    uw_resolver_type_str[resolver_to_check->type]);
		return;
	}

	log_debug("%s: create_resolver", __func__);
	if ((res = create_resolver(resolver_to_check->type, 0)) == NULL)
		fatal("%s", __func__);

	resolver_ref(resolver_to_check);

	if (resolve(res, ".", LDNS_RR_TYPE_NS, LDNS_RR_CLASS_IN,
	    resolver_to_check, check_resolver_done) != 0) {
		resolver_to_check->state = UNKNOWN;
		resolver_unref(resolver_to_check);
		resolver_to_check->check_tv.tv_sec = RESOLVER_CHECK_SEC;
		evtimer_add(&resolver_to_check->check_ev,
		    &resolver_to_check->check_tv);

		log_debug("%s: evtimer_add: %lld - %s: %s", __func__,
		    resolver_to_check->check_tv.tv_sec,
		    uw_resolver_type_str[resolver_to_check->type],
		    uw_resolver_state_str[resolver_to_check->state]);
	} else
		resolver_to_check->check_running++;

	if (!(resolver_to_check->type == UW_RES_DHCP ||
	    resolver_to_check->type == UW_RES_FORWARDER))
		return;

	log_debug("%s: create_resolver for oppdot", __func__);
	if ((res = create_resolver(resolver_to_check->type, 1)) == NULL)
		fatal("%s", __func__);

	resolver_ref(resolver_to_check);

	if (resolve(res, ".", LDNS_RR_TYPE_NS, LDNS_RR_CLASS_IN,
	    resolver_to_check, check_resolver_done) != 0) {
		log_debug("check oppdot failed");
		/* do not overwrite normal DNS state, it might work */
		resolver_unref(resolver_to_check);

		resolver_to_check->check_tv.tv_sec = RESOLVER_CHECK_SEC;
		evtimer_add(&resolver_to_check->check_ev,
		    &resolver_to_check->check_tv);

		log_debug("%s: evtimer_add: %lld - %s: %s", __func__,
		    resolver_to_check->check_tv.tv_sec,
		    uw_resolver_type_str[resolver_to_check->type],
		    uw_resolver_state_str[resolver_to_check->state]);
	} else
		resolver_to_check->check_running++;
}

void
check_resolver_done(struct uw_resolver *res, void *arg, int rcode,
    void *answer_packet, int answer_len, int sec, char *why_bogus)
{
	struct uw_resolver	*checked_resolver = arg;
	struct timeval		 tv = {0, 1};
	enum uw_resolver_state	 prev_state;
	char			*str;

	checked_resolver->check_running--;

	log_debug("%s: %s rcode: %d", __func__,
	    uw_resolver_type_str[checked_resolver->type], rcode);

	prev_state = checked_resolver->state;

	if (answer_len < LDNS_HEADER_SIZE) {
		checked_resolver->state = DEAD;
		log_warnx("bad packet: too short");
		goto out;
	}

	if (rcode == LDNS_RCODE_SERVFAIL) {
		if (res->oppdot == checked_resolver->oppdot) {
			checked_resolver->state = DEAD;
			if (checked_resolver->oppdot) {
				/* downgrade from opportunistic DoT */
				switch (checked_resolver->type) {
				case UW_RES_DHCP:
					new_forwarders(0);
					break;
				case UW_RES_FORWARDER:
					new_static_forwarders(0);
					break;
				default:
					break;
				}
			}
		}
		goto out;
	}

	if (res->oppdot && !checked_resolver->oppdot) {
		/* upgrade to opportunistic DoT */
		switch (checked_resolver->type) {
		case UW_RES_DHCP:
			new_forwarders(1);
			break;
		case UW_RES_FORWARDER:
			new_static_forwarders(1);
			break;
		default:
			break;
		}
	}

	if ((str = sldns_wire2str_pkt(answer_packet, answer_len)) != NULL) {
		log_debug("%s", str);
		free(str);
	}

	if (sec == SECURE) {
		checked_resolver->state = VALIDATING;
		if (!(evtimer_pending(&trust_anchor_timer, NULL)))
			evtimer_add(&trust_anchor_timer, &tv);
	 } else if (rcode == LDNS_RCODE_NOERROR &&
	    LDNS_RCODE_WIRE((uint8_t*)answer_packet) == LDNS_RCODE_NOERROR) {
		log_debug("%s: why bogus: %s", __func__, why_bogus);
		checked_resolver->state = RESOLVING;
		/* best effort */
		checked_resolver->why_bogus = strdup(why_bogus);
	} else
		checked_resolver->state = DEAD; /* we know the root exists */

out:
	if (!checked_resolver->stop && checked_resolver->state == DEAD) {
		if (prev_state == DEAD)
			checked_resolver->check_tv.tv_sec *= 2;
		else
			checked_resolver->check_tv.tv_sec = RESOLVER_CHECK_SEC;

		if (checked_resolver->check_tv.tv_sec > RESOLVER_CHECK_MAXSEC)
			checked_resolver->check_tv.tv_sec =
			    RESOLVER_CHECK_MAXSEC;

		evtimer_add(&checked_resolver->check_ev,
		    &checked_resolver->check_tv);

		log_debug("%s: evtimer_add: %lld - %s: %s", __func__,
		    checked_resolver->check_tv.tv_sec,
		    uw_resolver_type_str[checked_resolver->type],
		    uw_resolver_state_str[checked_resolver->state]);
	}

	log_debug("%s: %s: %s", __func__,
	    uw_resolver_type_str[checked_resolver->type],
	    uw_resolver_state_str[checked_resolver->state]);

	log_debug("%s: %p - %p", __func__, checked_resolver,
	    checked_resolver->ctx);

	resolver_unref(checked_resolver);
	res->stop = 1; /* do not free in callback */
}

void
asr_resolve_done(struct asr_result *ar, void *arg)
{
	struct resolver_cb_data	*cb_data = arg;
	cb_data->cb(cb_data->res, cb_data->data, ar->ar_rcode, ar->ar_data,
	    ar->ar_datalen, 0, "");
	free(ar->ar_data);
	resolver_unref(cb_data->res);
	free(cb_data);
}

void
ub_resolve_done(void *arg, int rcode, void *answer_packet, int answer_len,
    int sec, char *why_bogus, int was_ratelimited)
{
	struct resolver_cb_data	*cb_data = arg;
	cb_data->cb(cb_data->res, cb_data->data, rcode, answer_packet,
	    answer_len, sec, why_bogus);
	resolver_unref(cb_data->res);
	free(cb_data);
}

void
schedule_recheck_all_resolvers(void)
{
	struct timeval	 tv;
	int		 i;

	tv.tv_sec = 0;

	log_debug("%s", __func__);

	for (i = 0; i < UW_RES_NONE; i++) {
		if (resolvers[i] == NULL)
			continue;
		tv.tv_usec = arc4random() % 1000000; /* modulo bias is ok */
		evtimer_add(&resolvers[i]->check_ev, &tv);
	}
}

int
check_forwarders_changed(struct uw_forwarder_head *list_a,
    struct uw_forwarder_head *list_b)
{
	struct uw_forwarder	*a, *b;

	a = TAILQ_FIRST(list_a);
	b = TAILQ_FIRST(list_b);

	while(a != NULL && b != NULL) {
		if (strcmp(a->name, b->name) != 0)
			return 1;
		a = TAILQ_NEXT(a, entry);
		b = TAILQ_NEXT(b, entry);
	}

	if (a != NULL || b != NULL)
		return 1;
	return 0;
}

void
resolver_ref(struct uw_resolver *res)
{
	if (res->ref_cnt == INT_MAX)
		fatalx("%s: INT_MAX references", __func__);
	res->ref_cnt++;
}

void
resolver_unref(struct uw_resolver *res)
{
	struct timeval	 tv = { 0, 1};

	if (res->ref_cnt == 0)
		fatalx("%s: unreferenced resolver", __func__);

	res->ref_cnt--;

	/*
	 * Decouple from libunbound event callback.
	 * If we free the ctx inside of resolve_done or check_resovler_done
	 * we are cutting of the branch we are sitting on and hit a
	 * user-after-free
	 */
	if (res->stop && res->ref_cnt == 0) {
		evtimer_set(&res->free_ev, resolver_free_timo, res);
		evtimer_add(&res->free_ev, &tv);
	}
}

void
replace_forwarders(struct uw_forwarder_head *new_list, struct
    uw_forwarder_head *old_list)
{
	struct uw_forwarder	*uw_forwarder;

	while ((uw_forwarder =
	    TAILQ_FIRST(old_list)) != NULL) {
		TAILQ_REMOVE(old_list, uw_forwarder, entry);
		free(uw_forwarder);
	}

	while ((uw_forwarder = TAILQ_FIRST(new_list)) != NULL) {
		TAILQ_REMOVE(new_list, uw_forwarder, entry);
		TAILQ_INSERT_TAIL(old_list, uw_forwarder, entry);
	}
}

struct uw_resolver*
best_resolver(void)
{
	struct uw_resolver	*res = NULL;
	int			 i;

	log_debug("%s: %s: %s, %s: %s%s, %s: %s%s, %s: %s, %s: %s",
	    __func__,
	    uw_resolver_type_str[UW_RES_RECURSOR], resolvers[UW_RES_RECURSOR]
	    != NULL ? uw_resolver_state_str[resolvers[UW_RES_RECURSOR]->state]
	    : "NA",
	    uw_resolver_type_str[UW_RES_DHCP], resolvers[UW_RES_DHCP] != NULL ?
	    uw_resolver_state_str[resolvers[UW_RES_DHCP]->state] : "NA",
	    resolvers[UW_RES_DHCP] != NULL &&
	    resolvers[UW_RES_DHCP]->oppdot ? " (OppDot)" : "",
	    uw_resolver_type_str[UW_RES_FORWARDER],
	    resolvers[UW_RES_FORWARDER] != NULL ?
	    uw_resolver_state_str[resolvers[UW_RES_FORWARDER]->state] : "NA",
	    resolvers[UW_RES_FORWARDER] != NULL &&
	    resolvers[UW_RES_FORWARDER]->oppdot ? " (OppDot)" : "",
	    uw_resolver_type_str[UW_RES_DOT],
	    resolvers[UW_RES_DOT] != NULL ?
	    uw_resolver_state_str[resolvers[UW_RES_DOT]->state] : "NA",
	    uw_resolver_type_str[UW_RES_ASR],
	    resolvers[UW_RES_ASR] != NULL ?
	    uw_resolver_state_str[resolvers[UW_RES_ASR]->state] : "NA");

	res = resolvers[resolver_conf->res_pref.types[0]];

	for (i = 1; i < resolver_conf->res_pref.len; i++)
		if (resolver_cmp(res,
		    resolvers[resolver_conf->res_pref.types[i]]) < 0)
			res = resolvers[resolver_conf->res_pref.types[i]];

	if (res != NULL)
		log_debug("%s: %s state: %s%s", __func__,
		    uw_resolver_type_str[res->type],
		    uw_resolver_state_str[res->state],
		    res->oppdot ? " (OppDoT)" : "");
	else
		log_debug("%s: non found", __func__);

	return (res);
}

int
resolver_cmp(struct uw_resolver *a, struct uw_resolver *b)
{
	if (a == NULL && b == NULL)
		return 0;

	if (b == NULL)
		return 1;

	if (a == NULL)
		return -1;

	return (a->state < b->state ? -1 : a->state > b->state ? 1 : 0);
}

void
restart_resolvers(void)
{
	int	 verbose;

	verbose = log_getverbose() & OPT_VERBOSE2 ? UB_LOG_VERBOSE :
	    UB_LOG_BRIEF;
	log_debug("%s: %d", __func__, verbose);

	new_recursor();
	new_static_forwarders(0);
	new_static_dot_forwarders();
	new_forwarders(0);
	new_asr_forwarders();
}

void
show_status(enum uw_resolver_type type, pid_t pid)
{
	struct uw_resolver		*best;
	struct uw_forwarder		*uw_forwarder;
	struct ctl_forwarder_info	 cfi;
	int				 i;

	best = best_resolver();

	switch(type) {
	case UW_RES_NONE:
		for (i = 0; i < resolver_conf->res_pref.len; i++)
			send_resolver_info(
			    resolvers[resolver_conf->res_pref.types[i]],
			    resolvers[resolver_conf->res_pref.types[i]] ==
			    best, pid);

		TAILQ_FOREACH(uw_forwarder, &autoconf_forwarder_list, entry) {
			memset(&cfi, 0, sizeof(cfi));
			cfi.if_index = uw_forwarder->if_index;
			cfi.src = uw_forwarder->src;
			/* no truncation, structs are in sync */
			strlcpy(cfi.name, uw_forwarder->name,
			    sizeof(cfi.name));
			resolver_imsg_compose_frontend(
			    IMSG_CTL_AUTOCONF_RESOLVER_INFO,
			    pid, &cfi, sizeof(cfi));
		}
		break;
	case UW_RES_RECURSOR:
	case UW_RES_DHCP:
	case UW_RES_FORWARDER:
	case UW_RES_DOT:
	case UW_RES_ASR:
		send_resolver_info(resolvers[type], resolvers[type] == best,
		    pid);
		send_detailed_resolver_info(resolvers[type], pid);
		break;
	default:
		fatalx("unknown resolver type %d", type);
		break;
	}
	resolver_imsg_compose_frontend(IMSG_CTL_END, pid, NULL, 0);
}

void
send_resolver_info(struct uw_resolver *res, int selected, pid_t pid)
{
	struct ctl_resolver_info	 cri;

	if (res == NULL)
		return;

	cri.state = res->state;
	cri.type = res->type;
	cri.selected = selected;
	cri.oppdot = res->oppdot;
	resolver_imsg_compose_frontend(IMSG_CTL_RESOLVER_INFO, pid, &cri,
	    sizeof(cri));
}

void
send_detailed_resolver_info(struct uw_resolver *res, pid_t pid)
{
	char	 buf[1024];

	if (res == NULL)
		return;

	if (res->state == RESOLVING) {
		(void)strlcpy(buf, res->why_bogus, sizeof(buf));
		resolver_imsg_compose_frontend(IMSG_CTL_RESOLVER_WHY_BOGUS,
		    pid, buf, sizeof(buf));
	}
	send_resolver_histogram_info(res, pid);
}

void
send_resolver_histogram_info(struct uw_resolver *res, pid_t pid)
{
	int64_t	 histogram[nitems(histogram_limits)];

	memcpy(histogram, res->histogram, sizeof(histogram));

	resolver_imsg_compose_frontend(IMSG_CTL_RESOLVER_HISTOGRAM,
		    pid, histogram, sizeof(histogram));
}

void
trust_anchor_resolve(void)
{
	struct uw_resolver	*res;
	struct timeval		 tv = {TRUST_ANCHOR_RETRY_INTERVAL, 0};

	log_debug("%s", __func__);

	res = best_resolver();

	if (res == NULL || res->state < VALIDATING)
		goto err;

	if (resolve(res, ".",  LDNS_RR_TYPE_DNSKEY, LDNS_RR_CLASS_IN, NULL,
	    trust_anchor_resolve_done) != 0)
		goto err;

	return;
 err:
	evtimer_add(&trust_anchor_timer, &tv);
}

void
trust_anchor_timo(int fd, short events, void *arg)
{
	trust_anchor_resolve();
}

void
trust_anchor_resolve_done(struct uw_resolver *res, void *arg, int rcode,
    void *answer_packet, int answer_len, int sec, char *why_bogus)
{
	struct ub_result	*result = NULL;
	sldns_buffer		*buf = NULL;
	struct regional		*region = NULL;
	struct timeval		 tv = {TRUST_ANCHOR_RETRY_INTERVAL, 0};
	int			 i, tas, n;
	uint16_t		 dnskey_flags;
	char			*str, rdata_buf[1024], *ta;

	if (answer_len < LDNS_HEADER_SIZE) {
		log_warnx("bad packet: too short");
		goto out;
	}

	if ((result = calloc(1, sizeof(*result))) == NULL)
		goto out;

	log_debug("%s: rcode: %d", __func__, rcode);

	if (sec != SECURE) {
		log_debug("%s: sec: %d", __func__, sec);
		goto out;
	}

	if ((str = sldns_wire2str_pkt(answer_packet, answer_len)) != NULL) {
		log_debug("%s", str);
		free(str);
	}

	if ((buf = sldns_buffer_new(answer_len)) == NULL)
		goto out;
	if ((region = regional_create()) == NULL)
		goto out;
	result->rcode = LDNS_RCODE_SERVFAIL;

	sldns_buffer_clear(buf);
	sldns_buffer_write(buf, answer_packet, answer_len);
	sldns_buffer_flip(buf);
	libworker_enter_result(result, buf, region, sec);
	result->answer_packet = NULL;
	result->answer_len = 0;

	if (result->rcode != LDNS_RCODE_NOERROR) {
		log_debug("%s: result->rcode: %d", __func__,
		    result->rcode);
		goto out;
	}

	i = 0;
	tas = 0;
	while(result->data[i] != NULL) {
		if (result->len[i] < 2) {
			if (tas > 0)
				resolver_imsg_compose_frontend(
				    IMSG_NEW_TAS_ABORT, 0, NULL, 0);
			goto out;
		}
		n = sldns_wire2str_rdata_buf(result->data[i], result->len[i],
		    rdata_buf, sizeof(rdata_buf), LDNS_RR_TYPE_DNSKEY);

		if (n < 0 || (size_t)n >= sizeof(rdata_buf)) {
			log_warnx("trust anchor buffer to small");
			resolver_imsg_compose_frontend(IMSG_NEW_TAS_ABORT, 0,
			    NULL, 0);
			goto out;
		}

		memcpy(&dnskey_flags, result->data[i], 2);
		dnskey_flags = ntohs(dnskey_flags);
		if ((dnskey_flags & LDNS_KEY_SEP_KEY) && !(dnskey_flags &
		    LDNS_KEY_REVOKE_KEY)) {
			asprintf(&ta, ".\t%d\tIN\tDNSKEY\t%s", ROOT_DNSKEY_TTL,
			    rdata_buf);
			log_debug("%s: ta: %s", __func__, ta);
			resolver_imsg_compose_frontend(IMSG_NEW_TA, 0, ta,
			    strlen(ta) + 1);
			tas++;
			free(ta);
		}
		i++;
	}
	if (tas > 0) {
		resolver_imsg_compose_frontend(IMSG_NEW_TAS_DONE, 0, NULL, 0);
		tv.tv_sec = TRUST_ANCHOR_QUERY_INTERVAL;
	}
out:
	sldns_buffer_free(buf);
	regional_destroy(region);
	ub_resolve_free(result);
	evtimer_add(&trust_anchor_timer, &tv);
}

void
replace_autoconf_forwarders(struct imsg_rdns_proposal *rdns_proposal)
{
	struct uw_forwarder_head	 new_forwarder_list;
	struct uw_forwarder		*uw_forwarder, *tmp;
	int				 i, rdns_count, af, changed = 0;
	char				 ntopbuf[INET6_ADDRSTRLEN], *src;
	const char			*ns;

	TAILQ_INIT(&new_forwarder_list);
	af = rdns_proposal->rtdns.sr_family;
	src = rdns_proposal->rtdns.sr_dns;

	switch (af) {
	case AF_INET:
		rdns_count = (rdns_proposal->rtdns.sr_len -
		    offsetof(struct sockaddr_rtdns, sr_dns)) /
		    sizeof(struct in_addr);
		break;
	case AF_INET6:
		rdns_count = (rdns_proposal->rtdns.sr_len -
		    offsetof(struct sockaddr_rtdns, sr_dns)) /
		    sizeof(struct in6_addr);
		break;
	default:
		log_warnx("%s: unsupported address family: %d", __func__, af);
		return;
	}

	for (i = 0; i < rdns_count; i++) {
		switch (af) {
		case AF_INET:
			if (((struct in_addr *)src)->s_addr == INADDR_LOOPBACK)
				continue;
			ns = inet_ntop(af, (struct in_addr *)src, ntopbuf,
			    INET6_ADDRSTRLEN);
			src += sizeof(struct in_addr);
			break;
		case AF_INET6:
			if (IN6_IS_ADDR_LOOPBACK((struct in6_addr *)src))
				continue;
			ns = inet_ntop(af, (struct in6_addr *)src, ntopbuf,
			    INET6_ADDRSTRLEN);
			src += sizeof(struct in6_addr);
		}
		log_debug("%s: %s", __func__, ns);

		if ((uw_forwarder = calloc(1, sizeof(struct uw_forwarder))) ==
		    NULL)
			fatal(NULL);
		if (strlcpy(uw_forwarder->name, ns, sizeof(uw_forwarder->name))
		    >= sizeof(uw_forwarder->name))
			fatalx("strlcpy");
		uw_forwarder->if_index = rdns_proposal->if_index;
		uw_forwarder->src = rdns_proposal->src;
		TAILQ_INSERT_TAIL(&new_forwarder_list, uw_forwarder, entry);
	}

	TAILQ_FOREACH(tmp, &autoconf_forwarder_list, entry) {
		/* if_index of zero signals to clear all proposals */
		if (rdns_proposal->src == tmp->src &&
		    (rdns_proposal->if_index == 0 || rdns_proposal->if_index ==
		    tmp->if_index))
			continue;
		if ((uw_forwarder = calloc(1, sizeof(struct uw_forwarder))) ==
		    NULL)
			fatal(NULL);
		if (strlcpy(uw_forwarder->name, tmp->name,
		    sizeof(uw_forwarder->name)) >= sizeof(uw_forwarder->name))
			fatalx("strlcpy");
		uw_forwarder->src = tmp->src;
		uw_forwarder->if_index = tmp->if_index;
		TAILQ_INSERT_TAIL(&new_forwarder_list, uw_forwarder, entry);
	}

	changed = check_forwarders_changed(&new_forwarder_list,
	    &autoconf_forwarder_list);

	log_debug("%s: changed: %d", __func__, changed);
	if (changed) {
		replace_forwarders(&new_forwarder_list,
		    &autoconf_forwarder_list);
		new_forwarders(0);
		new_asr_forwarders();
		log_debug("%s: forwarders changed", __func__);
	} else {
		log_debug("%s: forwarders didn't change", __func__);
		while ((tmp = TAILQ_FIRST(&new_forwarder_list)) != NULL) {
			TAILQ_REMOVE(&new_forwarder_list, tmp, entry);
			free(tmp);
		}
	}
}

struct uw_forwarder *
find_forwarder(struct uw_forwarder_head *list, const char *name) {
	struct uw_forwarder	*uw_forwarder;

	TAILQ_FOREACH(uw_forwarder, list, entry) {
		if (strcmp(uw_forwarder->name, name) == 0)
			return uw_forwarder;
	}
	return NULL;
}
