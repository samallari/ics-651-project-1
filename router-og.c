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

// #include "slipnet.h"
// #include "simnet.h"

/* Simple data handler based on slipnet.c reference */
static void handle_packet(int tty, char data)
{
    print_packet("slip received packet", &data, sizeof(data));

    // Echo the character back to the same interface
    int result = write_tty_data(tty, data);
    if (result != 1) {
        printf("Error sending character on tty %d\n", tty);
    } else {
        printf("Successfully echoed character on tty %d\n", tty);
    }
    
}

int main(int argc, char *argv[]) {
    // arg check
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IPv6_addr1> <IPv6_addr2> ... <IPv6_addrN>\n", argv[0]);
        return 1;
    }

    int num_addrs = argc - 1;
    if (num_addrs > MAX_TTYS) {
        fprintf(stderr, "Error: Maximum number of addresses is %d\n", MAX_TTYS);
        return 1;
    }

     // parse command line IPv6 addresses into binary form
    char ipv6_strings[num_addrs][INET6_ADDRSTRLEN];

    struct in6_addr ipv6_addrs[num_addrs];
    for (int i = 0; i < num_addrs; i++) {
        if (inet_pton(AF_INET6, argv[i + 1], &ipv6_addrs[i]) != 1) {
            fprintf(stderr, "Error: Invalid IPv6 address '%s'. Input must be in literal form.\n", ipv6_strings[i]);
            return 1;
        }
        // else: store string representation
        strncpy(ipv6_strings[i], argv[i], INET6_ADDRSTRLEN);
    }

    // open simconfig file
    FILE *simconfig = fopen("simconfig", "r");
    if (simconfig == NULL) {
        perror("Error: could not open 'simconfig'");
        return 1;
    }
    char line[256];

    // get the number of interfaces defined in simconfig 
    int num_interfaces = 0;
    while (fgets(line, sizeof(line), simconfig)) {
        if (*line != '\0' && *line != '\n' && line[0] != '#') { // skip empty lines and comments
            num_interfaces++;
        }
    }

    if (num_addrs > num_interfaces) {
        fprintf(stderr, "Error: More IPv6 addresses provided than defined in simconfig\n");
        fclose(simconfig);
        return 1;
    }

    rewind(simconfig); // reset file pointer to start

    // read simconfig line by line and validate addresses
    int idx = 0;
    while (fgets(line, sizeof(line), simconfig)) {
        // Skip empty lines and comments
        if (line[0] == '\n' || line[0] == '#') {
            continue;
        }

        // extract local UDP port, remote UDP port, and remote hostname
        int local_port, remote_port;
        char remote_host[128];
        if (sscanf(line, "%d %d %s", &local_port, &remote_port, remote_host) != 3) {
            fprintf(stderr, "Error: Invalid line in simconfig: '%s'. Expected format: <local_port> <remote_port> <remote_host>\n", line);
            return 1;
        }

        // resolve hostname to IPv6 address
        struct addrinfo hints = {0}, *result;
        hints.ai_family = AF_INET6;

        if (getaddrinfo(remote_host, NULL, &hints, &result) != 0) {
            fprintf(stderr, "Error: Could not resolve hostname '%s' from simconfig.\n", remote_host);
            fclose(simconfig);
            return 1;
        }

        // Get the binary address from the resolved result
        struct sockaddr_in6 *resolved_ipv6 = (struct sockaddr_in6 *) result->ai_addr;

        // compare simconfig IP with command-line IP
        if (memcmp(&resolved_ipv6->sin6_addr, &ipv6_addrs[idx], sizeof(struct in6_addr)) != 0) {
            fprintf(stderr, "Error: IP for host '%s' in simconfig does not match command-line address '%s' for interface %d.\n",
                remote_host, ipv6_strings[idx], idx);
            freeaddrinfo(result);
            fclose(simconfig);
            return 1;
        }

        freeaddrinfo(result);
        idx++;
    }
    printf("Successfully validated %d interfaces from simconfig.\n", idx);
    fclose(simconfig);

    // install tty data handlers for each interface
    int tty_fds[num_addrs];
    for (int i = 0; i < num_addrs; i++) {
        tty_fds[i] = install_tty_data_handler(i, handle_packet);
        if (tty_fds[i] < 0) {
            fprintf(stderr, "Error: Failed to install tty data handler for interface %d\n", i);
            return 1;
        }
        printf("Installed tty data handler for interface %d with fd %d\n", i, tty_fds[i]);
    }

    printf("Router is running and ready to receive data...\n");
    printf("Press Ctrl+C to exit.\n");
    
    // Keep the router running to receive data
    while (1) {
        sleep(1);
    }

    return 0;
}

