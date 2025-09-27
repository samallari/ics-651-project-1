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

// Packet buffer structure for send thread
struct packet_buffer {
    char data[MAX_SLIP_SIZE];
    int size;
    int tty;
    struct in6_addr dest_addr;
};

// Global variables for send thread management
static pthread_t send_thread[MAX_TTYS];
static int send_thread_active[MAX_TTYS] = {0};
static pthread_mutex_t send_thread_mutex[MAX_TTYS];
static struct packet_buffer packet_to_send[MAX_TTYS];
static pthread_mutex_t packet_buffer_mutex[MAX_TTYS];
static int send_thread_initialized = 0;

// IPv6 header structure for parsing
struct ipv6_header {
    uint32_t version_class_label;  // version (4), traffic class (8), flow label (20)
    uint16_t payload_length;
    uint8_t next_header;
    uint8_t hop_limit;
    struct in6_addr src_addr;
    struct in6_addr dest_addr;
};

// Initialize send thread mutexes
static void init_send_thread_mutexes() {
    if (!send_thread_initialized) {
        for (int i = 0; i < MAX_TTYS; i++) {
            pthread_mutex_init(&send_thread_mutex[i], NULL);
            pthread_mutex_init(&packet_buffer_mutex[i], NULL);
        }
        send_thread_initialized = 1;
    }
}

// Send thread function
static void *send_thread_func(void *arg) {
    int tty = *(int *) arg;
    free(arg);  // Free the allocated tty parameter

    printf("Send thread started for tty %d\n", tty);

    // Lock the packet buffer and send the packet
    pthread_mutex_lock(&packet_buffer_mutex[tty]);

    struct packet_buffer *packet = &packet_to_send[tty];
    if (packet->size > 0) {
        char addr_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &packet->dest_addr, addr_str, sizeof(addr_str));
        printf("Sending packet to %s on tty %d\n", addr_str, tty);

        // Send the packet using SLIP
        if (write_slip_data(tty, packet->data, packet->size) < 0) {
            printf("Error: Failed to send packet on tty %d\n", tty);
        }

        packet->size = 0;  // Clear the buffer
    }

    pthread_mutex_unlock(&packet_buffer_mutex[tty]);

    // Mark send thread as inactive
    pthread_mutex_lock(&send_thread_mutex[tty]);
    send_thread_active[tty] = 0;
    pthread_mutex_unlock(&send_thread_mutex[tty]);

    printf("Send thread completed for tty %d\n", tty);
    return NULL;
}

// Enhanced receive handler with IPv6 routing capabilities
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
    init_send_thread_mutexes();

    // Check if send thread is already active
    pthread_mutex_lock(&send_thread_mutex[tty]);
    if (send_thread_active[tty]) {
        printf("Send thread already active for tty %d, dropping packet\n", tty);
        pthread_mutex_unlock(&send_thread_mutex[tty]);
        free(modified_packet);
        return;
    }

    // Copy packet to buffer for send thread
    pthread_mutex_lock(&packet_buffer_mutex[tty]);
    packet_to_send[tty].size = numbytes;
    packet_to_send[tty].tty = tty;
    packet_to_send[tty].dest_addr = dest_addr;
    memcpy(packet_to_send[tty].data, modified_packet, numbytes);
    pthread_mutex_unlock(&packet_buffer_mutex[tty]);

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
    }

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

