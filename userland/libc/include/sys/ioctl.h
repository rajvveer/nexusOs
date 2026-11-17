#ifndef NEXUS_IOCTL_H
#define NEXUS_IOCTL_H
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TIOCGWINSZ 0x5413
int ioctl(int, unsigned long, ...);
#endif
