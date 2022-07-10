/*** includes ***/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f) // mirrors what Ctrl key does in terminal

/*** data ***/

struct termios original_termios;

/*** terminal ***/

void die(const char *s) {
	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &original_termios) == -1)
		die("tcgetattr");

	atexit(disableRawMode);

	struct termios new_termios;
	new_termios = original_termios;

	new_termios.c_lflag &= ~(ECHO); // disable echo
	new_termios.c_lflag &= ~(ICANON); // disable canonical mode (so we read byte-by-byte from stdin)
	new_termios.c_lflag &= ~(ISIG); // disable SIGINT and SIGTSTP from Ctrl-C/Z
	new_termios.c_lflag &= ~(IEXTEN); // disable Ctrl-V and fixes Ctrl-O in macOS (usually discarded)

	new_termios.c_iflag &= ~(IXON); // disable pausing of data transmisison (freeze of terminal) with Ctrl-S/Q
	new_termios.c_iflag &= ~(ICRNL); // disable translation of carriage returns (13) to newlines (10)

	new_termios.c_oflag &= ~(OPOST); // disable output processing which maps \n to \r\n

	// Probably already defaults
	new_termios.c_iflag &= ~(BRKINT | INPCK | ISTRIP);
	new_termios.c_cflag |= (CS8);

	new_termios.c_cc[VMIN] = 0; // read returns as soon as any input available
	new_termios.c_cc[VTIME] = 1; // read always returns after 100ms


	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_termios) == -1)
		die("tcsetattr");
}

char editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}
	return c;
}

/*** input ***/

void editorProcessKeypress() {
	char c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			exit(0);
			break;
	}
}



/*** init ***/

int main() {
	enableRawMode();


	while (1) {
		editorProcessKeypress();
	}

	return 0;
}
