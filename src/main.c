#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#define _POSIX_SOURCE           // required for sigaction, clock_gettime, nanosleep
#include <signal.h>
#include <time.h>
#undef _POSIX_SOURCE
#include <sys/ioctl.h>
#include <termios.h>
#include <string.h>

void show_cursor(void);
void signal_handler(int);

void print_time(unsigned short, int, int, int, int);

void fill_dots(char (*)[27][5], int, int);

static struct termios *original_mode = NULL;

int main(int argc, char *argv[])
{
    int place = isatty(STDOUT_FILENO);
    unsigned short wsrow = 0, wscol = 0, row = 0, col = 0;
    int size = 1;
    if (place) {
        if (isatty(STDIN_FILENO)) {
            static struct termios mode;
            tcgetattr(STDIN_FILENO, &mode);
            tcflag_t original_c_lflag = mode.c_lflag;
            mode.c_lflag ^= ECHO;
            tcsetattr(STDIN_FILENO, TCSADRAIN, &mode);
            mode.c_lflag = original_c_lflag;
            original_mode = &mode;
        }
        atexit(show_cursor);
        struct sigaction sa = {
            .sa_handler = signal_handler,
            .sa_flags = SA_RESETHAND | SA_NODEFER,
        };
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_time;
        localtime_r(&ts.tv_sec, &tm_time);
        if (place) {
            struct winsize ws;
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
            if (ws.ws_row != wsrow || ws.ws_col != wscol) {
                printf("\033c");
                wsrow = ws.ws_row;
                wscol = ws.ws_col;
                int lines = wsrow / 16;
                size = ws.ws_col / 32;
                if (lines < size) {
                    size = lines;
                }
                if (size < 1) {
                    size = 1;
                }
                row = wsrow > 3 ? (wsrow - size * 5 / 2) / 2 : 0;
                col = wscol > 27 ? (wscol - size * 27) / 2 : 0;
            }
            printf("\033[?25l\033[%u;0H", row);
        }
        print_time(col, tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, size);
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000000 - ts.tv_nsec;
        nanosleep(&ts, &ts);
    }
    return 0;
}

void show_cursor(void)
{
    printf("\033[?25h");
    if (original_mode) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, original_mode);
    }
    fflush(stdout);
}

void signal_handler(int signal)
{
    show_cursor();
    kill(0, signal);
}

void print_time(unsigned short col, int hour, int min, int sec, int size)
{
    char dots[27][5];
    memset(&dots, 0, sizeof(dots));
    fill_dots(&dots, hour / 10, 0);
    fill_dots(&dots, hour % 10, 4);
    fill_dots(&dots, -1, 8);
    fill_dots(&dots, min / 10, 10);
    fill_dots(&dots, min % 10, 14);
    fill_dots(&dots, -1, 18);
    fill_dots(&dots, sec / 10, 20);
    fill_dots(&dots, sec % 10, 24);

    int lines = (size * 5 + 1) / 2;
    int hsize = 1;
    for (int i = 0; i < lines; i += hsize) {
        char buf[27 * 3 * size + 64];
        char *pos = buf;
        if (col) {
            pos += snprintf(pos, 64, "\033[%uC", col);
        }
        for (int j = 0; j < 27; j++) {
            char *save = pos;
            int lline = (i * 2 + 1) / size;
            char upper = dots[j][i * 2 / size];
            char lower = lline < 5 ? dots[j][lline] : 0;
            if (lower && upper) {
                *(pos++) = '\xe2';
                *(pos++) = '\x96';
                *(pos++) = '\x88';
            } else if (upper) {
                *(pos++) = '\xe2';
                *(pos++) = '\x96';
                *(pos++) = '\x80';
            } else if (lower) {
                *(pos++) = '\xe2';
                *(pos++) = '\x96';
                *(pos++) = '\x84';
            } else {
                *(pos++) = ' ';
            }
            size_t len = pos - save;
            for (int n = 1; n < size; n++) {
                memcpy(pos, save, len);
                pos += len;
            }
        }
        *pos = 0;
        if (i * 2 / size != (i * 2 + 3) / size) {
            hsize = 1;
        } else {
            hsize = (((i * 2) / size + 1) * size) / 2 - i;
        }
        for (int n = hsize; n--;) {
            puts(buf);
        }
    }
}

void fill_dots(char (*dots)[27][5], int num, int pos)
{
    switch(num) {
    case 0:
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 1:
        pos += 2;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 2:
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 3:
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 4:
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        pos++;
        (*dots)[pos][2] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 5:
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 6:
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 7:
        (*dots)[pos][0] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 8:
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][4] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    case 9:
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][2] = 1;
        pos++;
        (*dots)[pos][0] = 1;
        (*dots)[pos][1] = 1;
        (*dots)[pos][2] = 1;
        (*dots)[pos][3] = 1;
        (*dots)[pos][4] = 1;
        break;
    default:
        (*dots)[pos][1] = 1;
        (*dots)[pos][3] = 1;
    }
}
