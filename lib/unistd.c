#if LAB >= 4
/*
 * Basic user-space file and I/O support functions
 * compatible with Unix's "low-level" file API.
 *
 * Copyright (C) 2010 Yale University.
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Primary author: Bryan Ford
 */

#include <inc/file.h>
#include <inc/unistd.h>
#include <inc/dirent.h>
#include <inc/assert.h>
#include <inc/stdarg.h>
#if LAB >= 9
#include <inc/select.h>
#endif

int
creat(const char *path, mode_t mode)
{
	return open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

int
open(const char *path, int flags, ...)
{
	// Get the optional mode argument, which applies only with O_CREAT.
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	filedesc *fd = filedesc_open(NULL, path, flags, mode);
	if (fd == NULL)
		return -1;

	return fd - files->fd;
}

int
close(int fn)
{
	filedesc_close(&files->fd[fn]);
	return 0;
}

ssize_t
read(int fn, void *buf, size_t nbytes)
{
	return filedesc_read(&files->fd[fn], buf, 1, nbytes);
}

ssize_t
write(int fn, const void *buf, size_t nbytes)
{
	return filedesc_write(&files->fd[fn], buf, 1, nbytes);
}

off_t
lseek(int fn, off_t offset, int whence)
{
	return filedesc_seek(&files->fd[fn], offset, whence);
}

int
dup(int oldfn)
{
	filedesc *newfd = filedesc_alloc();
	if (!newfd)
		return -1;
	return dup2(oldfn, newfd - files->fd);
}

int
dup2(int oldfn, int newfn)
{
	filedesc *oldfd = &files->fd[oldfn];
	filedesc *newfd = &files->fd[newfn];
	assert(filedesc_isopen(oldfd));
	assert(filedesc_isvalid(newfd));

	if (filedesc_isopen(newfd))
		close(newfn);

	*newfd = *oldfd;

	return newfn;
}

int
truncate(const char *path, off_t newlength)
{
	int ino = dir_walk(path, 0);
	if (ino < 0)
		return -1;
	return fileino_truncate(ino, newlength);
}

int
ftruncate(int fn, off_t newlength)
{
	assert(filedesc_isopen(&files->fd[fn]));
	return fileino_truncate(files->fd[fn].ino, newlength);
}

int
isatty(int fn)
{
	assert(filedesc_isopen(&files->fd[fn]));
	return files->fd[fn].ino == FILEINO_CONSIN
		|| files->fd[fn].ino == FILEINO_CONSOUT;
}

int
stat(const char *path, struct stat *statbuf)
{
	int ino = dir_walk(path, 0);
	if (ino < 0)
		return -1;
	return fileino_stat(ino, statbuf);
}

int
fstat(int fn, struct stat *statbuf)
{
	assert(filedesc_isopen(&files->fd[fn]));
	return fileino_stat(files->fd[fn].ino, statbuf);
}

int
fsync(int fn)
{
	assert(filedesc_isopen(&files->fd[fn]));
	return fileino_flush(files->fd[fn].ino);
}

#if LAB >= 9
int select(int nfds, fd_set *rs, fd_set *ws, fd_set *xs,
		struct timeval *timeout)
{
	panic("select() not implemented");
}
#endif

#endif /* LAB >= 4 */
