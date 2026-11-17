/* ============================================================================
 * NexusOS — File Manager App (Header)
 * ============================================================================
 * Browse, view, create, and delete files in RamFS. Desktop window app.
 * ============================================================================ */

#ifndef FILEMGR_H
#define FILEMGR_H

/* Open a file manager window on the desktop. Returns window ID or -1 */
int filemgr_open(void);
int filemgr_open_path(const char* path);

#endif /* FILEMGR_H */
