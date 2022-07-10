/*** includes ***/

// feature test macro for getline()
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

/*** defines ***/

#define QUARK_VERSION "0.0.1"
#define WELCOME_MSG "Hello, friend. Welcome to Quark %s"
#define CTRL_KEY(k) ((k) & 0x1f) // mirrors what Ctrl key does in terminal

enum cursorKey {
	ARROW_UP = 1000,
	ARROW_DOWN,
	ARROW_RIGHT,
	ARROW_LEFT,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};


/*** prototypes ***/

void exitClearScreen(void);
void editorRefreshScreen(void);

/*** data ***/

typedef struct erow {
	int size;
	char *chars;
} erow;

struct editorConfig {
	int cx, cy;
	int screenrows;
	int screencols;
	int numrows;
	erow row;

	struct termios original_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
	exitClearScreen();
	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1)
		die("tcgetattr");

	atexit(disableRawMode);

	struct termios new_termios;
	new_termios = E.original_termios;

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

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	// handle escape sequence (e.g. for arrow keys)
	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if ('0' <= seq[1] && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

				if (seq[2] == '~') {
					switch (seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
					// arrow keys for cursor movement
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	// TIOCGWINSZ is a request for window size
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		return -1;
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** file io ***/

void editorOpen(char *filename) {
	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	linelen = getline(&line, &linecap, fp);
	fclose(fp);
	if (linelen != -1) {
		while (0 < linelen && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen --;
	}

	E.row.size = linelen;
	E.row.chars = malloc(linelen + 1);
	memcpy(E.row.chars, line, linelen);
	E.row.chars[linelen] = '\0';
	E.numrows = 1;
	free(line);
}

/*** append buffer ***/

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);
	if (new == NULL) return;

	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}

/*** input ***/

void editorMoveCursor(int c) {
	switch (c) {
		case ARROW_UP:
			if (0 < E.cy) E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.screenrows - 1) E.cy++;
			break;
		case ARROW_RIGHT:
			if (E.cx < E.screencols - 1) E.cx++;
			break;
		case ARROW_LEFT:
			if (0 < E.cx) E.cx--;
			break;
	}
}

void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			exitClearScreen();
			exit(0);
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = E.screenrows;
				while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			E.cx = E.screencols - 1;
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_RIGHT:
		case ARROW_LEFT:
			editorMoveCursor(c);
			break;
	}
}

/*** output ***/

void exitClearScreen() {
	editorRefreshScreen();
}

void editorDrawRows(struct abuf *ab) {
	for (int y = 0; y < E.screenrows; y++) {
		if (y < E.numrows) {
			int linelen = E.row.size;
			if (E.screencols < linelen) linelen = E.screencols;
			abAppend(ab, E.row.chars, linelen);
		
		} else if (E.numrows == 0 && y == E.screenrows / 3) {
			char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
						WELCOME_MSG, QUARK_VERSION);
				if (E.screencols < welcomelen) welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
		} else {
			abAppend(ab, "~", 1);
		}

		abAppend(ab, "\x1b[K", 3); // erase line to right of cursor
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorRefreshScreen() {
	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6); // hide cursor
	abAppend(&ab, "\x1b[H", 3); // moves cursor to top-left

	editorDrawRows(&ab);

	// put cursor in correct position
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));


	abAppend(&ab, "\x1b[?25h", 6); // show cursor
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}


/*** init ***/

void initEditor() {
	E.cx = 0;
	E.cy = 0;
	E.numrows = 0;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}


	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
