# $FreeBSD$

PROG=	relayd

MAN=	relayd.8 \
	relayd.conf.5

.PATH:	${.CURDIR}/../../lib/libutil
SRCS=	imsg-buffer.c \
	imsg.c

.PATH:	${.CURDIR}
SRCS+=	parse.y \
	ca.c \
	check_icmp.c \
	check_script.c \
	check_tcp.c \
	config.c \
	control.c \
	hce.c \
	log.c \
	name2id.c \
	pfe.c \
	pfe_filter.c \
	proc.c \
	relay.c \
	relay_http.c \
	relay_udp.c \
	relayd.c \
	shuffle.c \
	ssl.c

.PATH:	${.CURDIR}/../../../libevent
SRCS+=	buffer.c \
	evbuffer.c \
	event.c \
	evlog.c \
	evutil.c \
	kqueue.c \
	poll.c \
	select.c \
	signal.c

CFLAGS+=	-DSHA1_DIGEST_LENGTH=SHA_DIGEST_LENGTH \
		-DSHA1_DIGEST_STRING_LENGTH=SHA_DIGEST_LENGTH \
		-DOPENSSL_NO_SHA -DOPENSSL_NO_MD5 \
		-D__dead='' \
		-DHAVE_CONFIG_H
CFLAGS+=	-I${.CURDIR} -I${.CURDIR}/../../lib/libutil \
		-I${.CURDIR}/../../../libevent
CLEANFILES+=	y.tab.h

LDADD=		-lmd -L${PREFIX}/lib ${LIBEVENT} -lssl -lcrypto
DPADD=		${LIBEVENT} ${LIBSSL} ${LIBCRYPTO}

.include <bsd.prog.mk>
