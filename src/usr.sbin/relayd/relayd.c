/*	$OpenBSD: relayd.c,v 1.130 2014/07/13 00:32:08 benno Exp $	*/

/*
 * Copyright (c) 2007 - 2014 Reyk Floeter <reyk@openbsd.org>
 * Copyright (c) 2006 Pierre-Yves Ritschard <pyr@openbsd.org>
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

#ifdef __FreeBSD__
#include <sys/param.h>
#include <openssl/rand.h>
#else
#include <sys/types.h>
#endif
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/hash.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <getopt.h>
#include <fnmatch.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <unistd.h>
#include <ctype.h>
#include <pwd.h>
#include <md5.h>

#include <openssl/ssl.h>

#ifdef __FreeBSD__
 /* #include <sha.h>  */
 char   *SHA1_Data(const void *, unsigned int, char *);
#else
#include <sha1.h>
#endif
#include "relayd.h"

__dead void	 usage(void);

int		 parent_configure(struct relayd *);
void		 parent_configure_done(struct relayd *);
void		 parent_reload(struct relayd *, u_int, const char *);
void		 parent_sig_handler(int, short, void *);
void		 parent_shutdown(struct relayd *);
int		 parent_dispatch_pfe(int, struct privsep_proc *, struct imsg *);
int		 parent_dispatch_hce(int, struct privsep_proc *, struct imsg *);
int		 parent_dispatch_relay(int, struct privsep_proc *,
		    struct imsg *);
int		 parent_dispatch_ca(int, struct privsep_proc *,
		    struct imsg *);
int		 bindany(struct ctl_bindany *);

struct relayd			*relayd_env;

static struct privsep_proc procs[] = {
	{ "pfe",	PROC_PFE, parent_dispatch_pfe, pfe },
	{ "hce",	PROC_HCE, parent_dispatch_hce, hce },
	{ "relay",	PROC_RELAY, parent_dispatch_relay, relay },
	{ "ca",		PROC_CA, parent_dispatch_ca, ca }
};

void
parent_sig_handler(int sig, short event, void *arg)
{
	struct privsep	*ps = arg;
	int		 die = 0, status, fail, id;
	pid_t		 pid;
	char		*cause;

	switch (sig) {
	case SIGTERM:
	case SIGINT:
		die = 1;
		/* FALLTHROUGH */
	case SIGCHLD:
		do {
			pid = waitpid(WAIT_ANY, &status, WNOHANG);
			if (pid <= 0)
				continue;

			fail = 0;
			if (WIFSIGNALED(status)) {
				fail = 1;
				asprintf(&cause, "terminated; signal %d",
				    WTERMSIG(status));
			} else if (WIFEXITED(status)) {
				if (WEXITSTATUS(status) != 0) {
					fail = 1;
					asprintf(&cause, "exited abnormally");
				} else
					asprintf(&cause, "exited okay");
			} else
				fatalx("unexpected cause of SIGCHLD");

			die = 1;

			for (id = 0; id < PROC_MAX; id++)
				if (pid == ps->ps_pid[id]) {
					if (fail)
						log_warnx("lost child: %s %s",
						    ps->ps_title[id], cause);
					break;
				}

			free(cause);
		} while (pid > 0 || (pid == -1 && errno == EINTR));

		if (die)
			parent_shutdown(ps->ps_env);
		break;
	case SIGHUP:
		log_info("%s: reload requested with SIGHUP", __func__);

		/*
		 * This is safe because libevent uses async signal handlers
		 * that run in the event loop and not in signal context.
		 */
		parent_reload(ps->ps_env, CONFIG_RELOAD, NULL);
		break;
	case SIGPIPE:
		/* ignore */
		break;
	default:
		fatalx("unexpected signal");
	}
}

__dead void
usage(void)
{
	extern char	*__progname;

	fprintf(stderr, "usage: %s [-dnv] [-D macro=value] [-f file]\n",
	    __progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	int			 c;
	int			 debug = 0, verbose = 0;
	u_int32_t		 opts = 0;
	struct relayd		*env;
	struct privsep		*ps;
	const char		*conffile = CONF_FILE;
#ifdef __FreeBSD__
#if __FreeBSD_version > 800040
	u_int32_t               rnd[256];
#endif
#endif

	while ((c = getopt(argc, argv, "dD:nf:v")) != -1) {
		switch (c) {
		case 'd':
			debug = 2;
			break;
		case 'D':
			if (cmdline_symset(optarg) < 0)
				log_warnx("could not parse macro definition %s",
				    optarg);
			break;
		case 'n':
			debug = 2;
			opts |= RELAYD_OPT_NOACTION;
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'v':
			verbose++;
			opts |= RELAYD_OPT_VERBOSE;
			break;
		default:
			usage();
		}
	}

	log_init(debug ? debug : 1);	/* log to stderr until daemonized */

	argc -= optind;
	if (argc > 0)
		usage();

	if ((env = calloc(1, sizeof(*env))) == NULL ||
	    (ps = calloc(1, sizeof(*ps))) == NULL)
		exit(1);

	relayd_env = env;
	env->sc_ps = ps;
	ps->ps_env = env;
	TAILQ_INIT(&ps->ps_rcsocks);
	env->sc_conffile = conffile;
	env->sc_opts = opts;

	if (parse_config(env->sc_conffile, env) == -1)
		exit(1);

	if (debug)
		env->sc_opts |= RELAYD_OPT_LOGUPDATE;

	if (geteuid())
		errx(1, "need root privileges");

	if ((ps->ps_pw =  getpwnam(RELAYD_USER)) == NULL)
		errx(1, "unknown user %s", RELAYD_USER);

	/* Configure the control socket */
	ps->ps_csock.cs_name = RELAYD_SOCKET;

	log_init(debug);
	log_verbose(verbose);

	if (!debug && daemon(1, 0) == -1)
		err(1, "failed to daemonize");

	if (env->sc_opts & RELAYD_OPT_NOACTION)
		ps->ps_noaction = 1;
	else
		log_info("startup");

#ifdef __FreeBSD__
#if __FreeBSD_version > 800040
	arc4random_buf(rnd, sizeof(rnd));
	RAND_seed(rnd, sizeof(rnd));
#else
	RAND_load_file("/dev/random",2048);
#endif
#endif

	if (load_config(env->sc_conffile, env) == -1) {
		proc_kill(env->sc_ps);
		exit(1);
	}
	ps->ps_instances[PROC_RELAY] = env->sc_prefork_relay;
	ps->ps_instances[PROC_CA] = env->sc_prefork_relay;
	ps->ps_ninstances = env->sc_prefork_relay;

	proc_init(ps, procs, nitems(procs));

	setproctitle("parent");

	event_init();

	signal_set(&ps->ps_evsigint, SIGINT, parent_sig_handler, ps);
	signal_set(&ps->ps_evsigterm, SIGTERM, parent_sig_handler, ps);
	signal_set(&ps->ps_evsigchld, SIGCHLD, parent_sig_handler, ps);
	signal_set(&ps->ps_evsighup, SIGHUP, parent_sig_handler, ps);
	signal_set(&ps->ps_evsigpipe, SIGPIPE, parent_sig_handler, ps);

	signal_add(&ps->ps_evsigint, NULL);
	signal_add(&ps->ps_evsigterm, NULL);
	signal_add(&ps->ps_evsigchld, NULL);
	signal_add(&ps->ps_evsighup, NULL);
	signal_add(&ps->ps_evsigpipe, NULL);

	proc_listen(ps, procs, nitems(procs));

	if (env->sc_opts & RELAYD_OPT_NOACTION) {
		fprintf(stderr, "configuration OK\n");
		proc_kill(env->sc_ps);
		exit(0);
	}

	if (env->sc_flags & (F_SSL|F_SSLCLIENT))
		ssl_init(env);

	if (parent_configure(env) == -1)
		fatalx("configuration failed");

#ifndef __FreeBSD__
	init_routes(env);
#endif

	event_dispatch();

	parent_shutdown(env);
	/* NOTREACHED */

	return (0);
}

int
parent_configure(struct relayd *env)
{
	struct table		*tb;
	struct rdr		*rdr;
#ifndef __FreeBSD__
	struct router		*rt;
#endif
	struct protocol		*proto;
	struct relay		*rlay;
	int			 id;
	struct ctl_flags	 cf;
	int			 s, ret = -1;

	TAILQ_FOREACH(tb, env->sc_tables, entry)
		config_settable(env, tb);
	TAILQ_FOREACH(rdr, env->sc_rdrs, entry)
		config_setrdr(env, rdr);
#ifndef __FreeBSD__
	TAILQ_FOREACH(rt, env->sc_rts, rt_entry)
		config_setrt(env, rt);
#endif
	TAILQ_FOREACH(proto, env->sc_protos, entry)
		config_setproto(env, proto);
	TAILQ_FOREACH(proto, env->sc_protos, entry)
		config_setrule(env, proto);
	TAILQ_FOREACH(rlay, env->sc_relays, rl_entry) {
		/* Check for SSL Inspection */
		if ((rlay->rl_conf.flags & (F_SSL|F_SSLCLIENT)) ==
		    (F_SSL|F_SSLCLIENT) &&
		    rlay->rl_conf.ssl_cacert_len &&
		    rlay->rl_conf.ssl_cakey_len)
			rlay->rl_conf.flags |= F_SSLINSPECT;

		config_setrelay(env, rlay);
	}

	/* HCE, PFE, CA and the relays need to reload their config. */
	env->sc_reload = 2 + (2 * env->sc_prefork_relay);

	for (id = 0; id < PROC_MAX; id++) {
		if (id == privsep_process)
			continue;
		cf.cf_opts = env->sc_opts;
		cf.cf_flags = env->sc_flags;

		if ((env->sc_flags & F_NEEDPF) && id == PROC_PFE) {
			/* Send pf socket to the pf engine */
			if ((s = open(PF_SOCKET, O_RDWR)) == -1) {
				log_debug("%s: cannot open pf socket",
				    __func__);
				goto done;
			}
		} else
			s = -1;

		proc_compose_imsg(env->sc_ps, id, -1, IMSG_CFG_DONE, s,
		    &cf, sizeof(cf));
	}

	ret = 0;

 done:
	config_purge(env, CONFIG_ALL & ~CONFIG_RELAYS);
	return (ret);
}

void
parent_reload(struct relayd *env, u_int reset, const char *filename)
{
	if (env->sc_reload) {
		log_debug("%s: already in progress: %d pending",
		    __func__, env->sc_reload);
		return;
	}

	/* Switch back to the default config file */
	if (filename == NULL || *filename == '\0')
		filename = env->sc_conffile;

	log_debug("%s: level %d config file %s", __func__, reset, filename);

	config_purge(env, CONFIG_ALL);

	if (reset == CONFIG_RELOAD) {
		if (load_config(filename, env) == -1) {
			log_debug("%s: failed to load config file %s",
			    __func__, filename);
		}

		config_setreset(env, CONFIG_ALL);

		if (parent_configure(env) == -1) {
			log_debug("%s: failed to commit config from %s",
			    __func__, filename);
		}
	} else
		config_setreset(env, reset);
}

void
parent_configure_done(struct relayd *env)
{
	int	 id;

	if (env->sc_reload == 0) {
		log_warnx("%s: configuration already finished", __func__);
		return;
	}

	env->sc_reload--;
	if (env->sc_reload == 0) {
		for (id = 0; id < PROC_MAX; id++) {
			if (id == privsep_process)
				continue;

			proc_compose_imsg(env->sc_ps, id, -1, IMSG_CTL_START,
			    -1, NULL, 0);
		}
	}
}

void
parent_shutdown(struct relayd *env)
{
	config_purge(env, CONFIG_ALL);

	proc_kill(env->sc_ps);
	control_cleanup(&env->sc_ps->ps_csock);
#ifndef __FreeBSD__
	carp_demote_shutdown();
#endif

	free(env->sc_ps);
	free(env);

	log_info("parent terminating, pid %d", getpid());

	exit(0);
}

int
parent_dispatch_pfe(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct relayd		*env = p->p_env;
#ifndef __FreeBSD__
	struct ctl_demote	 demote;
	struct ctl_netroute	 crt;
#endif
	u_int			 v;
	char			*str = NULL;

	switch (imsg->hdr.type) {
#ifndef __FreeBSD__
	case IMSG_DEMOTE:
		IMSG_SIZE_CHECK(imsg, &demote);
		memcpy(&demote, imsg->data, sizeof(demote));
		carp_demote_set(demote.group, demote.level);
		break;
	case IMSG_RTMSG:
		IMSG_SIZE_CHECK(imsg, &crt);
		memcpy(&crt, imsg->data, sizeof(crt));
		pfe_route(env, &crt);
		break;
#endif
	case IMSG_CTL_RESET:
		IMSG_SIZE_CHECK(imsg, &v);
		memcpy(&v, imsg->data, sizeof(v));
		parent_reload(env, v, NULL);
		break;
	case IMSG_CTL_RELOAD:
		if (IMSG_DATA_SIZE(imsg) > 0)
			str = get_string(imsg->data, IMSG_DATA_SIZE(imsg));
		parent_reload(env, CONFIG_RELOAD, str);
		if (str != NULL)
			free(str);
		break;
	case IMSG_CTL_SHUTDOWN:
		parent_shutdown(env);
		break;
	case IMSG_CFG_DONE:
		parent_configure_done(env);
		break;
	default:
		return (-1);
	}

	return (0);
}

int
parent_dispatch_hce(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct relayd		*env = p->p_env;
	struct privsep		*ps = env->sc_ps;
	struct ctl_script	 scr;

	switch (imsg->hdr.type) {
	case IMSG_SCRIPT:
		IMSG_SIZE_CHECK(imsg, &scr);
		bcopy(imsg->data, &scr, sizeof(scr));
		scr.retval = script_exec(env, &scr);
		proc_compose_imsg(ps, PROC_HCE, -1, IMSG_SCRIPT,
		    -1, &scr, sizeof(scr));
		break;
#ifndef __FreeBSD__
	case IMSG_SNMPSOCK:
		(void)snmp_setsock(env, p->p_id);
		break;
#endif
	case IMSG_CFG_DONE:
		parent_configure_done(env);
		break;
	default:
		return (-1);
	}

	return (0);
}

int
parent_dispatch_relay(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct relayd		*env = p->p_env;
	struct privsep		*ps = env->sc_ps;
	struct ctl_bindany	 bnd;
	int			 s;

	switch (imsg->hdr.type) {
	case IMSG_BINDANY:
		IMSG_SIZE_CHECK(imsg, &bnd);
		bcopy(imsg->data, &bnd, sizeof(bnd));
		if (bnd.bnd_proc > env->sc_prefork_relay)
			fatalx("pfe_dispatch_relay: "
			    "invalid relay proc");
		switch (bnd.bnd_proto) {
		case IPPROTO_TCP:
		case IPPROTO_UDP:
			break;
		default:
			fatalx("pfe_dispatch_relay: requested socket "
			    "for invalid protocol");
			/* NOTREACHED */
		}
		s = bindany(&bnd);
		proc_compose_imsg(ps, PROC_RELAY, bnd.bnd_proc,
		    IMSG_BINDANY, s, &bnd.bnd_id, sizeof(bnd.bnd_id));
		break;
	case IMSG_CFG_DONE:
		parent_configure_done(env);
		break;
	default:
		return (-1);
	}

	return (0);
}

int
parent_dispatch_ca(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct relayd		*env = p->p_env;

	switch (imsg->hdr.type) {
	case IMSG_CFG_DONE:
		parent_configure_done(env);
		break;
	default:
		return (-1);
	}

	return (0);
}

void
purge_table(struct tablelist *head, struct table *table)
{
	struct host		*host;

	while ((host = TAILQ_FIRST(&table->hosts)) != NULL) {
		TAILQ_REMOVE(&table->hosts, host, entry);
		if (event_initialized(&host->cte.ev)) {
			event_del(&host->cte.ev);
			close(host->cte.s);
		}
		if (host->cte.buf != NULL)
			ibuf_free(host->cte.buf);
		if (host->cte.ssl != NULL)
			SSL_free(host->cte.ssl);
		free(host);
	}
	if (table->sendbuf != NULL)
		free(table->sendbuf);
	if (table->conf.flags & F_SSL)
		SSL_CTX_free(table->ssl_ctx);

	if (head != NULL)
		TAILQ_REMOVE(head, table, entry);
	free(table);
}

void
purge_key(char **ptr, off_t len)
{
	char	*key = *ptr;

	if (key == NULL || len == 0)
		return;

#ifndef __FreeBSD__
	explicit_bzero(key, len);
#else
	bzero(key, len);
#endif
	free(key);

	*ptr = NULL;
}

void
purge_relay(struct relayd *env, struct relay *rlay)
{
	struct rsession		*con;
	struct relay_table	*rlt;

	/* shutdown and remove relay */
	if (event_initialized(&rlay->rl_ev))
		event_del(&rlay->rl_ev);
	close(rlay->rl_s);
	TAILQ_REMOVE(env->sc_relays, rlay, rl_entry);

	/* cleanup sessions */
	while ((con =
	    SPLAY_ROOT(&rlay->rl_sessions)) != NULL)
		relay_close(con, NULL);

	/* cleanup relay */
	if (rlay->rl_bev != NULL)
		bufferevent_free(rlay->rl_bev);
	if (rlay->rl_dstbev != NULL)
		bufferevent_free(rlay->rl_dstbev);

	purge_key(&rlay->rl_ssl_cert, rlay->rl_conf.ssl_cert_len);
	purge_key(&rlay->rl_ssl_key, rlay->rl_conf.ssl_key_len);
	purge_key(&rlay->rl_ssl_ca, rlay->rl_conf.ssl_ca_len);
	purge_key(&rlay->rl_ssl_cakey, rlay->rl_conf.ssl_cakey_len);

	if (rlay->rl_ssl_x509 != NULL) {
		X509_free(rlay->rl_ssl_x509);
		rlay->rl_ssl_x509 = NULL;
	}
	if (rlay->rl_ssl_pkey != NULL) {
		EVP_PKEY_free(rlay->rl_ssl_pkey);
		rlay->rl_ssl_pkey = NULL;
	}
	if (rlay->rl_ssl_cacertx509 != NULL) {
		X509_free(rlay->rl_ssl_cacertx509);
		rlay->rl_ssl_cacertx509 = NULL;
	}
	if (rlay->rl_ssl_capkey != NULL) {
		EVP_PKEY_free(rlay->rl_ssl_capkey);
		rlay->rl_ssl_capkey = NULL;
	}

	if (rlay->rl_ssl_ctx != NULL)
		SSL_CTX_free(rlay->rl_ssl_ctx);

	while ((rlt = TAILQ_FIRST(&rlay->rl_tables))) {
		TAILQ_REMOVE(&rlay->rl_tables, rlt, rlt_entry);
		free(rlt);
	}

	free(rlay);
}


struct kv *
kv_add(struct kvtree *keys, char *key, char *value)
{
	struct kv	*kv, *oldkv;

	if (key == NULL)
		return (NULL);
	if ((kv = calloc(1, sizeof(*kv))) == NULL)
		return (NULL);
	if ((kv->kv_key = strdup(key)) == NULL) {
		free(kv);
		return (NULL);
	}
	if (value != NULL &&
	    (kv->kv_value = strdup(value)) == NULL) {
		free(kv->kv_key);
		free(kv);
		return (NULL);
	}
	TAILQ_INIT(&kv->kv_children);

	if ((oldkv = RB_INSERT(kvtree, keys, kv)) != NULL) {
		TAILQ_INSERT_TAIL(&oldkv->kv_children, kv, kv_entry);
		kv->kv_parent = oldkv;
	}

	return (kv);
}

int
kv_set(struct kv *kv, char *fmt, ...)
{
	va_list		  ap;
	char		*value = NULL;
	struct kv	*ckv;

	va_start(ap, fmt);
	if (vasprintf(&value, fmt, ap) == -1)
		return (-1);
	va_end(ap);

	/* Remove all children */
	while ((ckv = TAILQ_FIRST(&kv->kv_children)) != NULL) {
		TAILQ_REMOVE(&kv->kv_children, ckv, kv_entry);
		kv_free(ckv);
		free(ckv);
	}

	/* Set the new value */
	if (kv->kv_value != NULL)
		free(kv->kv_value);
	kv->kv_value = value;

	return (0);
}

int
kv_setkey(struct kv *kv, char *fmt, ...)
{
	va_list  ap;
	char	*key = NULL;

	va_start(ap, fmt);
	if (vasprintf(&key, fmt, ap) == -1)
		return (-1);
	va_end(ap);

	if (kv->kv_key != NULL)
		free(kv->kv_key);
	kv->kv_key = key;

	return (0);
}

void
kv_delete(struct kvtree *keys, struct kv *kv)
{
	struct kv	*ckv;

	RB_REMOVE(kvtree, keys, kv);

	/* Remove all children */
	while ((ckv = TAILQ_FIRST(&kv->kv_children)) != NULL) {
		TAILQ_REMOVE(&kv->kv_children, ckv, kv_entry);
		kv_free(ckv);
		free(ckv);
	}

	kv_free(kv);
	free(kv);
}

struct kv *
kv_extend(struct kvtree *keys, struct kv *kv, char *value)
{
	char		*newvalue;

	if (kv == NULL) {
		return (NULL);
	} else if (kv->kv_value != NULL) {
		if (asprintf(&newvalue, "%s%s", kv->kv_value, value) == -1)
			return (NULL);

		free(kv->kv_value);
		kv->kv_value = newvalue;
	} else if ((kv->kv_value = strdup(value)) == NULL)
		return (NULL);

	return (kv);
}

void
kv_purge(struct kvtree *keys)
{
	struct kv	*kv;

	while ((kv = RB_MIN(kvtree, keys)) != NULL)
		kv_delete(keys, kv);
}

void
kv_free(struct kv *kv)
{
	if (kv->kv_type == KEY_TYPE_NONE)
		return;
	if (kv->kv_key != NULL) {
		free(kv->kv_key);
	}
	kv->kv_key = NULL;
	if (kv->kv_value != NULL) {
		free(kv->kv_value);
	}
	kv->kv_value = NULL;
	kv->kv_matchtree = NULL;
	kv->kv_match = NULL;
	memset(kv, 0, sizeof(*kv));
}

struct kv *
kv_inherit(struct kv *dst, struct kv *src)
{
	memset(dst, 0, sizeof(*dst));
	memcpy(dst, src, sizeof(*dst));
	TAILQ_INIT(&dst->kv_children);

	if (src->kv_key != NULL) {
		if ((dst->kv_key = strdup(src->kv_key)) == NULL) {
			kv_free(dst);
			return (NULL);
		}
	}
	if (src->kv_value != NULL) {
		if ((dst->kv_value = strdup(src->kv_value)) == NULL) {
			kv_free(dst);
			return (NULL);
		}
	}

	if (src->kv_match != NULL)
		dst->kv_match = src->kv_match;
	if (src->kv_matchtree != NULL)
		dst->kv_matchtree = src->kv_matchtree;

	return (dst);
}

int
kv_log(struct rsession *con, struct kv *kv, u_int16_t labelid,
    enum direction dir)
{
	char	*msg;

	if (con->se_log == NULL)
		return (0);
	if (asprintf(&msg, " %s%s%s%s%s%s%s",
	    dir == RELAY_DIR_REQUEST ? "[" : "{",
	    labelid == 0 ? "" : label_id2name(labelid),
	    labelid == 0 ? "" : ", ",
	    kv->kv_key == NULL ? "(unknown)" : kv->kv_key,
	    kv->kv_value == NULL ? "" : ": ",
	    kv->kv_value == NULL ? "" : kv->kv_value,
	    dir == RELAY_DIR_REQUEST ? "]" : "}") == -1)
		return (-1);
	if (evbuffer_add(con->se_log, msg, strlen(msg)) == -1) {
		free(msg);
		return (-1);
	}
	free(msg);
	con->se_haslog = 1;
	return (0);
}

struct kv *
kv_find(struct kvtree *keys, struct kv *kv)
{
	struct kv	*match;
	const char	*key;

	if (kv->kv_flags & KV_FLAG_GLOBBING) {
		/* Test header key using shell globbing rules */
		key = kv->kv_key == NULL ? "" : kv->kv_key;
		RB_FOREACH(match, kvtree, keys) {
			if (fnmatch(key, match->kv_key, FNM_CASEFOLD) == 0)
				break;
		}
	} else {
		/* Fast tree-based lookup only works without globbing */
		match = RB_FIND(kvtree, keys, kv);
	}

	return (match);
}

int
kv_cmp(struct kv *a, struct kv *b)
{
	return (strcasecmp(a->kv_key, b->kv_key));
}

RB_GENERATE(kvtree, kv, kv_node, kv_cmp);

int
rule_add(struct protocol *proto, struct relay_rule *rule, const char *rulefile)
{
	struct relay_rule	*r = NULL;
	struct kv		*kv = NULL;
	FILE			*fp = NULL;
	char			 buf[BUFSIZ];
	int			 ret = -1;
	u_int			 i;

	for (i = 0; i < KEY_TYPE_MAX; i++) {
		kv = &rule->rule_kv[i];
		if (kv->kv_type != i)
			continue;

		switch (kv->kv_option) {
		case KEY_OPTION_LOG:
			/* log action needs a key or a file to be specified */
			if (kv->kv_key == NULL && rulefile == NULL &&
			    (kv->kv_key = strdup("*")) == NULL)
				goto fail;
			break;
		default:
			break;
		}

		switch (kv->kv_type) {
		case KEY_TYPE_QUERY:
		case KEY_TYPE_PATH:
		case KEY_TYPE_URL:
			if (rule->rule_dir != RELAY_DIR_REQUEST)
				goto fail;
			break;
		default:
			break;
		}

		if (kv->kv_value != NULL && strchr(kv->kv_value, '$') != NULL)
			kv->kv_flags |= KV_FLAG_MACRO;
		if (kv->kv_key != NULL && strpbrk(kv->kv_key, "*?[") != NULL)
			kv->kv_flags |= KV_FLAG_GLOBBING;
	}

	if (rulefile == NULL) {
		TAILQ_INSERT_TAIL(&proto->rules, rule, rule_entry);
		return (0);
	}

	if ((fp = fopen(rulefile, "r")) == NULL)
		goto fail;

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		/* strip whitespace and newline characters */
		buf[strcspn(buf, "\r\n\t ")] = '\0';
		if (!strlen(buf) || buf[0] == '#')
			continue;

		if ((r = rule_inherit(rule)) == NULL)
			goto fail;

		for (i = 0; i < KEY_TYPE_MAX; i++) {
			kv = &r->rule_kv[i];
			if (kv->kv_type != i)
				continue;
			if (kv->kv_key != NULL)
				free(kv->kv_key);
			if ((kv->kv_key = strdup(buf)) == NULL) {
				rule_free(r);
				free(r);
				goto fail;
			}
		}

		TAILQ_INSERT_TAIL(&proto->rules, r, rule_entry);
	}

	ret = 0;
	rule_free(rule);
	free(rule);

 fail:
	if (fp != NULL)
		fclose(fp);
	return (ret);
}

struct relay_rule *
rule_inherit(struct relay_rule *rule)
{
	struct relay_rule	*r;
	u_int			 i;
	struct kv		*kv;

	if ((r = calloc(1, sizeof(*r))) == NULL)
		return (NULL);
	memcpy(r, rule, sizeof(*r));

	for (i = 0; i < KEY_TYPE_MAX; i++) {
		kv = &rule->rule_kv[i];
		if (kv->kv_type != i)
			continue;
		if (kv_inherit(&r->rule_kv[i], kv) == NULL) {
			free(r);
			return(NULL);
		}
	}

	if (r->rule_label > 0)
		label_ref(r->rule_label);
	if (r->rule_tag > 0)
		tag_ref(r->rule_tag);
	if (r->rule_tagged > 0)
		tag_ref(r->rule_tagged);

	return (r);
}

void
rule_free(struct relay_rule *rule)
{
	u_int	i;

	for (i = 0; i < KEY_TYPE_MAX; i++)
		kv_free(&rule->rule_kv[i]);
	if (rule->rule_label > 0)
		label_unref(rule->rule_label);
	if (rule->rule_tag > 0)
		tag_unref(rule->rule_tag);
	if (rule->rule_tagged > 0)
		tag_unref(rule->rule_tagged);
}

void
rule_delete(struct relay_rules *rules, struct relay_rule *rule)
{
	TAILQ_REMOVE(rules, rule, rule_entry);
	rule_free(rule);
	free(rule);
}

void
rule_settable(struct relay_rules *rules, struct relay_table *rlt)
{
	struct relay_rule	*r;
	char		 	 pname[TABLE_NAME_SIZE];

	if (rlt->rlt_table == NULL || strlcpy(pname, rlt->rlt_table->conf.name,
	    sizeof(pname)) >= sizeof(pname))
		return;

	pname[strcspn(pname, ":")] = '\0';

	TAILQ_FOREACH(r, rules, rule_entry) {
		if (r->rule_tablename[0] &&
		    strcmp(pname, r->rule_tablename) == 0) {
			r->rule_table = rlt;
		} else {
			r->rule_table = NULL;
		}
	}
}

/*
 * Utility functions
 */

struct host *
host_find(struct relayd *env, objid_t id)
{
	struct table	*table;
	struct host	*host;

	TAILQ_FOREACH(table, env->sc_tables, entry)
		TAILQ_FOREACH(host, &table->hosts, entry)
			if (host->conf.id == id)
				return (host);
	return (NULL);
}

struct table *
table_find(struct relayd *env, objid_t id)
{
	struct table	*table;

	TAILQ_FOREACH(table, env->sc_tables, entry)
		if (table->conf.id == id)
			return (table);
	return (NULL);
}

struct rdr *
rdr_find(struct relayd *env, objid_t id)
{
	struct rdr	*rdr;

	TAILQ_FOREACH(rdr, env->sc_rdrs, entry)
		if (rdr->conf.id == id)
			return (rdr);
	return (NULL);
}

struct relay *
relay_find(struct relayd *env, objid_t id)
{
	struct relay	*rlay;

	TAILQ_FOREACH(rlay, env->sc_relays, rl_entry)
		if (rlay->rl_conf.id == id)
			return (rlay);
	return (NULL);
}

struct protocol *
proto_find(struct relayd *env, objid_t id)
{
	struct protocol	*p;

	TAILQ_FOREACH(p, env->sc_protos, entry)
		if (p->id == id)
			return (p);
	return (NULL);
}

struct rsession *
session_find(struct relayd *env, objid_t id)
{
	struct relay		*rlay;
	struct rsession		*con;

	TAILQ_FOREACH(rlay, env->sc_relays, rl_entry)
		SPLAY_FOREACH(con, session_tree, &rlay->rl_sessions)
			if (con->se_id == id)
				return (con);
	return (NULL);
}

#ifndef __FreeBSD__
struct netroute *
route_find(struct relayd *env, objid_t id)
{
	struct netroute	*nr;

	TAILQ_FOREACH(nr, env->sc_routes, nr_route)
		if (nr->nr_conf.id == id)
			return (nr);
	return (NULL);
}

struct router *
router_find(struct relayd *env, objid_t id)
{
	struct router	*rt;

	TAILQ_FOREACH(rt, env->sc_rts, rt_entry)
		if (rt->rt_conf.id == id)
			return (rt);
	return (NULL);
}
#endif

struct host *
host_findbyname(struct relayd *env, const char *name)
{
	struct table	*table;
	struct host	*host;

	TAILQ_FOREACH(table, env->sc_tables, entry)
		TAILQ_FOREACH(host, &table->hosts, entry)
			if (strcmp(host->conf.name, name) == 0)
				return (host);
	return (NULL);
}

struct table *
table_findbyname(struct relayd *env, const char *name)
{
	struct table	*table;

	TAILQ_FOREACH(table, env->sc_tables, entry)
		if (strcmp(table->conf.name, name) == 0)
			return (table);
	return (NULL);
}

struct table *
table_findbyconf(struct relayd *env, struct table *tb)
{
	struct table		*table;
	struct table_config	 a, b;

	bcopy(&tb->conf, &a, sizeof(a));
	a.id = a.rdrid = 0;
	a.flags &= ~(F_USED|F_BACKUP);

	TAILQ_FOREACH(table, env->sc_tables, entry) {
		bcopy(&table->conf, &b, sizeof(b));
		b.id = b.rdrid = 0;
		b.flags &= ~(F_USED|F_BACKUP);

		/*
		 * Compare two tables and return the existing table if
		 * the configuration seems to be the same.
		 */
		if (bcmp(&a, &b, sizeof(b)) == 0 &&
		    ((tb->sendbuf == NULL && table->sendbuf == NULL) ||
		    (tb->sendbuf != NULL && table->sendbuf != NULL &&
		    strcmp(tb->sendbuf, table->sendbuf) == 0)))
			return (table);
	}
	return (NULL);
}

struct rdr *
rdr_findbyname(struct relayd *env, const char *name)
{
	struct rdr	*rdr;

	TAILQ_FOREACH(rdr, env->sc_rdrs, entry)
		if (strcmp(rdr->conf.name, name) == 0)
			return (rdr);
	return (NULL);
}

struct relay *
relay_findbyname(struct relayd *env, const char *name)
{
	struct relay	*rlay;

	TAILQ_FOREACH(rlay, env->sc_relays, rl_entry)
		if (strcmp(rlay->rl_conf.name, name) == 0)
			return (rlay);
	return (NULL);
}

struct relay *
relay_findbyaddr(struct relayd *env, struct relay_config *rc)
{
	struct relay	*rlay;

	TAILQ_FOREACH(rlay, env->sc_relays, rl_entry)
		if (bcmp(&rlay->rl_conf.ss, &rc->ss, sizeof(rc->ss)) == 0 &&
		    rlay->rl_conf.port == rc->port)
			return (rlay);
	return (NULL);
}

EVP_PKEY *
pkey_find(struct relayd *env, objid_t id)
{
	struct ca_pkey	*pkey;

	TAILQ_FOREACH(pkey, env->sc_pkeys, pkey_entry)
		if (pkey->pkey_id == id)
			return (pkey->pkey);
	return (NULL);
}

struct ca_pkey *
pkey_add(struct relayd *env, EVP_PKEY *pkey, objid_t id)
{
	struct ca_pkey	*ca_pkey;

	if (env->sc_pkeys == NULL)
		fatalx("pkeys");

	if ((ca_pkey = calloc(1, sizeof(*ca_pkey))) == NULL)
		return (NULL);

	ca_pkey->pkey = pkey;
	ca_pkey->pkey_id = id;

	TAILQ_INSERT_TAIL(env->sc_pkeys, ca_pkey, pkey_entry);

	return (ca_pkey);
}

void
event_again(struct event *ev, int fd, short event,
    void (*fn)(int, short, void *),
    struct timeval *start, struct timeval *end, void *arg)
{
	struct timeval tv_next, tv_now, tv;

	getmonotime(&tv_now);
	bcopy(end, &tv_next, sizeof(tv_next));
	timersub(&tv_now, start, &tv_now);
	timersub(&tv_next, &tv_now, &tv_next);

	bzero(&tv, sizeof(tv));
	if (timercmp(&tv_next, &tv, >))
		bcopy(&tv_next, &tv, sizeof(tv));

	event_del(ev);
	event_set(ev, fd, event, fn, arg);
	event_add(ev, &tv);
}

int
expand_string(char *label, size_t len, const char *srch, const char *repl)
{
	char *tmp;
	char *p, *q;

	if ((tmp = calloc(1, len)) == NULL) {
		log_debug("%s: calloc", __func__);
		return (-1);
	}
	p = q = label;
	while ((q = strstr(p, srch)) != NULL) {
		*q = '\0';
		if ((strlcat(tmp, p, len) >= len) ||
		    (strlcat(tmp, repl, len) >= len)) {
			log_debug("%s: string too long", __func__);
			free(tmp);
			return (-1);
		}
		q += strlen(srch);
		p = q;
	}
	if (strlcat(tmp, p, len) >= len) {
		log_debug("%s: string too long", __func__);
		free(tmp);
		return (-1);
	}
	(void)strlcpy(label, tmp, len);	/* always fits */
	free(tmp);

	return (0);
}

void
translate_string(char *str)
{
	char	*reader;
	char	*writer;

	reader = writer = str;

	while (*reader) {
		if (*reader == '\\') {
			reader++;
			switch (*reader) {
			case 'n':
				*writer++ = '\n';
				break;
			case 'r':
				*writer++ = '\r';
				break;
			default:
				*writer++ = *reader;
			}
		} else
			*writer++ = *reader;
		reader++;
	}
	*writer = '\0';
}

char *
digeststr(enum digest_type type, const u_int8_t *data, size_t len, char *buf)
{
	switch (type) {
	case DIGEST_SHA1:
#ifdef __FreeBSD__
		return (SHA1_Data(data, len, buf));
#else
		return (SHA1Data(data, len, buf));
#endif
		break;
	case DIGEST_MD5:
		return (MD5Data(data, len, buf));
		break;
	default:
		break;
	}
	return (NULL);
}

const char *
canonicalize_host(const char *host, char *name, size_t len)
{
	struct sockaddr_in	 sin4;
	struct sockaddr_in6	 sin6;
	u_int			 i, j;
	size_t			 plen;
	char			 c;

	if (len < 2)
		goto fail;

	/*
	 * Canonicalize an IPv4/6 address
	 */
	if (inet_pton(AF_INET, host, &sin4) == 1)
		return (inet_ntop(AF_INET, &sin4, name, len));
	if (inet_pton(AF_INET6, host, &sin6) == 1)
		return (inet_ntop(AF_INET6, &sin6, name, len));

	/*
	 * Canonicalize a hostname
	 */

	/* 1. remove repeated dots and convert upper case to lower case */
	plen = strlen(host);
	bzero(name, len);
	for (i = j = 0; i < plen; i++) {
		if (j >= (len - 1))
			goto fail;
		c = tolower(host[i]);
		if ((c == '.') && (j == 0 || name[j - 1] == '.'))
			continue;
		name[j++] = c;
	}

	/* 2. remove trailing dots */
	for (i = j; i > 0; i--) {
		if (name[i - 1] != '.')
			break;
		name[i - 1] = '\0';
		j--;
	}
	if (j <= 0)
		goto fail;

	return (name);

 fail:
	errno = EINVAL;
	return (NULL);
}

int
bindany(struct ctl_bindany *bnd)
{
	int	s, v;

	s = -1;
	v = 1;

	if (relay_socket_af(&bnd->bnd_ss, bnd->bnd_port) == -1)
		goto fail;
	if ((s = socket(bnd->bnd_ss.ss_family,
	    bnd->bnd_proto == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM,
	    bnd->bnd_proto)) == -1)
		goto fail;
#ifdef SO_BINDANY
	if (setsockopt(s, SOL_SOCKET, SO_BINDANY,
	    &v, sizeof(v)) == -1)
		goto fail;
#else
#ifdef IP_BINDANY
	if (setsockopt(s, IPPROTO_IP, IP_BINDANY,
	    &v, sizeof(v)) == -1)
		goto fail;
#endif
#endif
	if (bind(s, (struct sockaddr *)&bnd->bnd_ss,
	    bnd->bnd_ss.ss_len) == -1)
		goto fail;

	return (s);

 fail:
	if (s != -1)
		close(s);
	return (-1);
}

int
map6to4(struct sockaddr_storage *in6)
{
	struct sockaddr_storage	 out4;
	struct sockaddr_in	*sin4 = (struct sockaddr_in *)&out4;
	struct sockaddr_in6	*sin6 = (struct sockaddr_in6 *)in6;

	bzero(sin4, sizeof(*sin4));
	sin4->sin_len = sizeof(*sin4);
	sin4->sin_family = AF_INET;
	sin4->sin_port = sin6->sin6_port;

	bcopy(&sin6->sin6_addr.s6_addr[12], &sin4->sin_addr.s_addr,
	    sizeof(sin4->sin_addr));

	if (sin4->sin_addr.s_addr == INADDR_ANY ||
	    sin4->sin_addr.s_addr == INADDR_BROADCAST ||
	    IN_MULTICAST(ntohl(sin4->sin_addr.s_addr)))
		return (-1);

	bcopy(&out4, in6, sizeof(*in6));

	return (0);
}

int
map4to6(struct sockaddr_storage *in4, struct sockaddr_storage *map)
{
	struct sockaddr_storage	 out6;
	struct sockaddr_in	*sin4 = (struct sockaddr_in *)in4;
	struct sockaddr_in6	*sin6 = (struct sockaddr_in6 *)&out6;
	struct sockaddr_in6	*map6 = (struct sockaddr_in6 *)map;

	if (sin4->sin_addr.s_addr == INADDR_ANY ||
	    sin4->sin_addr.s_addr == INADDR_BROADCAST ||
	    IN_MULTICAST(ntohl(sin4->sin_addr.s_addr)))
		return (-1);

	bcopy(map6, sin6, sizeof(*sin6));
	sin6->sin6_len = sizeof(*sin6);
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = sin4->sin_port;

	bcopy(&sin4->sin_addr.s_addr, &sin6->sin6_addr.s6_addr[12],
	    sizeof(sin4->sin_addr));

	bcopy(&out6, in4, sizeof(*in4));

	return (0);
}

void
socket_rlimit(int maxfd)
{
	struct rlimit	 rl;

	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
		fatal("socket_rlimit: failed to get resource limit");
#ifndef __FreeBSD__
	log_debug("%s: max open files %llu", __func__, rl.rlim_max);
#else
	log_debug("%s: max open files %llu", __func__,
	    (long long unsigned int)rl.rlim_max);
#endif

	/*
	 * Allow the maximum number of open file descriptors for this
	 * login class (which should be the class "daemon" by default).
	 */
	if (maxfd == -1)
		rl.rlim_cur = rl.rlim_max;
	else
		rl.rlim_cur = MAX(rl.rlim_max, (rlim_t)maxfd);
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1)
		fatal("socket_rlimit: failed to set resource limit");
}

char *
get_string(u_int8_t *ptr, size_t len)
{
	size_t	 i;
	char	*str;

	for (i = 0; i < len; i++)
		if (!(isprint(ptr[i]) || isspace(ptr[i])))
			break;

	if ((str = calloc(1, i + 1)) == NULL)
		return (NULL);
	memcpy(str, ptr, i);

	return (str);
}

void *
get_data(u_int8_t *ptr, size_t len)
{
	u_int8_t	*data;

	if ((data = calloc(1, len)) == NULL)
		return (NULL);
	memcpy(data, ptr, len);

	return (data);
}

int
sockaddr_cmp(struct sockaddr *a, struct sockaddr *b, int prefixlen)
{
	struct sockaddr_in	*a4, *b4;
	struct sockaddr_in6	*a6, *b6;
	u_int32_t		 av[4], bv[4], mv[4];

	if (a->sa_family == AF_UNSPEC || b->sa_family == AF_UNSPEC)
		return (0);
	else if (a->sa_family > b->sa_family)
		return (1);
	else if (a->sa_family < b->sa_family)
		return (-1);

	if (prefixlen == -1)
		memset(&mv, 0xff, sizeof(mv));

	switch (a->sa_family) {
	case AF_INET:
		a4 = (struct sockaddr_in *)a;
		b4 = (struct sockaddr_in *)b;

		av[0] = a4->sin_addr.s_addr;
		bv[0] = b4->sin_addr.s_addr;
		if (prefixlen != -1)
			mv[0] = prefixlen2mask(prefixlen);

		if ((av[0] & mv[0]) > (bv[0] & mv[0]))
			return (1);
		if ((av[0] & mv[0]) < (bv[0] & mv[0]))
			return (-1);
		break;
	case AF_INET6:
		a6 = (struct sockaddr_in6 *)a;
		b6 = (struct sockaddr_in6 *)b;

		memcpy(&av, &a6->sin6_addr.s6_addr, 16);
		memcpy(&bv, &b6->sin6_addr.s6_addr, 16);
		if (prefixlen != -1)
			prefixlen2mask6(prefixlen, mv);

		if ((av[3] & mv[3]) > (bv[3] & mv[3]))
			return (1);
		if ((av[3] & mv[3]) < (bv[3] & mv[3]))
			return (-1);
		if ((av[2] & mv[2]) > (bv[2] & mv[2]))
			return (1);
		if ((av[2] & mv[2]) < (bv[2] & mv[2]))
			return (-1);
		if ((av[1] & mv[1]) > (bv[1] & mv[1]))
			return (1);
		if ((av[1] & mv[1]) < (bv[1] & mv[1]))
			return (-1);
		if ((av[0] & mv[0]) > (bv[0] & mv[0]))
			return (1);
		if ((av[0] & mv[0]) < (bv[0] & mv[0]))
			return (-1);
		break;
	}

	return (0);
}

u_int32_t
prefixlen2mask(u_int8_t prefixlen)
{
	if (prefixlen == 0)
		return (0);

	if (prefixlen > 32)
		prefixlen = 32;

	return (htonl(0xffffffff << (32 - prefixlen)));
}

struct in6_addr *
prefixlen2mask6(u_int8_t prefixlen, u_int32_t *mask)
{
	static struct in6_addr  s6;
	int			i;

	if (prefixlen > 128)
		prefixlen = 128;

	bzero(&s6, sizeof(s6));
	for (i = 0; i < prefixlen / 8; i++)
		s6.s6_addr[i] = 0xff;
	i = prefixlen % 8;
	if (i)
		s6.s6_addr[prefixlen / 8] = 0xff00 >> i;

	memcpy(mask, &s6, sizeof(s6));

	return (&s6);
}

#ifndef __FreeBSD__ /* file descriptor accounting */
int
accept_reserve(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
    int reserve, volatile int *counter)
{
	int ret;
	if (getdtablecount() + reserve +
	    *counter >= getdtablesize()) {
		errno = EMFILE;
		return (-1);
	}

	if ((ret = accept(sockfd, addr, addrlen)) > -1) {
		(*counter)++;
		DPRINTF("%s: inflight incremented, now %d",__func__, *counter);
	}
	return (ret);
}
#endif
