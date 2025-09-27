/* slipnet.c: provide serial-line send and receive of packets */
/* link with (ttynet or simnet) and pthreads */
/* 2022: released under CC0 */

/* to compile: gcc -Wall -Wextra -DRUN_SLIP_TEST slipnet.c simnet.c -o slipnet */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "slipnet.h"
#include "simnet.h"

/* buffers for the data */
static char receive_buffer [MAX_TTYS] [MAX_SLIP_SIZE];
/* this is the position to which we add newly received characters */
static int receive_position [MAX_TTYS];
/* record whether the last character for this buffer was an escape character */
static int escaped [MAX_TTYS];
/* true if an error was detected in the current frame */
static int error_frame [MAX_TTYS];
/* serialize all access to the buffers */
static pthread_mutex_t receive_mutex [MAX_TTYS];
static pthread_mutex_t send_mutex [MAX_TTYS];
/* serialize access to the global data */
static pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
/* the data handlers are also global. */
typedef void (* my_data_handler) (int, const void *, int);
static my_data_handler slip_data_handler [MAX_TTYS];

/* useful for printing IPv6 and other packets */
/* prints the first 8 bytes, then 16 bytes per line.  This works well
 * for IPv6 headers, which will be in the first 3 lines */
void print_packet (char * string, const void * vdata, int numbytes)
{
  const char * data = (const char *) vdata; 
  int i;

  printf ("%s (%d bytes):\n", string, numbytes);
  for (i = 0; i < numbytes; i++) {
    /* must mask the byte with 0xff, since otherwise bytes greater
       than 0x80 will be converted to negative integers */
    printf ("%02x", (data [i]) & 0xff);
    if ((i == (numbytes - 1)) || (i % 16 == 7)) {
      printf ("\n");
    } else {
      printf (".");
    }
  }
}

static void put_char_in_buffer (int tty, unsigned char c)
{
  if (receive_position [tty] < MAX_SLIP_SIZE - 1) {
    receive_buffer [tty] [(receive_position [tty])++] = c;
  } else {
    printf ("error: slip framing error on port %d, maybe lost END\n", tty);
    /* discard the character -- basically, we don't save it anywhere. */
    /* also make sure the current frame is discarded */
    error_frame [tty] = 1;
  }
}

static void data_handler_for_tty (int tty, char signed_char)
{
  int c = signed_char & 0xff;   /* convert negative chars to chars >= 128 */
#ifdef DEBUG
  printf ("  received character %x/%o on port %d\n", c, c, tty);
#endif /* DEBUG */
  /* make sure we have been initialized */
  pthread_mutex_lock (&global_mutex);
  /* we have been initialized, so proceed */
  pthread_mutex_unlock (&global_mutex);
  /* acquire the lock for the receive buffer */
  pthread_mutex_lock (&(receive_mutex [tty]));
  if (error_frame [tty]) {
    if (c == SLIP_END) {
      error_frame [tty] = 0;
      receive_position [tty] = 0;
      escaped [tty] = 0;
    }
  } else {
    if (escaped [tty]) {	/* last character was an escape */
      escaped [tty] = 0;
      if (c == SLIP_ESC_END) {
        put_char_in_buffer (tty, SLIP_END);
      } else if (c == SLIP_ESC_ESC) {
        put_char_in_buffer (tty, SLIP_ESC);
      } else {   /* this may be a legitimate oversight in the sender */
        printf ("warning: accepting illegal character after ESC\n");
        put_char_in_buffer (tty, c);
      }
    } else {			/* last character was not ESC */
      if (c == SLIP_END) {	/* done, give packet to data handler. */
        if (receive_position [tty] > 0) { /* packet is not empty */
          if (slip_data_handler [tty] == NULL) {
            /* no handler, drop packet */
            printf ("error: received packet, but no slip data handler\n");
            print_packet ("received packet", receive_buffer [tty],
                          receive_position [tty]);
            receive_position [tty] = 0;
          } else {
#ifdef DEBUG
	    printf ("received %d bytes\n", receive_position [tty]);
            print_packet ("received packet", receive_buffer [tty],
                          receive_position [tty]);
#endif /* DEBUG */
            /* note the receive buffer remains locked while we call the
               slip data handler.  If the slip data handler never returns,
               slip will deadlock, i.e., be unable to ever again receive data.
               This would also block the receive thread in ttynet. */
            slip_data_handler [tty] (tty, receive_buffer [tty],
                                     receive_position [tty]);
          }
          /* get ready to start receiving a new packet */
          receive_position [tty] = 0;
        } /* else: silently ignore packets of size 0 */
      } else if (c == SLIP_ESC) {     /* signal for the next character */
	escaped [tty] = 1;
      } else {                        /* 'normal' character */
        put_char_in_buffer (tty, c);
      }
    }
  }
  /* finally make the buffer available to other threads. */
  pthread_mutex_unlock (&(receive_mutex [tty]));
}

/* returns the identifier (an integer >= 0) to be used for write_slip_data */
int install_slip_data_handler
      (int tty, void (* data_handler) (int, const void *, int))
{
  int fd;
  pthread_mutex_t tmp = PTHREAD_MUTEX_INITIALIZER;

  /* keep thread from executing until we are done initializing */
  pthread_mutex_lock (&(global_mutex));
  fd = install_tty_data_handler (tty, data_handler_for_tty); 
  if (fd < 0) {
    pthread_mutex_unlock (&global_mutex);
    return fd;
  }
  receive_position [fd] = 0;
  escaped [fd] = 0;
  error_frame [fd] = 0;
  memcpy (&(receive_mutex [fd]), &tmp, sizeof (tmp));
  memcpy (&(send_mutex [fd]), &tmp, sizeof (tmp));
  slip_data_handler [fd] = data_handler;
  pthread_mutex_unlock (&global_mutex);
  return fd;
}

/* this is a macro so the return statement returns from write_slip_data */
#define WRITE_BYTE(fd, c)                               \
    if (write_tty_data (fd, c) != 1) {                  \
      pthread_mutex_unlock (&(send_mutex [fd]));        \
      printf ("slip: error writing tty data\n");        \
      return -1;                                        \
    }

int write_slip_data (int fd, char * data, int numbytes)
{
  int byte;

  if ((numbytes <= 0) || (numbytes > MAX_SLIP_SEND)) {
    printf ("slip: bad size %d\n", numbytes);
    return -1;
  }
#ifdef DEBUG
  printf ("acquiring send lock for tty %d\n", fd);
#endif /* DEBUG */
  pthread_mutex_lock (&(send_mutex [fd]));
#ifdef DEBUG
  print_packet ("sending packet", data, numbytes);
#endif /* DEBUG */
  WRITE_BYTE (fd, SLIP_END);        /* start with an END byte */
  for (byte = 0; byte < numbytes; byte++) {
    int c = (data [byte]) & 0xff;
    if (c == SLIP_END) {
      WRITE_BYTE (fd, SLIP_ESC);
      WRITE_BYTE (fd, SLIP_ESC_END);
    } else if (c == SLIP_ESC) {
      WRITE_BYTE (fd, SLIP_ESC);
      WRITE_BYTE (fd, SLIP_ESC_ESC);
    } else {                        /* normal byte */
      WRITE_BYTE (fd, c);
    }
  }
  WRITE_BYTE (fd, SLIP_END);        /* end with an END byte */
  pthread_mutex_unlock (&(send_mutex [fd]));
  return numbytes;
}

#ifdef RUN_SLIP_TEST
/* this is a sample program to exercise the above code */

/* my data handler simply prints any received data to the screen */
static void my_test_data_handler (int tty, const void * vdata, int numbytes)
{
  const char * data = (const char *) vdata;
  printf ("tty %d, ", tty);
  print_packet ("slip received packet", data, numbytes);
}

int main (int argc, char ** argv)
{
  int slip0 = install_slip_data_handler (0, my_test_data_handler); 
  int slip1 = install_slip_data_handler (1, my_test_data_handler); 
  int slip2 = install_slip_data_handler (2, my_test_data_handler); 
  char data1 [] = "123\300\333\334\335xxx\300\334\335 321";
  char data2 [] = "\300";
  char data3 [] = "\333\334\335";
  char data4 [MAX_SLIP_SEND];
  int i;
  for (i = 0; i < MAX_SLIP_SEND; i++)
    data4 [i] = i % 256;
  if (argc < 0)
    printf ("%s: illegal value of argc %d\n", argv [0], argc);

  write_slip_data (slip0, data1, sizeof (data1) - 1);
  if (slip1 >= 0) write_slip_data (slip1, data1, sizeof (data1) - 1);
  if (slip2 >= 0) write_slip_data (slip2, data1, sizeof (data1) - 1);
  sleep (10);
  write_slip_data (slip0, data2, sizeof (data2) - 1);
  if (slip1 >= 0) write_slip_data (slip1, data2, sizeof (data2) - 1);
  if (slip2 >= 0) write_slip_data (slip2, data2, sizeof (data2) - 1);
  sleep (10);
  write_slip_data (slip0, data3, sizeof (data3) - 1);
  if (slip1 >= 0) write_slip_data (slip1, data3, sizeof (data3) - 1);
  if (slip2 >= 0) write_slip_data (slip2, data3, sizeof (data3) - 1);
  sleep (10);
  write_slip_data (slip0, data4, sizeof (data4));
  if (slip1 >= 0) write_slip_data (slip1, data4, sizeof (data4));
  if (slip2 >= 0) write_slip_data (slip2, data4, sizeof (data4));
  /* wait and see if we receive anything */
  printf ("sleeping 100 seconds\n");
  sleep (100);
  return 0;
}
#endif /* RUN_SLIP_TEST */
