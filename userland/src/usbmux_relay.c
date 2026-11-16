/*
 * Minimal device-side Apple USBMux (v2) TCP-over-USB relay for /dev/redwood-usb.
 * Relays host usbmuxd TCP-over-USB frames (protocol=0 version, protocol=2 setup,
 * protocol=6 TCP) to 127.0.0.1:<dport> (e.g. lockdownd on port 62078).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MUX_PROTO_VERSION 0
#define MUX_PROTO_SETUP   2
#define MUX_PROTO_TCP     6
#define MUX_MAGIC         0xfeedfaceu

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

struct __attribute__((packed)) mux_hdr {
	uint32_t protocol;
	uint32_t length;
};

struct __attribute__((packed)) mux_v2_hdr {
	uint32_t protocol;
	uint32_t length;
	uint32_t magic;
	uint16_t tx_seq;
	uint16_t rx_seq;
};

struct __attribute__((packed)) mux_ver {
	struct mux_hdr hdr;
	uint32_t major;
	uint32_t minor;
	uint32_t padding;
};

struct __attribute__((packed)) mux_tcp_hdr {
	uint16_t sport;
	uint16_t dport;
	uint32_t seq;
	uint32_t ack;
	uint8_t off;
	uint8_t flags;
	uint16_t win;
	uint16_t csum;
	uint16_t urp;
};

#define MAX_CONNS 32
struct mux_conn {
	int used;
	int fd;
	uint16_t host_port; /* network byte order */
	uint16_t dev_port;  /* network byte order */
	uint32_t seq;       /* host byte order */
	uint32_t ack;       /* host byte order */
};

static struct mux_conn conns[MAX_CONNS];
static int usb_fd = -1;
static uint16_t dev_tx_seq = 0;
static uint16_t dev_rx_seq = 0;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t usb_wmu = PTHREAD_MUTEX_INITIALIZER;

static int
write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void
usb_write_pkt(const void *buf, size_t len)
{
	pthread_mutex_lock(&usb_wmu);
	(void)write_all(usb_fd, buf, len);
	pthread_mutex_unlock(&usb_wmu);
}

static void
send_tcp(struct mux_conn *c, uint8_t flags, const uint8_t *data, size_t len)
{
	uint8_t pkt[sizeof(struct mux_v2_hdr) + sizeof(struct mux_tcp_hdr) + 1536];
	if (len > 1536) len = 1536;

	struct mux_v2_hdr *mh = (struct mux_v2_hdr *)pkt;
	struct mux_tcp_hdr *th = (struct mux_tcp_hdr *)(pkt + sizeof(*mh));
	uint32_t total = (uint32_t)(sizeof(*mh) + sizeof(*th) + len);

	mh->protocol = htonl(MUX_PROTO_TCP);
	mh->length = htonl(total);
	mh->magic = htonl(MUX_MAGIC);
	mh->tx_seq = htons(dev_tx_seq++);
	mh->rx_seq = htons(dev_rx_seq);
	memset(th, 0, sizeof(*th));
	th->sport = c->dev_port;
	th->dport = c->host_port;
	th->seq = htonl(c->seq);
	th->ack = htonl(c->ack);
	th->off = (uint8_t)((sizeof(*th) / 4) << 4);
	th->flags = flags;
	th->win = htons(65535);
	if (len > 0) {
		memcpy(pkt + sizeof(*mh) + sizeof(*th), data, len);
	}
	if (flags & (TH_SYN | TH_FIN)) c->seq += 1;
	c->seq += (uint32_t)len;
	usb_write_pkt(pkt, total);
}

static struct mux_conn *
find_conn(uint16_t host_port, uint16_t dev_port)
{
	for (int i = 0; i < MAX_CONNS; i++) {
		if (conns[i].used && conns[i].host_port == host_port && conns[i].dev_port == dev_port) {
			return &conns[i];
		}
	}
	return NULL;
}

static void
close_conn(struct mux_conn *c)
{
	if (c->fd >= 0) close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

static void
handle_usb_pkt(const uint8_t *buf, size_t len)
{
	if (len < sizeof(struct mux_hdr)) return;
	const struct mux_hdr *mh = (const struct mux_hdr *)buf;
	uint32_t proto = ntohl(mh->protocol);

	if (proto == MUX_PROTO_VERSION) {
		pthread_mutex_lock(&mu);
		for (int i = 0; i < MAX_CONNS; i++) {
			if (conns[i].used) close_conn(&conns[i]);
		}
		dev_tx_seq = 0;
		dev_rx_seq = 0;
		pthread_mutex_unlock(&mu);
		struct mux_ver rep;
		memset(&rep, 0, sizeof(rep));
		rep.hdr.protocol = htonl(MUX_PROTO_VERSION);
		rep.hdr.length = htonl(sizeof(rep));
		rep.major = htonl(2);
		rep.minor = htonl(0);
		usb_write_pkt(&rep, sizeof(rep));
		return;
	}
	if (len < sizeof(struct mux_v2_hdr)) return;

	const struct mux_v2_hdr *v2h = (const struct mux_v2_hdr *)buf;
	dev_rx_seq = ntohs(v2h->tx_seq);
	if (proto == MUX_PROTO_SETUP) return;
	if (proto != MUX_PROTO_TCP || len < sizeof(struct mux_v2_hdr) + sizeof(struct mux_tcp_hdr)) {
		return;
	}

	const struct mux_tcp_hdr *th = (const struct mux_tcp_hdr *)(buf + sizeof(*v2h));
	size_t hdr_bytes = (size_t)((th->off >> 4) & 0x0f) * 4;
	if (hdr_bytes < sizeof(*th) || sizeof(*v2h) + hdr_bytes > len) return;

	const uint8_t *payload = buf + sizeof(*v2h) + hdr_bytes;
	size_t plen = len - (sizeof(*v2h) + hdr_bytes);
	uint32_t h_seq = ntohl(th->seq);

	pthread_mutex_lock(&mu);
	struct mux_conn *c = find_conn(th->sport, th->dport);
	if (th->flags & TH_RST) {
		if (c) close_conn(c);
		pthread_mutex_unlock(&mu);
		return;
	}
	if ((th->flags & TH_SYN) && !(th->flags & TH_ACK)) {
		if (c) close_conn(c);
		for (int i = 0; i < MAX_CONNS; i++) {
			if (!conns[i].used) { c = &conns[i]; break; }
		}
		if (!c) {
			pthread_mutex_unlock(&mu);
			return;
		}
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sin.sin_port = th->dport;
		if (s < 0 || connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
			if (s >= 0) close(s);
			struct mux_conn tmp = { .host_port = th->sport, .dev_port = th->dport, .seq = 0, .ack = h_seq + 1 };
			send_tcp(&tmp, TH_RST | TH_ACK, NULL, 0);
			pthread_mutex_unlock(&mu);
			return;
		}
		int one = 1;
		(void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		c->used = 1;
		c->fd = s;
		c->host_port = th->sport;
		c->dev_port = th->dport;
		c->seq = 0;
		c->ack = h_seq + 1;
		send_tcp(c, TH_SYN | TH_ACK, NULL, 0);
		pthread_mutex_unlock(&mu);
		return;
	}
	if (!c) {
		pthread_mutex_unlock(&mu);
		return;
	}
	if (plen > 0) {
		c->ack = h_seq + (uint32_t)plen;
		if (write_all(c->fd, payload, plen) != 0) {
			send_tcp(c, TH_RST | TH_ACK, NULL, 0);
			close_conn(c);
			pthread_mutex_unlock(&mu);
			return;
		}
		send_tcp(c, TH_ACK, NULL, 0);
	}
	if (th->flags & TH_FIN) {
		c->ack = h_seq + (uint32_t)plen + 1;
		send_tcp(c, TH_FIN | TH_ACK, NULL, 0);
		close_conn(c);
	}
	pthread_mutex_unlock(&mu);
}

static void *
usb_reader_thread(void *arg)
{
	(void)arg;
	uint8_t rxbuf[65536];
	size_t rxlen = 0;

	for (;;) {
		ssize_t n = read(usb_fd, rxbuf + rxlen, sizeof(rxbuf) - rxlen);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			usleep(10000);
			continue;
		}
		rxlen += (size_t)n;
		while (rxlen >= sizeof(struct mux_hdr)) {
			struct mux_hdr *mh = (struct mux_hdr *)rxbuf;
			uint32_t pkt_len = ntohl(mh->length);
			if (pkt_len < sizeof(struct mux_hdr) || pkt_len > sizeof(rxbuf)) {
				rxlen = 0;
				break;
			}
			if (rxlen < pkt_len) break;
			handle_usb_pkt(rxbuf, pkt_len);
			rxlen -= pkt_len;
			if (rxlen > 0) memmove(rxbuf, rxbuf + pkt_len, rxlen);
		}
	}
	return NULL;
}

static void
wait_for_lockdownd(void)
{
	static const char qxml[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<plist version=\"1.0\"><dict><key>Request</key><string>QueryType</string></dict></plist>\n";
	uint8_t req[4 + sizeof(qxml) - 1];
	uint32_t qlen = htonl((uint32_t)(sizeof(qxml) - 1));
	memcpy(req, &qlen, 4);
	memcpy(req + 4, qxml, sizeof(qxml) - 1);

	for (int tries = 0; tries < 120; tries++) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s >= 0) {
			struct sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			sin.sin_port = htons(62078);
			struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
			(void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) == 0 &&
			    write_all(s, req, sizeof(req)) == 0) {
				uint8_t hdr[4];
				if (read(s, hdr, sizeof(hdr)) == 4) {
					close(s);
					return;
				}
			}
			close(s);
		}
		usleep(500000);
	}
}

int
main(void)
{
	for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = -1;
	wait_for_lockdownd();
	usb_fd = open("/dev/redwood-usb", O_RDWR);
	if (usb_fd < 0) {
		perror("open /dev/redwood-usb");
		return 1;
	}

	pthread_t th;
	if (pthread_create(&th, NULL, usb_reader_thread, NULL) != 0) {
		perror("pthread_create");
		return 1;
	}
	pthread_detach(th);

	for (;;) {
		struct pollfd pfds[MAX_CONNS];
		int idx_map[MAX_CONNS];
		nfds_t nfds = 0;

		pthread_mutex_lock(&mu);
		for (int i = 0; i < MAX_CONNS; i++) {
			if (conns[i].used && conns[i].fd >= 0) {
				pfds[nfds].fd = conns[i].fd;
				pfds[nfds].events = POLLIN;
				idx_map[nfds++] = i;
			}
		}
		pthread_mutex_unlock(&mu);

		if (nfds == 0) {
			usleep(20000);
			continue;
		}
		int prc = poll(pfds, nfds, 20);
		if (prc < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (prc == 0) continue;
		for (nfds_t p = 0; p < nfds; p++) {
			if (!(pfds[p].revents & (POLLIN | POLLHUP | POLLERR))) continue;
			pthread_mutex_lock(&mu);
			struct mux_conn *c = &conns[idx_map[p]];
			if (!c->used || c->fd != pfds[p].fd) {
				pthread_mutex_unlock(&mu);
				continue;
			}
			uint8_t tbuf[1536];
			ssize_t n = read(c->fd, tbuf, sizeof(tbuf));
			if (n > 0) {
				send_tcp(c, TH_ACK, tbuf, (size_t)n);
			} else {
				send_tcp(c, TH_FIN | TH_ACK, NULL, 0);
				close_conn(c);
			}
			pthread_mutex_unlock(&mu);
		}
	}
	return 0;
}
