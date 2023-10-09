/*
 * filefrag.c -- report if a particular file is fragmented
 *
 * Copyright 2003 by Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */
#ifndef __linux__
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
    fputs("This program is only supported on Linux!\n", stderr);
    exit(EXIT_FAILURE);
}
#else
#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern char *optarg;
extern int optind;
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <linux/fd.h>
#include <ext2fs/ext2_types.h>
// #include <linux/fiemap.h>
#include "fiemap.h"

int verbose = 1;
int no_bs = 0;		/* Don't use the files blocksize, use 1K blocksize */
int sync_file = 0;	/* fsync file before getting the mapping */
int xattr_map = 0;	/* get xattr mapping */
int force_bmap = 0;
int logical_width = 12;
int physical_width = 14;
unsigned long long filesize;
#define FILEFRAG_FIEMAP_FLAGS_COMPAT (FIEMAP_FLAG_SYNC | FIEMAP_FLAG_XATTR)
#define FIBMAP		_IO(0x00, 1)	/* bmap access */
#define FIGETBSZ	_IO(0x00, 2)	/* get the block size used for bmap */
#define FS_IOC_FIEMAP	_IOWR('f', 11, struct fiemap)
#define	EXT4_EXTENTS_FL			0x00080000 /* Inode uses extents */
#define	EXT3_IOC_GETFLAGS		_IOR('f', 1, long)
#define F2FS	0xf2f52010
#define EXT2_3_4_FS	0xef53


static int int_log2(int arg)
{
	int     l = 0;
	arg >>= 1;
	while (arg) {
		l++;
		arg >>= 1;
	}
	return l;
}
static int int_log10(unsigned long long arg)
{
	int     l = 0;
	arg = arg / 10;
	while (arg) {
		l++;
		arg = arg / 10;
	}
	return l;
}
static unsigned int div_ceil(unsigned int a, unsigned int b)
{
	if (!a)
		return 0;
	return ((a - 1) / b) + 1;
}
static int get_bmap(int fd, unsigned long block, unsigned long *phy_blk)
{
	int	ret;
	unsigned int b;
	b = block;
	ret = ioctl(fd, FIBMAP, &b); /* FIBMAP takes pointer to integer */
	if (ret < 0) {
		if (errno == EPERM) {
			fprintf(stderr, "No permission to use FIBMAP ioctl; "
				"must have root privileges\n");
			exit(1);
		}
		perror("FIBMAP");
	}
	*phy_blk = b;
	return ret;
}
static void print_extent_info(struct fiemap_extent *fm_extent, int cur_ex,
			      unsigned long long expected, int blk_shift)
{
	__u64 phy_blk;
	unsigned long long logical_blk;
	unsigned long ext_len;
	char flags[256] = "";
	/* For inline data all offsets should be in terms of bytes, not blocks */
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_INLINE)
		blk_shift = 0;
	ext_len = fm_extent->fe_length >> blk_shift;				// 算术右移了，所以是以4KB为单位
	logical_blk = fm_extent->fe_logical >> blk_shift;
	phy_blk = fm_extent->fe_physical >> blk_shift;	
	if (fm_extent->fe_flags & FIEMAP_EXTENT_UNKNOWN)
		strcat(flags, "unknown,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DELALLOC)
		strcat(flags, "delalloc,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_ENCRYPTED)
		strcat(flags, "encrypted,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_NOT_ALIGNED)
		strcat(flags, "not_aligned,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_INLINE)
		strcat(flags, "inline,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_TAIL)
		strcat(flags, "tail_packed,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN)
		strcat(flags, "unwritten,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_MERGED)
		strcat(flags, "merged,");
	if (fm_extent->fe_logical + fm_extent->fe_length >= filesize)
		strcat(flags, "eof,");
	/* Remove trailing comma, if any */
	if (flags[0])
		flags[strlen(flags) - 1] = '\0';
	if (expected)
		printf("%4d %*llu %*llu %*llu %6lu %s\n",
		       cur_ex, logical_width, logical_blk,
		       physical_width, phy_blk, physical_width, expected,
		       ext_len, flags);
	else
		printf("%4d %*llu %*llu %*s %6lu %s\n",
		       cur_ex, logical_width, logical_blk,
		       physical_width, phy_blk, physical_width, "",
		       ext_len, flags);
}

// 原版
static int filefrag_fiemap(int fd, int blk_shift, int *num_extents)
{
	char buf[4096] = "";
	struct fiemap *fiemap = (struct fiemap *)buf;
	struct fiemap_extent *fm_ext = &fiemap->fm_extents[0];
	int count = (sizeof(buf) - sizeof(*fiemap)) /		// buffer剩余可以放fiemap_extent的数量
			sizeof(struct fiemap_extent);
	unsigned long long last_blk = 0;
	unsigned long flags = 0;
	unsigned int i;
	static int fiemap_incompat_printed;
	int fiemap_header_printed = 0;
	int tot_extents = 1, n = 0;
	int last = 0;
	int rc;
	fiemap->fm_length = ~0ULL;
	memset(fiemap, 0, sizeof(struct fiemap));
	if (!verbose)
		count = 0;
	if (sync_file)
		flags |= FIEMAP_FLAG_SYNC;
	if (xattr_map)
		flags |= FIEMAP_FLAG_XATTR;
	do {
		fiemap->fm_length = ~0ULL;
		fiemap->fm_flags = flags;
		fiemap->fm_extent_count = count;
		rc = ioctl(fd, FS_IOC_FIEMAP, (unsigned long) fiemap);
		if (rc < 0) {
			if (errno == EBADR && fiemap_incompat_printed == 0) {
				printf("FIEMAP failed with unsupported "
				       "flags %x\n", fiemap->fm_flags);
				fiemap_incompat_printed = 1;
			}
			return rc;
		}
		if (verbose && !fiemap_header_printed) {
			printf(" ext %*s %*s %*s length flags\n", logical_width,
			       "logical", physical_width, "physical",
			       physical_width, "expected");
			fiemap_header_printed = 1;
		}
		if (!verbose) {
			*num_extents = fiemap->fm_mapped_extents;
			goto out;
		}
		/* If 0 extents are returned, then more ioctls are not needed */
		if (fiemap->fm_mapped_extents == 0)
			break;
		for (i = 0; i < fiemap->fm_mapped_extents; i++) {
			__u64 phy_blk, logical_blk;
			unsigned long ext_len;
			phy_blk = fm_ext[i].fe_physical >> blk_shift;
			ext_len = fm_ext[i].fe_length >> blk_shift;
			logical_blk = fm_ext[i].fe_logical >> blk_shift;
			if (logical_blk && phy_blk != last_blk + 1)
				tot_extents++;
			else
				last_blk = 0;
			print_extent_info(&fm_ext[i], n, last_blk, blk_shift);
			last_blk = phy_blk + ext_len - 1;
			if (fm_ext[i].fe_flags & FIEMAP_EXTENT_LAST)
				last = 1;
			n++;
		}
		fiemap->fm_start = (fm_ext[i-1].fe_logical +
				    fm_ext[i-1].fe_length);
	} while (last == 0);
	*num_extents = tot_extents;
out:
	return 0;
}

// 改写，以输出extent信息
static int filefrag_fiemap2(int fd, int blk_shift, int *num_extents, extent_List* extents)
{
	extent_Node *tail = &(extents->headnode);
	
	char buf[4096] = "";

	struct fiemap *fiemap = (struct fiemap *)buf;
	struct fiemap_extent *fm_ext = &fiemap->fm_extents[0];
	int count = (sizeof(buf) - sizeof(*fiemap)) /		// buffer剩余可以放fiemap_extent的数量
			sizeof(struct fiemap_extent);
	
	unsigned long long last_blk = 0;
	unsigned long flags = 0;
	unsigned int i;
	static int fiemap_incompat_printed;
	int fiemap_header_printed = 0;
	int tot_extents = 1, n = 0;
	int last = 0;
	int rc;
	fiemap->fm_length = ~0ULL;
	memset(fiemap, 0, sizeof(struct fiemap));
	if (!verbose)
		count = 0;
	if (sync_file)
		flags |= FIEMAP_FLAG_SYNC;
	if (xattr_map)
		flags |= FIEMAP_FLAG_XATTR;
	do {
		fiemap->fm_length = ~0ULL;
		fiemap->fm_flags = flags;
		fiemap->fm_extent_count = count;
		rc = ioctl(fd, FS_IOC_FIEMAP, (unsigned long) fiemap);
		if (rc < 0) {
			if (errno == EBADR && fiemap_incompat_printed == 0) {
				printf("FIEMAP failed with unsupported "
				       "flags %x\n", fiemap->fm_flags);
				fiemap_incompat_printed = 1;
			}
			return rc;
		}
		// if (verbose && !fiemap_header_printed) {
		// 	printf(" ext %*s %*s %*s length flags\n", logical_width,
		// 	       "logical", physical_width, "physical",
		// 	       physical_width, "expected");
		// 	fiemap_header_printed = 1;
		// }
		if (!verbose) {
			*num_extents = fiemap->fm_mapped_extents;
			goto out;
		}
		/* If 0 extents are returned, then more ioctls are not needed */
		if (fiemap->fm_mapped_extents == 0)
			break;
		
		extents->size += fiemap->fm_mapped_extents;

		for (i = 0; i < fiemap->fm_mapped_extents; i++) {
			__u64 phy_blk, logical_blk;
			unsigned long ext_len;
			phy_blk = fm_ext[i].fe_physical >> blk_shift;
			ext_len = fm_ext[i].fe_length >> blk_shift;
			logical_blk = fm_ext[i].fe_logical >> blk_shift;
			if (logical_blk && phy_blk != last_blk + 1)			// 如果逻辑块号不为0（不是文件开肉=头），且物理块不与上次的连续，则是另外一个extent
				tot_extents++;
			else
				last_blk = 0;

			extent_Node *p = (extent_Node *)malloc(sizeof(extent_Node));
			p->extent.fe_flags = fm_ext[i].fe_flags;
			p->extent.fe_length = fm_ext[i].fe_length;
			p->extent.fe_logical = fm_ext[i].fe_logical;
			p->extent.fe_physical = fm_ext[i].fe_physical;
			p->next = NULL;
			tail->next = p;
			tail = p;

			//print_extent_info(&fm_ext[i], n, last_blk, blk_shift);
			last_blk = phy_blk + ext_len - 1;
			if (fm_ext[i].fe_flags & FIEMAP_EXTENT_LAST)
				last = 1;
			n++;
		}
		fiemap->fm_start = (fm_ext[i-1].fe_logical +
				    fm_ext[i-1].fe_length);
	} while (last == 0);
	*num_extents = tot_extents;
	// extents->size = tot_extents;
out:
	return 0;
}

#define EXT2_DIRECT	12
static void frag_report(const char *filename)
{
	struct statfs	fsinfo;
#ifdef HAVE_FSTAT64
	struct stat64	fileinfo;
#else
	struct stat	fileinfo;
#endif
	int		bs;
	long		fd;
	unsigned long	block, last_block = 0, numblocks, i, count;
	long		bpib;	/* Blocks per indirect block */
	long		cylgroups;
	int		num_extents = 0, expected;
	int		is_ext2 = 0;
	static int	once = 1;
	unsigned int	flags;
	int rc;
#ifdef HAVE_OPEN64
	fd = open64(filename, O_RDONLY);
#else
	fd = open(filename, O_RDONLY);
#endif
	if (fd < 0) {
		perror("open");
		return;
	}
	if (statfs(filename, &fsinfo) < 0) {			// https://man7.org/linux/man-pages/man2/statfs.2.html	
		perror("statfs");
		return;
	}
#ifdef HAVE_FSTAT64
	if (stat64(filename, &fileinfo) < 0) {
#else
	if (stat(filename, &fileinfo) < 0) {
#endif
		perror("stat");
		return;
	}
	if (ioctl(fd, EXT3_IOC_GETFLAGS, &flags) < 0)
		flags = 0;
	if (!(flags & EXT4_EXTENTS_FL) &&
	    ((fsinfo.f_type == 0xef51) || (fsinfo.f_type == 0xef52) ||
	     (fsinfo.f_type == 0xef53)))								// 没有启用Extents特性，并且是ext2、ext3、ext4
		is_ext2++;
	if (verbose && once)
		printf("Filesystem type is: %lx\n",
		       (unsigned long) fsinfo.f_type);
	cylgroups = div_ceil(fsinfo.f_blocks, fsinfo.f_bsize*8);
	if (verbose && is_ext2 && once)
		printf("Filesystem cylinder groups is approximately %ld\n",
		       cylgroups);

	physical_width = int_log10(fsinfo.f_blocks);
	if (physical_width < 8)
		physical_width = 8;
	// 获取block_size
	if (ioctl(fd, FIGETBSZ, &bs) < 0) { /* FIGETBSZ takes an int */
		perror("FIGETBSZ");
		close(fd);
		return;
	}
	if (no_bs)
		bs = 1024;
	bpib = bs / 4;
	numblocks = (fileinfo.st_size + (bs-1)) / bs;		// 向上取整
	
	logical_width = int_log10(numblocks);
	if (logical_width < 7)
		logical_width = 7;
	
	filesize = (long long)fileinfo.st_size;
	if (verbose)
		printf("File size of %s is %lld (%ld block%s, blocksize %d)\n",
		       filename, (long long) fileinfo.st_size, numblocks,
		       numblocks == 1 ? "" : "s", bs);
	if (force_bmap ||
	    filefrag_fiemap(fd, int_log2(bs), &num_extents) != 0) {
		for (i = 0, count = 0; i < numblocks; i++) {
			if (is_ext2 && last_block) {
				if (((i-EXT2_DIRECT) % bpib) == 0)
					last_block++;
				if (((i-EXT2_DIRECT-bpib) % (bpib*bpib)) == 0)
					last_block++;
				if (((i-EXT2_DIRECT-bpib-bpib*bpib) %
				     (((__u64) bpib)*bpib*bpib)) == 0)
					last_block++;
			}
			rc = get_bmap(fd, i, &block);
			if (block == 0)
				continue;
			if (!num_extents)
				num_extents++;
			count++;
			if (last_block && (block != last_block+1) ) {
				if (verbose)
					printf("Discontinuity: Block %ld is at "
					       "%lu (was %lu)\n",
					       i, block, last_block+1);
				num_extents++;
			}
			last_block = block;
		}
	}
	if (num_extents == 1)
		printf("%s: 1 extent found", filename);
	else
		printf("%s: %d extents found", filename, num_extents);
	expected = (count/((bs*8)-(fsinfo.f_files/8/cylgroups)-3))+1;
	if (is_ext2 && expected < num_extents)
		printf(", perfection would be %d extent%s\n", expected,
			(expected>1) ? "s" : "");
	else
		fputc('\n', stdout);
	close(fd);
	once = 0;
}

void get_extent_info(const char *filename, extent_List* extents, unsigned long long* arg_filesize){

	struct statfs	fsinfo;
#ifdef HAVE_FSTAT64
	struct stat64	fileinfo;
#else
	struct stat	fileinfo;
#endif
	int		bs;
	long	fd;
	unsigned long	block, last_block = 0, numblocks, i, count;
	long		bpib;	/* Blocks per indirect block */
	long		cylgroups;
	int		num_extents = 0, expected;
	int		is_ext2 = 0;
	static int	once = 1;
	unsigned int	flags;
	int rc;
#ifdef HAVE_OPEN64
	fd = open64(filename, O_RDONLY);
#else
	fd = open(filename, O_RDONLY);
#endif
	if (fd < 0) {
		perror("open");
		return;
	}
	if (statfs(filename, &fsinfo) < 0) {			// https://man7.org/linux/man-pages/man2/statfs.2.html	
		perror("statfs");
		return;
	}
#ifdef HAVE_FSTAT64
	if (stat64(filename, &fileinfo) < 0) {
#else
	if (stat(filename, &fileinfo) < 0) {			// https://linux.die.net/man/2/stat
#endif
		perror("stat");
		return;
	}
	if (ioctl(fd, EXT3_IOC_GETFLAGS, &flags) < 0)	// https://man7.org/linux/man-pages/man2/ioctl.2.html
		flags = 0;
	if (!(flags & EXT4_EXTENTS_FL) &&
	    ((fsinfo.f_type == 0xef51) || (fsinfo.f_type == 0xef52) ||
	     (fsinfo.f_type == 0xef53)))				// 没有启用Extents特性，并且是ext2、ext3、ext4
		is_ext2++;

	// if (verbose && once) 
	// 打印对应文件系统类型
	char *fs_type;
	switch ((unsigned long)fsinfo.f_type)
	{
	case F2FS:
		fs_type = "F2FS";
		break;
	case EXT2_3_4_FS:
		fs_type = "EXT_FS";
		break;
	default:
		fs_type = "Unknown";
		break;
	}
	printf("Filesystem type is: %s\n",fs_type);

	cylgroups = div_ceil(fsinfo.f_blocks, fsinfo.f_bsize*8);
	//if (verbose && is_ext2 && once)
	//	printf("Filesystem cylinder groups is approximately %ld\n",
	//	       cylgroups);

	physical_width = int_log10(fsinfo.f_blocks);		// 依据文件系统的总的块数目来动态控制输出字符的对齐
	if (physical_width < 8)
		physical_width = 8;

	// 获取block_size
	if (ioctl(fd, FIGETBSZ, &bs) < 0) { /* FIGETBSZ takes an int */
		perror("FIGETBSZ");
		close(fd);
		return;
	}
	//if (no_bs)
	//	bs = 1024;
	bpib = bs / 4;
	numblocks = (fileinfo.st_size + (bs-1)) / bs;		// 向上取整（表示文件大小所占的4KB块数目）
	
	logical_width = int_log10(numblocks);				// 依据块的数目大小来动态控制输出字符的对齐
	if (logical_width < 7)
		logical_width = 7;
	
	filesize = (long long)fileinfo.st_size;
	*arg_filesize = filesize;
	//if (verbose)

	printf("File size of %s is %lld (%ld block%s, blocksize %d)\n",		// 打印文件信息（对应文件的大小(B)、块数目(4KB)，块大小）
		    filename, (long long) fileinfo.st_size, numblocks,
		    numblocks == 1 ? "" : "s", bs);
	
	// 获取extent信息，如果fiemap获取失败则用fibmap获取

	if (force_bmap ||
	    filefrag_fiemap2(fd, int_log2(bs), &num_extents, extents) != 0) {
		for (i = 0, count = 0; i < numblocks; i++) {
			if (is_ext2 && last_block) {
				if (((i-EXT2_DIRECT) % bpib) == 0)
					last_block++;
				if (((i-EXT2_DIRECT-bpib) % (bpib*bpib)) == 0)
					last_block++;
				if (((i-EXT2_DIRECT-bpib-bpib*bpib) %
				     (((__u64) bpib)*bpib*bpib)) == 0)
					last_block++;
			}
			rc = get_bmap(fd, i, &block);
			if (block == 0)
				continue;
			if (!num_extents)
				num_extents++;
			count++;
			if (last_block && (block != last_block+1) ) {
				if (verbose)
					printf("Discontinuity: Block %ld is at "
					       "%lu (was %lu)\n",
					       i, block, last_block+1);
				num_extents++;
			}
			last_block = block;
		}
	}
	if (num_extents == 1)
		printf("%s: 1 extent found", filename);
	else
		printf("%s: %d extents found", filename, num_extents);
	expected = (count/((bs*8)-(fsinfo.f_files/8/cylgroups)-3))+1;
	if (is_ext2 && expected < num_extents)
		printf(", perfection would be %d extent%s\n", expected,
			(expected>1) ? "s" : "");
	else
		fputc('\n', stdout);
	close(fd);
	once = 0;
}



/*
static void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [-Bbvsx] file ...\n", progname);
	exit(1);
}
*/

/*
int main(int argc, char**argv)
{
	char **cpp;
	int c;
	while ((c = getopt(argc, argv, "Bbsvx")) != EOF)
		switch (c) {
		case 'B':
			force_bmap++;
//			printf("B%c\n",c);
			break;
		case 'b':
			no_bs++;
//			printf("b%c\n",c);
			break;
		case 'v':
			verbose++;
//			printf("v%c\n",c);
			break;
		case 's':
			sync_file++;
//			printf("s%c\n",c);
			break;
		case 'x':
			xattr_map++;
//			printf("x%c\n",c);
			break;
		default:
			printf("default%c\n",c);
			usage(argv[0]);
			break;
		}
//		printf("zzzzz%d\n",argc);
//		for(int i = 0; i < argc; i++)
//			printf("qqqq%s\n",argv[i]);
//		printf("wwwwww%d\n",optind);
	if (optind == argc)
		usage(argv[0]);
	for (cpp=argv+optind; *cpp; cpp++)
		frag_report(*cpp);
	return 0;
}
*/
#endif