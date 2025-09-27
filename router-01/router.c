/**
 * @author Samantha Mallari
 * @date Sept. 16 2025
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

// Packet buffer structure for send thread (queue size 1)
struct packet_buffer {
    char data[MAX_SLIP_SIZE];
    int size;
    int tty;
    struct in6_addr dest_addr;
    int is_full;  // Flag to indicate if queue is full
};

// Global variables for send thread management
static pthread_t send_thread[MAX_TTYS];
static int send_thread_active[MAX_TTYS] = {0};
static pthread_mutex_t send_thread_mutex[MAX_TTYS];
static struct packet_buffer send_queue[MAX_TTYS];  // Size 1 queue per tty
static pthread_mutex_t queue_mutex[MAX_TTYS];

// Global variables for timer thread management
static pthread_t timer_thread[MAX_TTYS];
static int timer_thread_active[MAX_TTYS] = {0};
static pthread_mutex_t timer_thread_mutex[MAX_TTYS];
static struct in6_addr local_addresses[MAX_TTYS];  // Store local addresses for routing packets
static int num_local_addresses = 0;

static int threads_initialized = 0;

// IPv6 header structure for parsing
struct ipv6_header {
    uint32_t version_class_label;  // version (4), traffic class (8), flow label (20)
    uint16_t payload_length;
    uint8_t next_header;
    uint8_t hop_limit;
    struct in6_addr src_addr;
    struct in6_addr dest_addr;
};

// Create IPv6 header with specified values
static void create_ipv6_header(struct ipv6_header *hdr,
    uint16_t payload_len,
    uint8_t next_hdr,
    const struct in6_addr *src_addr,
    const struct in6_addr *dest_addr) {
    // IPv6 version (6), traffic class (0), flow label (0)
    hdr->version_class_label = htonl(0x60000000);  // Version 6, TC=0, FL=0
    hdr->payload_length = htons(payload_len);
    hdr->next_header = next_hdr;
    hdr->hop_limit = 1;  // Set hop limit to 1 as requested
    hdr->src_addr = *src_addr;
    hdr->dest_addr = *dest_addr;
}

// Initialize thread mutexes
static void init_thread_mutexes() {
    if (!threads_initialized) {
        for (int i = 0; i < MAX_TTYS; i++) {
            pthread_mutex_init(&send_thread_mutex[i], NULL);
            pthread_mutex_init(&queue_mutex[i], NULL);
            pthread_mutex_init(&timer_thread_mutex[i], NULL);
            send_queue[i].is_full = 0;
            send_queue[i].size = 0;
        }
        threads_initialized = 1;
    }
}

// Send thread function
static void *send_thread_func(void *arg) {
    int tty = *(int *) arg;
    // Keep a local copy of packet data
    struct packet_buffer local_packet;

    printf("Send thread started for tty %d\n", tty);

    // Lock the queue and copy packet data locally
    pthread_mutex_lock(&queue_mutex[tty]);
    if (send_queue[tty].is_full && send_queue[tty].size > 0) {
        memcpy(&local_packet, &send_queue[tty], sizeof(struct packet_buffer));
        // Clear the queue
        send_queue[tty].is_full = 0;
        send_queue[tty].size = 0;
    }
    else {
        local_packet.size = 0;  // No packet to send
    }
    pthread_mutex_unlock(&queue_mutex[tty]);

    // Send packet using local copy (outside of mutex)
    if (local_packet.size > 0) {
        char addr_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &local_packet.dest_addr, addr_str, sizeof(addr_str));
        printf("Sending packet to %s on tty %d\n", addr_str, tty);

        // Send the packet using SLIP
        if (write_slip_data(tty, local_packet.data, local_packet.size) < 0) {
            printf("Error: Failed to send packet on tty %d\n", tty);
        }
    }

    // Mark send thread as inactive and free parameter
    pthread_mutex_lock(&send_thread_mutex[tty]);
    send_thread_active[tty] = 0;
    pthread_mutex_unlock(&send_thread_mutex[tty]);

    free(arg);  // Free the allocated tty parameter
    printf("Send thread completed for tty %d\n", tty);
    return NULL;
}

// Timer thread function - sends routing packets every 30 seconds
static void *timer_thread_func(void *arg) {
    int tty = *(int *) arg;
    free(arg);  // Free the allocated tty parameter

    printf("Timer thread started for tty %d\n", tty);

    while (1) {
        sleep(30);  // Wait 30 seconds

        // Check if timer thread should still be active
        pthread_mutex_lock(&timer_thread_mutex[tty]);
        if (!timer_thread_active[tty]) {
            pthread_mutex_unlock(&timer_thread_mutex[tty]);
            break;
        }
        pthread_mutex_unlock(&timer_thread_mutex[tty]);

        // Create and send routing packet
        printf("Sending routing packet on tty %d\n", tty);

        // Create a simple routing packet (you can customize this based on your routing protocol)
        char routing_packet[64];  // Small routing packet
        struct ipv6_header *routing_hdr = (struct ipv6_header *) routing_packet;

        if (tty < num_local_addresses) {
            // Use multicast address for routing packets (ff02::1 is all nodes multicast)
            struct in6_addr multicast_addr;
            inet_pton(AF_INET6, "ff02::1", &multicast_addr);

            create_ipv6_header(routing_hdr,
                sizeof(routing_packet) - sizeof(struct ipv6_header),
                58,  // ICMPv6 protocol
                &local_addresses[tty],
                &multicast_addr);

// Send routing packet
            if (write_slip_data(tty, routing_packet, sizeof(routing_packet)) < 0) {
                printf("Error: Failed to send routing packet on tty %d\n", tty);
            }
            else {
                printf("Routing packet sent successfully on tty %d\n", tty);
            }
        }
    }

    // Mark timer thread as inactive
    pthread_mutex_lock(&timer_thread_mutex[tty]);
    timer_thread_active[tty] = 0;
    pthread_mutex_unlock(&timer_thread_mutex[tty]);

    printf("Timer thread completed for tty %d\n", tty);
    return NULL;
}

// receive handler with IP processing and routing
static void receive_handler(int tty, const void *vdata, int numbytes) {
    const char *data = (const char *) vdata;

    printf("tty %d, ", tty);
    print_packet("slip received packet", data, numbytes);

    // Check if packet is large enough to contain IPv6 header
    if (numbytes < (int) sizeof(struct ipv6_header)) {
        printf("Error: Packet too small for IPv6 header, dropping\n");
        return;
    }

    // Parse IPv6 header
    struct ipv6_header *ipv6_hdr = (struct ipv6_header *) data;

    // Extract destination address for routing
    struct in6_addr dest_addr = ipv6_hdr->dest_addr;
    char dest_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &dest_addr, dest_str, sizeof(dest_str));
    printf("IPv6 destination: %s\n", dest_str);

    // Check and decrement hop limit
    uint8_t current_hop_limit = ipv6_hdr->hop_limit;
    printf("Current hop limit: %d\n", current_hop_limit);

    if (current_hop_limit <= 1) {
        printf("Hop limit reached 0, discarding packet\n");
        return;
    }

    // Create modified packet with decremented hop limit
    char *modified_packet = malloc(numbytes);
    if (!modified_packet) {
        printf("Error: Failed to allocate memory for packet buffer\n");
        return;
    }

    memcpy(modified_packet, data, numbytes);
    struct ipv6_header *modified_hdr = (struct ipv6_header *) modified_packet;
    modified_hdr->hop_limit = current_hop_limit - 1;

    printf("Decremented hop limit to: %d\n", modified_hdr->hop_limit);

    // Initialize mutexes if needed
    init_thread_mutexes();

    // Send queue logic (size 1)
    pthread_mutex_lock(&queue_mutex[tty]);
    if (send_queue[tty].is_full) {
        printf("Send queue full for tty %d, dropping packet\n", tty);
        pthread_mutex_unlock(&queue_mutex[tty]);
        free(modified_packet);
        return;
    }

    // Add packet to queue
    send_queue[tty].size = numbytes;
    send_queue[tty].tty = tty;
    send_queue[tty].dest_addr = dest_addr;
    memcpy(send_queue[tty].data, modified_packet, numbytes);
    send_queue[tty].is_full = 1;
    pthread_mutex_unlock(&queue_mutex[tty]);

    // Check if send thread is already active
    pthread_mutex_lock(&send_thread_mutex[tty]);
    if (send_thread_active[tty]) {
        printf("Send thread already active for tty %d, dropping packet\n", tty);
        pthread_mutex_unlock(&send_thread_mutex[tty]);
        // Clear the queue since we can't process it
        pthread_mutex_lock(&queue_mutex[tty]);
        send_queue[tty].is_full = 0;
        send_queue[tty].size = 0;
        pthread_mutex_unlock(&queue_mutex[tty]);
        free(modified_packet);
        return;
    }

    // Start send thread
    send_thread_active[tty] = 1;
    int *tty_param = malloc(sizeof(int));
    *tty_param = tty;

    if (pthread_create(&send_thread[tty], NULL, send_thread_func, tty_param) != 0) {
        printf("Error: Failed to create send thread for tty %d\n", tty);
        send_thread_active[tty] = 0;
        free(tty_param);
    }

    pthread_mutex_unlock(&send_thread_mutex[tty]);

    // Start timer thread if not already active
    pthread_mutex_lock(&timer_thread_mutex[tty]);
    if (!timer_thread_active[tty]) {
        timer_thread_active[tty] = 1;
        int *timer_tty_param = malloc(sizeof(int));
        *timer_tty_param = tty;

        if (pthread_create(&timer_thread[tty], NULL, timer_thread_func, timer_tty_param) != 0) {
            printf("Error: Failed to create timer thread for tty %d\n", tty);
            timer_thread_active[tty] = 0;
            free(timer_tty_param);
        }
        else {
            printf("Timer thread started for tty %d\n", tty);
        }
    }
    pthread_mutex_unlock(&timer_thread_mutex[tty]);

    free(modified_packet);
}

int main(int argc, char *argv[]) {
    // arg check
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IPv6_addr1> <IPv6_addr2> ... <IPv6_addrN>\n", argv[0]);
        return 1;
    }

    // check that there are no more than MAX_TTYS addresses
    int num_addrs = argc - 1;
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

    // parse each address and store in array
    struct in6_addr simulators[num_addrs];
    for (int i = 0; i < num_addrs; i++) {
        struct in6_addr addr;
        if (inet_pton(AF_INET6, argv[i + 1], &addr) != 1) {
            fprintf(stderr, "Error: Invalid IPv6 address '%s'. Input must be in literal form.\n", argv[i + 1]);
            return 1;
        }
        simulators[i] = addr;

        // Store local addresses for timer thread
        if (i < MAX_TTYS) {
            local_addresses[i] = addr;
        }
    }
    num_local_addresses = num_addrs;

    // install SLIP interface for each simulated address
    int tty_fds[num_addrs];
    for (int i = 0; i < num_addrs; i++) {
        char addr_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &simulators[i], addr_str, sizeof(addr_str));
        printf("Interface %d: %s\n", i, addr_str); // ex. Interface 0: <address>

        // open SLIP interface
        printf("Opening SLIP interface for %s on tty %d\n", addr_str, i);
        tty_fds[i] = install_slip_data_handler(i, receive_handler);
        if (tty_fds[i] < 0) {
            fprintf(stderr, "Error: Failed to install tty data handler for interface %d\n", i);
            return 1;
        }
        printf("Installed tty data handler for interface %d with fd %d\n", i, tty_fds[i]);
    }

    // Keep the router running to receive data
    while (1) {
        sleep(1);
    }

}

