/*
Written by Radek Podgorny

This is offered under a BSD-style license. This means you can use the code for whatever you desire in any way you may want but you MUST NOT forget to give me appropriate credits when spreading your work which is based on mine. Something like "original implementation by Radek Podgorny" should be fine.
*/

#ifdef linux
	/* For pread()/pwrite() */
	#define _XOPEN_SOURCE 500
#endif

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/statfs.h>

#ifdef HAVE_SETXATTR
	#include <sys/xattr.h>
#endif

#include "unionfs.h"
#include "opts.h"
#include "cache.h"
#include "stats.h"
#include "debug.h"


int findroot(const char *path) {
	int i = cache_lookup(path);

	if (i != -1) return i;

	for (i = 0; i < nroots; i++) {
		char p[PATHLEN_MAX];
		snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

		struct stat stbuf;
		int res = lstat(p, &stbuf);

		if (res == 0) {
			cache_save(path, i);
			return i;
		}
	}

	return -1;
}

/* Try to find root when we cut the last path element */
int findroot_cutlast(const char *path) {
	char* ri = rindex(path, '/'); //this char should always be found
	int len = ri - path;

	char p[PATHLEN_MAX];
	strncpy(p, path, len);
	p[len] = '\0';

	return findroot(p);
}

static int unionfs_access(const char *path, int mask) {
	DBG("access\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = access(p, mask);
	if (res == -1) {
		if (res == ENOENT) {
			// The user may have moved the file among roots
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = access(p, mask);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_chmod(const char *path, mode_t mode) {
	DBG("chmod\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = chmod(p, mode);
	if (res == -1) {
		if (errno == ENOENT) {
			// The user may have moved the file among roots
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = chmod(p, mode);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_chown(const char *path, uid_t uid, gid_t gid) {
	DBG("chown\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = lchown(p, uid, gid);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = lchown(p, uid, gid);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

/* flush may be called multiple times for an open file, this must not really close the file. This is important if used on a network filesystem like NFS which flush the data/metadata on close() */
static int unionfs_flush(const char *path, struct fuse_file_info *fi) {
	DBG("flush\n");

	int fd = dup(fi->fh);

	if (fd == -1) {
		// What to do now?
		if (fsync(fi->fh) == -1) return -EIO;
		return 0;
	}

	if (close(fd) == -1) return -errno;

	return 0;
}

/* Just a stub. This method is optional and can safely be left unimplemented */
static int unionfs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
	DBG("fsync\n");

	return 0;
}

static int unionfs_getattr(const char *path, struct stat *stbuf) {
	DBG("getattr\n");

	if (stats_enabled && strcmp(path, "/stats") == 0) {
		memset(stbuf, 0, sizeof(stbuf));
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = STATS_SIZE;
		return 0;
	}

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = lstat(p, stbuf);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = lstat(p, stbuf);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}


static int unionfs_link(const char *from, const char *to) {
	DBG("link\n");

	int i = findroot(to);
	if (i == -1) {
		if (errno == ENOENT) i = findroot_cutlast(to);
		if (i == -1) return -errno;
	}

	char t[PATHLEN_MAX];
	snprintf(t, PATHLEN_MAX, "%s%s", roots[i], to);

	int res = link(from, t);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(to);

			i = findroot(to);
			if (i == -1) return -errno;

			snprintf(t, PATHLEN_MAX, "%s%s", roots[i], to);

			res = link(from, t);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_mkdir(const char *path, mode_t mode) {
	DBG("mkdir\n");

	int i = findroot(path);
	if (i == -1) {
		if (errno == ENOENT) i = findroot_cutlast(path);
		if (i == -1) return -errno;
	}

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = mkdir(p, mode);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = mkdir(p, mode);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_mknod(const char *path, mode_t mode, dev_t rdev) {
	DBG("mknod\n");

	int i = findroot(path);
	if (i == -1) {
		if (errno == ENOENT) i = findroot_cutlast(path);
		if (i == -1) return -errno;
	}

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = mknod(p, mode, rdev);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = mknod(p, mode, rdev);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_open(const char *path, struct fuse_file_info *fi) {
	DBG("open\n");

	if (stats_enabled && strcmp(path, "/stats") == 0) {
		if ((fi->flags & 3) == O_RDONLY) return 0;
		return -EACCES;
	}

	int i = findroot(path);
	if (i == -1) {
		if (errno == ENOENT && fi->flags & O_CREAT) i = findroot_cutlast(path);
		if (i == -1) return -errno;
	}

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int fd = open(p, fi->flags);
	if (fd == -1) {
		if (errno == ENOENT) {
			// The user may have moved the file among roots
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;
		
			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			fd = open(p, fi->flags);
			if (fd == -1) return -errno;
		} else {
			return -errno;
		}
	}

	fi->fh = (unsigned long)fd;

	return 0;
}

static int unionfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	DBG("read\n");

	if (stats_enabled && strcmp(path, "/stats") == 0) {
		char out[STATS_SIZE] = "";
		stats_sprint(out);

		int s = size;
		if (offset < strlen(out)) {
			if (s > strlen(out)-offset) s = strlen(out)-offset;
			memcpy(buf, out+offset, s);
		} else {
			s = 0;
		}

		return s;
	}

	int res = pread(fi->fh, buf, size, offset);
	if (res == -1) return -errno;

	if (stats_enabled) stats_add_read(size);

	return res;
}

static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	DBG("readdir\n");

	int nadded = 0;
	char **added;
	added = malloc(1);

	int i = 0;
	for (i = 0; i < nroots; i++) {
		char p[PATHLEN_MAX];
		snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

		DIR *dp = opendir(p);
		if (dp == NULL) continue;

		struct dirent *de;
		while ((de = readdir(dp)) != NULL) {
			int j = 0;
			for (j = 0; j < nadded; j++) {
				if (strcmp(added[j], de->d_name) == 0) break;
			}
			if (j < nadded) continue;

			added = (char**)realloc(added, (nadded+1)*sizeof(char*));
			added[nadded] = malloc(PATHLEN_MAX);
			strncpy(added[nadded], de->d_name, PATHLEN_MAX);
			nadded++;

			struct stat st;
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
			if (filler(buf, de->d_name, &st, 0)) break;
		}

		closedir(dp);
	}

	for (i = 0; i < nadded; i++) free(added[i]);
	free(added);

	if (stats_enabled && strcmp(path, "/") == 0) {
		filler(buf, "stats", NULL, 0);
	}

	return 0;
}

static int unionfs_readlink(const char *path, char *buf, size_t size) {
	DBG("readlink\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = readlink(p, buf, size - 1);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = readlink(p, buf, size - 1);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	buf[res] = '\0';

	return 0;
}

static int unionfs_release(const char *path, struct fuse_file_info *fi) {
	DBG("release\n");

	close(fi->fh);

	return 0;
}

static int unionfs_rename(const char *from, const char *to) {
	DBG("rename\n");

	int i = findroot(from);
	if (i == -1) return -errno;

	char f[PATHLEN_MAX];
	snprintf(f, PATHLEN_MAX, "%s%s", roots[i], from);

	char t[PATHLEN_MAX];
	snprintf(t, PATHLEN_MAX, "%s%s", roots[i], to);

	int res = rename(f, t);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(from);

			i = findroot(from);
			if (i == -1) return -errno;

			snprintf(f, PATHLEN_MAX, "%s%s", roots[i], from);
			snprintf(t, PATHLEN_MAX, "%s%s", roots[i], to);

			res = rename(f, t);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	// The path should no longer exist
	cache_invalidate(from);

	return 0;
}

static int unionfs_rmdir(const char *path) {
	DBG("rmdir\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = rmdir(p);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = rmdir(p);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	// The path should no longer exist
	cache_invalidate(path);

	return 0;
}

static int unionfs_statfs(const char *path, struct statvfs *stbuf) {
	DBG("statfs\n");

	int first = 1;

	int i = 0;
	for (i = 0; i < nroots; i++) {
		struct statvfs stb;
		int res = statvfs(roots[i], &stb);
		if (res == -1) continue;

		if (first) {
			memcpy(stbuf, &stb, sizeof(*stbuf));
			first = 0;
		} else {
			// Filesystem can have different block sizes -> normalize to first's block size
			double ratio = (double)stb.f_bsize / (double)stbuf->f_bsize;

			stbuf->f_blocks += stb.f_blocks * ratio;
			stbuf->f_bfree += stb.f_bfree * ratio;
			stbuf->f_bavail += stb.f_bavail * ratio;

			stbuf->f_files += stb.f_files;
			stbuf->f_ffree += stb.f_ffree;
			stbuf->f_favail += stb.f_favail;

			if (stb.f_namemax < stbuf->f_namemax) stbuf->f_namemax = stb.f_namemax;
		}
	}

	return 0;
}

static int unionfs_symlink(const char *from, const char *to) {
	DBG("symlink\n");

	int i = findroot(to);
	if (i == -1) {
		if (errno == ENOENT) i = findroot_cutlast(to);
		if (i == -1) return -errno;
	}

	char t[PATHLEN_MAX];
	snprintf(t, PATHLEN_MAX, "%s%s", roots[i], to);

	int res = symlink(from, t);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(to);

			i = findroot(to);
			if (i == -1) return -errno;

			snprintf(t, PATHLEN_MAX, "%s%s", roots[i], to);

			res = symlink(from, t);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_truncate(const char *path, off_t size) {
	DBG("truncate\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = truncate(p, size);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = truncate(p, size);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_unlink(const char *path) {
	DBG("unlink\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = unlink(p);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = unlink(p);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	// The path should no longer exist
	cache_invalidate(path);

	return 0;
}

static int unionfs_utime(const char *path, struct utimbuf *buf) {
	DBG("utime\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = utime(p, buf);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_invalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = utime(p, buf);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	DBG("write\n");

	int res = pwrite(fi->fh, buf, size, offset);
	if (res == -1) return -errno;

	if (stats_enabled) stats_add_written(size);

	return res;
}

#ifdef HAVE_SETXATTR
static int unionfs_getxattr(const char *path, const char *name, char *value, size_t size) {
	DBG("getxattr\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = lgetxattr(p, name, value, size);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_ivalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = lgetxattr(p, name, value, size);
			if (res == -1) return 
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_listxattr(const char *path, char *list, size_t size) {
	DBG("listxattr\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = llistxattr(p, list, size);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_ivaliedate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = llistxattr(p, list, size);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_removexattr(const char *path, const char *name) {
	DBG("removexattr\n");
	
	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = lremovexattr(p, name);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_ivalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = lremovexattr(p, name);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}

static int unionfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
	DBG("sexattr\n");

	int i = findroot(path);
	if (i == -1) return -errno;

	char p[PATHLEN_MAX];
	snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

	int res = lsetxattr(p, name, value, size, flags);
	if (res == -1) {
		if (errno == ENOENT) {
			cache_ivalidate(path);

			i = findroot(path);
			if (i == -1) return -errno;

			snprintf(p, PATHLEN_MAX, "%s%s", roots[i], path);

			res = lsetxattr(p, name, value, size, flags);
			if (res == -1) return -errno;
		} else {
			return -errno;
		}
	}

	return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations unionfs_oper = {
	.access	= unionfs_access,
	.chmod	= unionfs_chmod,
	.chown	= unionfs_chown,
	.flush	= unionfs_flush,
	.fsync	= unionfs_fsync,
	.getattr	= unionfs_getattr,
	.link	= unionfs_link,
	.mkdir	= unionfs_mkdir,
	.mknod	= unionfs_mknod,
	.open	= unionfs_open,
	.read	= unionfs_read,
	.readlink	= unionfs_readlink,
	.readdir	= unionfs_readdir,
	.release	= unionfs_release,
	.rename	= unionfs_rename,
	.rmdir	= unionfs_rmdir,
	.statfs	= unionfs_statfs,
	.symlink	= unionfs_symlink,
	.truncate	= unionfs_truncate,
	.unlink	= unionfs_unlink,
	.utime	= unionfs_utime,
	.write	= unionfs_write,
#ifdef HAVE_SETXATTR
	.getxattr	= unionfs_getxattr,
	.listxattr	= unionfs_listxattr,
	.removexattr	= unionfs_removexattr,
	.setxattr	= unionfs_setxattr,
#endif
};

int main(int argc, char *argv[]) {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

#ifdef DEBUG
	char *dbgpath = "./unionfs_debug.log";
	printf("Debug mode, log will be written to %s\n", dbgpath);

	dbgfile = fopen(dbgpath, "w");
	if (!dbgfile) {
		printf("Failed to open %s for writing, exitting\n", dbgpath);
		return 2;
	}
#endif

	stats_init();

	doexit = 0;

	if (fuse_opt_parse(&args, NULL, unionfs_opts, unionfs_opt_proc) == -1) return 1;

	if (!doexit) {
		if (nroots == 0) {
			printf("You need to specify at least one root!\n");
			return 1;
		}
		cache_init();
	}

	umask(0);
	return fuse_main(args.argc, args.argv, &unionfs_oper);
}
