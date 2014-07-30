# $FreeBSD$

PROG=	relayctl

.PATH:	${.CURDIR}/../../lib/libutil
SRCS=	imsg-buffer.c \
	imsg.c

.PATH:	${.CURDIR}/../relayd
SRCS+=	log.c

.PATH:	${.CURDIR}
SRCS+=	relayctl.c \
	parser.c

MAN=	relayctl.8

CFLAGS+=	-D__dead='' \
		-DHAVE_CONFIG_H
CFLAGS+=	-I${.CURDIR} \
		-I${.CURDIR}/../../lib/libutil \
		-I${.CURDIR}/../relayd \
		-I${.CURDIR}/../../../libevent

.include <bsd.prog.mk>
