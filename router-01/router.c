/**
 * @author Samantha Mallari
 * @date 2025 09 16
 * ICS 651 Project 1
 * IPv6 Distance Vector Router Implementation
 */

// ============================================================================
// INCLUDES AND HEADERS
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "slipnet.h"
#include "simnet.h"

// ============================================================================
// DATA STRUCTURES
// ============================================================================

// IPv6 header structure for parsing
struct ipv6_header {
    uint8_t ver_class_hi;        /* version and 4 bits of class, usually 0x60 */
    uint8_t class_lo_flow_hi;    /* 4 bits each of class and flow label */
    uint16_t flow_lo;            /* 16 bits of flow label */
    uint16_t length;             /* payload length */
    uint8_t next_header;         /* next header: TCP, UDP, ICMP, or extension */
    uint8_t hop_limit;           /* same as IPv4 TTL */
    uint8_t source[16];          /* source address */
    uint8_t destination[16];     /* destination address */
};

// Routing table entry structure
struct route_entry {
    struct in6_addr destination; /* Network address (first 64 bits matter) */
    struct in6_addr gateway;     /* Next hop IP address */
    uint32_t metric;             /* Distance/cost */
    time_t timestamp;            /* When route was added */
    int is_direct;               /* 1 for direct routes, 0 for learned */
};

// Routing protocol packet structure
struct routing_packet_header {
    struct in6_addr sender;      /* Sender's IP address */
    uint32_t num_routes;         /* Number of routes following */
    /* followed by routing table entries */
};

// Send thread arguments
typedef struct {
    int fd;                      /* File descriptor */
    char *data;                  /* Packet data */
    int numbytes;                /* Data length */
} send_arg_t;

// Send lock structure for interface management
typedef struct {
    pthread_mutex_t lock;        /* Interface lock */
    int in_use;                  /* Interface busy flag */
} send_lock_t;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

// Configuration
static struct in6_addr sim_addrs[MAX_TTYS];  /* Local interface addresses */
static int num_addrs = 0;                    /* Number of interfaces */

// Routing table
#define MAX_ROUTES 29
static struct route_entry routing_table[MAX_ROUTES];
static int num_routes = 0;
pthread_mutex_t routing_lock = PTHREAD_MUTEX_INITIALIZER;

// Interface management
send_lock_t send_slots[MAX_TTYS];

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Extract network prefix (first 64 bits) from IPv6 address
 */
void get_network_prefix(const struct in6_addr *addr, struct in6_addr *prefix) {
    memcpy(prefix, addr, sizeof(*prefix));
    // Zero out the last 64 bits (host portion)
    for (int i = 8; i < 16; i++) {
        prefix->s6_addr[i] = 0;
    }
}

/**
 * Check if packet destination matches any local interface or broadcast
 */
int is_packet_for_router(const struct ipv6_header *ip6) {
    // Check against all local interface addresses
    for (int i = 0; i < num_addrs; i++) {
        if (memcmp(ip6->destination, &sim_addrs[i], 16) == 0) {
            return 1;
        }
    }
    
    // Check against link-local broadcast address ff02::1
    struct in6_addr broadcast_addr;
    inet_pton(AF_INET6, "ff02::1", &broadcast_addr);
    if (memcmp(ip6->destination, &broadcast_addr, 16) == 0) {
        return 1;
    }
    
    return 0;
}

// ============================================================================
// ROUTING TABLE MANAGEMENT
// ============================================================================

/**
 * Print the current routing table
 */
void print_routing_table() {
    pthread_mutex_lock(&routing_lock);

    printf("\n=== Routing Table ===\n");
    printf("Number of routes: %d\n", num_routes);

    if (num_routes == 0) {
        printf("No routes in table\n");
    } else {
        printf("%-25s %-25s %-8s %-6s %-10s\n", 
               "Destination", "Gateway", "Metric", "Type", "Age");
        printf("%-25s %-25s %-8s %-6s %-10s\n", 
               "-------------------------", "-------------------------", 
               "--------", "------", "----------");

        time_t current_time = time(NULL);
        for (int i = 0; i < num_routes; i++) {
            char dest_str[INET6_ADDRSTRLEN];
            char gateway_str[INET6_ADDRSTRLEN];
            
            inet_ntop(AF_INET6, &routing_table[i].destination, dest_str, sizeof(dest_str));
            inet_ntop(AF_INET6, &routing_table[i].gateway, gateway_str, sizeof(gateway_str));
            
            long age = current_time - routing_table[i].timestamp;
            printf("%-25s %-25s %-8u %-6s %-10lds\n", 
                   dest_str, gateway_str, routing_table[i].metric,
                   routing_table[i].is_direct ? "Direct" : "Learn", age);
        }
    }
    printf("====================\n\n");

    pthread_mutex_unlock(&routing_lock);
}

/**
 * Add or update a route in the routing table
 */
void update_routing_table(const struct in6_addr *dest, const struct in6_addr *gateway, 
                         uint32_t metric, int is_direct) {
    pthread_mutex_lock(&routing_lock);
    
    // Get network prefix (first 64 bits) of destination
    struct in6_addr dest_prefix;
    get_network_prefix(dest, &dest_prefix);
    
    char dest_str[INET6_ADDRSTRLEN];
    char gateway_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &dest_prefix, dest_str, sizeof(dest_str));
    inet_ntop(AF_INET6, gateway, gateway_str, sizeof(gateway_str));
    
    // Search for existing route to same network (compare first 64 bits)
    for (int i = 0; i < num_routes; i++) {
        if (memcmp(&routing_table[i].destination, &dest_prefix, 8) == 0) {
            // Found matching network route
            if (metric < routing_table[i].metric) {
                // New route is better, replace it and reset timestamp
                uint32_t old_metric = routing_table[i].metric;
                routing_table[i].destination = dest_prefix;
                routing_table[i].gateway = *gateway;
                routing_table[i].metric = metric;
                routing_table[i].timestamp = time(NULL);
                routing_table[i].is_direct = is_direct;
                printf("Updated route to %s via %s with better metric %u (was %u)\n",
                       dest_str, gateway_str, metric, old_metric);
            } else if (metric == routing_table[i].metric) {
                // Same metric, refresh the route but keep timestamp for age tracking
                routing_table[i].destination = dest_prefix;
                routing_table[i].gateway = *gateway;
                routing_table[i].metric = metric;
                routing_table[i].is_direct = is_direct;
                printf("Refreshed route to %s via %s with same metric %u\n",
                       dest_str, gateway_str, metric);
            } else {
                printf("Not updating route to %s - existing metric %u is better than %u\n",
                       dest_str, routing_table[i].metric, metric);
            }
            pthread_mutex_unlock(&routing_lock);
            return;
        }
    }
    
    // No existing route found, add new route if space available
    if (num_routes < MAX_ROUTES) {
        routing_table[num_routes].destination = dest_prefix;
        routing_table[num_routes].gateway = *gateway;
        routing_table[num_routes].metric = metric;
        routing_table[num_routes].timestamp = time(NULL);
        routing_table[num_routes].is_direct = is_direct;
        num_routes++;
        printf("Added new route to %s via %s with metric %u\n", 
               dest_str, gateway_str, metric);
    } else {
        printf("Routing table full, cannot add route to %s\n", dest_str);
    }
    
    pthread_mutex_unlock(&routing_lock);
}

/**
 * Remove expired routes (older than 100 seconds)
 */
void remove_expired_routes() {
    pthread_mutex_lock(&routing_lock);
    
    time_t current_time = time(NULL);
    int i = 0;
    
    while (i < num_routes) {
        // Don't remove direct routes
        if (!routing_table[i].is_direct && 
            (current_time - routing_table[i].timestamp) > 100) {
            
            char dest_str[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &routing_table[i].destination, dest_str, sizeof(dest_str));
            printf("Removing expired route to %s (age: %ld seconds)\n", 
                   dest_str, current_time - routing_table[i].timestamp);
            
            // Shift remaining routes down
            for (int j = i; j < num_routes - 1; j++) {
                routing_table[j] = routing_table[j + 1];
            }
            num_routes--;
        } else {
            i++;
        }
    }
    
    pthread_mutex_unlock(&routing_lock);
}

// ============================================================================
// NETWORK PACKET HANDLING
// ============================================================================

/**
 * Send thread function - writes data to SLIP interface
 */
void *send_thread(void *arg) {
    send_arg_t *s = arg;
    printf("[Send] Sending packet on interface %d\n", s->fd);
    write_slip_data(s->fd, s->data, s->numbytes);

    // Release interface slot
    pthread_mutex_lock(&send_slots[s->fd].lock);
    send_slots[s->fd].in_use = 0;
    pthread_mutex_unlock(&send_slots[s->fd].lock);

    free(s->data);
    free(s);
    return NULL;
}

/**
 * Queue a send operation on an interface
 */
void queue_send(int fd, const char *data, int numbytes) {
    // Check if interface is busy
    pthread_mutex_lock(&send_slots[fd].lock);
    if (send_slots[fd].in_use) {
        printf("Dropping packet on interface %d (busy)\n", fd);
        pthread_mutex_unlock(&send_slots[fd].lock);
        return;
    }
    send_slots[fd].in_use = 1;
    pthread_mutex_unlock(&send_slots[fd].lock);

    // Prepare send thread arguments
    send_arg_t *arg = malloc(sizeof(send_arg_t));
    arg->fd = fd;
    arg->data = malloc(numbytes);
    memcpy(arg->data, data, numbytes);
    arg->numbytes = numbytes;

    // Spawn send thread
    pthread_t send_tid;
    pthread_create(&send_tid, NULL, send_thread, arg);
    pthread_detach(send_tid);
}

/**
 * Process received routing protocol packet
 */
void process_routing_packet(const char *data, int numbytes, const struct in6_addr *src_addr) {
    char src_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, src_addr, src_str, sizeof(src_str));
    
    printf("Received a routing protocol packet from %s\n", src_str);
    
    // Validate packet size
    size_t min_size = sizeof(struct ipv6_header) + sizeof(struct routing_packet_header) + sizeof(struct route_entry);
    if (numbytes < (int)min_size) {
        printf("Routing packet too short, dropping packet from %s\n", src_str);
        return;
    }

    // Parse routing packet header
    struct routing_packet_header *rp_hdr = (struct routing_packet_header *)(data + sizeof(struct ipv6_header));
    uint32_t num_advertised = ntohl(rp_hdr->num_routes);
    
    // Parse advertised routes
    struct route_entry *advertised_routes = (struct route_entry *)(rp_hdr + 1);
    
    printf("Processing %u advertised routes from %s\n", num_advertised, src_str);
    
    // Process each advertised route
    size_t max_routes = (numbytes - sizeof(struct ipv6_header) - sizeof(struct routing_packet_header)) / sizeof(struct route_entry);
    for (uint32_t i = 0; i < num_advertised && i < max_routes; i++) {
        uint32_t new_metric = ntohl(advertised_routes[i].metric) + 1; // Increment metric
        update_routing_table(&advertised_routes[i].destination, src_addr, new_metric, 0);
    }
}

/**
 * Forward packet to next hop
 */
void forward_packet(const char *data, int numbytes, int input_interface, const struct ipv6_header *ip6) {
    char src_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, ip6->source, src_str, sizeof(src_str));
    
    printf("Packet not for this router, attempting to forward\n");

    // Check hop limit
    if (ip6->hop_limit <= 1) {
        printf("Hop limit reached 0, dropping packet from %s\n", src_str);
        return;
    }

    // Make a copy of the packet for forwarding
    char *packet_copy = malloc(numbytes);
    if (packet_copy == NULL) {
        printf("Error: Failed to allocate memory for packet copy\n");
        return;
    }
    memcpy(packet_copy, data, numbytes);

    // Modify hop limit in the copy
    struct ipv6_header *ip6_copy = (struct ipv6_header *)packet_copy;
    ip6_copy->hop_limit = ip6->hop_limit - 1;

    printf("Forwarding packet with decremented hop limit\n");
    print_packet("Forwarding packet", packet_copy, numbytes);

    // TODO: Implement proper routing table lookup for output interface
    // For now, just send back out the same interface
    queue_send(input_interface, packet_copy, numbytes);
    free(packet_copy);
}

/**
 * Main packet processing function - handles all incoming packets
 */
static void data_handler(int tty, const void *vdata, int numbytes) {
    const char *data = (const char *)vdata;

    // Validate IPv6 header size
    if (numbytes < (int)sizeof(struct ipv6_header)) {
        printf("Received packet too short for IPv6 header, dropping packet\n");
        return;
    }

    struct ipv6_header *ip6 = (struct ipv6_header *)data;

    // Extract source and destination addresses
    struct in6_addr src_addr, dst_addr;
    memcpy(&src_addr, ip6->source, sizeof(src_addr));
    memcpy(&dst_addr, ip6->destination, sizeof(dst_addr));

    // Print packet information
    char src_str[INET6_ADDRSTRLEN];
    char dst_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &src_addr, src_str, sizeof(src_str));
    inet_ntop(AF_INET6, &dst_addr, dst_str, sizeof(dst_str));
    printf("Parsed IPv6 packet - src=%s, dst=%s\n", src_str, dst_str);

    // Check if packet is for this router
    if (is_packet_for_router(ip6)) {
        printf("Packet is for this router, processing locally\n");
        
        if (ip6->next_header == 2) {
            // Routing protocol packet
            process_routing_packet(data, numbytes, &src_addr);
        } else {
            // Other protocol - just acknowledge receipt
            printf("[%s]: Packet is not a routing packet, dropping packet from src=%s\n", 
                   dst_str, src_str);
        }
    } else {
        // Forward packet
        forward_packet(data, numbytes, tty, ip6);
    }
}

// ============================================================================
// ROUTING PROTOCOL TIMER
// ============================================================================

/**
 * Timer thread for periodic routing updates
 */
void *timer_thread(void *n_ifaces) {
    int num_ifaces = *(int *)n_ifaces;
    
    while (1) {
        sleep(30);

        // Remove expired routes first
        remove_expired_routes();

        // Copy routing table under lock
        pthread_mutex_lock(&routing_lock);
        struct route_entry entries[num_routes];
        int current_routes = num_routes;
        memcpy(entries, routing_table, num_routes * sizeof(struct route_entry));
        pthread_mutex_unlock(&routing_lock);

        // Calculate packet size
        int packet_size = sizeof(struct ipv6_header) + sizeof(struct routing_packet_header) + 
                         current_routes * sizeof(struct route_entry);

        // Build IPv6 header
        struct ipv6_header ip6_hdr;
        memset(&ip6_hdr, 0, sizeof(ip6_hdr));
        ip6_hdr.ver_class_hi = 0x60;
        ip6_hdr.class_lo_flow_hi = 0;
        ip6_hdr.flow_lo = htons(0);
        ip6_hdr.length = htons(packet_size - sizeof(ip6_hdr));
        ip6_hdr.next_header = 2;  // Routing protocol
        ip6_hdr.hop_limit = 1;    // Send only to neighbors

        // Set destination to link-local broadcast IP ff02::1
        struct in6_addr dest_addr;
        inet_pton(AF_INET6, "ff02::1", &dest_addr);
        memcpy(ip6_hdr.destination, dest_addr.s6_addr, 16);

        // Build and send routing announcements for each interface
        for (int i = 0; i < num_ifaces; i++) {
            // Set source IP address to interface address
            memcpy(ip6_hdr.source, sim_addrs[i].s6_addr, 16);

            // Build routing packet header
            struct routing_packet_header routing_hdr;
            memcpy(routing_hdr.sender.s6_addr, sim_addrs[i].s6_addr, 16);
            routing_hdr.num_routes = htonl(current_routes);

            // Convert metrics to network byte order for transmission
            struct route_entry network_entries[current_routes];
            for (int j = 0; j < current_routes; j++) {
                network_entries[j].destination = entries[j].destination;
                network_entries[j].gateway = entries[j].gateway;
                network_entries[j].metric = htonl(entries[j].metric);
                network_entries[j].timestamp = entries[j].timestamp;
                network_entries[j].is_direct = entries[j].is_direct;
            }

            // Assemble complete packet
            char *announce_packet = malloc(packet_size);
            memcpy(announce_packet, &ip6_hdr, sizeof(ip6_hdr)); // IPv6 header
            memcpy(announce_packet + sizeof(ip6_hdr), &routing_hdr, sizeof(routing_hdr)); // Routing header
            memcpy(announce_packet + sizeof(ip6_hdr) + sizeof(routing_hdr), 
                   network_entries, current_routes * sizeof(struct route_entry)); // Routing entries

            // Send announcement
            printf("[Timer] Sending a routing packet on interface %d\n", i);
            queue_send(i, announce_packet, packet_size);
            free(announce_packet);
        }
    }
    return NULL;
}

// ============================================================================
// INITIALIZATION AND MAIN
// ============================================================================

/**
 * Initialize routing table with directly connected routes
 */
void initialize_routing_table() {
    printf("Initializing routing table with directly connected routes...\n");
    
    for (int i = 0; i < num_addrs; i++) {
        struct in6_addr prefix;
        get_network_prefix(&sim_addrs[i], &prefix);
        
        routing_table[num_routes].destination = prefix;
        routing_table[num_routes].gateway = sim_addrs[i];  // Gateway is self for direct routes
        routing_table[num_routes].metric = 0;             // Direct routes have metric 0
        routing_table[num_routes].timestamp = time(NULL);
        routing_table[num_routes].is_direct = 1;          // Mark as direct route
        num_routes++;
    }
    
    print_routing_table();
}

/**
 * Initialize send locks for all interfaces
 */
void initialize_send_locks() {
    for (int i = 0; i < num_addrs; i++) {
        pthread_mutex_init(&send_slots[i].lock, NULL);
        send_slots[i].in_use = 0;
    }
}

/**
 * Main function - entry point
 */
int main(int argc, char *argv[]) {
    // Validate command line arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IPv6_addr1> <IPv6_addr2> ... <IPv6_addrN>\n", argv[0]);
        return 1;
    }

    // Check maximum number of addresses
    num_addrs = argc - 1;
    if (num_addrs > MAX_TTYS) {
        fprintf(stderr, "Error: Maximum number of addresses is %d.\n", MAX_TTYS);
        return 1;
    }

    // Validate simconfig file
    FILE *simconfig = fopen("simconfig", "r");
    if (simconfig == NULL) {
        fprintf(stderr, "Error: Could not open simconfig file.\n");
        return 1;
    }

    // Count interfaces in simconfig
    char line[256];
    int num_interfaces = 0;
    while (fgets(line, sizeof(line), simconfig)) {
        if (*line != '\0' && *line != '\n' && line[0] != '#') {
            num_interfaces++;
        }
    }
    fclose(simconfig);

    if (num_addrs > num_interfaces) {
        fprintf(stderr, "Error: More IPv6 addresses provided than defined in simconfig.\n");
        return 1;
    }

    // Parse and validate IPv6 addresses
    for (int i = 0; i < num_addrs; i++) {
        if (inet_pton(AF_INET6, argv[i + 1], &sim_addrs[i]) != 1) {
            fprintf(stderr, "Error: Invalid IPv6 address '%s'.\n", argv[i + 1]);
            return 1;
        }
    }

    // Initialize routing table
    initialize_routing_table();

    // Install SLIP data handlers
    int slip_fds[num_addrs];
    for (int i = 0; i < num_addrs; i++) {
        char addr_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &sim_addrs[i], addr_str, sizeof(addr_str));

        printf("Setting up SLIP data handler on interface: %s\n", addr_str);
        slip_fds[i] = install_slip_data_handler(i, data_handler);
        if (slip_fds[i] < 0) {
            fprintf(stderr, "Error: Failed to install SLIP data handler on %s\n", addr_str);
            return 1;
        }
        printf("Success: Installed SLIP data handler for interface %s with fd %d\n", 
               addr_str, slip_fds[i]);
    }

    // Initialize interface locks
    initialize_send_locks();

    // Start timer thread for routing updates
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, timer_thread, &num_addrs);

    // Main loop
    printf("Router is running. Press Ctrl+C to exit.\n");
    while (1) {
        sleep(1);
    }

    return 0;
}