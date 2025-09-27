/* slipnet.h: interface to provide serial-line send and receive of packets */
/* link with (ttynet or simnet) and pthreads */
/* released under the X11 license -- see "license" for details */

#ifndef SLIPNET_H
#define SLIPNET_H

#define MAX_SLIP_SEND   1006
#define MAX_SLIP_SIZE   1024   /* let senders send us a larger packet */

#ifndef MAX_TTYS
#define MAX_TTYS	100
#endif /* MAX_TTYS */

/* call to install a data handler to receive incoming packets.
 * returns its first parameter, which should be a valid TTY number.
 * the handler is called once a complete packet has been received.
 * the three parameters to the handler are:
 *  - the tty number (same as the first parameter to install_slip_data_handler)
 *  - a pointer to the received buffer
 *  - the number of bytes (chars) in the received buffer
 * returns the tty value (to be used in calls to write_slip_data),
 *    or -1 for errors
 */
extern int
  install_slip_data_handler (int tty,
                             void (* handler) (int, const void *, int));

extern int write_slip_data (int, char *, int);

/* special characters (bytes) defined by SLIP */
#define SLIP_END             0300    /* indicates end of packet */
#define SLIP_ESC             0333    /* indicates byte stuffing */
#define SLIP_ESC_END         0334    /* ESC ESC_END means END data byte */
#define SLIP_ESC_ESC         0335    /* ESC ESC_ESC means ESC data byte */

/* useful for printing IPv6 and other packets */
/* prints the first 8 bytes, then 16 bytes per line.  This works well
 * for IPv6 headers, which will be in the first 3 lines */
extern void print_packet (char * string, const void * vdata, int numbytes);

#endif /* SLIPNET_H */
