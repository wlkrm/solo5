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

#define PLEN_IPV4  4

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

struct udppkt {
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

static void write_u64_be(uint8_t *dst, uint64_t value)
{
    for (int i = 0; i < 8; i++)
        dst[i] = (uint8_t)(value >> (56 - (i * 8)));
}

static bool update_pong_timestamp(struct udppkt *up, uint16_t udp_len,
        size_t index, uint64_t value)
{
    size_t data_len = udp_len - 8;
    size_t offset = index * 8;

    if (data_len < (4 * 8))
        return false;
    if (offset + 8 > data_len)
        return false;

    write_u64_be(up->data + offset, value);
    up->checksum = 0;
    up->checksum = udp_checksum(&up->ip, up, udp_len);
    return true;
}


struct netif {
    uint8_t ipaddr[4];
    uint8_t ipaddr_brdnet[4];
    solo5_handle_t h;
    struct solo5_net_info info;
};

struct netif ni[] = {
    {
        .ipaddr = { 0x0a, 0x00, 0x00, 0x02 }, /* 10.0.0.2 */
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

static unsigned long n_pings_received = 0;
static bool opt_verbose = false;
static bool opt_limit = false;


static bool handle_ip(int ifindex, uint8_t *buf, uint8_t *data_buf, uint8_t *buffer_len, bool *was_udp)
{
    struct udppkt *p = (struct udppkt *)buf;

    if (p->ip.version_ihl != 0x45)
        return false; /* we don't support IPv6, yet :-) */

    if (p->ip.type != 0x00)
        return false;
   
    switch(p->ip.proto) {        
        case 0x11: /* UDP */
            struct udppkt *up = (struct udppkt *)buf;
            uint16_t udp_len = htons(up->length);

            if (udp_len < 8)
                return false;

            if (memcmp(up->ip.dst_ip, ni[ifindex].ipaddr, PLEN_IPV4) &&
                    memcmp(up->ip.dst_ip, ni[ifindex].ipaddr_brdnet, PLEN_IPV4) &&
                    memcmp(up->ip.dst_ip, ipaddr_brdall, PLEN_IPV4))
                return false; /* not ip addressed to us */

            /* reorder ip net header addresses */
            memcpy(up->ip.dst_ip, up->ip.src_ip, PLEN_IPV4);
            memcpy(up->ip.src_ip, ni[ifindex].ipaddr, PLEN_IPV4);

            /* swap UDP ports */
            uint16_t tmp_port = up->src_port;
            up->src_port = up->dst_port;
            up->dst_port = tmp_port;

            /* recalculate ip checksum for return pkt */
            up->ip.checksum = 0;
            up->ip.checksum = checksum((uint16_t *) &up->ip, sizeof(struct ip));

            /* recalculate UDP checksum (IPv4 optional, but safer to include) */
            up->checksum = 0;
            up->checksum = udp_checksum(&up->ip, up, udp_len);
            update_pong_timestamp(up, udp_len, 1, solo5_clock_monotonic());
            if (opt_verbose) xputs(ifindex, "Received UDP packet, sending reply\n");
            *was_udp = true;
            *buffer_len = udp_len - 8;
            memcpy(data_buf, up->data, *buffer_len);
            return true;

        default:
            return false; /* not supported */

    }


    return true;
}

static const solo5_time_t NSEC_PER_SEC = 1000000000ULL;

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

static bool handle_packet(int ifindex, uint8_t *data_buf, uint8_t *buffer_len,
    bool *was_udp, uint8_t *reply_buf, size_t *reply_len,
    bool *reply_pending)
{
    uint8_t buf[ni[ifindex].info.mtu + SOLO5_NET_HLEN];
    solo5_result_t result;
    size_t len;
    bool handled = false;

    result = solo5_net_read(ni[ifindex].h, buf, sizeof buf, &len);
    if (result != SOLO5_R_OK) {
        xputs(ifindex, "Read error\n");
        return false;
    }

    if (opt_verbose) {
        xputs(ifindex, "Received packet: ");
        put_uint8_t(len);
        puts("\n");
    }

    if (handle_ip(ifindex, buf, data_buf, buffer_len, was_udp)) {
        // if (opt_verbose)
        //     xputs(ifindex, "Received ping, sending reply\n");
        handled = true;
    }

    if (handled) {
        if (*was_udp) {
            memcpy(reply_buf, buf, len);
            *reply_len = len;
            *reply_pending = true;
        }
        else {
            if (solo5_net_write(ni[ifindex].h, buf, len) != SOLO5_R_OK) {
                xputs(ifindex, "Write error\n");
                return false;
            }
        }
    }
    else {
        // xputs(ifindex, "Unknown or unsupported packet, dropped\n");
    }

    return true;
}

static bool ping_serve(void)
{
    if (solo5_net_acquire("service0", &ni[0].h, &ni[0].info) != SOLO5_R_OK) {
        puts("Could not acquire 'service0' network\n");
        return false;
    }
#ifdef TWO_INTERFACES
    if (solo5_net_acquire("service1", &ni[1].h, &ni[1].info) != SOLO5_R_OK) {
        puts("Could not acquire 'service1' network\n");
        return false;
    }
#endif

    xputs(0, "Serving UDP on 10.0.0.2\n");

    uint8_t data_buffer[2048] = { 0 };
    uint8_t buffer_len = 0;
    bool was_udp = false;
    uint8_t reply_buffer[ni[0].info.mtu + SOLO5_NET_HLEN];
    size_t reply_len = 0;
    bool reply_pending = false;

    for (;;) {
        solo5_handle_set_t ready_set = 0;

        was_udp = false;
        reply_pending = false;
        solo5_yield(solo5_clock_monotonic() + NSEC_PER_SEC, &ready_set);
        if (ready_set & 1U << ni[0].h) {
            if (!handle_packet(0, data_buffer, &buffer_len, &was_udp,
                    reply_buffer, &reply_len, &reply_pending)) {
                puts("Error handling packet\n");

                return false;
            } 
            if (was_udp) {
                /* Echo back UDP data on console */
                
                if (opt_verbose) {
                    xputs(0, "UDP data: ");
                    for (int i = 0; i < 1; i++) {
                        puts((char*)&data_buffer[i]);
                    }
                    puts(" (");
                    put_uint8_t(buffer_len);
                    puts(")");
                }
                if (reply_pending) {
                    struct udppkt *up = (struct udppkt *)reply_buffer;
                    uint16_t udp_len = htons(up->length);
                    update_pong_timestamp(up, udp_len, 2, solo5_clock_monotonic());
                    if (solo5_net_write(ni[0].h, reply_buffer, reply_len)
                            != SOLO5_R_OK) {
                        xputs(0, "Write error\n");
                        return false;
                    }
                }
            }
        }

            if (opt_limit && n_pings_received >= 100000) {
                puts("Limit reached, exiting\n");
                break;
            }
        }


    return true;
}

int solo5_app_main(const struct solo5_start_info *si)
{
    puts("\n**** Solo5 standalone test_net ****\n\n");
    if (solo5_sched_setscheduler(SOLO5_SCHED_FIFO, 90) != SOLO5_R_OK)
        puts("sched_setscheduler not supported\n");

    if (strlen(si->cmdline) >= 1) {
        switch (si->cmdline[0]) {
        case 'v':
            opt_verbose = true;
            break;
        case 'l':
            opt_limit = true;
            break;
        default:
            puts("Error in command line.\n");
            puts("Usage: test_net [ verbose | limit ]\n");
            return SOLO5_EXIT_FAILURE;
        }
    }

    if (ping_serve()) {
        puts("SUCCESS\n");
        return SOLO5_EXIT_SUCCESS;
    }
    else {
        puts("FAILURE\n");
        return SOLO5_EXIT_FAILURE;
    }
}
