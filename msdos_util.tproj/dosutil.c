/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1998 Robert Nordier
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*	@(#)dosutil.c	3.0	13/09/00	(c) 2000 Apple Computer, Inc.	*/

#include <stdint.h>
#include <mach/machine/boolean.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/loadable_fs.h>

#include <sys/disk.h>

#include <machine/byte_order.h>

#include <err.h>
#include <sysexits.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h> 
#include <pwd.h>

#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFStringEncodingExt.h>

#include "../msdosfs.kextproj/msdosfs.kmodproj/bootsect.h"
#include "../msdosfs.kextproj/msdosfs.kmodproj/bpb.h"
#include "../msdosfs.kextproj/msdosfs.kmodproj/direntry.h"

#define FS_TYPE			"msdos"
#define FS_NAME_FILE		"MSDOS"
#define FS_BUNDLE_NAME		"msdosfs.kext"
#define FS_KEXT_DIR			"/System/Library/Extensions/msdosfs.kext"
#define FS_KMOD_DIR			"/System/Library/Extensions/msdosfs.kext/msdosfs"
#define RAWDEV_PREFIX		"/dev/r"
#define BLOCKDEV_PREFIX		"/dev/"
#define MOUNT_COMMAND		"/sbin/mount"
#define UMOUNT_COMMAND		"/sbin/umount"
#define KEXTLOAD_COMMAND	"/sbin/kextload"
#define KMODLOAD_COMMAND	"/sbin/kmodload"
#define READWRITE_OPT		"-w"
#define READONLY_OPT		"-r"
#define SUID_OPT			"suid"
#define NOSUID_OPT			"nosuid"
#define DEV_OPT				"dev"
#define NODEV_OPT			"nodev"
#define LABEL_LENGTH		11
#define MAX_DOS_BLOCKSIZE	4096

#define FSUC_LABEL		'n'

#define UNKNOWN_LABEL		"Unlabeled"

#define	DEVICE_SUID			"suid"
#define	DEVICE_NOSUID		"nosuid"

#define	DEVICE_DEV			"dev"
#define	DEVICE_NODEV		"nodev"

#define	CLUST_FIRST	2			/* first legal cluster number */
#define	CLUST_RSRVD	0x0ffffff6	/* reserved cluster range */



/* globals */
const char	*progname;	/* our program name, from argv[0] */
int		debug;	/* use -D to enable debug printfs */



/*
 * The following code is re-usable for all FS_util programs
 */
void usage(void);

static int fs_probe(char *devpath, int removable, int writable);
static int fs_mount(char *devpath, char *mount_point, int removable, 
	int writable, int suid, int dev);
static int fs_unmount(char *devpath);
static int fs_label(char *devpath, char *volName);
static void fs_set_label_file(char *labelPtr);

static int safe_open(char *path, int flags, mode_t mode);
static void safe_read(int fd, char *buf, int nbytes, off_t off);
static void safe_close(int fd);
static void safe_write(int fd, char *data, int len, off_t off);
static void safe_execv(char *args[]);
static void safe_unlink(char *path);

#ifdef DEBUG
static void report_exit_code(int ret);
#endif

static int checkLoadable();
static int oklabel(const char *src);
static void mklabel(u_int8_t *dest, const char *src);

int ret = 0;


void usage()
{
        fprintf(stderr, "usage: %s action_arg device_arg [mount_point_arg] [Flags]\n", progname);
        fprintf(stderr, "action_arg:\n");
        fprintf(stderr, "       -%c (Probe)\n", FSUC_PROBE);
        fprintf(stderr, "       -%c (Mount)\n", FSUC_MOUNT);
        fprintf(stderr, "       -%c (Unmount)\n", FSUC_UNMOUNT);
        fprintf(stderr, "       -%c name\n", 'n');
    fprintf(stderr, "device_arg:\n");
    fprintf(stderr, "       device we are acting upon (for example, 'disk0s2')\n");
    fprintf(stderr, "mount_point_arg:\n");
    fprintf(stderr, "       required for Mount and Force Mount \n");
    fprintf(stderr, "Flags:\n");
    fprintf(stderr, "       required for Mount, Force Mount and Probe\n");
    fprintf(stderr, "       indicates removable or fixed (for example 'fixed')\n");
    fprintf(stderr, "       indicates readonly or writable (for example 'readonly')\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "		%s -p disk0s2 fixed writable\n", progname);
    fprintf(stderr, "		%s -m disk0s2 /my/hfs removable readonly\n", progname);
        exit(FSUR_INVAL);
}

int main(int argc, char **argv)
{
    char		rawdevpath[MAXPATHLEN];
    char		blockdevpath[MAXPATHLEN];
    char		opt;
    struct stat	sb;
    int			ret = FSUR_INVAL;


    /* save & strip off program name */
    progname = argv[0];
    argc--;
    argv++;

    /* secret debug flag - must be 1st flag */
    debug = (argc > 0 && !strcmp(argv[0], "-D"));
    if (debug) { /* strip off debug flag argument */
        argc--;
        argv++;
    }

    if (argc < 2 || argv[0][0] != '-')
        usage();
    opt = argv[0][1];
    if (opt != FSUC_PROBE && opt != FSUC_MOUNT && opt != FSUC_UNMOUNT && opt != FSUC_LABEL)
        usage(); /* Not supported action */
    if ((opt == FSUC_MOUNT || opt == FSUC_UNMOUNT || opt == FSUC_LABEL) && argc < 3)
        usage(); /* mountpoint arg missing! */

    sprintf(rawdevpath, "%s%s", RAWDEV_PREFIX, argv[1]);
    if (stat(rawdevpath, &sb) != 0) {
        fprintf(stderr, "%s: stat %s failed, %s\n", progname, rawdevpath,
                strerror(errno));
        exit(FSUR_INVAL);
    }

    sprintf(blockdevpath, "%s%s", BLOCKDEV_PREFIX, argv[1]);
    if (stat(blockdevpath, &sb) != 0) {
        fprintf(stderr, "%s: stat %s failed, %s\n", progname, blockdevpath,
                strerror(errno));
        exit(FSUR_INVAL);
    }

    switch (opt) {
        case FSUC_PROBE: {
            if (argc != 4)
                usage();
            ret = fs_probe(rawdevpath,
                        strcmp(argv[2], DEVICE_FIXED),
                        strcmp(argv[3], DEVICE_READONLY));
            break;
        }

        case FSUC_MOUNT:
        case FSUC_MOUNT_FORCE:
            if (argc != 7)
                usage();
            if (strcmp(argv[3], DEVICE_FIXED) && strcmp(argv[3], DEVICE_REMOVABLE)) {
                printf("msdosfs.util: ERROR: unrecognized flag (removable/fixed) argv[%d]='%s'\n",3,argv[3]);
                usage();
            }
                if (strcmp(argv[4], DEVICE_READONLY) && strcmp(argv[4], DEVICE_WRITABLE)) {
                    printf("msdosfs.util: ERROR: unrecognized flag (readonly/writable) argv[%d]='%s'\n",4,argv[4]);
                    usage();
                }
                if (strcmp(argv[5], DEVICE_SUID) && strcmp(argv[5], DEVICE_NOSUID)) {
                    printf("msdosfs.util: ERROR: unrecognized flag (suid/nosuid) argv[%d]='%s'\n",5,argv[5]);
                    usage();
                }
                if (strcmp(argv[6], DEVICE_DEV) && strcmp(argv[6], DEVICE_NODEV)) {
                    printf("msdosfs.util: ERROR: unrecognized flag (dev/nodev) argv[%d]='%s'\n",6,argv[6]);
                    usage();
                }
                ret = fs_mount(blockdevpath,
                            argv[2],
                            strcmp(argv[3], DEVICE_FIXED),
                            strcmp(argv[4], DEVICE_READONLY),
                            strcmp(argv[5], DEVICE_NOSUID),
                            strcmp(argv[6], DEVICE_NODEV));
            break;
        case FSUC_UNMOUNT:
            ret = fs_unmount(rawdevpath);
            break;
        case FSUC_LABEL:
            ret = fs_label(rawdevpath, argv[2]);
            break;
        default:
            usage();
    }

    #ifdef DEBUG
    report_exit_code(ret);
    #endif
    exit(ret);

    return(ret);
}

/*
 * Begin Filesystem-specific code
 */
static int fs_probe(char *devpath, int removable, int writable)
{
    int fd;
    struct dosdirentry *dirp;
    union bootsector *bsp;
    struct byte_bpb33 *b33;
    struct byte_bpb50 *b50;
    struct byte_bpb710 *b710;
    u_int16_t	bps;
    u_int8_t	spc;
    unsigned	rootDirSectors;
    unsigned	i,j, finished;
	char diskLabel[LABEL_LENGTH];
    char buf[MAX_DOS_BLOCKSIZE];

    fd = safe_open(devpath, O_RDONLY, 0);


    /*
     * Read the boot sector of the filesystem, and then check the
     * boot signature.  If not a dos boot sector then error out.
     *
     * NOTE: 4096 is a maximum sector size in current...
     */
    safe_read(fd, buf, MAX_DOS_BLOCKSIZE, 0);

    bsp = (union bootsector *)buf;
    b33 = (struct byte_bpb33 *)bsp->bs33.bsBPB;
    b50 = (struct byte_bpb50 *)bsp->bs50.bsBPB;
    b710 = (struct byte_bpb710 *)bsp->bs710.bsBPB;
    
    /* [2699033]
     *
     * The first three bytes are an Intel x86 jump instruction.  It should be one
     * of the following forms:
     *    0xE9 0x?? 0x??
     *    0xEC 0x?? 0x90
     * where 0x?? means any byte value is OK.
     */
    if (bsp->bs50.bsJump[0] != 0xE9
        && (bsp->bs50.bsJump[0] != 0xEB || bsp->bs50.bsJump[2] != 0x90))
    {
        return FSUR_UNRECOGNIZED;
    }

    /* It is possible that the above check could match a partition table, or some */
    /* non-FAT disk meant to boot a PC.  Check some more fields for sensible values. */

    /* We only work with 512, 1024, 2048 and 4096 byte sectors */
    bps = getushort(b33->bpbBytesPerSec);
    if ((bps < 0x200) || (bps & (bps - 1)) || (bps > MAX_DOS_BLOCKSIZE))
	{
        return(FSUR_UNRECOGNIZED);
	}

    /* Check to make sure valid sectors per cluster */
    spc = b33->bpbSecPerClust;
    if ((spc == 0 ) || (spc & (spc - 1)))
	{
        return(FSUR_UNRECOGNIZED);
	}

	/* Make sure the number of FATs is OK; on NTFS, this will be zero */
	if (b33->bpbFATs == 0)
	{
		return(FSUR_UNRECOGNIZED);
	}
	
	/* Make sure the total sectors is non-zero */
	if (getushort(b33->bpbSectors) == 0 && getulong(b50->bpbHugeSectors) == 0)
	{
		return(FSUR_UNRECOGNIZED);
	}

	/* Make sure there is a root directory */
	if (getushort(b33->bpbRootDirEnts) == 0 && getulong(b710->bpbRootClust) == 0)
	{
		return(FSUR_UNRECOGNIZED);
	}

    /* we know this disk, find the volume label */
    /* First, find the root directory */
    diskLabel[0] = 0;
    finished = false;
    rootDirSectors = ((getushort(b50->bpbRootDirEnts) * sizeof(struct dosdirentry)) +
                      (bps-1)) / bps;
    if (rootDirSectors) {			/* FAT12 or FAT16 */
    	unsigned firstRootDirSecNum;
    	char rootdirbuf[MAX_DOS_BLOCKSIZE];
    	
        firstRootDirSecNum = getushort(b33->bpbResSectors) + (b33->bpbFATs * getushort(b33->bpbFATsecs));
        for (i=0; i< rootDirSectors; i++) {
            safe_read(fd, rootdirbuf, bps, (firstRootDirSecNum+i)*bps);
            dirp = (struct dosdirentry *)rootdirbuf;
            for (j=0; j<bps; j+=sizeof(struct dosdirentry), dirp++) {
                if (dirp->deName[0] == SLOT_EMPTY) {
                    finished = true;
                    break;
                }
                else if (dirp->deName[0] == SLOT_DELETED)
                    continue;
                else if (dirp->deAttributes == ATTR_WIN95)
                    continue;
                else if (dirp->deAttributes & ATTR_VOLUME) {
                    strncpy(diskLabel, dirp->deName, LABEL_LENGTH);
                    finished = true;
                    break;
                }
            }	/* j */
		if (finished == true)
            break;
        }	/* i */
    }
    else {	/* FAT32 */
        u_int32_t cluster;
        u_int32_t bytesPerCluster;
        u_int8_t *rootDirBuffer;
        off_t readOffset;
        
        bytesPerCluster = (u_int32_t) bps * (u_int32_t) spc;
        rootDirBuffer = malloc(bytesPerCluster);
        cluster = getulong(b710->bpbRootClust);
        
        finished = false;
        while (!finished && cluster >= CLUST_FIRST && cluster < CLUST_RSRVD)
        {
            /* Find sector where clusters start */
            readOffset = getushort(b710->bpbResSectors) + (b710->bpbFATs * getulong(b710->bpbBigFATsecs));
            /* Find sector where "cluster" starts */
            readOffset += ((off_t) cluster - CLUST_FIRST) * (off_t) spc;
            /* Convert to byte offset */
            readOffset *= (off_t) bps;
            
            /* Read in "cluster" */
            safe_read(fd, rootDirBuffer, bytesPerCluster, readOffset);
            dirp = (struct dosdirentry *) rootDirBuffer;
            
            /* Examine each directory entry in this cluster */
            for (i=0; i < bytesPerCluster; i += sizeof(struct dosdirentry), dirp++)
            {
                if (dirp->deName[0] == SLOT_EMPTY) {
                    finished = true;	// Reached end of directory (never used entry)
                    break;
                }
                else if (dirp->deName[0] == SLOT_DELETED)
                    continue;
                else if (dirp->deAttributes == ATTR_WIN95)
                    continue;
                else if (dirp->deAttributes & ATTR_VOLUME) {
                    strncpy(diskLabel, dirp->deName, LABEL_LENGTH);
                    finished = true;
                    break;
                }
            }
            if (finished)
                break;

            /* Find next cluster in the chain by reading the FAT */
            
            /* Find first sector of FAT */
            readOffset = getushort(b710->bpbResSectors);
            /* Find sector containing "cluster" entry in FAT */
            readOffset += (cluster * 4) / bps;
            /* Convert to byte offset */
            readOffset *= bps;
            
            /* Read one sector of the FAT */
            safe_read(fd, rootDirBuffer, bps, readOffset);
            
            cluster = getulong(rootDirBuffer + ((cluster * 4) % bps));
            cluster &= 0x0FFFFFFF;	// ignore reserved upper bits
        }
        free(rootDirBuffer);
    }	/* rootDirSectors */

	/* else look in the boot blocks */
    if (diskLabel[0] == 0) {
        if (getushort(b50->bpbRootDirEnts) == 0) { /* Its a FAT32 */
            if (((struct extboot *)bsp->bs710.bsExt)->exBootSignature == EXBOOTSIG) {
            	strncpy(diskLabel, ((struct extboot *)bsp->bs710.bsExt)->exVolumeLabel, LABEL_LENGTH);
			}
        }
        else if (((struct extboot *)bsp->bs50.bsExt)->exBootSignature == EXBOOTSIG) {
            strncpy(diskLabel, ((struct extboot *)bsp->bs50.bsExt)->exVolumeLabel, LABEL_LENGTH);
        }
    }

    fs_set_label_file(diskLabel);

    safe_close(fd);

    return(FSUR_RECOGNIZED);
}


static int fs_mount(char *devpath, char *mount_point, int removable, int writable, int suid, int dev) {
    const char *kextargs[] = {KEXTLOAD_COMMAND, FS_KEXT_DIR, NULL};
    const char *mountargs[] = {MOUNT_COMMAND, READWRITE_OPT, "-o", SUID_OPT, "-o",
        DEV_OPT, "-t", FS_TYPE, devpath, mount_point, NULL};

    if (! writable)
        mountargs[1] = READONLY_OPT;

    if (! suid)
        mountargs[3] = NOSUID_OPT;

    if (! dev)
        mountargs[5] = NODEV_OPT;

    if (checkLoadable())
        safe_execv(kextargs); /* better here than in mount_udf */
    safe_execv(mountargs);
    ret = FSUR_IO_SUCCESS;

    return ret;
}

static int fs_unmount(char *devpath) {
        const char *umountargs[] = {UMOUNT_COMMAND, devpath, NULL};

        safe_execv(umountargs);
        return(FSUR_IO_SUCCESS);
}



/*
 * Begin Filesystem-specific code
 */
static int fs_label(char *devpath, char *volName)
{
        int fd;
        union bootsector *bsp;
        struct byte_bpb33 *b33;
        struct byte_bpb50 *b50;
        u_int16_t	bps;
        u_int8_t	spc;
        char		tmplabel[LABEL_LENGTH], label[LABEL_LENGTH];
        char 		buf[MAX_DOS_BLOCKSIZE];
        CFStringRef 	cfstr;


        /* First normalize the label */
        if (volName == NULL)
                errx(EX_USAGE, "No label was given");

        /* Convert it from UTF-8 */
        cfstr = CFStringCreateWithCString(kCFAllocatorDefault, volName, kCFStringEncodingUTF8);
        if (cfstr == NULL)
                errx(EX_DATAERR, "Bad UTF8 Name");
        if (! CFStringGetCString(cfstr, tmplabel, LABEL_LENGTH, kCFStringEncodingWindowsLatin1))
               errx(EX_DATAERR, "Could not convert to DOS Latin1");
        CFRelease(cfstr);

        if (! oklabel(tmplabel))
                errx(EX_DATAERR, "Label has illegal characters");

        /* Finall format it */
        mklabel(label, tmplabel);

        
        fd = safe_open(devpath, O_RDWR, 0);

        /*
         * Read the boot sector of the filesystem, and then check the
         * boot signature.  If not a dos boot sector then error out.
         *
         * NOTE: 4096 is a maximum sector size in current...
         */
        safe_read(fd, buf, MAX_DOS_BLOCKSIZE, 0);

        bsp = (union bootsector *)buf;
        b33 = (struct byte_bpb33 *)bsp->bs33.bsBPB;
        b50 = (struct byte_bpb50 *)bsp->bs50.bsBPB;

        if (bsp->bs50.bsBootSectSig0 != BOOTSIG0
            || bsp->bs50.bsBootSectSig1 != BOOTSIG1) {
                return(FSUR_UNRECOGNIZED);
        }

        /* Both partitions tables and boot sectors pass the above test, do do some more */

        /* We only work with 512, 1024, 2048, and 4096 byte sectors */
        bps = getushort(b33->bpbBytesPerSec);
        if ((bps < 0x200) || (bps & (bps - 1)) || (bps > MAX_DOS_BLOCKSIZE))
                return(FSUR_UNRECOGNIZED);

        /* Check to make sure valid sectors per cluster */
        spc = b33->bpbSecPerClust;
        if ((spc == 0 ) || (spc & (spc - 1)))
                return(FSUR_UNRECOGNIZED);

        /* we know this disk, find the volume label */
        if (getushort(b50->bpbRootDirEnts) == 0) {
                /* Its a FAT32 */
                strncpy(((struct extboot *)bsp->bs710.bsExt)->exVolumeLabel, label, LABEL_LENGTH);
        }
        else if (((struct extboot *)bsp->bs50.bsExt)->exBootSignature == EXBOOTSIG) {
                strncpy(((struct extboot *)bsp->bs50.bsExt)->exVolumeLabel, label, LABEL_LENGTH);
        }


        safe_write(fd, buf, MAX_DOS_BLOCKSIZE, 0);

        safe_close(fd);

        return(FSUR_IO_SUCCESS);
}


static CFStringEncoding GetDefaultDOSEncoding(void)
{
	CFStringEncoding encoding;
    struct passwd *passwdp;
	int fd;
	size_t size;
	char buffer[MAXPATHLEN + 1];

	/*
	 * Get a default (Mac) encoding.  We use the CFUserTextEncoding
	 * file since CFStringGetSystemEncoding() always seems to
	 * return 0 when msdos.util is executed via disk arbitration.
	 */
	encoding = kCFStringEncodingMacRoman;	/* Default to Roman/Latin */
	if ((passwdp = getpwuid(getuid()))) {
		strcpy(buffer, passwdp->pw_dir);
		strcat(buffer, "/.CFUserTextEncoding");

		if ((fd = open(buffer, O_RDONLY, 0)) > 0) {
			size = read(fd, buffer, MAXPATHLEN);
			buffer[(size < 0 ? 0 : size)] = '\0';
			close(fd);
			encoding = strtol(buffer, NULL, 0);
		}
	}

	/* Convert the Mac encoding to a DOS/Windows encoding. */
	switch (encoding) {
		case kCFStringEncodingMacRoman:
			encoding = kCFStringEncodingDOSLatin1;
			break;
		case kCFStringEncodingMacJapanese:
			encoding = kCFStringEncodingDOSJapanese;
			break;
		case kCFStringEncodingMacChineseTrad:
			encoding = kCFStringEncodingDOSChineseTrad;
			break;
		case kCFStringEncodingMacKorean:
			encoding = kCFStringEncodingDOSKorean;
			break;
		case kCFStringEncodingMacArabic:
			encoding = kCFStringEncodingDOSArabic;
			break;
		case kCFStringEncodingMacHebrew:
			encoding = kCFStringEncodingDOSHebrew;
			break;
		case kCFStringEncodingMacGreek:
			encoding = kCFStringEncodingDOSGreek;
			break;
		case kCFStringEncodingMacCyrillic:
		case kCFStringEncodingMacUkrainian:
			encoding = kCFStringEncodingDOSCyrillic;
			break;
		case kCFStringEncodingMacThai:
			encoding = kCFStringEncodingDOSThai;
			break;
		case kCFStringEncodingMacChineseSimp:
			encoding = kCFStringEncodingDOSChineseSimplif;
			break;
		case kCFStringEncodingMacCentralEurRoman:
		case kCFStringEncodingMacCroatian:
		case kCFStringEncodingMacRomanian:
			encoding = kCFStringEncodingDOSLatin2;
			break;
		case kCFStringEncodingMacTurkish:
			encoding = kCFStringEncodingDOSTurkish;
			break;
		case kCFStringEncodingMacIcelandic:
			encoding = kCFStringEncodingDOSIcelandic;
			break;
		case kCFStringEncodingMacFarsi:
			encoding = kCFStringEncodingDOSArabic;
			break;
		default:
			encoding = kCFStringEncodingInvalidId;	/* Error: no corresponding Windows encoding */
			break;
	}
	
	return encoding;
}

/* Set the name of this file system */
static void fs_set_label_file(char *labelPtr)
{
	int				i;
    CFStringEncoding encoding;
    unsigned char	label[LABEL_LENGTH+1];
    unsigned char	labelUTF8[LABEL_LENGTH*3];
    CFStringRef 	cfstr;

	/* Make a local copy of the label */
	strncpy(label, labelPtr, LABEL_LENGTH);
	label[LABEL_LENGTH] = 0;

	/* Convert leading 0x05 to 0xE5 for multibyte languages like Japanese */
	if (label[0] == 0x05)
		label[0] = 0xE5;

	/* Check for illegal characters */
	if (!oklabel(label))
		label[0] = 0;

	/* Remove any trailing spaces */
	for (i=LABEL_LENGTH-1; i>=0; --i) {
		if (label[i] == ' ')
			label[i] = 0;
		else
			break;
	}

    /* Convert it to UTF-8 */
    encoding = GetDefaultDOSEncoding();
    cfstr = CFStringCreateWithCString(NULL, label, encoding);
    if (cfstr == NULL && encoding != kCFStringEncodingDOSLatin1)
        cfstr = CFStringCreateWithCString(NULL, label, kCFStringEncodingDOSLatin1);
	if (cfstr == NULL)
		labelUTF8[0] = 0;
	else {
		(void) CFStringGetCString(cfstr, labelUTF8, sizeof(labelUTF8), kCFStringEncodingUTF8);
		CFRelease(cfstr);
	}

    /* At this point, labelUTF8 should contain a correctly formatted name (possibly empty) */
    write(1, labelUTF8, strlen(labelUTF8));
}

/*
 * Based from newfs_msdos....to support the same 'functionality'...thanks
 */
        	
/*
 * Check a volume label.
 */
static int
oklabel(const char *src)
{
    int c, i;

    for (i = 0, c = 0; i <= 11; i++) {
        c = (u_char)*src++;
        if (c < ' ' + !i || strchr("\"*+,./:;<=>?[\\]|", c))
            break;
    }
    return i && !c;
}

/*
 * Make a volume label.
 */
static void
mklabel(u_int8_t *dest, const char *src)
{
    int c, i;

    for (i = 0; i < 11; i++) {
	c = *src ? toupper(*src++) : ' ';
	*dest++ = !i && c == '\xe5' ? 5 : c;
    }
}

static int
safe_open(char *path, int flags, mode_t mode)
{
	int fd = open(path, flags, mode);

	if (fd < 0) {
		fprintf(stderr, "%s: open %s failed, %s\n", progname, path,
			strerror(errno));
		exit(FSUR_IO_FAIL);
	}
	return(fd);
}


static void
safe_close(int fd)
{
	if (close(fd)) {
		fprintf(stderr, "%s: safe_close failed, %s\n", progname,
			strerror(errno));
		exit(FSUR_IO_FAIL);
	}
}

void
safe_execv(char *args[])
{
	int		pid;
	union wait	status;

	pid = fork();
	if (pid == 0) {
		(void)execv(args[0], args);
		fprintf(stderr, "%s: execv %s failed, %s\n", progname, args[0],
			strerror(errno));
		exit(FSUR_IO_FAIL);
	}
	if (pid == -1) {
		fprintf(stderr, "%s: fork failed, %s\n", progname,
			strerror(errno));
		exit(FSUR_IO_FAIL);
	}
	if (wait4(pid, (int *)&status, 0, NULL) != pid) {
		fprintf(stderr, "%s: BUG executing %s command\n", progname,
			args[0]);
		exit(FSUR_IO_FAIL);
	} else if (!WIFEXITED(status)) {
		fprintf(stderr, "%s: %s command aborted by signal %d\n",
			progname, args[0], WTERMSIG(status));
		exit(FSUR_IO_FAIL);
	} else if (WEXITSTATUS(status)) {
		fprintf(stderr, "%s: %s command failed, exit status %d: %s\n",
			progname, args[0], WEXITSTATUS(status),
			strerror(WEXITSTATUS(status)));
		exit(FSUR_IO_FAIL);
	}
}


static void
safe_unlink(char *path)
{
	if (unlink(path) && errno != ENOENT) {
		fprintf(stderr, "%s: unlink %s failed, %s\n", progname, path,
			strerror(errno));
		exit(FSUR_IO_FAIL);
	}
}


static void
safe_read(int fd, char *buf, int nbytes, off_t off)
{
	if (lseek(fd, off, SEEK_SET) == -1) {
		fprintf(stderr, "%s: device seek error @ %qu, %s\n", progname,
			off, strerror(errno));
		exit(FSUR_IO_FAIL);
	}
	if (read(fd, buf, nbytes) != nbytes) {
		fprintf(stderr, "%s: device safe_read error @ %qu, %s\n", progname,
			off, strerror(errno));
		exit(FSUR_IO_FAIL);
	}
}


void
safe_write(int fd, char *buf, int nbytes, off_t off)
{
        if (lseek(fd, off, SEEK_SET) == -1) {
                fprintf(stderr, "%s: device seek error @ %qu, %s\n", progname,
                        off, strerror(errno));
                exit(FSUR_IO_FAIL);
        }
        if (write(fd, buf, nbytes) != nbytes) {
                fprintf(stderr, "%s: write failed, %s\n", progname,
                        strerror(errno));
                exit(FSUR_IO_FAIL);
        }
}


/* Return non-zero if the file system is not yet loaded. */
static int checkLoadable(void)
{
	int error;
	struct vfsconf vfc;
	
	error = getvfsbyname(FS_TYPE, &vfc);

	return error;
}



#ifdef DEBUG
static void
report_exit_code(int ret)
{
    printf("...ret = %d\n", ret);

    switch (ret) {
    case FSUR_RECOGNIZED:
	printf("File system recognized; a mount is possible.\n");
	break;
    case FSUR_UNRECOGNIZED:
	printf("File system unrecognized; a mount is not possible.\n");
	break;
    case FSUR_IO_SUCCESS:
	printf("Mount, unmount, or repair succeeded.\n");
	break;
    case FSUR_IO_FAIL:
	printf("Unrecoverable I/O error.\n");
	break;
    case FSUR_IO_UNCLEAN:
	printf("Mount failed; file system is not clean.\n");
	break;
    case FSUR_INVAL:
	printf("Invalid argument.\n");
	break;
    case FSUR_LOADERR:
	printf("kern_loader error.\n");
	break;
    case FSUR_INITRECOGNIZED:
	printf("File system recognized; initialization is possible.\n");
	break;
    }
}
#endif

/* end of DOS.util.c */
