/* simnet.c: provide a simulation of serial-line send and receive */
/* on linux (e.g. projects), link with -lpthread */
/* on solaris (e.g. uhunix), link with -lpthread -lsocket -lnsl -lrt */
/* released under the X11 license -- see "license" for details */

/* this program simulates a collection of serial ports on a single
   machine.  The simulation uses UDP packets containing a single
   byte to transfer the data among simulated serial ports.

   The simulation is configured by reading a configuration
   file "simconfig" in which each line has the following format:
      my-udp-port other-udp-port other-physical-machine
   where the udp ports are numbers between 1025 and 65535, and
   the physical machine is either an IP address in dotted decimal
   notation (e.g. 128.171.104.5) or a domain name (e.g.
   projects.ics.hawaii.edu).  For example,
      "1234  4521 localhost"
   is a valid entry. Successive tty numbers, starting with zero, are
   assigned to successive valid lines.  Any invalid entry causes 
   initialize_tty to abort.

   The "simconfig" file must be in the current directory, and lines
   beginning with # are considered comments.  To run multiple
   simulators on the same machine, simply run them in different
   directories.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "simnet.h"

/* this is the simulator configuration information we need */
struct simulation_config {
  short local_port;
  struct sockaddr_in remote;	/* port and IP number, in network byte order */
  int socket;
  int in_use;
};

static struct simulation_config tty_sim [MAX_TTYS];

static int valid_ttys = 0;

#define simerror(message) { perror (message); exit(1); }

static void read_config_file ()
{
  if (valid_ttys == 0) {	/* nothing initialized yet */
    char linebuf [1000];
    FILE * f;
    /* valid_ttys and line differ by the number of invalid lines
       in simconfig */
    int line = 0;
    struct protoent * protocolentry = getprotobyname ("udp");
    int protocol;
    int unused_tty;

    for (unused_tty = 0; unused_tty < MAX_TTYS; unused_tty++) {
      tty_sim [unused_tty].socket = -1;
      tty_sim [unused_tty].in_use = 0;
    }
    if (protocolentry == NULL)
      simerror ("getprotobyname");
    protocol = protocolentry->p_proto;
    f = fopen (CONFIG_FILE, "r");
    if (f == NULL)
      simerror ("opening simconfig for reading");

    while (fgets (linebuf, sizeof(linebuf), f) != NULL) {
      char * comment;
      char * rport;
      char * hostname;
      int remote_port, local_port;
      struct hostent * hostentry;

      line++;
      if ((comment = index (linebuf, '#')) != NULL) { /* comment found */
	*comment = '\0';
      }
      local_port = strtol (linebuf, &rport, 10);
      remote_port = strtol (rport, &hostname, 10);
      if ((rport != linebuf) && (hostname != rport) && /* conversion ok */
	  (local_port <= 65535) && (local_port > 0) && /* looks good */
	  (remote_port <= 65535) && (remote_port > 0)) {
	tty_sim [valid_ttys].local_port = local_port;
	/* get rid of whitespace before calling gethostbyname */
	/* first get rid of any initial whitespace */
	while ((*hostname == ' ') || (*hostname == '\t')) {
	  hostname++;
	}
	/* now get rid of any final whitespace */
	while ((hostname [strlen (hostname) - 1] == ' ') ||
	       (hostname [strlen (hostname) - 1] == '\t') ||
	       (hostname [strlen (hostname) - 1] == '\n')) {
	  hostname [strlen (hostname) - 1] = '\0';
	}
	printf ("resolving host name %s\n", hostname);
	/* note gethostbyname also accepts dotted IP addresses, i.e. 1.2.3.4 */
	hostentry = gethostbyname(hostname);
	if ((hostentry == NULL) || (hostentry->h_addr_list == NULL)) {
				/* assume this is a bad entry */
	  printf ("line %d of simconfig, hostname unknown, ignoring (%s)\n",
		  line, linebuf);
	} else {	/* create the socket for this simulated interface */
	  struct sockaddr_in sin;
	  struct sockaddr * sap = (struct sockaddr *) &sin;

	  /* create the address that this socket sends data to */
	  bzero (&(tty_sim [valid_ttys].remote), sizeof (struct sockaddr_in));
	  tty_sim [valid_ttys].remote.sin_family = AF_INET;
	  memcpy(&(tty_sim [valid_ttys].remote.sin_addr),
		 hostentry->h_addr_list[0], hostentry->h_length);
	  tty_sim [valid_ttys].remote.sin_port = htons (remote_port);

	  /* create the socket and bind it to the port */
	  tty_sim [valid_ttys].socket = socket (PF_INET, SOCK_DGRAM, protocol);
	  if (tty_sim [valid_ttys].socket < 0)
	    simerror ("socket");
	  sin.sin_family = AF_INET;
	  sin.sin_port = htons (local_port);
	  sin.sin_addr.s_addr = INADDR_ANY;
	  if (bind (tty_sim [valid_ttys].socket, sap, sizeof (sin)) != 0)
	    simerror("bind");
	  valid_ttys++;		/* we have initialized another simulated tty */
	}
      } else {			/* some error, ignore this line */
	char * thiserror = "remote port < 0";
	if (remote_port > 65535)
	  thiserror = "remote port > 65535";
	if (local_port < 0)
	  thiserror = "local port < 0";
	if (local_port > 65535)
	  thiserror = "local port > 65535";
	if (hostname == rport)
	  thiserror = "no number given for the remote port";
	if (rport == linebuf)
	  thiserror = "no number given for the local port";
	while (linebuf [strlen (linebuf) - 1] == '\n')
	  linebuf [strlen (linebuf) - 1] = '\0';
	printf ("line %d of simconfig, %s, ignoring (%s)\n",
		line, thiserror, linebuf);
      }
    }
  }
}

/* any static function is NOT exported */
static int initialize_tty (int tty_number)
{
  read_config_file ();	/* only actually read once, but called many times */

  if (tty_number >= MAX_TTYS) { simerror ("tty number"); }
  if (tty_number >= valid_ttys) { return -1; }
  if (tty_sim [tty_number].socket < 0) { simerror ("invalid tty"); }
  if (tty_sim [tty_number].in_use) { simerror ("tty already in use"); }
  tty_sim [tty_number].in_use = 1;
  return tty_number;
}

struct receive_thread_arg {
  void (* data_handler) (int, char);
  int tty;
};

static void * tty_receive_thread (void * argument)
{
  /* cast the argument back to a pointer to the receive_thread_arg */
  struct receive_thread_arg * rta = (struct receive_thread_arg *) argument; 
  void (* data_handler) (int, char) = rta->data_handler; 
  int tty = rta->tty;

  printf ("tty_receive_thread is starting\n");
  /* we have read the argument, it won't be used ever again, so free it */
  free (argument);
  /* set the argument to NULL to guarantee it won't ever be used again */
  argument = NULL;

  /* loop forever, and whenever data is received, call the data handler */
  /* when no data is available, the loop blocks on read. */
  while (1) {
    char buffer [1];
    struct sockaddr from;
    socklen_t fromlen = sizeof (from);
    int bytes = recvfrom (tty_sim [tty].socket, buffer, 1, 0, &from, &fromlen);

    if (bytes == -1) {
      perror ("recvfrom");
      exit (1);
    }
    if (bytes == 1) {
      /* deliver the data with an upcall */
      data_handler (tty, buffer [0]);
    } else {
      printf ("ttynet error: got value %d from 'recvfrom', expected 1\n",
	      bytes);
    }
  }
  /* we never return, but if we ever did, we'd want to return a void *  */
  printf ("error: returning from infinite loop\n");
  return NULL;
}

/* returns the identifier (an integer >= 0) to be used for write_tty_data */
int install_tty_data_handler (int tty, void (* data_handler) (int, char))
{
  pthread_t thread;
  int actual_tty = initialize_tty (tty);
  struct receive_thread_arg * arg =
    (struct receive_thread_arg *) malloc (sizeof (struct receive_thread_arg));

  if (actual_tty < 0) {
    free (arg);
    return -1;
  }
  arg->tty = actual_tty;
  arg->data_handler = data_handler;
  if (pthread_create (&thread, NULL, &tty_receive_thread, (void *) arg) < 0) {
    perror ("pthread_create");
    exit (1);
  }
  return actual_tty;
}

int write_tty_data (int tty, char data)
{
  char buffer [1];
  struct timespec wait_time;

  wait_time.tv_sec = 0;
  wait_time.tv_nsec = 1000000000 / (9600 / 8); /* 8 bits at 9600 b/s */
  nanosleep (&wait_time, NULL);
  buffer [0] = data;
  return sendto (tty_sim [tty].socket, buffer, 1, 0,
		 (struct sockaddr *) (&(tty_sim [tty].remote)),
		 sizeof (struct sockaddr_in));
}

#ifdef RUN_THIS_TEST
/* this is a sample program to exercise the above code */

/* my data handler simply prints any received data to the screen */
static void my_test_data_handler (int tty, char c)
{
  static int new [MAX_TTYS];
  static int initialized = 0;
  static int most_recent = MAX_TTYS;

  if (! initialized) {
    int i;
    for (i = 0; i < MAX_TTYS; i++)
      new [i] = 1;
    initialized = 1;
  }
  if (tty == most_recent) {
    putchar (c);
    if (c == '\n')
      most_recent = MAX_TTYS;
  } else {
    most_recent = tty;
    printf ("\n-- %d: ", tty);
    putchar (c);
  }
}

int main (int argc, char ** argv)
{
  if (argc < 0)
    printf ("%s: error, argc is %d\n", argv [0], argc);
  int ttys [MAX_TTYS];
  char data_to_send [] = "this is my test data\n123\n";
  int i, tty;

  for (tty = 0; tty < MAX_TTYS; tty++) {
    ttys [tty] = -1;
    if (tty < 4) {
      ttys [tty] = install_tty_data_handler (tty, my_test_data_handler); 
      if (ttys [tty] < 0) {
	printf ("unable to open tty%d\n", tty);
      }
    }
  }
  for (i = 0; i < sizeof (data_to_send); i++) {
    for (tty = 0; tty < 3; tty++) {
      if (ttys [tty] >= 0) {
	write_tty_data (ttys [tty], data_to_send [i]);
      }
    }
  }
  /* wait and see if we receive anything */
  printf ("sleeping 100 seconds\n");
  sleep (100);
  return 0;
}

#endif /* RUN_THIS_TEST */

