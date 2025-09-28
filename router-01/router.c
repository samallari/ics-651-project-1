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
#include <time.h>

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
    struct in6_addr gateway;      // Next hop IP address
    uint32_t metric;              // Distance/cost
    time_t timestamp;             // When route was added
    int is_direct;                // 1 for direct routes, 0 for learned
};

// routing protocol packet structure
struct routing_packet_header {
    struct in6_addr sender;
    uint32_t num_routes;
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

// helper function to print the routing table
void print_routing_table() {
    pthread_mutex_lock(&routing_lock);

    printf("\n=== Routing Table ===\n");
    printf("Number of routes: %d\n", num_routes);

    if (num_routes == 0) {
        printf("No routes in table\n");
    }
    else {
        printf("%-25s %-25s %-8s %-6s %-10s\n", "Destination", "Gateway", "Metric", "Type", "Age");
        printf("%-25s %-25s %-8s %-6s %-10s\n", "-------------------------", "-------------------------", "--------", "------", "----------");

        time_t current_time = time(NULL);
        for (int i = 0; i < num_routes; i++) {
            char dest_str[INET6_ADDRSTRLEN];
            char hop_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &routing_table[i].destination, dest_str, sizeof(dest_str));
            inet_ntop(AF_INET6, &routing_table[i].gateway, hop_str, sizeof(hop_str));

            long age = current_time - routing_table[i].timestamp;
            printf("%-25s %-25s %-8u %-6s %-10lds\n",
                dest_str, hop_str, routing_table[i].metric,
                routing_table[i].is_direct ? "Direct" : "Learn", age);
        }
    }
    printf("====================\n\n");

    pthread_mutex_unlock(&routing_lock);
}

// helper function to get the network prefix (first 64 bits) of an IPv6 address
void get_network_prefix(const struct in6_addr *addr, struct in6_addr *prefix) {
    memcpy(prefix, addr, sizeof(*prefix));
    // zero out the last 64 bits (host portion)
    for (int i = 8; i < 16; i++) {
        prefix->s6_addr[i] = 0;
    }
}

// helper function to add or update a route in the routing table
void update_routing_table(const struct in6_addr *dest, const struct in6_addr *gateway, uint32_t metric, int is_direct) {
    pthread_mutex_lock(&routing_lock);

    // get network prefix (first 64 bits) of destination
    struct in6_addr dest_prefix;
    get_network_prefix(dest, &dest_prefix);

    char dest_str[INET6_ADDRSTRLEN];
    char hop_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &dest_prefix, dest_str, sizeof(dest_str));
    inet_ntop(AF_INET6, gateway, hop_str, sizeof(hop_str));

    // search for existing route to same network (compare first 64 bits)
    for (int i = 0; i < num_routes; i++) {
        if (memcmp(&routing_table[i].destination, &dest_prefix, 8) == 0) {
            // found matching network route
            if (metric < routing_table[i].metric) {
                // new route is better, replace it and reset timestamp
                uint32_t old_metric = routing_table[i].metric;
                routing_table[i].destination = dest_prefix;
                routing_table[i].gateway = *gateway;
                routing_table[i].metric = metric;
                routing_table[i].timestamp = time(NULL);  // reset timestamp for better route
                routing_table[i].is_direct = is_direct;
                printf("Updated route to %s via %s with better metric %u (was %u)\n",
                    dest_str, hop_str, metric, old_metric);
            }
            else if (metric == routing_table[i].metric) {
                // same metric, refresh the route but keep timestamp for age tracking
                routing_table[i].destination = dest_prefix;
                routing_table[i].gateway = *gateway;
                routing_table[i].metric = metric;
                // Don't update timestamp - let age continue to increase
                routing_table[i].is_direct = is_direct;
                printf("Refreshed route to %s via %s with same metric %u\n",
                    dest_str, hop_str, metric);
            }
            else {
                printf("Not updating route to %s - existing metric %u is better than %u\n",
                    dest_str, routing_table[i].metric, metric);
            }
            pthread_mutex_unlock(&routing_lock);
            return;
        }
    }

    // no existing route found, add new route if space available
    if (num_routes < MAX_ROUTES) {
        routing_table[num_routes].destination = dest_prefix;
        routing_table[num_routes].gateway = *gateway;
        routing_table[num_routes].metric = metric;
        routing_table[num_routes].timestamp = time(NULL);
        routing_table[num_routes].is_direct = is_direct;
        num_routes++;
        printf("Added new route to %s via %s with metric %u\n", dest_str, hop_str, metric);
    }
    else {
        printf("Routing table full, cannot add route to %s\n", dest_str);
    }
    pthread_mutex_unlock(&routing_lock);
}

// helper function to remove expired routes (older than 100 seconds)
void remove_expired_routes() {
    pthread_mutex_lock(&routing_lock);

    time_t current_time = time(NULL);
    int i = 0;

    while (i < num_routes) {
        // don't remove direct routes
        if (!routing_table[i].is_direct &&
            (current_time - routing_table[i].timestamp) > 100) {

            char dest_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &routing_table[i].destination, dest_str, sizeof(dest_str));
            printf("Removing expired route to %s (age: %ld seconds)\n",
                dest_str, current_time - routing_table[i].timestamp);

         // shift remaining routes down
            for (int j = i; j < num_routes - 1; j++) {
                routing_table[j] = routing_table[j + 1];
            }
            num_routes--;
            // don't increment i since we shifted routes down
        }
        else {
            i++;
        }
    }

    pthread_mutex_unlock(&routing_lock);
}

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

        // remove expired routes first
        remove_expired_routes();

        // build routing announcement packet: IPv6 header + routing packet header + routing table entries
        struct ipv6_header ip6_hdr;
        struct routing_packet_header routing_hdr;

        // copy routing table under lock
        pthread_mutex_lock(&routing_lock);
        struct route_entry entries[num_routes];
        int current_routes = num_routes;
        memcpy(entries, routing_table, num_routes * sizeof(struct route_entry));
        pthread_mutex_unlock(&routing_lock);

        int packet_size = sizeof(ip6_hdr) + sizeof(routing_hdr) + current_routes * sizeof(struct route_entry);

        // fill in IPv6 header fields
        memset(&ip6_hdr, 0, sizeof(ip6_hdr));
        ip6_hdr.ver_class_hi = 6;
        ip6_hdr.class_lo_flow_hi = 0;
        ip6_hdr.flow_lo = 0;
        ip6_hdr.length = htons(packet_size - sizeof(ip6_hdr));
        ip6_hdr.next_header = 2;  // routing protocol
        ip6_hdr.hop_limit = 1;    // limit to local network
        memcpy(ip6_hdr.source, sim_addrs[0].s6_addr, 16);

        // set destination to link-local all-nodes multicast ff02::1
        struct in6_addr dest_addr;
        inet_pton(AF_INET6, "ff02::1", &dest_addr);
        memcpy(ip6_hdr.destination, dest_addr.s6_addr, 16);

        // fill in routing packet header fields
        routing_hdr.num_routes = htonl(current_routes);

        for (int i = 0; i < num_ifaces; i++) {
            // use interface IP as sender
            memcpy(routing_hdr.sender.s6_addr, sim_addrs[i].s6_addr, 16);

            // convert metrics to network byte order for transmission
            struct route_entry network_entries[current_routes];
            for (int j = 0; j < current_routes; j++) {
                network_entries[j].destination = entries[j].destination;
                network_entries[j].gateway = entries[j].gateway;
                network_entries[j].metric = htonl(entries[j].metric);  // convert to network byte order
                network_entries[j].timestamp = entries[j].timestamp;   // timestamps not transmitted
                network_entries[j].is_direct = entries[j].is_direct;   // flags not transmitted
            }

            // assemble full packet
            char *announce_packet = malloc(packet_size);
            memcpy(announce_packet, &ip6_hdr, sizeof(ip6_hdr));
            memcpy(announce_packet + sizeof(ip6_hdr), &routing_hdr, sizeof(routing_hdr));
            memcpy(announce_packet + sizeof(ip6_hdr) + sizeof(routing_hdr), network_entries, current_routes * sizeof(struct route_entry));

            // send packet
            printf("[Timer] Sending a routing packet on interface %d\n", i);
            queue_send(i, announce_packet, packet_size);
            free(announce_packet);
        }
    }
    return NULL;
}

// data handler starts receive thread with IP processing and routing, spawns send threads as needed
static void data_handler(int tty, const void *vdata, int numbytes) {
    const char *data = (const char *) vdata;

    // print received packet
    // printf("Interface %d, ", tty);
    // print_packet("slip received packet", data, numbytes);

    // ==== IP Processing ====
    // parse IPv6 header from packet
    if (numbytes < (int) sizeof(struct ipv6_header)) {
        printf("Received packet too short for IPv6 header, dropping packet\n");
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
    printf("Parsed IPv6 packet - src=%s, dst=%s\n", src_str, dst_str);

    // check if packet is addressed to this router
    int is_for_router = 0;
    // check against all local interface addresses
    for (int i = 0; i < num_addrs; i++) {
        if (memcmp(ip6->destination, &sim_addrs[i], 16) == 0) {
            printf("Packet matches local interface %d: (%s), processing packet\n", i, dst_str);
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
            printf("Hop limit reached 0, dropping packet from %s\n", src_str);
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

        print_packet("Forwarding packet", packet_copy, numbytes);


        // TODO: look up routing table to find correct output interface then send packet
        // for now, just send out the same interface it came in on
        queue_send(tty, packet_copy, numbytes);
        free(packet_copy);
        return;
    }
    else {
        printf("Packet is for this router, processing locally\n");
        if (ip6->next_header != 2) {  // not routing protocol
            printf("[%s]: Packet IPv6 next_header is not 0x02, dropping packet from src=%s\n", dst_str, src_str);
            // printf("Packet next_header: 0x%02x\n", ip6->next_header);
            return;
        }
        else {
            // process routing protocol packet
            printf("Received a routing protocol packet from %s\n", src_str);
            // check that packet meets minimum size: IPV6 header + routing packet header + at least 1 route entry
            if (numbytes < (int) (sizeof(struct ipv6_header) + sizeof(struct routing_packet_header) + sizeof(struct route_entry))) {
                printf("Routing packet too short, dropping packet from %s\n", src_str);
                return;
            }

            // parse routing packet header
            struct routing_packet_header *rp_hdr = (struct routing_packet_header *) (data + sizeof(struct ipv6_header));
            uint32_t num_advertised = ntohl(rp_hdr->num_routes);

            // parse advertised routes
            struct route_entry *advertised_routes = (struct route_entry *) (rp_hdr + 1);

            printf("Processing %u advertised routes from %s\n", num_advertised, src_str);

            // update routing table with each advertised route
            for (uint32_t i = 0; i < num_advertised && i < (numbytes - sizeof(struct ipv6_header) - sizeof(struct routing_packet_header)) / sizeof(struct route_entry); i++) {
                uint32_t new_metric = ntohl(advertised_routes[i].metric) + 1; // increment metric
                update_routing_table(&advertised_routes[i].destination, &src_addr, new_metric, 0);
            }

            // for debugging, print the routing table after processing
            print_routing_table();
            return;
        }
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

    // initialize routing table with directly connected routes before spawning any threads
    printf("Initializing routing table with directly connected routes...\n");
    for (int i = 0; i < num_addrs; i++) {
        struct in6_addr prefix;
        get_network_prefix(&sim_addrs[i], &prefix);

        routing_table[num_routes].destination = prefix;
        routing_table[num_routes].gateway = sim_addrs[i];  // next hop is self for direct routes
        routing_table[num_routes].metric = 0; // directly connected
        routing_table[num_routes].timestamp = time(NULL);
        routing_table[num_routes].is_direct = 1;
        num_routes++;
    }
    print_routing_table();

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