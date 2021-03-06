/** gtping/gtping.c
 *
 * GTP Ping
 *
 * By Thomas Habets <thomas@habets.se> 2008-2010
 *
 * Send GTP Ping and time the reply.
 *
 *
 */
/*
 *  Copyright (C) 2008-2010 Thomas Habets <thomas@habets.se>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <netdb.h>
#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "getaddrinfo.h"

#include "gtping.h"

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

#ifndef SOL_IPV6
#define SOL_IPV6 IPPROTO_IPV6
#endif

/* pings older than TRACKPINGS_SIZE * the_wait_time are ignored.
 * They are old and are considered lost.
 */
#define TRACKPINGS_SIZE 1000

/* For those OSs that don't read RFC3493, even though their manpage
 * points to it. */
#ifndef AI_ADDRCONFIG
# define AI_ADDRCONFIG 0
#endif

static const char *version = PACKAGE_VERSION;

static volatile sig_atomic_t sigintReceived = 0;
static unsigned int curSeq = 0;
static double startTime;
static double sendTimes[TRACKPINGS_SIZE]; /* RTT data*/
static int gotIt[TRACKPINGS_SIZE];        /* duplicate-check scratchpad  */
static unsigned int totalTimeCount = 0;
static double totalTime = 0;
static double totalTimeSquared = 0;
static double totalMin = -1;
static double totalMax = -1;
static unsigned int dups = 0;
static unsigned int reorder = 0;
static unsigned int highestSeq = 0;
static unsigned int connectionRefused = 0;

/* from cmdline */
const char *argv0 = 0;
struct Options options = {
        port: DEFAULT_PORT,         /* -p <port> */
        verbose: DEFAULT_VERBOSE,   /* -v increments */

        flood: 0,      /* -f */

        /* if still <0, set to DEFAULT_INTERVAL.
         * set this way to make -f work with -i  */
        interval: -1,  /* -i <time> */

        wait: -1,      /* -w <time> */
        autowait: 0,   /* 0 = -w not used, continuously update options.wait  */

        count: 0,      /* -c, 0 is infinite */
        target: 0,     /* arg */
        targetip: 0,   /* resolved arg */
        ttl: -1,       /* -T <ttl> */
        tos: -1,       /* -Q <dscp> */
        has_teid: 0,   /* -t <teid> */
        teid: 0,       /* -t <teid> */
        af: AF_UNSPEC, /* -4 or -6 */
        version: DEFAULT_GTPVERSION, /* -g <version> */

        source:      NULL,  /* -s <source if or addr> */
        source_port: "0",   /* -P <num> */

        traceroute: 0, /* -r */
        traceroutehops: DEFAULT_TRACEROUTEHOPS,  /* -r[<# per hop>] */
};

static const char *dscpTable[][2] = {
        /* dscp values */
        {"ef",   "184"}, {"be",     "0"}, {"de",     "0"},
        {"af11",  "40"}, {"af12",  "48"}, {"af13",  "56"},
        {"af21",  "72"}, {"af22",  "80"}, {"af23",  "88"},
        {"af31", "104"}, {"af32", "112"}, {"af33", "120"},
        {"af41", "136"}, {"af42", "144"}, {"af43", "152"},
        {"cs0",    "0"}, {"cs1",   "32"}, {"cs2",   "64"},
        {"cs3",   "96"}, {"cs4",  "128"}, {"cs5",  "160"},
        {"cs6",  "192"}, {"cs7",  "224"},
        {(char*)NULL,(char*)NULL}
};

static const char *tosTable[][2] = {
        /* tos names */
        {"lowdelay",         "16"},
        {"throughput",        "8"},
        {"reliability",       "4"},
        {"lowcost",           "2"},
        {"mincost",           "2"},
        {"tosbit7",           "1"}, /* not a real tos name, but define all */
        /* precedence */
        {"netcontrol",      "224"},
        {"internetcontrol", "192"},
        {"critic_ecp",      "160"},
        {"flashoverride",   "128"},
        {"flash",            "96"},
        {"immediate",        "64"},
        {"priority",         "32"},
        {"routine",           "0"},
        {(char*)NULL,(char*)NULL}
};

/**
 *
 */
static int
tryBind(int fd, const struct addrinfo *curaddr)
{
        int ret;
        int err;

        char host[NI_MAXHOST];
        if ((err = getnameinfo(curaddr->ai_addr,
                               curaddr->ai_addrlen,
                               host, sizeof(host),
                               NULL, 0,
                               NI_NUMERICHOST))) {
                return 1;
        }

        ret = bind(fd, curaddr->ai_addr, curaddr->ai_addrlen);

        if (!ret) {
                /* if ok */
                if (options.verbose) {
                        fprintf(stderr,
                                "%s: successfully bound to %s\n",
                                argv0,
                                host);
                }
        } else {
                /* if error */
                if (options.verbose) {
                        fprintf(stderr,
                                "%s: bind(%s): %s\n",
                                argv0,
                                host,
                                strerror(errno));
                }
        }

        return ret;
}

/**
 *
 */
int
sockaddrlen(int af)
{
        switch(af) {
        case AF_INET:
                return sizeof(struct sockaddr_in);
        case AF_INET6:
                return sizeof(struct sockaddr_in6);
        default:
                fprintf(stderr,
                        "%s: Internal error: unknown AF %d\n",
                        argv0, af);
                return 0;
        }
}


/**
 *
 */
static void
bindSocket(int fd, const struct addrinfo *dest)
{
        struct addrinfo hints;
        struct addrinfo *addrs = 0;
        struct addrinfo *curaddr = 0;
        int gerr;
        const char *source = options.source;

        if (!source) {
                if (!strcmp(options.source_port, "0")) {
                        return;
                }
                if (dest->ai_family == AF_INET) {
                        source = "0.0.0.0";
                } else if (dest->ai_family == AF_INET6) {
                        source = "::";
                } else {
                        fprintf(stderr, "%s: Internal error: "
                                "source unspecified, af of dest unspecified, "
                                "port specified\n", argv0);
                        goto err;
                }
        }

        addrs = getIfAddrs(dest);
        if (addrs) {
                goto try_to_bind;
        }

        /* try to resolve it as an address. By now the target is set up so
         * we know what we want */
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = AI_ADDRCONFIG;
        hints.ai_family = dest->ai_family;
        hints.ai_socktype = dest->ai_socktype;
        hints.ai_protocol = dest->ai_protocol;
        if (0 > (gerr = getaddrinfo(source,
                                    options.source_port,
                                    &hints,
                                    &addrs))) {
                if (gerr == EAI_SYSTEM) {
                        fprintf(stderr, "%s: getaddrinfo(%s): %s\n",
                                argv0, source, strerror(errno));
                }
                fprintf(stderr, "%s: getaddrinfo(addr=%s, port=%s): %s\n",
                        argv0, source, options.source_port,
                        gai_strerror(gerr));
                goto err;
        }

 try_to_bind:
        /* try to bind to every one */
        for (curaddr = addrs; curaddr; curaddr = curaddr->ai_next) {
                if (!tryBind(fd, curaddr)) {
                        goto success;
                }
        }
 err:;
        fprintf(stderr,
                "%s: bind(addr=%s, port=%s) failed. "
                "Using dynamic addr/port.\n",
                argv0, source, options.source_port);
 success:;
        /* manpage doesn't say what happens if addrs is null, so don't take
         * any chances */
        if (addrs) {
                freeaddrinfo(addrs);
        }
}

/**
 * callback function for SIGINT. Will terminate the mainloop.
 */
static void
sigint(int unused)
{
	unused = unused; /* silence warning */
	sigintReceived = 1;
}

/**
 * Create socket and "connect" it to target
 * allocates and sets options.targetip
 *
 * return fd, or <0 (-errno) on error
 */
static int
setupSocket()
{
	int fd = -1;
	int err = 0;
	struct addrinfo *addrs = 0;
	struct addrinfo hints;

	if (options.verbose > 2) {
		fprintf(stderr, "%s: setupSocket(%s)\n",
			argv0, options.target);
	}
	if (!(options.targetip = malloc(NI_MAXHOST))) {
		err = errno;
		fprintf(stderr, "%s: malloc(NI_MAXHOST): %s\n",
			argv0, strerror(err));
		goto errout;
	}

	/* resolve to sockaddr */
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_ADDRCONFIG;
	hints.ai_family = options.af;
	hints.ai_socktype = SOCK_DGRAM;
	if (0 > (err = getaddrinfo(options.target,
				   options.port,
				   &hints,
				   &addrs))) {
		int gai_err;
		gai_err = err;
		if (gai_err == EAI_SYSTEM) {
			err = errno;
		} else {
			err = EINVAL;
		}
		if (gai_err == EAI_NONAME) {
			fprintf(stderr, "%s: unknown host %s\n",
				argv0, options.target);
			err = EINVAL;
			goto errout;
		}
                fprintf(stderr, "%s: getaddrinfo(%s): %s\n",
                        argv0, options.target, gai_strerror(gai_err));
		goto errout;
	}

	/* get ip address string options.targetip */
	if ((err = getnameinfo(addrs->ai_addr,
			       addrs->ai_addrlen,
			       options.targetip,
			       NI_MAXHOST,
			       NULL, 0,
			       NI_NUMERICHOST))) {
		int gai_err;
		gai_err = err;
		if (gai_err == EAI_SYSTEM) {
			err = errno;
		} else {
			err = EINVAL;
		}
		fprintf(stderr, "%s: getnameinfo(): %s\n",
			argv0,	gai_strerror(gai_err));
		goto errout;
	}
	if (options.verbose > 1) {
		fprintf(stderr, "%s: target=<%s> targetip=<%s>\n",
			argv0,
			options.target,
			options.targetip);
	}

	/* socket() */
	if (0 > (fd = socket(addrs->ai_family,
			     addrs->ai_socktype,
			     addrs->ai_protocol))) {
		err = errno;
		fprintf(stderr, "%s: socket(%d, %d, %d): %s\n",
			argv0,
			addrs->ai_family,
			addrs->ai_socktype,
			addrs->ai_protocol,
			strerror(err));
		goto errout;
	}

        errInspectionInit(fd, addrs);

        bindSocket(fd, addrs);

	if (addrs->ai_family == AF_INET) {
                int on = 1;
		if (options.ttl > 0) {
			if (setsockopt(fd,
				       SOL_IP,
				       IP_TTL,
				       &options.ttl,
				       sizeof(options.ttl))) {
				fprintf(stderr,
					"%s: setsockopt(%d, SOL_IP, IP_TTL, "
					"%d): %s\n", argv0, fd, options.ttl,
					strerror(errno));
			}
		}
		if (options.tos >= 0) {
			if (setsockopt(fd,
				       SOL_IP,
				       IP_TOS,
				       &options.tos,
				       sizeof(options.tos))) {
				fprintf(stderr,
					"%s: setsockopt(%d, SOL_IP, IP_TOS, "
					"%d): %s\n", argv0, fd, options.tos,
					strerror(errno));
			}
		}
		if (setsockopt(fd,
			       SOL_IP,
			       IP_RECVTTL,
			       &on,
			       sizeof(on))) {
			fprintf(stderr,
				"%s: setsockopt(%d, SOL_IP, "
				"IP_RECVTTL, on): %s\n",
				argv0, fd, strerror(errno));
		}
#ifdef IP_RECVTOS
		if (setsockopt(fd,
			       SOL_IP,
			       IP_RECVTOS,
			       &on,
			       sizeof(on))) {
			fprintf(stderr,
				"%s: setsockopt(%d, SOL_IP, "
				"IP_RECVTOS, on): %s\n",
				argv0, fd, strerror(errno));
		}
#endif
	}
	if (addrs->ai_family == AF_INET6) {
                int on = 1;
		if (options.ttl > 0) {
#ifndef IPV6_HOPLIMIT
                        fprintf(stderr,
                                "%s: Setting hoplimit on IPv6 "
                                "is not supported on your OS\n", argv0);
#else
			if (setsockopt(fd,
				       SOL_IPV6,
				       IPV6_HOPLIMIT,
				       &options.ttl,
				       sizeof(options.ttl))) {
				fprintf(stderr,
					"%s: setsockopt(%d, SOL_IPV6, "
					"IPV6_HOPLIMIT, %d): %s\n",
					argv0, fd, options.ttl,
					strerror(errno));
			}
#endif
		}
		if (options.tos >= 0) {
#ifndef IPV6_TCLASS
                        fprintf(stderr,
                                "%s: Setting traffic class on IPv6 "
                                "is not supported on your OS\n", argv0);
#else
			if (setsockopt(fd,
				       SOL_IPV6,
				       IPV6_TCLASS,
				       &options.tos,
				       sizeof(options.tos))) {
				fprintf(stderr,
					"%s: setsockopt(%d, SOL_IPV6, "
                                        "IPV6_TCLASS, %d): %s\n",
                                        argv0, fd, options.tos,
					strerror(errno));
			}
#endif
		}
#ifdef IPV6_RECVHOPLIMIT
		if (setsockopt(fd,
			       SOL_IPV6,
			       IPV6_RECVHOPLIMIT,
			       &on,
			       sizeof(on))) {
			fprintf(stderr,
				"%s: setsockopt(%d, SOL_IPV6, "
				"IPV6_RECVHOPLIMIT, on): %s\n",
				argv0, fd, strerror(errno));
		}
#endif
#ifdef IPV6_RECVTCLASS
                if (setsockopt(fd,
			       SOL_IPV6,
			       IPV6_RECVTCLASS,
			       &on,
			       sizeof(on))) {
			fprintf(stderr,
				"%s: setsockopt(%d, SOL_IPV6, "
				"IPV6_RECVTCLASS, on): %s\n",
				argv0, fd, strerror(errno));
		}
#endif
	}

	/* connect() */
	if (connect(fd,
		    addrs->ai_addr,
		    addrs->ai_addrlen)) {
		err = errno;
		fprintf(stderr, "%s: connect(%d, ...): %s\n",
			argv0, fd, strerror(err));
		close(fd);
		goto errout;
	}

	freeaddrinfo(addrs);
	return fd;
 errout:
	if (addrs) {
		freeaddrinfo(addrs);
		addrs = 0;
	}
	if (options.targetip) {
		free(options.targetip);
		options.targetip = 0;
	}
	if (err == 0) {
		err = -EINVAL;
	}
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
	if (err > 0) {
		err = -err;
	}
	return err;
}

/**
 *
 */
static size_t
mkping_v1(int seq, void **packet)
{
        struct GtpEchoV1 *gtp;
        if (!(gtp = malloc(sizeof(struct GtpEchoV1)))) {
                return -errno;
        }

        *packet = gtp;

        memset(gtp, 0, sizeof(struct GtpEchoV1));
        gtp->version = options.version;
        gtp->has_seq = 1;   /* turn on sequence numbers */
        gtp->proto_type = 1; /* GTP, as opposed to GTP' */
        gtp->msg = GTPMSG_ECHO;
        gtp->len = htons(4);
        if (options.teid >= 0) {
                gtp->teid = htonl(options.teid);
        } else {
                gtp->teid = 0;
        }
        gtp->seq = htons(seq);
        gtp->npdu = 0x00;
        gtp->next = 0x00;

        return sizeof(struct GtpEchoV1);
}

/**
 *
 */
static size_t
mkping_v2(int seq, void **packet)
{
        struct GtpEchoV2 *gtp;
        if (!(gtp = malloc(sizeof(struct GtpEchoV2)))) {
                return -errno;
        }

        *packet = gtp;

        memset(gtp, 0, sizeof(struct GtpEchoV2));
        gtp->version = options.version;
        gtp->msg = GTPMSG_ECHO;

        if (options.has_teid) {
                gtp->len = htons(4); /* FIXME; 6? */
                gtp->u2.s.teid = htonl(options.teid);
                gtp->u2.s.seq = htons(seq);
                gtp->has_teid = 1;
                return GTPECHOv2_LEN_WITHOUT_TEID + 4;
        } else {
                gtp->len = 0; /* FIXME: 2? */
                gtp->u2.seq = htons(seq);
                return GTPECHOv2_LEN_WITHOUT_TEID;
        }
}

/**
 *
 */
static size_t
mkping(int seq, void **packet)
{
        switch (options.version) {
        case 1:
                return mkping_v1(seq, packet);
        case 2:
                return mkping_v2(seq, packet);
        }
        fprintf(stderr,
                "%s: internal error, bad version %d\n",
                argv0, options.version);
        exit(1);
}

/**
 * return 0 on succes, <0 on fail (nothing sent), >0 on sent, but something
 * failed (do increment sent counter)
 */
static int
sendEcho(int fd, int seq)
{
	int err = 0;
        void *packet = 0;
        ssize_t packetlen;

	if (options.verbose > 2) {
		fprintf(stderr, "%s: sendEcho(%d, %d)\n", argv0, fd, seq);
	}

        if (0 > (packetlen = mkping(seq, &packet))) {
                err = packetlen;
                goto errout;
        }

	if (options.verbose > 1) {
		fprintf(stderr,	"%s: Sending GTP ping with seq=%d size %d\n",
			argv0, curSeq, (int)packetlen);
	}

        sendTimes[seq % TRACKPINGS_SIZE] = clock_get_dbl();
        gotIt[seq % TRACKPINGS_SIZE] = 0;

	if (packetlen != send(fd, packet, packetlen, 0)) {
		err = errno;
		if (err == ECONNREFUSED) {
                        printf("Connection refused\n");
                        connectionRefused++;
                        goto errout;
		}
                fprintf(stderr, "%s: send(%d, ...): %s\n",
                        argv0, fd, strerror(errno));
                err = -err;
                goto errout;
	}
 errout:
        free(packet);
	return 0;
}

/**
 * For a given tos number, find the tos name.
 * Output is written to buffer of length buflen (incl null terminator).
 */
const char*
tos2String(int tos, char *buf, size_t buflen)
{
        int c;

        if (!buflen) {
                fprintf(stderr, "%s: tos2String called with buflen=0\n",
                        argv0);
                return buf;
        }

        buf[0] = 0;
        buf[buflen-1] = 0;

        for (c = 0; dscpTable[c][0]; c++) {
                const char **cur = dscpTable[c];
                if (tos == atoi(cur[1])) {
                        snprintf(buf, buflen, "DSCP=%s", cur[0]);
                        return buf;
                }
        }

        for (c = 0; tosTable[c][0]; c++) {
                const char **cur = tosTable[c];
                int curTos = atoi(cur[1]);
                if (curTos && (tos & curTos & 0x1E) == curTos) {
                        if (buf[0]) {
                                strncat(buf, ",", buflen);
                        } else {
                                strncpy(buf, "ToS=", buflen);
                        }
                        strncat(buf, cur[0], buflen);
                        tos &= ~curTos;
                }
        }
        tos >>= 5;
        if (tos) {
                char b[128];
                snprintf(b, sizeof(b), " Prec=%d", tos);
                strncat(buf, b, buflen);
        }

        return buf;
}

/**
 *
 */
static struct GtpReply
parseReply_v1(const void *packet, size_t packetlen)
{
        struct GtpReply ret;
        const struct GtpEchoV1 *gtp = packet;
        memset(&ret, 0, sizeof(ret));
        /* check packet size */

        if (packetlen < sizeof(struct GtpEchoV1)) {
                fprintf(stderr, "%s: Short GTPv1 packet received: %d < 12\n",
                        argv0, (int)packetlen);
                return ret;
        }

        if (packetlen > sizeof(struct GtpEchoV1)) {
                if (options.verbose) {
                        printf("%s: Long packet received: %d < 12\n",
                               argv0, (int)packetlen);
                }
                /* continue parsing packet... */
        }

        ret.ok = 1;
        ret.msg = gtp->msg;
        ret.version = 1;

        ret.has_seq = gtp->has_seq;
        ret.seq = htons(gtp->seq);

        ret.has_teid = 1;
        ret.teid = htonl(gtp->teid);

        ret.has_npdu = gtp->has_npdu;
        ret.npdu = gtp->npdu;

        ret.has_ext_head = gtp->has_ext_head;
        ret.next = gtp->next;

        return ret;
}

/**
 *
 */
static struct GtpReply
parseReply_v2(const void *packet, size_t packetlen)
{
        struct GtpReply ret;
        const struct GtpEchoV2 *gtp = packet;
        size_t right_len = 0;

        memset(&ret, 0, sizeof(ret));

        /* shortest possible GTPv2 packet is 8 bytes */
        if (packetlen < GTPECHOv2_LEN_WITHOUT_TEID) {
                fprintf(stderr,
                        "%s: Short GTPv2 packet received: %d < 8\n",
                        argv0, (int)packetlen);
                return ret;
        }

        if (gtp->has_teid) {
                right_len = GTPECHOv2_LEN_WITHOUT_TEID + 4;
        } else {
                right_len = GTPECHOv2_LEN_WITHOUT_TEID;
        }

        if (gtp->piggyback) {
                fprintf(stderr,
                        "%s: Get GTP packet with piggyback flag "
                        "unexpectedly set. "
                        "Not parsing piggybacked data.",
                        argv0);
                if (packetlen > right_len) {
                        packetlen = right_len;
                }
        }

        if (packetlen != right_len) {
                fprintf(stderr,
                        "%s: GTPv2 packet length error: %d should be %d\n",
                        argv0, (int)packetlen, (int)right_len);
                if (packetlen < right_len) {
                        return ret;
                }
                /* continue parsing long packets */
        }

        ret.ok = 1;
        ret.msg = gtp->msg;
        ret.version = 2;
        ret.has_seq = 1;

        ret.has_teid = gtp->has_teid;
        if (ret.has_teid) {
                ret.teid = htonl(gtp->u2.s.teid);
                ret.seq = htons(gtp->u2.s.seq);
        } else {
                ret.seq = htons(gtp->u2.seq);
        }

        return ret;
}

/**
 *
 */
static struct GtpReply
parseReply(const void *packet, size_t packetlen)
{
        const struct GtpEchoV1 *gtp; /* it's ok, we'll just check version */
        struct GtpReply err;
        err.ok = 0;

        if (packetlen < 1) {
                return err;
        }

        gtp = packet;

        switch (gtp->version) {
        case 1:
                return parseReply_v1(packet, packetlen);
        case 2:
                return parseReply_v2(packet, packetlen);
        }

        fprintf(stderr,
                "%s: Bad packet with version %d received\n",
                argv0, gtp->version);
        return err;
}



/**
 * return 0 on success/got reply,
 *        <0 on fail. Errno returned.
 *        >0 on success, but no packet (EINTR or dup packet)
 */
static int
recvEchoReply(int fd)
{
	int err;
        char packet[1024];
        ssize_t packetlen;
	double now;
	char lag[128];
        int isDup = 0;
        int isReorder = 0;
        int ttl;
        int tos;
        char tosString[128] = {0};
        char ttlString[128] = {0};
        struct GtpReply gtp;

	if (options.verbose > 2) {
		fprintf(stderr, "%s: recvEchoReply()\n", argv0);
	}

	now = clock_get_dbl();

	memset(packet, 0, sizeof(packet));
        if (0 > (packetlen = doRecv(fd,
                                    (void*)packet,
                                    sizeof(packet),
                                    &ttl,
                                    &tos))) {
		switch(errno) {
                case ECONNREFUSED:
                        connectionRefused++;
			handleRecvErr(fd, "Port closed", 0);
                        return 1;
		case EINTR:
                        return 1;
                case EHOSTUNREACH:
			handleRecvErr(fd, "Host unreachable or TTL exceeded",
                                      0);
                        return 1;
		default:
			err = errno;
			fprintf(stderr, "%s: recv(%d, ...): %s\n",
				argv0, fd, strerror(errno));
                        return err;
		}
	}

        /* create ttl string */
        if (0 <= ttl) {
                snprintf(ttlString, sizeof(ttlString), "ttl=%d ", ttl);
        }

        /* create tos string */
        if (0 <= tos) {
                char scratch[128];
                snprintf(tosString, sizeof(tosString),
                         "%s ", tos2String(tos,
                                           scratch,
                                           sizeof(scratch)));
        }

        gtp = parseReply(packet, packetlen);
        if (!gtp.ok) {
                return 1;
        }

	if (gtp.msg != GTPMSG_ECHOREPLY) {
		fprintf(stderr,
			"%s: Got non-EchoReply type of msg (type: %d)\n",
			argv0, gtp.msg);
                return 1;
	}

        if (curSeq - gtp.seq >= TRACKPINGS_SIZE) {
		strcpy(lag, "Inf");
	} else {
                int pos = gtp.seq % TRACKPINGS_SIZE;
                double lagf = now - sendTimes[pos];
                if (gotIt[pos]) {
                        isDup = 1;
                }
                gotIt[pos]++;
		snprintf(lag, sizeof(lag), "%.2f ms", 1000 * lagf);
                if (!isDup) {
                        totalTime += lagf;
                        totalTimeSquared += lagf * lagf;
                        totalTimeCount++;
                        if ((0 > totalMin) || (lagf < totalMin)) {
                                totalMin = lagf;
                        }
                        if ((0 > totalMax) || (lagf > totalMax)) {
                                totalMax = lagf;
                        }
                }
                if (options.autowait) {
                        options.wait = 2 * (totalTime / totalTimeCount);
                        if (options.verbose > 1) {
                                fprintf(stderr,
                                        "%s: Adjusting waittime to %.6f\n",
                                        argv0, options.wait);
                        }
                }
	}

        /* detect packet reordering */
        if (!isDup) {
                if (highestSeq > gtp.seq) {
                        reorder++;
                        isReorder = 1;
                } else {
                        highestSeq = gtp.seq;
                }
        }

        if (options.flood) {
                if (!isDup) {
                        printf("\b \b");
                }
        } else {
                printf("%u bytes from %s: ver=%d seq=%u %s%stime=%s%s%s\n",
                       (int)packetlen,
                       options.targetip,
                       gtp.version,
                       gtp.seq,
                       tosString[0] ? tosString : "",
                       ttlString[0] ? ttlString : "",
                       lag,
                       isDup ? " (DUP)" : "",
                       isReorder ? " (out of order)" : "");
        }
        if (isDup) {
                dups++;
        }
	return isDup;
}

/**
 * FIXME: this function needs a cleanup, and probably some merging
 * with pingMainloop()
 */
static int
tracerouteMainloop(int fd)
{
        int ttl = 0;
        int ttlTry = 0;
        double curPingTime;
        double lastRecvTime = 0;
        double lastPingTime = 0;
        int n;
        int endOfTraceroute = 0;
        int printStar = 0;
        double timewait;

	printf("GTPING traceroute to %s (%s) packet version %d.\n",
	       options.target,
	       options.targetip,
	       (int)options.version);


	while (!sigintReceived) {
		struct pollfd fds;

		fds.fd = fd;
		fds.events = POLLIN;
		fds.revents = 0;

                /* time to send yet? */
		curPingTime = clock_get_dbl();
		if ((lastRecvTime >= lastPingTime)
                    || (curPingTime > lastPingTime + options.interval)) {
                        if (printStar) {
                                printf("*\n");
                        }
                        ttlTry++;
                        if (ttlTry < options.traceroutehops && ttl != 0) {
                                printf("     ");
                        } else {
                                if (endOfTraceroute) {
                                        break;
                                }
                                ttl++;
                                ttlTry = 0;
                                printf("%4d ", ttl);
                                fflush(stdout);
                        }
                        if (setsockopt(fd,
                                       SOL_IP,
                                       IP_TTL,
                                       &ttl,
                                       sizeof(ttl))) {
                                fprintf(stderr,
                                        "%s: setsockopt(%d, SOL_IP, IP_TTL, "
                                        "%d): %s\n", argv0, fd, ttl,
                                        strerror(errno));
                        }

                        if (0 <= sendEcho(fd, curSeq++)) {
                                lastPingTime = curPingTime;
                                printStar = 1;
                        }
                }

                /* max waittime: until it's time to send the next one */
		timewait = lastPingTime+options.interval - clock_get_dbl();
		if (timewait < 0) {
			timewait = 0;
		}
                timewait *= 0.5; /* leave room for overhead */

		switch ((n = poll(&fds, 1, (int)(timewait * 1000)))) {
		case 1: /* read ready */
                        printStar = 0;
			if (fds.revents & POLLERR) {
                                int e;
				e = handleRecvErr(fd, NULL, lastPingTime);
                                if (e) {
                                        lastRecvTime = clock_get_dbl();
                                }
                                if (e > 1) {
                                        endOfTraceroute = 1;
                                }
			}
			if (fds.revents & POLLIN) {
				n = recvEchoReply(fd);
                                endOfTraceroute = 1;
                                if (!n) {
                                        lastRecvTime = clock_get_dbl();
                                } else if (n > 0) {
                                        /* still ok, but no reply */
                                        printStar = 1;
                                } else {
                                        return 1;
                                }
			}
			break;
		case 0: /* timeout */
			break;
		case -1: /* error */
			switch (errno) {
			case EINTR:
			case EAGAIN:
				break;
			default:
				fprintf(stderr, "%s: poll([%d], 1, %d): %s\n",
					argv0,
					fd,
					(int)(timewait*1000),
					strerror(errno));
				exit(2);
			}
			break;
		default: /* can't happen */
			fprintf(stderr, "%s: poll() returned %d!\n", argv0, n);
			exit(2);
			break;
		}
        }
        return 0;
}

/**
 * return value is sent directly to return value of main()
 */
static int
pingMainloop(int fd)
{
	unsigned sent = 0;
	unsigned recvd = 0;
	double lastpingTime = 0; /* last time we sent out a ping */
	double curPingTime;   /* if we ping now, this is the timestamp of it */
        double lastRecvTime = 0; /* last time we got a reply */
        int recvErrors = 0;

	if (options.verbose > 2) {
		fprintf(stderr, "%s: mainloop(%d)\n", argv0, fd);
	}

	startTime = clock_get_dbl();

	printf("GTPING %s (%s) packet version %d\n",
	       options.target,
	       options.targetip,
	       options.version);

        lastRecvTime = startTime;
	while (!sigintReceived) {
                /* max time to wait for replies before checking if it's time
                 * to send another ping */
		double timewait;
		int n;
		struct pollfd fds;

                /* sent all we are going to send, and got all replies
                 * (either errors or good replies)
                 */
                if (options.count
                    && (sent == options.count)
                    && (sent == (recvd + recvErrors))) {
                        break;
                }

                /* time to send yet? */
		curPingTime = clock_get_dbl();

                /* if clock is not monotonic and time set backwards
                 * since last ping, start a new ping cycle */
                if (curPingTime < lastpingTime) {
                        lastpingTime = curPingTime - options.interval - 0.001;
                }

		if (curPingTime > lastpingTime + options.interval) {
			if (options.count && (curSeq == options.count)) {
				if (lastRecvTime+options.wait < curPingTime) {
                                        break;
                                }
			} else if (0 <= sendEcho(fd, curSeq++)) {
                                sent++;
                                lastpingTime = curPingTime;
                                if (options.flood) {
                                        printf(".");
                                        fflush(stdout);
                                }
			}
		}

		fds.fd = fd;
		fds.events = POLLIN;
		fds.revents = 0;

                /* max waittime: until it's time to send the next one */
                timewait = lastpingTime+options.interval - clock_get_dbl();

                /* never wait more than an interval. this can happen if
                 * clock is not monotonic */
                if (timewait > options.interval) {
                        timewait = options.interval;
                }

                /* this should never happen, should have been taken care of
                 * above. */
		if (timewait < 0) {
			timewait = 0;
		}

                /* leave room for overhead */
                timewait *= 0.5;

		switch ((n = poll(&fds, 1, (int)(timewait * 1000)))) {
		case 1: /* read ready */
			if (fds.revents & POLLERR) {
                                if (handleRecvErr(fd, NULL, 0)) {
                                        recvErrors++;
                                }
			}
			if (fds.revents & POLLIN) {
				n = recvEchoReply(fd);
                                if (!n) {
                                        recvd++;
                                        lastRecvTime = clock_get_dbl();
                                } else if (n > 0) {
                                        /* still ok, but no reply */
                                } else { /* n < 0 */
                                        return 1;
                                }
			}
			break;
		case 0: /* timeout */
			break;
		case -1: /* error */
			switch (errno) {
			case EINTR:
			case EAGAIN:
				break;
			default:
				fprintf(stderr, "%s: poll([%d], 1, %d): %s\n",
					argv0,
					fd,
					(int)(timewait*1000),
					strerror(errno));
				exit(2);
			}
			break;
		default: /* can't happen */
			fprintf(stderr, "%s: poll() returned %d!\n", argv0, n);
			exit(2);
			break;
		}

	}
	printf("\n--- %s GTP ping statistics ---\n"
               "%u packets transmitted, %u received, "
               "%d%% packet loss, "
               "time %dms\n"
               "%u out of order, %u dups, "
               "%u connection refused",
	       options.target,
               sent, recvd,
	       (int)((100.0*(sent-recvd))/sent),
               (int)(1000*(clock_get_dbl()-startTime)),
               reorder, dups,
               connectionRefused);
        errInspectionPrintSummary();
        printf("\n");
	if (totalTimeCount) {
		printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms",
		       1000*totalMin,
		       1000*(totalTime / totalTimeCount),
		       1000*totalMax,
		       1000*sqrt((totalTimeSquared -
				  (totalTime * totalTime)
				  /totalTimeCount)/totalTimeCount));
	}
	printf("\n");
	return recvd == 0;
}

/**
 * return a string of spaces as long as argv0.
 * if strlen(argv0) > oh say 19, just use 6 spaces.
 */
static char*
argv0lenSpaces()
{
        static char buf[20];
        size_t n;

        memset(buf, ' ', sizeof(buf));
        buf[sizeof(buf)-1] = 0;

        n = strlen(argv0);

        if (n < sizeof(buf)) {
                buf[n] = 0;
        } else {
                buf[strlen("gtping")] = 0;
        }
        return buf;
}


/**
 *
 */
static void
usage(int err)
{
        printf("Usage: %s "
               "[ -46hfvV ] "
               "[ -c <count> ] "
               "[ -i <time> ] "
               "\n       %s "
               "[ -p <port> ] "
               "[ -P <port> ] "
               "[ -Q <dscp> ] "
               "[ -r[<perhop>] ] "
               "\n       %s "
               "[ -s <source> ] "
               "[ -t <teid> ] "
               "[ -T <ttl> ] "
               "\n       %s "
               "[ -w <time> ] "
               "<target>\n"
               "\t-4               Force IPv4 (default: auto-detect)\n"
               "\t-6               Force IPv6 (default: auto-detect)\n"
               "\t-c <count>       Stop after sending count pings "
               "(default: 0=Infinite)\n"
               "\t-f               Flood ping mode (limit with -i)\n"
               "\t-h, --help       Show this help text\n"
               "\t-g <version>     Set GTP version (default: %u)\n"
               "\t-i <time>        Time between pings in seconds "
               "(default: %.1f)\n"
               "\t-p <port>        GTP-C UDP port to ping (default: %s)\n"
               "\t                 GTP-C is 2123, GTP-U is port 2152, "
               "GTP' is port 3386.\n"
               "\t-P <port>        Source port to use. Name or number.\n"
               "\t                 (default 0 = pick dynamically)\n"
               "\t-Q <dscp>        Set ToS/DSCP bit (default: don't set)\n"
               "\t                 Examples: ef, af21, 0xb8, lowdelay\n"
               "\t-r[<perhop>]     Traceroute. Number of pings per TTL "
               "(default: %d)\n"
               "\t                 Traceroute will only work correctly "
               "on Linux.\n"
               "\t-s <source>      Use this source address or interface\n"
               "\t                 Interface name will not work on all OSs\n"
               "\t-t <teid>        Transaction ID "
               "(default: not present or 0)\n"
               "\t-T <ttl>         IP TTL (default: system default)\n"
               "\t-v               Increase verbosity level (default: %d)\n"
               "\t-V, --version    Show version info and exit\n"
               "\t-w <time>        Time to wait for a response "
               "(default: 2*RTT or %.2fs)\n"
               "\n"
               "Report bugs to: thomas@habets.se\n"
               "gtping home page: "
               "<http://www.habets.pp.se/synscan/programs.php?prog=gtping>\n",
               argv0,
               argv0lenSpaces(),
               argv0lenSpaces(),
               argv0lenSpaces(),
               DEFAULT_GTPVERSION,
               DEFAULT_INTERVAL,
               DEFAULT_PORT,
               DEFAULT_TRACEROUTEHOPS,
               DEFAULT_VERBOSE,
               DEFAULT_WAIT);
        exit(err);
}

/**
 *
 */
static void
printVersion()
{
        printf("Copyright (C) 2008-2010 Thomas Habets <thomas@habets.se>\n"
               "License GPLv2: GNU GPL version 2 or later "
               "<http://gnu.org/licenses/gpl-2.0.html>\n"
               "This is free software: you are free to change and "
               "redistribute it.\n"
               "There is NO WARRANTY, to the extent permitted by law.\n");
        exit(0);
}


/**
 * return -1 on error, or 8bit number to put in IP ToS-field.
 */
static int
string2Tos(const char *instr)
{
        const char *rets = NULL;
        int ret = -1;
        const char *cp;
        int c;

        /* check for empty string. Not allowed */
        if (!strlen(instr)) {
                return -1;
        }

        /* check for special case instr = zeroes because strtol() can't.
         * This code depends on instr not being empty (checked above) */
        for (cp = instr; ; cp++) {
                if (*cp != '0') {
                        break;
                }
                if (*cp == 0) {
                        return 0;
                }
        }

        /* find match in table */
        for (c = 0; dscpTable[c][0]; c++) {
                const char **cur = dscpTable[c];
                if (!strcasecmp(instr, cur[0])) {
                        rets = cur[1];
                }
        }
        /* find match in table */
        for (c = 0; tosTable[c][0]; c++) {
                const char **cur = tosTable[c];
                if (!strcasecmp(instr, cur[0])) {
                        rets = cur[1];
                }
        }

        if (rets) {
                /* if match was found, translate to number */
                ret = (int)strtol(rets, 0, 0);
        } else {
                /* if no match, try to parse as a number directly */
                ret = (int)strtol(instr, 0, 0);
                if (!ret) {
                        /* the real case of -Q 0 is handled above */
                        ret = -1;
                }
        }
        if (ret > 255) {
                ret = -1;
        }
        return ret;
}

/**
 *
 */
int
main(int argc, char **argv)
{
	int fd;
        int port_set = 0;

	printf("GTPing %s\n", version);

	argv0 = argv[0];

        { /* handle GNU options */
                int c;
                for (c = 1; c < argc; c++) {
                        if (!strcmp(argv[c], "--")) {
                                break;
                        } else if (!strcmp(argv[c], "--help")) {
                                usage(0);
                        } else if (!strcmp(argv[c], "--version")) {
                                printVersion();
                        }
                }
        }

        /* parse options */
	{
		int c;
                unsigned int tmpu;
		while (-1 != (c=getopt(argc,
                                       argv,
                                       "46c:fhi:g:p:P:Q:r::s:t:T:vVw:"))) {
			switch(c) {
                        case '4':
                                options.af = AF_INET;
                                break;
                        case '6':
                                options.af = AF_INET6;
                                break;
			case 'c':
				options.count = strtoul(optarg, 0, 0);
				break;
                        case 'f':
                                options.flood = 1;
                                /* if interval not alread set, set it to 0 */
                                if (0 > options.interval) {
                                        options.interval = 0;
                                        fprintf(stderr,
                                                "%s: invalid interval \"%s\", "
                                                "set to 0\n",
                                                argv0, optarg);
                                }
                                break;
			case 'g':
				tmpu = strtoul(optarg, 0, 0);
                                if (tmpu < 1 || tmpu > 2) {
                                        fprintf(stderr,
                                                "%s: invalid GTP version %u. "
                                                "Supported: 1 & 2.",
                                                argv0, tmpu);
                                }
                                options.version = tmpu;
				break;
			case 'h':
				usage(0);
				break;
			case 'p':
				options.port = optarg;
                                port_set = 1;
				break;
                        case 'P':
                                options.source_port = optarg;
                                break;
                        case 's':
                                options.source = optarg;
                                break;
			case 't':
				options.teid = strtoul(optarg, 0, 0);
                                options.has_teid = 1;
				break;
			case 'T':
				options.ttl = strtoul(optarg, 0, 0);
                                if (options.ttl > 255) {
                                        options.ttl = 255;
                                }
				break;
			case 'v':
				options.verbose++;
				break;
                        case 'V':
                                printVersion();
			case 'i':
				options.interval = atof(optarg);
				break;
			case 'w':
				options.wait = atof(optarg);
				break;
                        case 'Q':
                                if (-1 == (options.tos = string2Tos(optarg))) {
                                        fprintf(stderr,
                                                "%s: invalid ToS/DSCP \"%s\", "
                                                "left as-is.\n", argv0,optarg);
                                        fprintf(stderr, "%s: "
                                                "Valid are "
                                                "BE,EF,AF[1-4][1-3],CS[0-7] "
                                                "and numeric (0x for hex).\n",
                                                argv0);
                                }
                                break;
                        case 'r':
                                options.traceroute = 1;
                                if (optarg) {
                                        options.traceroutehops = atoi(optarg);
                                }
                                break;
			case '?':
			default:
				usage(2);
			}
		}
	}
        if (0 > options.interval) {
                options.interval = DEFAULT_INTERVAL;
        }
        if (0 > options.wait) {
                options.wait = DEFAULT_WAIT;
                options.autowait = 1;
                if (options.verbose > 1) {
                        fprintf(stderr, "%s: autowait is ON. "
                                "Initial wait: %6.3f seconds\n",
                                argv0, options.wait);
                }
        }

	if (optind + 1 != argc) {
		usage(2);
	}

	options.target = argv[optind];

	if (SIG_ERR == signal(SIGINT, sigint)) {
		fprintf(stderr, "%s: signal(SIGINT, ...): %s\n",
			argv0, strerror(errno));
		return 1;
	}

	if (0 > (fd = setupSocket())) {
		return 1;
	}
        if (options.traceroute) {
                return tracerouteMainloop(fd);
        } else {
                return pingMainloop(fd);
        }
}

/* ---- Emacs Variables ----
 * Local Variables:
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
