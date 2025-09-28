# Project 1

course: ICS 651
date created: September 15, 2025 9:04 AM
last modified: September 24, 2025 9:55 PM
materials: https://www2.hawaii.edu/~esb/2025fall.ics651/project1.html
due: 10/01/2025
type: assignment

# Overall Design

- [ ]  read command line args
    - [ ]  specify simulated interfaces and their IP addresses, i.e. `./router <IPv6 addr 1> ... <IPv6 addr N>`
- [ ]  opens SLIP on each simulated interface
- [ ]  do IP processing on each received packet
- [ ]  look up destination in routing table to determine next hop
    - [ ]  forwards packet along that simulated link
    - [ ]  if packet is for this router, do router processing

---

## Command Line

- [x]  Each simulated interface is associated with a simulated IPv6 addresses
    - [x]  each address can be parsed with `inet_pton`
- [x]  each IPv6 arg corresponds to an interface in the simconfig
- [x]  for each simulated interface in the command line, open a SLIP interface
- [ ]  the netmask for all addresses in 64 bits

## Communications

- [x]  router need not support more than MAX_TTYS (100) interfaces
- [ ]  simulator sleeps about 800 ms before sending each character
- [ ]  must establish a lock on a port before a packet can be sent → done in slipnet.c

## Threads

### Receive Thread

- [x]  installs SLIP data handler on an interface
- [ ]  does IP processing (see below)
    - [ ]  creates the send thread
- [ ]  does routing (see below)
    - [ ]  creates the timer thread
- [ ]  if processing a routing packet, acquire a lock for the routing table before updating it, then release lock

### Send Thread

- [ ]  if packets is being forwarded, put packet in send queue (size 1)
    - [ ]  if full, drop packet
    - [ ]  else, create a send thread
        - [ ]  if active, drop packet
- [ ]  keeps a local variable copy of packet

### Timer Thread

- [ ]  sends routing packets every 30 seconds

## SLIP Processing

- [ ]  use functions defined in slipnet.c and simnet.c

### Milestone 2 due 9/16

## IP Processing

- [ ]  done in receive thread
- [ ]  look up IPv6 destination in packet header, use this for routing or local processing
- [ ]  if forwarding packet, decrement hop limit
    - [ ]  if resulting hop limit is 0: discard packet
    - [ ]  else, copy packet to buffer to be used by send thread and start send thread
        - [ ]  if send thread is already active, do nothing

## Destination Lookup

- [ ]  checks if packet is a routing packet addressed to this router, if so do router processing (see below)
    - [ ]  routing packets have next header of 2
- [ ]  if not addressed to this router, forward packet
    - [ ]  search for address in routing table
    - [ ]  if route is found, put in queue & start send thread, else drop

### Milestone 3 due 9/23

## Routing Protocol

- [ ]  each router initializes its routing table with all the networks it is directly connected to
- [ ]  every 30 seconds, each router sends the contents of its routing table to the network broadcast address ff02::1
    - [ ]  see routing table in project instructions for format
- [ ]  all routing packets have at least one route in them and not more than 29 routes
- [ ]  all information must be encoded in big endian order and in hexadecimal
- [ ]  the distance sent must be the same as the distance shown in router’s own routing table
    - [ ]  receivers must increment this value before comparing their own routes or updating their own routing table
- [ ]  each routing packet must be prefixed with a IPv6 header then sent using write_slip_data
    - [ ]  hop limit should be 1

- [ ]  When a routing packet is received, extract all routes from the packet.
- For each route in the packet, perform the following steps:
    - [ ]  Search the routing table for a matching route (first 64 bits / 8 bytes of the destination network match).
    - [ ]  If a matching route exists:
        - [ ]  Increment the distance (metric) of the route from the packet by 1.
        - [ ]  Compare with the existing route’s distance:
            - [ ]  If the new route’s distance ≤ existing distance, replace the old route with the new one.
            - [ ]  If the existing distance is lower, do not update.
    - [ ]  If no matching route exists in the routing table, add the new route (with incremented metric).
    - [ ]  Set the route’s next hop to the **Sender IP** of the incoming packet.
- [ ]  Record the timestamp when each route is added to the table.
- [ ]  Periodically check route timestamps:
    - [ ]  Discard routes older than 100 seconds.
    - [ ]  Do not discard permanent routes corresponding to local interfaces.

## Documentation

- [ ]  include a status.txt that describes the status of your code
- [ ]  include suitable documentation that describes your thinking and provides references or other indications of what you are doing

## Testing and Interoperation

- [ ]  see project instructions for methods of verifying your code

### Project due 10/1, via Lamaku