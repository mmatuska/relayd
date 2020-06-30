/*	$OpenBSD: ca.c,v 1.8 2014/05/04 16:38:19 reyk Exp $	*/

/*
 * Copyright (c) 2014 Reyk Floeter <reyk@openbsd.org>
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

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <net/if.h>
#include <netinet/in.h>

#include <limits.h>
#include <event.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>

#include "relayd.h"

void	 ca_init(struct privsep *, struct privsep_proc *p, void *);
void	 ca_launch(void);

int	 ca_dispatch_parent(int, struct privsep_proc *, struct imsg *);
int	 ca_dispatch_relay(int, struct privsep_proc *, struct imsg *);

static int	 rsae_pub_enc(int, const u_char *, u_char *, RSA *, int);
static int	 rsae_pub_dec(int,const u_char *, u_char *, RSA *, int);
static int	 rsae_priv_enc(int, const u_char *, u_char *, RSA *, int);
static int	 rsae_priv_dec(int, const u_char *, u_char *, RSA *, int);
static int	 rsae_mod_exp(BIGNUM *, const BIGNUM *, RSA *, BN_CTX *);
static int	 rsae_bn_mod_exp(BIGNUM *, const BIGNUM *, const BIGNUM *,
	    const BIGNUM *, BN_CTX *, BN_MONT_CTX *);
static int	 rsae_init(RSA *);
static int	 rsae_finish(RSA *);
static int	 rsae_sign(int, const u_char *, u_int, u_char *, u_int *,
	    const RSA *);
static int	 rsae_verify(int dtype, const u_char *m, u_int, const u_char *,
	    u_int, const RSA *);
static int	 rsae_keygen(RSA *, int, BIGNUM *, BN_GENCB *);

static struct relayd *env = NULL;
extern int		 proc_id;

static struct privsep_proc procs[] = {
	{ "parent",	PROC_PARENT,	ca_dispatch_parent },
	{ "relay",	PROC_RELAY,	ca_dispatch_relay },
};

pid_t
ca(struct privsep *ps, struct privsep_proc *p)
{
	env = ps->ps_env;

	return (proc_run(ps, p, procs, nitems(procs), ca_init, NULL));
}

void
ca_init(struct privsep *ps, struct privsep_proc *p, void *arg)
{
	if (config_init(ps->ps_env) == -1)
		fatal("failed to initialize configuration");

	proc_id = p->p_instance;
	env->sc_id = getpid() & 0xffff;
}

void
ca_launch(void)
{
	BIO		*in = NULL;
	EVP_PKEY	*pkey = NULL;
	struct relay	*rlay;

	TAILQ_FOREACH(rlay, env->sc_relays, rl_entry) {
		if ((rlay->rl_conf.flags & (F_SSL|F_SSLCLIENT)) == 0)
			continue;

		if (rlay->rl_conf.ssl_key_len) {
			if ((in = BIO_new_mem_buf(rlay->rl_ssl_key,
			    rlay->rl_conf.ssl_key_len)) == NULL)
				fatalx("ca_launch: key");

			if ((pkey = PEM_read_bio_PrivateKey(in,
			    NULL, NULL, NULL)) == NULL)
				fatalx("ca_launch: PEM");
			BIO_free(in);

			rlay->rl_ssl_pkey = pkey;

			if (pkey_add(env, pkey,
			    rlay->rl_conf.ssl_keyid) == NULL)
				fatalx("ssl pkey");

			purge_key(&rlay->rl_ssl_key,
			    rlay->rl_conf.ssl_key_len);
		}
		if (rlay->rl_conf.ssl_cert_len) {
			purge_key(&rlay->rl_ssl_cert,
			    rlay->rl_conf.ssl_cert_len);
		}
		if (rlay->rl_conf.ssl_cakey_len) {
			if ((in = BIO_new_mem_buf(rlay->rl_ssl_cakey,
			    rlay->rl_conf.ssl_cakey_len)) == NULL)
				fatalx("ca_launch: key");

			if ((pkey = PEM_read_bio_PrivateKey(in,
			    NULL, NULL, NULL)) == NULL)
				fatalx("ca_launch: PEM");
			BIO_free(in);

			rlay->rl_ssl_capkey = pkey;

			if (pkey_add(env, pkey,
			    rlay->rl_conf.ssl_cakeyid) == NULL)
				fatalx("ca pkey");

			purge_key(&rlay->rl_ssl_cakey,
			    rlay->rl_conf.ssl_cakey_len);
		}
		if (rlay->rl_conf.ssl_cacert_len) {
			purge_key(&rlay->rl_ssl_cacert,
			    rlay->rl_conf.ssl_cacert_len);
		}
	}
}

int
ca_dispatch_parent(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	switch (imsg->hdr.type) {
	case IMSG_CFG_RELAY:
		config_getrelay(env, imsg);
		break;
	case IMSG_CFG_DONE:
		config_getcfg(env, imsg);
		break;
	case IMSG_CTL_START:
		ca_launch();
		break;
	case IMSG_CTL_RESET:
		config_getreset(env, imsg);
		break;
	default:
		return (-1);
	}

	return (0);
}

int
ca_dispatch_relay(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct ctl_keyop	 cko;
	EVP_PKEY		*pkey;
	RSA			*rsa;
	u_char			*from = NULL, *to = NULL;
	struct iovec		 iov[2];
	int			 c = 0;

	switch (imsg->hdr.type) {
	case IMSG_CA_PRIVENC:
	case IMSG_CA_PRIVDEC:
		IMSG_SIZE_CHECK(imsg, (&cko));
		bcopy(imsg->data, &cko, sizeof(cko));
		if (cko.cko_proc > env->sc_prefork_relay)
			fatalx("ca_dispatch_relay: "
			    "invalid relay proc");
		if (IMSG_DATA_SIZE(imsg) != (sizeof(cko) + cko.cko_flen))
			fatalx("ca_dispatch_relay: "
			    "invalid key operation");
		if ((pkey = pkey_find(env, cko.cko_id)) == NULL ||
		    (rsa = EVP_PKEY_get1_RSA(pkey)) == NULL)
			fatalx("ca_dispatch_relay: "
			    "invalid relay key or id");

		DPRINTF("%s:%d: key id %d", __func__, __LINE__, cko.cko_id);

		from = (u_char *)imsg->data + sizeof(cko);
		if ((to = calloc(1, cko.cko_tlen)) == NULL)
			fatalx("ca_dispatch_relay: calloc");

		switch (imsg->hdr.type) {
		case IMSG_CA_PRIVENC:
			cko.cko_tlen = RSA_private_encrypt(cko.cko_flen,
			    from, to, rsa, cko.cko_padding);
			break;
		case IMSG_CA_PRIVDEC:
			cko.cko_tlen = RSA_private_decrypt(cko.cko_flen,
			    from, to, rsa, cko.cko_padding);
			break;
		}

		iov[c].iov_base = &cko;
		iov[c++].iov_len = sizeof(cko);
		if (cko.cko_tlen) {
			iov[c].iov_base = to;
			iov[c++].iov_len = cko.cko_tlen;
		}

		proc_composev_imsg(env->sc_ps, PROC_RELAY, cko.cko_proc,
		    imsg->hdr.type, -1, iov, c);

		free(to);
		RSA_free(rsa);
		break;
	default:
		return (-1);
	}

	return (0);
}

/*
 * RSA privsep engine (called from unprivileged processes)
 */

const RSA_METHOD *rsa_default = NULL;

static RSA_METHOD *rsae_method = NULL;

static int
rsae_send_imsg(int flen, const u_char *from, u_char *to, RSA *rsa,
    int padding, u_int cmd)
{
	struct ctl_keyop cko;
	int		 ret = 0;
	objid_t		*id;
	struct iovec	 iov[2];
	struct imsgbuf	*ibuf;
	struct imsgev	*iev;
	struct imsg	 imsg;
	int		 n, done = 0, cnt = 0;
	u_char		*toptr;

	if ((id = RSA_get_ex_data(rsa, 0)) == NULL)
		return (0);

	iev = proc_iev(env->sc_ps, PROC_CA, proc_id);
	ibuf = &iev->ibuf;

	/*
	 * XXX this could be nicer...
	 */

	cko.cko_id = *id;
	cko.cko_proc = proc_id;
	cko.cko_flen = flen;
	cko.cko_tlen = RSA_size(rsa);
	cko.cko_padding = padding;

	iov[cnt].iov_base = &cko;
	iov[cnt++].iov_len = sizeof(cko);
	iov[cnt].iov_base = (void *)from;
	iov[cnt++].iov_len = flen;

	/*
	 * Send a synchronous imsg because we cannot defer the RSA
	 * operation in OpenSSL's engine layer.
	 */
	imsg_composev(ibuf, cmd, 0, 0, -1, iov, cnt);
	imsg_flush(ibuf);

	while (!done) {
		if ((n = imsg_read(ibuf)) == -1)
			fatalx("imsg_read");
		if (n == 0)
			fatalx("pipe closed");

		while (!done) {
			if ((n = imsg_get(ibuf, &imsg)) == -1)
				fatalx("imsg_get error");
			if (n == 0)
				break;
			if (imsg.hdr.type != cmd)
				fatalx("invalid response");

			IMSG_SIZE_CHECK(&imsg, (&cko));
			memcpy(&cko, imsg.data, sizeof(cko));
			if (IMSG_DATA_SIZE(&imsg) !=
			    (sizeof(cko) + cko.cko_tlen))
				fatalx("data size");

			ret = cko.cko_tlen;
			if (ret) {
				toptr = (u_char *)imsg.data + sizeof(cko);
				memcpy(to, toptr, ret);
			}
			done = 1;

			imsg_free(&imsg);
		}
	}
	imsg_event_add(iev);

	return (ret);
}

static int
rsae_pub_enc(int flen,const unsigned char *from, unsigned char *to, RSA *rsa,
    int padding)
{
        DPRINTF("%s:%d", __func__, __LINE__);
	return (RSA_meth_get_pub_enc(rsa_default)(flen, from, to, rsa, padding));
}

static int
rsae_pub_dec(int flen,const unsigned char *from, unsigned char *to, RSA *rsa,
    int padding)
{
	DPRINTF("%s:%d", __func__, __LINE__);
	return (RSA_meth_get_pub_dec(rsa_default)(flen, from, to, rsa, padding));
}

static int
rsae_priv_enc(int flen, const unsigned char *from, unsigned char *to, RSA *rsa,
    int padding)
{
	DPRINTF("%s:%d", __func__, __LINE__);
	if (RSA_get_ex_data(rsa, 0) != NULL)
		return (rsae_send_imsg(flen, from, to, rsa, padding,
		    IMSG_CA_PRIVENC));
	return (RSA_meth_get_priv_enc(rsa_default)(flen, from, to, rsa, padding));
}

static int
rsae_priv_dec(int flen, const unsigned char *from, unsigned char *to, RSA *rsa,
    int padding)
{
        DPRINTF("%s:%d", __func__, __LINE__);
	if (RSA_get_ex_data(rsa, 0) != NULL)
		return (rsae_send_imsg(flen, from, to, rsa, padding,
		    IMSG_CA_PRIVDEC));

	return (RSA_meth_get_priv_dec(rsa_default)(flen, from, to, rsa, padding));
}

static int
rsae_mod_exp(BIGNUM *r0, const BIGNUM *I, RSA *rsa, BN_CTX *ctx)
{

        DPRINTF("%s:%d", __func__, __LINE__);
	return (RSA_meth_get_mod_exp(rsa_default)(r0, I, rsa, ctx));
}

static int
rsae_bn_mod_exp(BIGNUM *r, const BIGNUM *a, const BIGNUM *p,
    const BIGNUM *m, BN_CTX *ctx, BN_MONT_CTX *m_ctx)
{
        DPRINTF("%s:%d", __func__, __LINE__);
	return (RSA_meth_get_bn_mod_exp(rsa_default)(r, a, p, m, ctx, m_ctx));
}

static int
rsae_init(RSA *rsa)
{
        DPRINTF("%s:%d", __func__, __LINE__);
	if (RSA_meth_get_init(rsa_default) == NULL)
		return (1);
	return (RSA_meth_get_init(rsa_default)(rsa));
}

static int
rsae_finish(RSA *rsa)
{
        DPRINTF("%s:%d", __func__, __LINE__);
	if (RSA_meth_get_finish(rsa_default) == NULL)
		return (1);
	return (RSA_meth_get_finish(rsa_default)(rsa));
}

static int
rsae_keygen(RSA *rsa, int bits, BIGNUM *e, BN_GENCB *cb)
{
        DPRINTF("%s:%d", __func__, __LINE__);
	return (RSA_meth_get_keygen(rsa_default)(rsa, bits, e, cb));
}

void
ca_engine_init(struct relayd *x_env)
{
	ENGINE		*e;
	const char	*errstr, *name;

	if (env == NULL)
		env = x_env;
        if ((rsae_method = RSA_meth_new("RSA privsep engine", 0)) == NULL) {
		errstr = "RSA_meth_new";
		goto fail;
	}

	RSA_meth_set_pub_enc(rsae_method, rsae_pub_enc);
	RSA_meth_set_pub_dec(rsae_method, rsae_pub_dec);
	RSA_meth_set_priv_enc(rsae_method, rsae_priv_enc);
	RSA_meth_set_priv_dec(rsae_method, rsae_priv_dec);
	RSA_meth_set_mod_exp(rsae_method, rsae_mod_exp);
	RSA_meth_set_bn_mod_exp(rsae_method, rsae_bn_mod_exp);
	RSA_meth_set_init(rsae_method, rsae_init);
	RSA_meth_set_finish(rsae_method, rsae_finish);
	RSA_meth_set_keygen(rsae_method, rsae_keygen);

	if ((e = ENGINE_get_default_RSA()) == NULL) {
		if ((e = ENGINE_new()) == NULL) {
			errstr = "ENGINE_new";
			goto fail;
		}
		if (!ENGINE_set_name(e, RSA_meth_get0_name(rsae_method))) {
			errstr = "ENGINE_set_name";
			goto fail;
		}
		if ((rsa_default = RSA_get_default_method()) == NULL) {
			errstr = "RSA_get_default_method";
			goto fail;
		}
	} else if ((rsa_default = ENGINE_get_RSA(e)) == NULL) {
		errstr = "ENGINE_get_RSA";
		goto fail;
	}

	if ((name = ENGINE_get_name(e)) == NULL)
		name = "unknown RSA engine";

	log_debug("%s: using %s", __func__, name);

        if (RSA_meth_get_mod_exp(rsa_default) == NULL)
		RSA_meth_set_mod_exp(rsae_method, NULL);
	if (RSA_meth_get_bn_mod_exp(rsa_default) == NULL)
		RSA_meth_set_bn_mod_exp(rsae_method, NULL);
	if (RSA_meth_get_keygen(rsa_default) == NULL)
		RSA_meth_set_keygen(rsae_method, NULL);
	RSA_meth_set_flags(rsae_method,
		RSA_meth_get_flags(rsa_default) | RSA_METHOD_FLAG_NO_CHECK);
	RSA_meth_set0_app_data(rsae_method,
		RSA_meth_get0_app_data(rsa_default));

	if (!ENGINE_set_RSA(e, rsae_method)) {
		errstr = "ENGINE_set_RSA";
		goto fail;
	}
	if (!ENGINE_set_default_RSA(e)) {
		errstr = "ENGINE_set_default_RSA";
		goto fail;
	}

	return;

 fail:
	ssl_error(__func__, errstr);
	fatalx(errstr);
}
