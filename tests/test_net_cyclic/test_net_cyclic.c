/*
 * Copyright (c) 2015-2019 Contributors as noted in the AUTHORS file
 *
 * This file is part of Solo5, a sandboxed execution environment.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "solo5.h"
#include "../../bindings/lib.c"

static void puts(const char *s)
{
    solo5_console_write(s, strlen(s));
}

static void xputs(int ifindex, const char *s)
{
    char which[] = "[serviceX] ";

    which[8] = '0' + ifindex;
    puts(which);
    puts(s);
}

#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806
#define HLEN_ETHER  6
#define PLEN_IPV4  4

struct ether {
    uint8_t target[HLEN_ETHER];
    uint8_t source[HLEN_ETHER];
    uint16_t type;
};

struct arp {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t op;
    uint8_t sha[HLEN_ETHER];
    uint8_t spa[PLEN_IPV4];
    uint8_t tha[HLEN_ETHER];
    uint8_t tpa[PLEN_IPV4];
};

struct ip {
    uint8_t version_ihl;
    uint8_t type;
    uint16_t length;
    uint16_t id;
    uint16_t flags_offset;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint8_t src_ip[PLEN_IPV4];
    uint8_t dst_ip[PLEN_IPV4];
};

struct ping {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seqnum;
    uint8_t data[0];
};

struct arppkt {
    struct ether ether;
    struct arp arp;
};

struct pingpkt {
    struct ether ether;
    struct ip ip;
    struct ping ping;
};

struct udppkt {
    struct ether ether;
    struct ip ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
    uint8_t data[0];
};

/* Copied from https://tools.ietf.org/html/rfc1071 */
static uint16_t checksum(uint16_t *addr, size_t count)
{
    /* Compute Internet Checksum for "count" bytes
     * beginning at location "addr".*/
    register long sum = 0;

    while (count > 1)  {
        /*  This is the inner loop */
        sum += * (unsigned short *) addr++;
        count -= 2;
    }

    /* Add left-over byte, if any */
    if (count > 0)
        sum += * (unsigned char *) addr;

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);

    return ~sum;
}

static uint16_t htons(uint16_t x)
{
    return (x << 8) + (x >> 8);
}

static uint16_t udp_checksum(const struct ip *ip, const struct udppkt *udp,
        uint16_t udp_len)
{
    struct {
        uint8_t src_ip[PLEN_IPV4];
        uint8_t dst_ip[PLEN_IPV4];
        uint8_t zero;
        uint8_t proto;
        uint16_t len;
    } pseudo;

    pseudo.zero = 0;
    pseudo.proto = 0x11;
    pseudo.len = udp->length;
    memcpy(pseudo.src_ip, ip->src_ip, PLEN_IPV4);
    memcpy(pseudo.dst_ip, ip->dst_ip, PLEN_IPV4);

    uint8_t buf[sizeof(pseudo) + udp_len];
    memcpy(buf, &pseudo, sizeof(pseudo));
    memcpy(buf + sizeof(pseudo), &udp->src_port, udp_len);

    uint16_t sum = checksum((uint16_t *)buf, sizeof(pseudo) + udp_len);
    return (sum == 0) ? 0xffff : sum;
}

static bool parse_u64(const char *s, uint64_t *out)
{
    uint64_t value = 0;

    if (*s == '\0')
        return false;

    while (*s) {
        if (*s < '0' || *s > '9')
            return false;
        uint64_t next = value * 10 + (uint64_t)(*s - '0');
        if (next < value)
            return false;
        value = next;
        s++;
    }

    *out = value;
    return true;
}

static void write_u64_be(uint8_t *dst, uint64_t value)
{
    for (int i = 0; i < 8; i++)
        dst[i] = (uint8_t)(value >> (56 - (i * 8)));
}

static void tohexs(char *dst, uint8_t *src, size_t size)
{
    while (size--) {
        uint8_t n = *src >> 4;
        *dst++ = (n < 10) ? (n + '0') : (n - 10 + 'a');
        n = *src & 0xf;
        *dst++ = (n < 10) ? (n + '0') : (n - 10 + 'a');
        src++;
    }
    *dst = '\0';
}

struct netif {
    uint8_t ipaddr[4];
    uint8_t ipaddr_brdnet[4];
    solo5_handle_t h;
    struct solo5_net_info info;
};

struct netif ni[] = {
    {
        .ipaddr = { 0x0a, 0x00, 0x00, 0x03 }, /* 10.0.0.3 */
        .ipaddr_brdnet = { 0x0a, 0x00, 0x00, 0xff } /* 10.0.0.255 */
    },
#ifdef TWO_INTERFACES
    {
        .ipaddr = { 0x0a, 0x01, 0x00, 0x02 }, /* 10.1.0.2 */
        .ipaddr_brdnet = { 0x0a, 0x01, 0x00, 0xff } /* 10.1.0.255 */
    }
#endif
};

uint8_t ipaddr_brdall[4] = { 0xff, 0xff, 0xff, 0xff }; /* 255.255.255.255 */
uint8_t macaddr_brd[HLEN_ETHER] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };


void put_uint8_t(uint8_t buffer_len) {
    char str[4]; // Max 255 + null terminator
    int i = 0;

    // Handle 0 explicitly
    if (buffer_len == 0) {
        str[i++] = '0';
    } else {
        uint8_t num = buffer_len;
        char temp[3];  // temporary storage for digits
        int j = 0;

        // Extract digits in reverse
        while (num > 0) {
            temp[j++] = '0' + (num % 10);
            num /= 10;
        }

        // Reverse digits into str
        while (j > 0) {
            str[i++] = temp[--j];
        }
    }

    str[i] = '\0'; // null-terminate
    puts(str);
}

static bool send_udp_packet(int ifindex, const uint8_t *payload,
        size_t payload_len)
{
    uint8_t buf[ni[ifindex].info.mtu + SOLO5_NET_HLEN];
    struct udppkt *p = (struct udppkt *)buf;
    uint16_t udp_len = (uint16_t)(8 + payload_len);
    uint16_t ip_len = (uint16_t)(sizeof(struct ip) + udp_len);
    uint8_t dst_ip[PLEN_IPV4] = { 0x0a, 0x00, 0x00, 0x01 }; /* 10.0.0.1 */

    if (sizeof(*p) + payload_len > sizeof(buf))
        return false;

    memcpy(p->ether.target, macaddr_brd, HLEN_ETHER);
    memcpy(p->ether.source, ni[ifindex].info.mac_address, HLEN_ETHER);
    p->ether.type = htons(ETHERTYPE_IP);

    p->ip.version_ihl = 0x45;
    p->ip.type = 0x00;
    p->ip.length = htons(ip_len);
    p->ip.id = 0;
    p->ip.flags_offset = 0;
    p->ip.ttl = 64;
    p->ip.proto = 0x11;
    p->ip.checksum = 0;
    memcpy(p->ip.src_ip, ni[ifindex].ipaddr, PLEN_IPV4);
    memcpy(p->ip.dst_ip, dst_ip, PLEN_IPV4);
    p->ip.checksum = checksum((uint16_t *) &p->ip, sizeof(struct ip));

    p->src_port = htons(8000);
    p->dst_port = htons(8000);
    p->length = htons(udp_len);
    p->checksum = 0;
    memcpy(p->data, payload, payload_len);
    p->checksum = udp_checksum(&p->ip, p, udp_len);

    if (solo5_net_write(ni[ifindex].h, (uint8_t *)p,
            sizeof(struct udppkt) + payload_len) != SOLO5_R_OK) {
        xputs(ifindex, "Write error\n");
        return false;
    }

    return true;
}

static void send_garp(int ifindex)
{
    struct arppkt p;
    uint8_t zero[HLEN_ETHER] = { 0 };

    /*
     * Send a gratuitous ARP packet announcing our MAC address.
     */
    memcpy(p.ether.source, ni[ifindex].info.mac_address, HLEN_ETHER);
    memcpy(p.ether.target, macaddr_brd, HLEN_ETHER);
    p.ether.type = htons(ETHERTYPE_ARP);
    p.arp.htype = htons(1);
    p.arp.ptype = htons(ETHERTYPE_IP);
    p.arp.hlen = HLEN_ETHER;
    p.arp.plen = PLEN_IPV4;
    p.arp.op = htons(1);
    memcpy(p.arp.sha, ni[ifindex].info.mac_address, HLEN_ETHER);
    memcpy(p.arp.tha, zero, HLEN_ETHER);
    memcpy(p.arp.spa, ni[ifindex].ipaddr, PLEN_IPV4);
    memcpy(p.arp.tpa, ni[ifindex].ipaddr, PLEN_IPV4);

    if (solo5_net_write(ni[ifindex].h, (uint8_t *)&p, sizeof p) != SOLO5_R_OK)
        xputs(ifindex, "Could not send GARP packet\n");
}


static bool cyclic_udp_send(solo5_time_t interval_ns)
{
    if (solo5_net_acquire("service0", &ni[0].h, &ni[0].info) != SOLO5_R_OK) {
        puts("Could not acquire 'service0' network\n");
        return false;
    }

    char macaddr_s[(HLEN_ETHER * 2) + 2];
    tohexs(macaddr_s, ni[0].info.mac_address, HLEN_ETHER);
    xputs(0, "Sending UDP to 10.0.0.1:8000, MAC: ");
    puts(macaddr_s);
    puts("\n");

    send_garp(0);
    solo5_time_t start_time = solo5_clock_monotonic();
    uint64_t cycle = 1;
    for (;;) {
        solo5_time_t planned_wakeup = start_time + (cycle * interval_ns);
        solo5_clock_nanosleep(planned_wakeup);
        solo5_time_t actual_wakeup = solo5_clock_monotonic();
        solo5_time_t before_send = solo5_clock_monotonic();

        uint8_t payload[24];
        write_u64_be(&payload[0], planned_wakeup);
        write_u64_be(&payload[8], actual_wakeup);
        write_u64_be(&payload[16], before_send);

        if (!send_udp_packet(0, payload, sizeof(payload)))
            return false;
        cycle++;
    }
}

int solo5_app_main(const struct solo5_start_info *si)
{
    puts("\n**** Solo5 standalone test_net_cyclic ****\n\n");

    if (solo5_sched_setscheduler(SOLO5_SCHED_FIFO, 90) != SOLO5_R_OK)
        puts("sched_setscheduler not supported\n");

    uint64_t cycle_us = 0;
    if (!parse_u64(si->cmdline, &cycle_us) || cycle_us == 0) {
        puts("Error in command line.\n");
        puts("Usage: test_net_cyclic <cycle_us>\n");
        return SOLO5_EXIT_FAILURE;
    }

    solo5_time_t interval_ns = cycle_us * 1000ULL;

    if (cyclic_udp_send(interval_ns)) {
        puts("SUCCESS\n");
        return SOLO5_EXIT_SUCCESS;
    }
    else {
        puts("FAILURE\n");
        return SOLO5_EXIT_FAILURE;
    }
}
