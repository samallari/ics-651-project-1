/* simnet.h: header file for simnet.c */
/* released under the X11 license -- see "license" for details */

/* call to install a data handler to receive incoming characters.
 * returns its first parameter, which should be a valid TTY number.
 * the handler is called once a char has been received.
 * the two parameters to the handler are:
 *  - the tty number (same as the first parameter to install_slip_data_handler)
 *  - the character that was received
 */
int install_tty_data_handler (int tty, void (*) (int, char));

int write_tty_data (int tty, char data);

#define MAX_TTYS        100

#define CONFIG_FILE "./simconfig"
