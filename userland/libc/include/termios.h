#ifndef NEXUS_TERMIOS_H
#define NEXUS_TERMIOS_H
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
#define NCCS 16
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_cc[NCCS]; };
#define BRKINT 0x0002
#define ICRNL 0x0100
#define INPCK 0x0010
#define ISTRIP 0x0020
#define IXON 0x0400
#define OPOST 0x0001
#define ONLCR 0x0004
#define CS8 0x0030
#define ISIG 0x0001
#define ICANON 0x0002
#define ECHO 0x0008
#define ECHOE 0x0010
#define ECHOK 0x0020
#define ECHONL 0x0040
#define IEXTEN 0x8000
#define VINTR 0
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
int tcgetattr(int, struct termios *);
int tcsetattr(int, int, const struct termios *);
#endif
