/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define VERSION "0.0.1"
#define TAB_STOP 8
#define QUIT_TIMES 3
// adding these line below so that compiler does not complain about getline()
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
/*** flags for syntax highlighting ***/
#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

enum EditorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
};

enum EditorSyntaxHighlight {
  HL_Normal = 0,
  HL_String,
  HL_Keyword1,
  HL_Keyword2,
  HL_Comment,
  HL_MLComment,
  HL_Number,
  HL_Match,
};

/*** data ***/
typedef struct erow {
  // this is editor row for text viewing
  int idx;
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl; // for syntax highlighting
  int hl_open_comment;
} erow;

struct EditorSyntax {
  // this is to allow different rule for different file types
  char *filetype;
  char **filematch; // this is an array of strings
  char **keywords;
  char *singleline_comment_start;
  char *multiline_comment_start;
  char *multiline_comment_end;
  int flags;
};

struct EditorConfig {
  int cx, cy;
  int rx; // this is for the render field
  int row;
  int col;
  int rowoff;
  int coloff;
  int numrows;
  int dirty;  // tell if buffer is modified or not
  erow *erow; // making an array of dynamic erow
  char *filename;
  char statusmsg[80];
  time_t time;
  struct termios global_terminal;
  struct EditorSyntax *syntax;
};

struct EditorConfig E;
// this is the append buffer which will reduce the need of write() everytime we
// want a freshscreen

/*** filetypes ***/
char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};
char *C_HL_keywords[] = {"switch",    "if",      "while",   "for",    "break",
                         "continue",  "return",  "else",    "struct", "union",
                         "typedef",   "static",  "enum",    "class",  "case",
                         "int|",      "long|",   "double|", "float|", "char|",
                         "unsigned|", "signed|", "void|",   NULL};
struct EditorSyntax HLDB[] = {
    {"c", C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/
void EditorUpdateSyntax(erow *);
int EditorSyntax2Color(int);
int is_separator(int);
void EditorFreshScreen();
void EditorMoveCursor(int);
char *EditorPrompt(char *, void (*)(char *, int));
void EditorSave();
void EditorFind();

/*** terminal ***/
void die(const char *s) {
  write(1, "\x1b[2J", 4);
  write(1, "\x1b[H", 3);
  perror(s);
  exit(1);
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
  // we are just appending the string to the end of the ab buffer
}

void abFree(struct abuf *ab) { free(ab->b); }

void disableRawMode() {
  if (tcsetattr(0, TCSAFLUSH, &E.global_terminal) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode() {
  if (tcgetattr(0, &E.global_terminal) == -1)
    die("getattr");

  atexit(disableRawMode);

  struct termios raw = E.global_terminal;
  raw.c_iflag &= ~(IXON | ICRNL); // ixon disable ctrl s and ctrl q
  // icrnl fixes ctrl m and other as carriage return and new line
  raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP);
  raw.c_oflag &=
      ~(OPOST); // put processing turn off the translation of /n to /r/n
  raw.c_cflag |= (CS8);
  raw.c_lflag &=
      ~(ECHO | ICANON | ISIG | IEXTEN); // local flag is like a dumping ground
  // isig diables ctrl c and ctrl z
  // iexten disables ctrl v
  raw.c_cc[VMIN] =
      0; // control characters we set vmin to deteremine minimum number of
         // characters before the terminal can start reading
  raw.c_cc[VTIME] = 1; // max amount of time in tenths of seconds before read
                       // returns 0
  if (tcsetattr(0, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
  // tcsaflush argument tells us when the change will be
  // applied. in this case it waits till all output is
  // written and discards all input that hasnot been read
}

int EditorReadKey() {
  int n;
  char c;
  while ((n = read(0, &c, 1)) != 1) {
    if (n == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b') {
    char store[3];

    if (!read(0, &store[0], 1))
      return '\x1b';
    if (!read(0, &store[1], 1))
      return '\x1b';

    if (store[0] != '[') {
      return '\x1b';
    } else if (store[0] == '[') {

      if (store[1] >= '0' && store[1] <= '9') {
        // page up and page down
        if (read(0, &store[2], 1) == '\x1b')
          return '\x1b';
        if (store[2] == '~') {
          if (store[1] == '1')
            return HOME_KEY;
          if (store[1] == '3')
            return DEL_KEY;
          if (store[1] == '4')
            return END_KEY;
          if (store[1] == '7')
            return HOME_KEY;
          if (store[1] == '8')
            return END_KEY;
          if (store[1] == '5')
            return PAGE_UP;
          if (store[1] == '6')
            return PAGE_DOWN;
        } else
          return '\x1b';
      } else {
        switch (store[1]) {

        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (store[0] == '0') {
      switch (store[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }
  }
  return c;
}

int getCursorPosition(int *row, int *col) {
  // if (write(1, "\x1b[6n", 4) != 4)
  //   return -1;
  //
  // printf("\r\n");
  // char c;
  // while (read(0, &c, 1) == 1) {
  //   if (iscntrl(c)) {
  //     printf("%d\r\n", c);
  //   } else {
  //     printf("%d ('%c')\r\n", c, c);
  //   }
  // }
  // EditorReadKey();
  // return -1;
  char buf[32];
  if (write(1, "\x1b[6n", 4) != 4)
    return -1;
  unsigned int i;
  for (i = 0; i < sizeof(buf) - 1; i++) {
    if (read(0, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
  }
  buf[i] = '\0';
  // printf(
  //     "\r\n&buf[1] : '%s'\r\n",
  //     &buf[1]); // the fisrt character is a escape character which we want to
  //               // avoid since the terminal will interpret and will not print
  //               it
  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;

  if (sscanf(&buf[2], "%d;%d", row, col) != 2)
    return -1;
  return 0;
}

int getEditorsize(int *row, int *col) {
  struct winsize w;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) !=
        12) { // the cursor is not at the last position

      return -1;
    }
    return getCursorPosition(row, col);
  } else {
    *col = w.ws_col;
    *row = w.ws_row;
    return 0;
  }
}

void EditorStatusMessage(const char *fmtstring, ...) {

  va_list ap;
  va_start(ap, fmtstring);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmtstring, ap);
  va_end(ap);
  E.time = time(NULL);
}

/*** syntax highlighting ***/

void EditorSelectSyntaxHighlight() {
  E.syntax = NULL;
  if (E.filename == NULL)
    return;

  char *ext = strchr(E.filename, '.');

  for (unsigned int i = 0; i < HLDB_ENTRIES; i++) {
    struct EditorSyntax *s = &HLDB[i];
    unsigned int j = 0;
    while (s->filematch[j]) {
      int is_ext = (s->filematch[j][0] == '.');
      if ((is_ext && ext && !strcmp(ext, s->filematch[j])) ||
          (!is_ext && strstr(E.filename, s->filematch[j]))) {
        E.syntax = s;

        // now highlight the entire file
        int filerow;
        for (filerow = 0; filerow < E.numrows; filerow++) {
          EditorUpdateSyntax(&E.erow[filerow]);
        }
        return;
      }
      j++;
    }
  }
}

void EditorUpdateSyntax(erow *erow) {
  erow->hl = realloc(erow->hl, erow->rsize);
  memset(erow->hl, HL_Normal, erow->rsize);

  if (E.syntax == NULL)
    return;

  char **keywords = E.syntax->keywords;

  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;

  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  int prev_sep = 1;
  int in_string = 0;
  int in_comment = (erow->idx > 0 && E.erow[erow->idx - 1].hl_open_comment);

  int i = 0;

  while (i < erow->rsize) {
    char c = erow->render[i];
    unsigned char prev_hl = (i > 0) ? erow->hl[i - 1] : HL_Normal;

    if (scs_len && !in_string && !in_comment) {
      if (!strncmp(&erow->render[i], scs, scs_len)) {
        memset(&erow->hl[i], HL_Comment, erow->rsize - i);
        break;
      }
    }

    if (mcs_len && mce_len && !in_string) {
      if (in_comment) {
        erow->hl[i] = HL_MLComment;
        if (!strncmp(&erow->render[i], mce, mce_len)) {
          memset(&erow->hl[i], HL_MLComment, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          i++;
          continue;
        }
      } else if (!strncmp(&erow->render[i], mcs, mcs_len)) {
        memset(&erow->hl[i], HL_MLComment, mcs_len);
        in_comment = 1;
        i += mcs_len;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        erow->hl[i] = HL_String;
        if (c == '\\' && i + 1 < erow->rsize) {
          erow->hl[i + 1] = HL_String;
          i += 2;
          continue;
        }
        if (c == in_string)
          in_string = 0;
        i++;
        prev_sep = 1;
        continue;
      } else {
        if (c == '"' || c == '\'') {
          in_string = c;
          erow->hl[i] = HL_String;
          i++;
          continue;
        }
      }
    }
    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(c) && (prev_sep || prev_hl == HL_Number)) ||
          (c == '.' && prev_hl == HL_Number)) {
        erow->hl[i] = HL_Number;
        i++;
        prev_sep = 0;
        continue;
      }
    }
    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen = strlen(keywords[j]);
        int kw2 = keywords[j][klen - 1] == '|';
        if (kw2)
          klen--;

        if (!strncmp(&erow->render[i], keywords[j], klen) &&
            is_separator(erow->render[i + klen])) {
          memset(&erow->hl[i], kw2 ? HL_Keyword2 : HL_Keyword1, klen);
          i += klen;
          break;
        }
      }
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }
    prev_sep = is_separator(c);
    i++;
  }

  int changed = (erow->hl_open_comment != in_comment);
  erow->hl_open_comment = in_comment;
  if (changed && erow->idx + 1 < E.numrows) {
    EditorUpdateSyntax(&E.erow[erow->idx + 1]);
  }
}

int EditorSyntax2Color(int hl) {
  switch (hl) {
  case (HL_Number):
    return 31;
  case (HL_Keyword1):
    return 33;
  case (HL_Keyword2):
    return 32;
  case (HL_String):
    return 35;
  case (HL_Comment):
  case (HL_MLComment):
    return 36;
  case (HL_Match):
    return 34;
  default:
    return 39;
  }
}

int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

/*** row operations ***/

int ConvertCx2Rx(erow *erow, int cx) {
  int rx = 0;
  for (int i = 0; i < cx; i++) {
    if (erow->chars[i] == '\t')
      rx += (TAB_STOP - 1) - (rx % TAB_STOP);
    rx++;
  }
  return rx;
}

int ConvertRx2Cx(erow *erow, int rx) {
  int cur_rx = 0;
  int i;
  for (i = 0; i < erow->size; i++) {
    if (erow->chars[i] == '\t')
      cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
    cur_rx++;

    if (cur_rx > rx)
      // here we return only when greater because of the fact that i will update
      // only in the next loop
      return i;
  }
  return i;
}

void EditorUpdateRow(erow *erow) {
  // this is for rendering the control characters in the terminal
  int tabs = 0;
  for (int i = 0; i < erow->size; i++) {
    if (erow->chars[i] == '\t')
      tabs++;
  }
  free(erow->render);

  erow->render = malloc(erow->size + 1 + tabs * TAB_STOP);
  int j;
  int idx = 0;

  for (j = 0; j < erow->size; j++) {
    if (erow->chars[j] == '\t') {
      erow->render[idx++] = ' ';
      while (idx % TAB_STOP != 0) {
        erow->render[idx++] = ' ';
      }
    } else
      erow->render[idx++] = erow->chars[j];
  }
  erow->render[idx] = '\0';
  erow->rsize = idx;

  EditorUpdateSyntax(erow);
}

void EditorRowInsertChar(erow *erow, int at, char c) {
  if (at < 0 || at > erow->size)
    at = erow->size;

  erow->chars = realloc(erow->chars, erow->size + 2);
  memmove(
      &erow->chars[at + 1], &erow->chars[at],
      erow->size - at +
          1); // like memcpy but safe to use when source and destination is same
  erow->size++;
  erow->chars[at] = c;
  EditorUpdateRow(erow);
  E.dirty++;
}

void EditorRowDelChar(erow *erow, int at) {
  if (at < 0 || at >= erow->size)
    return;
  memmove(&erow->chars[at], &erow->chars[at + 1], erow->size - at);
  erow->size--;
  EditorUpdateRow(erow);
  E.dirty++;
}

void EditorInsertRow(int at, char *s, size_t linelen) {
  if (at < 0 || at > E.numrows)
    return;

  E.erow = realloc(E.erow, sizeof(erow) * (E.numrows + 1));
  memmove(&E.erow[at + 1], &E.erow[at], sizeof(erow) * (E.numrows - at));
  for (int i = at + 1; i < E.numrows; i++)
    E.erow[i].idx++; // for multi line comments
  // int at = E.numrows;
  E.erow[at].idx = at;
  E.erow[at].size = linelen;
  E.erow[at].chars = malloc(linelen + 1);
  memcpy(E.erow[at].chars, s, linelen);
  E.erow[at].chars[linelen] = '\0';

  E.erow[at].rsize = 0;
  E.erow[at].render = NULL;
  E.erow[at].hl = NULL;
  E.erow[at].hl_open_comment = 0;

  EditorUpdateRow(&E.erow[at]);
  E.numrows++;
  E.dirty++;
}

void EditorInsertNewLine() {
  if (E.cx == 0) {
    EditorInsertRow(E.cy, "", 0);
  } else {
    erow *erow = &E.erow[E.cy];
    EditorInsertRow(E.cy + 1, &erow->chars[E.cx], erow->size - E.cx);
    erow = &E.erow[E.cy];
    // reassigning due to realloc
    erow->size = E.cx;
    erow->chars[erow->size] = '\0';
    EditorUpdateRow(erow);
  }
  E.cy++;
  E.cx = 0;
}

void EditorFreeRow(erow *erow) {
  free(erow->render);
  free(erow->chars);
  free(erow->hl);
}

void EditorAppendString(erow *erow, char *s, size_t len) {
  erow->chars = realloc(erow->chars, erow->size + len + 1);
  memcpy(&erow->chars[erow->size], s, len);
  erow->size += len;
  erow->chars[erow->size] = '\0';
  EditorUpdateRow(erow);
  E.dirty++;
}

void EditorDelRow(int at) {
  if (at < 0 || at >= E.numrows)
    return;
  EditorFreeRow(&E.erow[at]);
  memmove(&E.erow[at], &E.erow[at + 1], sizeof(erow) * (E.numrows - at - 1));
  for (int i = at; i < E.numrows - 1; i--)
    E.erow[i].idx--; // for multi line comments
  E.numrows--;
  E.dirty++;
}

/*** editor operations ***/

void EditorInsertChar(int c) {
  if (E.cy == E.numrows) {
    EditorInsertRow(E.numrows, "", 0);
  }
  EditorRowInsertChar(&E.erow[E.cy], E.cx, c);
  E.cx++;
}

void EditorDelChar() {
  if (E.cy == E.numrows)
    return;
  if (E.cx == 0 && E.cy == 0)
    return;
  erow *erow = &E.erow[E.cy];
  if (E.cx > 0) {
    EditorRowDelChar(erow, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.erow[E.cy - 1].size;
    EditorAppendString(&E.erow[E.cy - 1], erow->chars, erow->size);
    EditorDelRow(E.cy);
    E.cy--;
  }
}

void EditorProcessKeypress() {
  int c = EditorReadKey();
  // printf("%c\r\n", c);
  static int quit_times = QUIT_TIMES;
  switch (c) {
  case '\r':
    // Enter key
    EditorInsertNewLine();
    break;
  case CTRL_KEY('q'):
    if (E.dirty && quit_times > 0) {
      EditorStatusMessage(
          "Warning! You have unsaved changes in this file do you want. Press "
          "quit %d more times to close without saving",
          --quit_times + 1);
      return;
    }
    write(1, "\x1b[2J", 4);
    write(1, "\x1b[H", 3);
    exit(0);
    break;
  case CTRL_KEY('s'):
    EditorSave();
    break;
  case CTRL_KEY('f'):
    EditorFind();
    break;
  case BACKSPACE:
  case DEL_KEY:
  case CTRL_KEY('h'): {
    if (c == DEL_KEY) {
      EditorMoveCursor(ARROW_RIGHT);
    }
    EditorDelChar();
    break;
  }
  case PAGE_UP:
  case PAGE_DOWN: {
    // To scroll up or down a page, we position the cursor either at the top or
    // bottom of the screen, and then simulate an entire screen’s worth of ↑ or
    // ↓ keypresses.
    if (c == PAGE_UP) {
      E.cy = E.rowoff;
    } else if (c == PAGE_DOWN) {
      E.cy = E.rowoff + E.row - 1;
      if (E.cy > E.numrows)
        E.cy = E.numrows;
    }
    int times = E.row;
    while (times--) {
      EditorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;
  }
  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows) {
      E.cx = E.erow[E.cy].size;
    }
    break;
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    EditorMoveCursor(c);
    break;
  case CTRL_KEY('l'):
  case '\x1b':
    break;
  default:
    EditorInsertChar(c);
    break;
  }
  quit_times = QUIT_TIMES;
  // static variable remembers its value even after use so we need to reapply
  // its value
}

/*** file i/o ***/

char *EditorRowstoString(int *buflen) {
  int totallen = 0;
  for (int i = 0; i < E.numrows; i++) {
    totallen += E.erow[i].size + 1; // one extra to save the newline character
  }
  *buflen = totallen;
  char *buf = malloc(totallen);
  char *curr = buf;

  for (int i = 0; i < E.numrows; i++) {
    memcpy(curr, E.erow[i].chars, E.erow[i].size);
    curr += E.erow[i].size;
    *curr = '\n';
    curr++;
  }
  return buf;
}

void EditorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  EditorSelectSyntaxHighlight();

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  ssize_t linecap = 0, linelen;
  while ((

             linelen = getline(&line, &linecap, fp)) !=
         -1 // these are similar to long in size_t
  ) {

    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }
    EditorInsertRow(E.numrows, line, linelen);
  }

  free(line);
  fclose(fp);
  E.dirty = 0;
  // E.erow.size = linelen;
  // E.erow.chars = malloc(linelen + 1);
  // memcpy(E.erow.chars, line, linelen);
  // E.erow.chars[linelen] = '\0';
  // E.numrows = 1;
}

void EditorSave() {
  if (E.filename == NULL) {
    E.filename = EditorPrompt("Save as : %s", NULL);
    if (!E.filename) {
      EditorStatusMessage("Save cancelled");
      return;
    }
    EditorSelectSyntaxHighlight();
  }

  int len;

  char *str = EditorRowstoString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, str, len) == len) {
        close(fd);
        free(str);
        E.dirty = 0;
        EditorStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(str);
  EditorStatusMessage("Error Saving file! Unable to save file : %s",
                      strerror(errno));
}

/*** search operation ***/

void EditorFindCallBack(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;
  static int hl_line;
  static char *hl_saved = NULL;

  if (hl_saved) {
    memcpy(E.erow[hl_line].hl, hl_saved, E.erow[hl_line].rsize);
    free(hl_saved);
    hl_saved = NULL;
  }
  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }
  if (last_match == -1)
    direction = 1;

  int curr_match = last_match;

  for (int i = 0; i < E.numrows; i++) {
    curr_match += direction;
    if (curr_match == -1)
      curr_match = E.numrows - 1;
    else if (curr_match == E.numrows)
      curr_match = 0;
    erow *erow = &E.erow[curr_match];
    char *occur = strstr(erow->render, query);

    if (occur) {
      last_match = curr_match;
      E.cy = curr_match;
      E.cx = ConvertRx2Cx(erow, occur - erow->render);
      E.rowoff = E.numrows;
      hl_line = curr_match;
      hl_saved = malloc(erow->rsize);
      memcpy(hl_saved, erow->hl, erow->rsize);
      memset(&erow->hl[occur - erow->render], HL_Match, strlen(query));
      break;
    }
  }
}

void EditorFind() {
  int temp[] = {E.cx, E.cy, E.coloff, E.rowoff};
  char *query =
      EditorPrompt("Search: %s (ESC / Arrows / Enter)", EditorFindCallBack);
  if (query)
    free(query);
  else {
    E.cx = temp[0];
    E.cy = temp[1];
    E.coloff = temp[2];
    E.rowoff = temp[3];
  }
}

/*** output ***/

void EditorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = ConvertCx2Rx(&E.erow[E.cy], E.cx);
  }
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.row + E.rowoff) {
    E.rowoff = E.cy - E.row + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.col + E.coloff) {
    E.coloff = E.rx - E.col + 1;
  }
}

void EditorDrawRows(struct abuf *ab) {
  for (int i = 0; i < E.row; i++) {
    int filerow = E.rowoff + i;
    if (filerow >= E.numrows) {

      if (E.numrows == 0 && i == E.row / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Mayows's editor -- version %s", VERSION);
        if (welcomelen > E.col)
          welcomelen = E.col;
        abAppend(ab, welcome, welcomelen);
      }
      abAppend(ab, "~", 1);
    } else {
      int len = E.erow[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.col)
        len = E.col;
      // abAppend(ab, &E.erow[filerow].render[E.coloff], len);
      char *c = &E.erow[filerow].render[E.coloff];
      unsigned char *hl = &E.erow[filerow].hl[E.coloff];
      int curr_color = -1;
      for (int i = 0; i < len; i++) {
        // if (isdigit(c[i])) {
        //   abAppend(ab, "\x1b[31m", 5);
        //   abAppend(ab, &c[i], 1);
        //   abAppend(ab, "\x1b[39m", 5);
        // } else
        //   abAppend(ab, &c[i], 1);
        //
        if (iscntrl(c[i])) {
          char sym = (c[i] <= 26) ? '@' + c[i] : '?';
          abAppend(ab, "\x1b[7m", 4);
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3); // this will turn off all text coloring so
                                     // we need to write again
          if (curr_color != -1) {
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\1xb[%dm", curr_color);
            abAppend(ab, buf, clen);
          }
        } else if (hl[i] == HL_Normal) {
          if (curr_color != -1) {

            abAppend(ab, "\x1b[39m", 5);
            curr_color = -1;
          }
          abAppend(ab, &c[i], 1);
        } else {
          int color = EditorSyntax2Color(hl[i]);
          if (color != curr_color) {
            char buf[16];
            int c_len = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            abAppend(ab, buf, c_len);
            curr_color = color;
          }
          abAppend(ab, &c[i], 1);
        }
      }

      abAppend(ab, "\x1b[39m", 5);
    }
    abAppend(ab, "\x1b[K",
             3); // this erases the part of the rest of the current
                 // line so we dont need to remove the entire line
                 // if (i < E.row - 1) {
    abAppend(ab, "\r\n", 2);
    // }
  }
}

void EditorDrawStatusLine(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No name]", E.numrows,
                     E.dirty ? "modified" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                      E.syntax ? E.syntax->filetype : "no filetype", E.cy + 1,
                      E.numrows);
  if (len > E.col)
    len = E.col;
  abAppend(ab, status, len);
  while (len < E.col) {
    if (E.col - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {

      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void EditorDrawStatusMessage(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int messagelen = strlen(E.statusmsg);
  if (messagelen > E.col)
    messagelen = E.col;
  if (messagelen && time(NULL) - E.time < 5) {
    abAppend(ab, E.statusmsg, messagelen);
  }
}

void EditorFreshScreen() {
  // write(1, "\x1b[2J", 4); // the middle one is escape sequence with
  // additional
  //                         // information of where to where print empty space
  // write(1, "\x1b[H", 3);
  EditorScroll();
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6); // this hides the cursor to avoid flickering
  // abAppend(&ab, "\x1b[2J", 4); since we are already erasing the line with
  // with drawrows
  abAppend(&ab, "\x1b[H", 3);

  EditorDrawRows(&ab);
  EditorDrawStatusLine(&ab);
  EditorDrawStatusMessage(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
           E.rx - E.coloff + 1);
  abAppend(&ab, buf, strlen(buf));
  // abAppend(&ab, "\x1b[H", 3);
  abAppend(&ab, "\x1b[?25h", 6); // this shows the cursor to avoid flickering

  write(1, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

char *EditorPrompt(char *prompt, void (*callback)(char *, int)) {
  // save as feature
  size_t buf_size = 128;
  char *buf = malloc(buf_size);

  size_t buf_len = 0;
  buf[0] = '\0';

  while (1) {
    EditorStatusMessage(prompt, buf);
    EditorFreshScreen();

    int c = EditorReadKey();
    if (c == DEL_KEY || c == BACKSPACE || c == CTRL_KEY('h')) {
      if (buf_len)
        buf[--buf_len] = '\0';
    } else if (c == '\x1b') {
      EditorStatusMessage("");
      if (callback)
        callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buf_len != 0) {
        EditorStatusMessage("");
        if (callback)
          callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buf_len == buf_size - 1) {
        buf_size *= 2;
        buf = realloc(buf, buf_size);
      }
      buf[buf_len++] = c;
      buf[buf_len] = '\0';
    }

    if (callback)
      callback(buf, c);
  }
}

void EditorMoveCursor(int key) {
  erow *temprow = (E.cy >= E.numrows) ? NULL : &E.erow[E.cy];
  switch (key) {
  case ARROW_DOWN:
    if (E.cy < E.numrows)
      E.cy++;
    break;
  case ARROW_UP:
    if (E.cy)
      E.cy--;
    break;
  case ARROW_LEFT:
    if (E.cx)
      E.cx--;
    else if (E.cy) {
      E.cy--;
      E.cx = E.erow[E.cy].size;
    }
    break;
  case ARROW_RIGHT:
    if (temprow && E.cx < temprow->size)
      E.cx++;
    else if (temprow && E.cx == temprow->size) {
      E.cx = 0;
      E.cy++;
    }
    break;
  }

  temprow = (E.cy >= E.numrows) ? NULL : &E.erow[E.cy];
  int rowlen =
      (!temprow) ? 0 : temprow->size; // this is for making the cursor move to
                                      // the end of each line when going down
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

/*** init ***/

void Editorinit() {
  E.cx = 0, E.cy = 0, E.rx = 0;
  E.numrows = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.erow = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.time = 0;
  E.dirty = 0;
  E.syntax = NULL;
  if (getEditorsize(&E.row, &E.col) == -1)
    die("getEditorsize");
  E.row -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  Editorinit();
  if (argc >= 2) {

    EditorOpen(argv[1]);
  }
  EditorStatusMessage("HELP :CTRL S = Save | CTRL Q = Quit | CTRL F = Find");
  while (1) {
    EditorFreshScreen();
    EditorProcessKeypress();
  }
  return 0;
}
