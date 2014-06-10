#ifndef _FIND_H
#define _FIND_H

#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif

#include <sys/file.h>
#include <sys/param.h>
#if HAVE_UTIME_H
#include <utime.h>
#else
struct utimbuf
{
	long actime;
	long modtime;
};
#endif

#define MODE_RALL (S_IRUSR|S_IRGRP|S_IROTH)

#ifdef HAVE_REGEX_H
#include <regex.h>
#endif

#ifndef HAVE_READDIR_R
int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
#endif

#define FT_LNK_H	1  // hard link to file already saved.
#define FT_REG		3  // Regular file.
#define FT_LNK_S	4  // Soft Link.
#define FT_DIR		5  // Directory.
#define FT_SPEC		6  // Special file -- chr, blk, fifo, sock.
#define FT_NOFOLLOW	8  // Could not follow link.
#define FT_NOSTAT	9  // Could not stat file.
#define FT_NOFSCHG	14  // Different file system, prohibited.
#define FT_NOOPEN	15  // Could not open directory.
#define FT_RAW		16  // Raw block device.
#define FT_FIFO		17  // Raw fifo device.
#define FT_REPARSE	21  // Win NTFS reparse point.
#define FT_JUNCTION	26  // Win32 Junction point.

/*
 * Definition of the find_files packet passed as the
 * first argument to the find_files callback subroutine.
 */
struct FF_PKT
{
	char *fname;		/* full filename */
	long flen;		/* length of name component */
	char *link;		/* link if file linked */
	struct stat statp;	/* stat packet */
	uint64_t winattr;	/* windows attributes */
	int type;		/* FT_ type from above */
};

extern struct FF_PKT *find_files_init(void);
extern void find_files_free(struct FF_PKT *ff);
extern int find_files_begin(struct asfd *asfd,
	struct FF_PKT *ff_pkt, struct conf *conf, char *fname);
extern int file_is_included(struct conf *conf, const char *fname,
	int top_level);
extern int in_include_regex(struct strlist *incre, const char *fname);
extern int in_exclude_regex(struct strlist *excre, const char *fname);
// Returns the level of compression.
extern int in_exclude_comp(struct strlist *excom, const char *fname,
	int compression);

#endif
