/**
 * @author Samantha Mallari
 * @date 2025 09 16
 * ICS 651 Project 1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "slipnet.h"
#include "simnet.h"

// IPv6 header structure for parsing
struct ipv6_header {
    uint8_t ver_class_hi; /* version and 4 bits of class, usually 0x60 */
    uint8_t class_lo_flow_hi; /* 4 bits each of class and flow label */
    uint16_t flow_lo;     /* 16 bits of flow label */
    uint16_t length;      /* payload length */
    uint8_t next_header;  /* next header: TCP, UDP, ICMP, or extension */
    uint8_t hop_limit;    /* same as IPv4 TTL */
    uint8_t source[16];  /* source address */
    uint8_t destination[16];  /* destination address */
};

// global array of simulated addresses
static struct in6_addr sim_addrs[MAX_TTYS];
static int num_addrs = 0;

// routing table lock
pthread_mutex_t routing_lock = PTHREAD_MUTEX_INITIALIZER;

// routing table entry structure
struct route_entry {
    struct in6_addr destination;  // Network address (first 64 bits matter)
    int metric;                   // Distance/cost
    int interface_index;          // Which interface to send to
    int is_directly_connected;    // 1 for static routes, 0 for learned
    time_t last_updated;         // For potential route aging
};

// routing protocol packet structure
struct routing_packet {
    struct in6_addr sender_ip;    // 16 bytes
    struct in6_addr num_routes;   // 16 bytes (only last 8 bits used)
    // followed by pairs of destination (16 bytes) + distance (16 bytes)
};

// global routing table
#define MAX_ROUTES 29
static struct route_entry routing_table[MAX_ROUTES];
static int num_routes = 0;

// send locks, one per interface
typedef struct {
    pthread_mutex_t lock;
    int in_use;
} send_lock;
send_lock send_slots[MAX_TTYS];

// send thread args
typedef struct {
    int fd;
    char *data;
    int numbytes;
} send_arg_t;

// called by pthread_create, writes data to SLIP interface, releases slot when done
void *send_thread(void *arg) {
    send_arg_t *s = arg;
    printf("[Send] Sending packet on interface %d\n", s->fd);
    write_slip_data(s->fd, s->data, s->numbytes);

    // release slot
    pthread_mutex_lock(&send_slots[s->fd].lock);
    send_slots[s->fd].in_use = 0;
    pthread_mutex_unlock(&send_slots[s->fd].lock);

    free(s->data);
    free(s);
    return NULL;
}

// send thread helper function, used to queue a send operation on an interface
void queue_send(int fd, const char *data, int numbytes) {
    // check if interface is busy (i.e. a send thread is active)
    pthread_mutex_lock(&send_slots[fd].lock);
    if (send_slots[fd].in_use) {
        printf("Dropping packet on interface %d (busy)\n", fd);
        pthread_mutex_unlock(&send_slots[fd].lock);
        return;
    } // else, mark as busy
    send_slots[fd].in_use = 1;
    pthread_mutex_unlock(&send_slots[fd].lock);

    // copy packet to send
    send_arg_t *arg = malloc(sizeof(send_arg_t));
    arg->fd = fd;
    arg->data = malloc(numbytes);
    memcpy(arg->data, data, numbytes);
    arg->numbytes = numbytes;

    // spawn send thread
    pthread_t send_tid;
    pthread_create(&send_tid, NULL, send_thread, arg);
    pthread_detach(send_tid);
}

// timer thread for periodic routing updates. this thread wakes up every 30 seconds, builds a routing packet, then sends it out all interfaces
void *timer_thread(void *n_ifaces) {
    int num_ifaces = *(int *) n_ifaces;
    while (1) {
        sleep(30);

        pthread_mutex_lock(&routing_lock);
        // TODO: access routing table
        pthread_mutex_unlock(&routing_lock);

        // TODO: build routing announcement packet: IPv6 header + routing packet

        for (int i = 0; i < num_ifaces; i++) {
            // TODO: queue_send(i, &announce_packet);
            printf("[Timer] Sending a routing packet on interface %d\n", i);
        }
    }
    return NULL;
}

// data handler starts receive thread with IP processing and routing, spawns send threads as needed
static void data_handler(int tty, const void *vdata, int numbytes) {
    const char *data = (const char *) vdata;

    // print received packet
    printf("Interface %d, ", tty);
    print_packet("slip received packet", data, numbytes);

    // for now, echo back the received packet
    // queue_send(tty, data, numbytes);

    // ==== IP Processing ====
    // parse IPv6 header from packet
    if (numbytes < (int) sizeof(struct ipv6_header)) {
        printf("Received packet too short for IPv6 header on interface %d, dropping packet\n", tty);
        return;
    }
    struct ipv6_header *ip6 = (struct ipv6_header *) data;

    // get source and destination addresses
    struct in6_addr src_addr, dst_addr;
    memcpy(&src_addr, ip6->source, sizeof(src_addr));
    memcpy(&dst_addr, ip6->destination, sizeof(dst_addr));

    // print source and destination addresses
    char src_str[INET6_ADDRSTRLEN];
    char dst_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &src_addr, src_str, sizeof(src_str));
    inet_ntop(AF_INET6, &dst_addr, dst_str, sizeof(dst_str));
    printf("Parsed IPv6 packet: src=%s, dst=%s\n", src_str, dst_str);

    // check if packet is addressed to this router
    int is_for_router = 0;
    // check against all local interface addresses
    for (int i = 0; i < num_addrs; i++) {
        if (memcmp(ip6->destination, &sim_addrs[i], 16) == 0) {
            printf("Packet matches local interface %d: %s, processing packet\n", i, sim_addrs[i]);
            is_for_router = 1;
            break;
        }
    }

    // check against link-local broadcast address ff02::1
    struct in6_addr broadcast_addr;
    inet_pton(AF_INET6, "ff02::1", &broadcast_addr);
    if (memcmp(ip6->destination, &broadcast_addr, 16) == 0) {
        printf("Packet matches link-local broadcast: (ff02::1), processing packet\n");
        is_for_router = 1;
    }

    // ==== Routing Processing ====
    if (!is_for_router) {
        printf("Packet not for this router, will try forwarding\n");

        // check hop limit before forwarding
        if (ip6->hop_limit <= 1) {
            // drop packet if hop limit would become 0
            printf("Iface %d: Hop limit reached 0, dropping packet from %s\n", tty, src_str);
            return;
        }

        // make a copy of the packet for forwarding
        char *packet_copy = malloc(numbytes);
        if (packet_copy == NULL) {
            printf("Error: Failed to allocate memory for packet copy\n");
            return;
        }
        memcpy(packet_copy, data, numbytes);

        // modify hop limit in the copy
        struct ipv6_header *ip6_copy = (struct ipv6_header *) packet_copy;
        ip6_copy->hop_limit = ip6->hop_limit - 1;

        // TODO: look up routing table to find correct output interface then send packet
        queue_send(tty, packet_copy, numbytes);
        free(packet_copy);
        return;
    }
    else {
        printf("Packet is for this router, processing locally\n");
        if (ip6->next_header != 2) {  // not routing protocol
            printf("Packet IPv6 next_header != 2, dropping packet\n");
        }
        else {
            // Parse packet, IPv6 header followed by routing table 
    }
}

int main(int argc, char *argv[]) {
    // arg check
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IPv6_addr1> <IPv6_addr2> ... <IPv6_addrN>\n", argv[0]);
        return 1;
    }

    // check that there are no more than MAX_TTYS addresses
    num_addrs = argc - 1;
    if (num_addrs > MAX_TTYS) {
        fprintf(stderr, "Error: Maximum number of addresses is %d.\n", MAX_TTYS);
        return 1;
    }

    // check that each address has a corresponding interface in simconfig
    FILE *simconfig = fopen("simconfig", "r");
    if (simconfig == NULL) {
        fprintf(stderr, "Error: Could not open simconfig file. Check that simconfig exists and is in current directory.\n");
        return 1;
    }

    char line[256];
    int num_interfaces = 0;
    while (fgets(line, sizeof(line), simconfig)) {
        if (*line != '\0' && *line != '\n' && line[0] != '#') { // skip empty lines and comments
            num_interfaces++;
        }
    }
    fclose(simconfig);

    if (num_addrs > num_interfaces) {
        fprintf(stderr, "Error: More IPv6 addresses provided than defined in simconfig.\n");
        return 1;
    }

    // parse each address and store in global array
    for (int i = 0; i < num_addrs; i++) {
        struct in6_addr addr;
        if (inet_pton(AF_INET6, argv[i + 1], &addr) != 1) {
            fprintf(stderr, "Error: Invalid IPv6 address '%s'. Input must be in literal form.\n", argv[i + 1]);
            return 1;
        }
        sim_addrs[i] = addr;
    }

    // install SLIP interface for each simulated IP address
    int slip_fds[num_addrs];
    for (int i = 0; i < num_addrs; i++) {
        char addr_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &sim_addrs[i], addr_str, sizeof(addr_str)); // Convert address to string for printing

        // open SLIP interface
        printf("Setting up a SLIP data handler on interface: %s\n", addr_str);
        slip_fds[i] = install_slip_data_handler(i, data_handler);
        if (slip_fds[i] < 0) {
            fprintf(stderr, "Error: Failed to install SLIP data handler on %s\n", addr_str);
            return 1;
        }
        printf("Success: Installed SLIP data handler for interface %s with fd %d\n", addr_str, slip_fds[i]);
    }

    // initialize send locks for each interface
    for (int i = 0; i < num_addrs; i++) {
        pthread_mutex_init(&send_slots[i].lock, NULL);
        send_slots[i].in_use = 0;
    }

    // start timer thread
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, timer_thread, &num_addrs);

    // Keep the router running to receive data
    printf("Router is running. Press Ctrl+C to exit.\n");
    while (1) {
        sleep(1);
    }
    return 0;
}