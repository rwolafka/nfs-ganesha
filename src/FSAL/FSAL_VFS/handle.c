/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ------------- 
 */

/* handle.c
 * VFS object (file|dir) handle object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fsal.h"
#include <libgen.h>             /* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <mntent.h>
#include "nlm_list.h"
#include "fsal_internal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "vfs_methods.h"

/* Handle object shared methods vector
 */

static struct fsal_obj_ops obj_ops;

/* helpers
 */

/*
 * is_supported_attribute
 */

static int is_supported_attribute(struct fsal_obj_handle *obj_hdl,
					 fsal_attrib_list_t *attr)
{
	fsal_attrib_mask_t mask;

	mask = obj_hdl->export->ops->fs_supported_attrs(obj_hdl->export);
	if(attr->asked_attributes & ~mask) {
		LogFullDebug(COMPONENT_FSAL,
			     "Unsupported attributes: asked = %#llX, allowed = %#llX",
			     attr->asked_attributes, mask);
		return FALSE;
	}
	return TRUE;
}

/* alloc_handle
 * allocate and fill in a handle
 * this uses malloc/free for the time being.
 */

static struct vfs_fsal_obj_handle *alloc_handle(struct file_handle *fh,
						struct stat *stat,
						const char *link_content,
						struct file_handle *dir_fh,
						const char *sock_name,
						struct fsal_export *exp_hdl)
{
	struct vfs_fsal_obj_handle *hdl;
	pthread_mutexattr_t attrs;
	fsal_status_t st;
	int retval;

	hdl = malloc(sizeof(struct vfs_fsal_obj_handle) +
		     sizeof(struct file_handle) +
		     fh->handle_bytes);
	if(hdl == NULL)
		return NULL;
	memset(hdl, 0, (sizeof(struct vfs_fsal_obj_handle) +
			sizeof(struct file_handle) +
			fh->handle_bytes));
	hdl->handle = (struct file_handle *)&hdl[1];
	memcpy(hdl->handle, fh,
	       sizeof(struct file_handle) + fh->handle_bytes);
	hdl->obj_handle.type = posix2fsal_type(stat->st_mode);
	if(hdl->obj_handle.type == REGULAR_FILE) {
		hdl->u.file.fd = -1;  /* no open on this yet */
		hdl->u.file.openflags = FSAL_O_CLOSED;
		hdl->u.file.lock_status = 0;
	} else if(hdl->obj_handle.type == SYMBOLIC_LINK
	   && link_content != NULL) {
		size_t len = strlen(link_content) + 1;

		hdl->u.symlink.link_content = malloc(len);
		if(hdl->u.symlink.link_content == NULL) {
			goto spcerr;
		}
		memcpy(hdl->u.symlink.link_content, link_content, len);
		hdl->u.symlink.link_size = len;
	} else if(hdl->obj_handle.type == SOCKET_FILE
		  && dir_fh != NULL
		  && sock_name != NULL) {
		hdl->u.sock.sock_dir = malloc(sizeof(struct file_handle)
				       + dir_fh->handle_bytes);
		if(hdl->u.sock.sock_dir == NULL)
			goto spcerr;
		memcpy(hdl->u.sock.sock_dir,
		       dir_fh,
		       sizeof(struct file_handle) + dir_fh->handle_bytes);
		hdl->u.sock.sock_name = malloc(strlen(sock_name) + 1);
		if(hdl->u.sock.sock_name == NULL)
			goto spcerr;
		strcpy(hdl->u.sock.sock_name, sock_name);
	}
	hdl->obj_handle.export = exp_hdl;
	hdl->obj_handle.attributes.asked_attributes
		= exp_hdl->ops->fs_supported_attrs(exp_hdl);
	hdl->obj_handle.attributes.supported_attributes
		= hdl->obj_handle.attributes.asked_attributes;
	st = posix2fsal_attributes(stat, &hdl->obj_handle.attributes);
	if(FSAL_IS_ERROR(st))
		goto spcerr;
	hdl->obj_handle.refs = 1;  /* we start out with a reference */
	hdl->obj_handle.ops = &obj_ops;
	init_glist(&hdl->obj_handle.handles);
	pthread_mutexattr_settype(&attrs, PTHREAD_MUTEX_ADAPTIVE_NP);
	pthread_mutex_init(&hdl->obj_handle.lock, &attrs);

	/* lock myself before attaching to the export.
	 * keep myself locked until done with creating myself.
	 */

	pthread_mutex_lock(&hdl->obj_handle.lock);
	retval = fsal_attach_handle(exp_hdl, &hdl->obj_handle.handles);
	if(retval != 0)
		goto errout; /* seriously bad */
	pthread_mutex_unlock(&hdl->obj_handle.lock);
	return hdl;

errout:
	hdl->obj_handle.ops = NULL;
	pthread_mutex_unlock(&hdl->obj_handle.lock);
	pthread_mutex_destroy(&hdl->obj_handle.lock);
spcerr:
	if(hdl->obj_handle.type == SYMBOLIC_LINK) {
		if(hdl->u.symlink.link_content != NULL)
			free(hdl->u.symlink.link_content);
	} else if(hdl->obj_handle.type == SOCKET_FILE) {
		if(hdl->u.sock.sock_name != NULL)
			free(hdl->u.sock.sock_name);
		if(hdl->u.sock.sock_dir != NULL)
			free(hdl->u.sock.sock_dir);
	}
	free(hdl);  /* elvis has left the building */
	return NULL;
}

/* handle methods
 */

/* lookup
 * deprecated NULL parent && NULL path implies root handle
 */

static fsal_status_t lookup(struct fsal_obj_handle *parent,
			    const char *path,
			    struct fsal_obj_handle **handle)
{
	struct vfs_fsal_obj_handle *parent_hdl, *hdl;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval, dirfd, fd;
	int mount_fd;
	int mnt_id = 0;
	struct stat stat;
	char *link_content = NULL;
	struct file_handle *dir_hdl = NULL;
	const char *sock_name = NULL;
	ssize_t retlink;
	char link_buff[FSAL_MAX_PATH_LEN];
	struct file_handle *fh
		= alloca(sizeof(struct file_handle) + MAX_HANDLE_SZ);

	if( !path)
		ReturnCode(ERR_FSAL_FAULT, 0);
	memset(fh, 0, sizeof(struct file_handle) + MAX_HANDLE_SZ);
	fh->handle_bytes = MAX_HANDLE_SZ;
	mount_fd = vfs_get_root_fd(parent->export);
	parent_hdl = container_of(parent, struct vfs_fsal_obj_handle, obj_handle);
	if( !parent->ops->handle_is(parent, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			parent);
		ReturnCode(ERR_FSAL_NOTDIR, 0);
	}
	dirfd = open_by_handle_at(mount_fd, parent_hdl->handle, O_PATH|O_NOACCESS);
	if(dirfd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = name_to_handle_at(dirfd, path, fh, &mnt_id, 0);
	if(retval < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(dirfd);
		goto errout;
	}
	close(dirfd);
	fd = open_by_handle_at(mount_fd, fh, O_PATH|O_NOACCESS);
	if(fd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = fstatat(fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(fd);
		goto errout;
	}
	if(S_ISLNK(stat.st_mode)) { /* I could lazy eval this... */
		retlink = readlinkat(fd, "", link_buff, FSAL_MAX_PATH_LEN);
		if(retlink < 0 || retlink == FSAL_MAX_PATH_LEN) {
			retval = errno;
			if(retlink == FSAL_MAX_PATH_LEN)
				retval = ENAMETOOLONG;
			fsal_error = posix2fsal_error(retval);
			close(fd);
			goto errout;
		}
		link_buff[retlink] = '\0';
		link_content = &link_buff[0];
	} else if(S_ISSOCK(stat.st_mode)) {
		dir_hdl = parent_hdl->handle;
		sock_name = path;
	}
	close(fd);
	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(fh, &stat,
			   link_content,
			   dir_hdl,
			   sock_name,
			   parent->export);
	if(hdl != NULL) {
		*handle = &hdl->obj_handle;
	} else {
		if(link_content != NULL) {
			free(link_content);
		}
		fsal_error = ERR_FSAL_NOMEM;
		*handle = NULL; /* poison it */
		goto errout;
	}
	ReturnCode(ERR_FSAL_NO_ERROR, 0);
	
errout:
	ReturnCode(fsal_error, retval);	
}

/* create
 * create a regular file and set its attributes
 */

static fsal_status_t create(struct fsal_obj_handle *dir_hdl,
			    fsal_name_t *name,
			    fsal_attrib_list_t *attrib,
			    struct fsal_obj_handle **handle)
{
	struct vfs_fsal_obj_handle *myself, *hdl;
	int mnt_id = 0;
	int fd, mount_fd, dir_fd;
	struct stat stat;
	mode_t unix_mode;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	uid_t user;
	gid_t group;
	struct file_handle *fh
		= alloca(sizeof(struct file_handle) + MAX_HANDLE_SZ);

	*handle = NULL; /* poison it */
	if( !dir_hdl->ops->handle_is(dir_hdl, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			dir_hdl);
		ReturnCode(ERR_FSAL_NOTDIR, 0);
	}
	memset(fh, 0, sizeof(struct file_handle) + MAX_HANDLE_SZ);
	fh->handle_bytes = MAX_HANDLE_SZ;
	myself = container_of(dir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mount_fd = vfs_get_root_fd(dir_hdl->export);
	user = attrib->owner;
	group = attrib->group;
	unix_mode = fsal2unix_mode(attrib->mode)
		& ~dir_hdl->export->ops->fs_umask(dir_hdl->export);
	dir_fd = open_by_handle_at(mount_fd, myself->handle, O_PATH|O_NOACCESS);
	if(dir_fd < 0) {
		retval = errno;
		if(retval == ENOENT)
			fsal_error = ERR_FSAL_STALE;
		else
			fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = fstatat(dir_fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(dir_fd);
		goto errout;
	}
	if(stat.st_mode & S_ISGID)
		group = -1; /*setgid bit on dir propagates dir group owner */

	/* create it with no access because we are root when we do this */
	fd = openat(dir_fd, name->name, O_CREAT|O_WRONLY|O_TRUNC|O_EXCL, 0000);
	if(fd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(dir_fd);
		goto errout;
	}
	close(dir_fd); /* done with parent */

	retval = fchown(fd, user, group);
	if(retval < 0) {
		goto fileerr;
	}

	/* now that it is owned properly, set to an accessible mode */
	retval = fchmod(fd, unix_mode);
	if(retval < 0) {
		goto fileerr;
	}
	retval = name_to_handle_at(fd, "", fh, &mnt_id, AT_EMPTY_PATH);
	if(retval < 0) {
		goto fileerr;
	}
	retval = fstatat(fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		goto fileerr;
	}
	close(fd);

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(fh, &stat, NULL, NULL, NULL, dir_hdl->export);
	if(hdl != NULL) {
		*handle = &hdl->obj_handle;
	} else {
		fsal_error = ERR_FSAL_NOMEM;
		goto errout;
	}
	ReturnCode(ERR_FSAL_NO_ERROR, 0);

fileerr:
	retval = errno;
	fsal_error = posix2fsal_error(retval);
	close(fd);
errout:
	ReturnCode(fsal_error, retval);	
}

static fsal_status_t makedir(struct fsal_obj_handle *dir_hdl,
			     fsal_name_t *name,
			     fsal_attrib_list_t *attrib,
			     struct fsal_obj_handle **handle)
{
	struct vfs_fsal_obj_handle *myself, *hdl;
	int mnt_id = 0;
	int fd, mount_fd, dir_fd;
	struct stat stat;
	mode_t unix_mode;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	uid_t user;
	gid_t group;
	struct file_handle *fh
		= alloca(sizeof(struct file_handle) + MAX_HANDLE_SZ);

	*handle = NULL; /* poison it */
	if( !dir_hdl->ops->handle_is(dir_hdl, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			dir_hdl);
		ReturnCode(ERR_FSAL_NOTDIR, 0);
	}
	memset(fh, 0, sizeof(struct file_handle) + MAX_HANDLE_SZ);
	fh->handle_bytes = MAX_HANDLE_SZ;
	myself = container_of(dir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mount_fd = vfs_get_root_fd(dir_hdl->export);
	user = attrib->owner;
	group = attrib->group;
	unix_mode = fsal2unix_mode(attrib->mode)
		& ~dir_hdl->export->ops->fs_umask(dir_hdl->export);
	dir_fd = open_by_handle_at(mount_fd, myself->handle, O_PATH|O_NOACCESS);
	if(dir_fd < 0) {
		retval = errno;
		if(retval == ENOENT)
			fsal_error = ERR_FSAL_STALE;
		else
			fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = fstatat(dir_fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		goto direrr;
	}
	if(stat.st_mode & S_ISGID)
		group = -1; /*setgid bit on dir propagates dir group owner */

	/* create it with no access because we are root when we do this */
	retval = mkdirat(dir_fd, name->name, 0000);
	if(retval < 0) {
		goto direrr;
	}
	fd = openat(dir_fd, name->name, O_RDONLY | O_DIRECTORY);
/** @TODO. my fd leak caused this to fail, leaving the dir around.
 * do an unlinkat on failed dirs.  same for other f/s object is we don't
 * get through the full dance to a real and safe object
 */
	if(fd < 0) {
		goto direrr;
	}
	close(dir_fd); /* done with the parent */

	retval = fchown(fd, user, group);
	if(retval < 0) {
		goto fileerr;
	}

	/* now that it is owned properly, set accessible mode */
	retval = fchmod(fd, unix_mode);
	if(retval < 0) {
		goto fileerr;
	}
	retval = name_to_handle_at(fd, "", fh, &mnt_id, AT_EMPTY_PATH);
	if(retval < 0) {
		goto fileerr;
	}
	retval = fstatat(fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		goto fileerr;
	}
	close(fd);

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(fh, &stat, NULL, NULL, NULL, dir_hdl->export);
	if(hdl != NULL) {
		*handle = &hdl->obj_handle;
	} else {
		fsal_error = ERR_FSAL_NOMEM;
		goto errout;
	}
	ReturnCode(ERR_FSAL_NO_ERROR, 0);
	
direrr:
	retval = errno;
	fsal_error = posix2fsal_error(retval);
	close(dir_fd);

	ReturnCode(fsal_error, retval);	

fileerr:
	retval = errno;
	fsal_error = posix2fsal_error(retval);
	close(fd);
errout:
	ReturnCode(fsal_error, retval);	
}

static fsal_status_t makenode(struct fsal_obj_handle *dir_hdl,
			      fsal_name_t *name,
			      object_file_type_t nodetype,  /* IN */
			      fsal_dev_t *dev,  /* IN */
			      fsal_attrib_list_t *attrib,
			      struct fsal_obj_handle **handle)
{
	struct vfs_fsal_obj_handle *myself, *hdl;
	int mnt_id = 0;
	int mount_fd, dir_fd;
	struct stat stat;
	mode_t unix_mode, create_mode = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	uid_t user;
	gid_t group;
	dev_t unix_dev = 0;
	struct file_handle *dir_fh = NULL;
	char *sock_name = NULL;
	struct file_handle *fh
		= alloca(sizeof(struct file_handle) + MAX_HANDLE_SZ);

	*handle = NULL; /* poison it */
	if( !dir_hdl->ops->handle_is(dir_hdl, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			dir_hdl);


		ReturnCode(ERR_FSAL_NOTDIR, 0);
	}
	memset(fh, 0, sizeof(struct file_handle) + MAX_HANDLE_SZ);
	fh->handle_bytes = MAX_HANDLE_SZ;
	myself = container_of(dir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mount_fd = vfs_get_root_fd(dir_hdl->export);
	user = attrib->owner;
	group = attrib->group;
	unix_mode = fsal2unix_mode(attrib->mode)
		& ~dir_hdl->export->ops->fs_umask(dir_hdl->export);
	switch (nodetype) {
	case BLOCK_FILE:
		if( !dev) {
			fsal_error = ERR_FSAL_FAULT;
			goto errout;
		}
		create_mode = S_IFBLK;
		unix_dev = makedev(dev->major, dev->minor);
		break;
	case CHARACTER_FILE:
		if( !dev) {
 			fsal_error = ERR_FSAL_FAULT;
			goto errout;


		}
		create_mode = S_IFCHR;
		unix_dev = makedev(dev->major, dev->minor);
		break;
	case FIFO_FILE:
		create_mode = S_IFIFO;
		break;
	case SOCKET_FILE:
		create_mode = S_IFSOCK;
		dir_fh = myself->handle;
		sock_name = name->name;
		break;
	default:
		LogMajor(COMPONENT_FSAL,
			 "Invalid node type in FSAL_mknode: %d",
			 nodetype);
		fsal_error = ERR_FSAL_INVAL;
		goto errout;
	}
	dir_fd = open_by_handle_at(mount_fd, myself->handle, O_PATH|O_NOACCESS);
	if(dir_fd < 0) {
		retval = errno;
		if(retval == ENOENT)
			fsal_error = ERR_FSAL_STALE;
		else
			fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = fstatat(dir_fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		goto direrr;
	}
	if(stat.st_mode & S_ISGID)
		group = -1; /*setgid bit on dir propagates dir group owner */

	/* create it with no access because we are root when we do this */
	retval = mknodat(dir_fd, name->name, create_mode, unix_dev);
	if(retval < 0) {
		goto direrr;
	}
	retval = name_to_handle_at(dir_fd, name->name, fh, &mnt_id, 0);
	if(retval < 0) {
		goto direrr;
	}

	retval = fchownat(dir_fd, name->name,
			  user, group, AT_SYMLINK_NOFOLLOW);
	if(retval < 0) {
		goto direrr;
	}

	/* now that it is owned properly, set accessible mode */
	retval = fchmodat(dir_fd, name->name,
			  unix_mode, 0);
	if(retval < 0) {
		goto direrr;
	}
	retval = fstatat(dir_fd, name->name, &stat, 0);
	if(retval < 0) {
		goto direrr;
	}

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(fh, &stat, NULL, dir_fh, sock_name, dir_hdl->export);
	if(hdl == NULL) {
		fsal_error = ERR_FSAL_NOMEM;
		goto errout;
	}
	*handle = &hdl->obj_handle;
	ReturnCode(ERR_FSAL_NO_ERROR, 0);
	
	
direrr:
	retval = errno;
	fsal_error = posix2fsal_error(retval);
errout:
	unlinkat(dir_fd, name->name, 0);
	close(dir_fd); /* done with parent */
	ReturnCode(fsal_error, retval);	
}

/** makesymlink
 *  Note that we do not set mode bits on symlinks for Linux/POSIX
 *  They are not really settable in the kernel and are not checked
 *  anyway (default is 0777) because open uses that target's mode
 */

static fsal_status_t makesymlink(struct fsal_obj_handle *dir_hdl,
				 fsal_name_t *name,
				 fsal_path_t *link_path,
				 fsal_attrib_list_t *attrib,
				 struct fsal_obj_handle **handle)
{
	struct vfs_fsal_obj_handle *myself, *hdl;
	int mnt_id = 0;
	int mount_fd, dir_fd = -1;
	struct stat stat;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	uid_t user;
	gid_t group;
	struct file_handle *fh
		= alloca(sizeof(struct file_handle) + MAX_HANDLE_SZ);

	*handle = NULL; /* poison it first */
	if( !dir_hdl->ops->handle_is(dir_hdl, DIRECTORY)) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			dir_hdl);
		ReturnCode(ERR_FSAL_NOTDIR, 0);
	}
	memset(fh, 0, sizeof(struct file_handle) + MAX_HANDLE_SZ);
	fh->handle_bytes = MAX_HANDLE_SZ;
	myself = container_of(dir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mount_fd = vfs_get_root_fd(dir_hdl->export);
	user = attrib->owner;
	group = attrib->group;
	dir_fd = open_by_handle_at(mount_fd, myself->handle, O_PATH|O_NOACCESS);
	if(dir_fd < 0) {
		retval = errno;
		goto errout;
	}
	retval = fstatat(dir_fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		goto direrr;
	}
	if(stat.st_mode & S_ISGID)
		group = -1; /*setgid bit on dir propagates dir group owner */
	
	/* create it with no access because we are root when we do this */
	retval = symlinkat(link_path->path, dir_fd, name->name);
	if(retval < 0) {
		goto direrr;
	}
	retval = name_to_handle_at(dir_fd, name->name, fh, &mnt_id, 0);
	if(retval < 0) {
		goto linkerr;
	}
	retval = fchownat(dir_fd, name->name, user, group, AT_SYMLINK_NOFOLLOW);
	if(retval < 0) {
		goto linkerr;
	}

	/* now get attributes info, being careful to get the link, not the target */
	retval = fstatat(dir_fd, name->name, &stat, AT_SYMLINK_NOFOLLOW);
	if(retval < 0) {
		goto linkerr;
	}
	close(dir_fd);

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(fh, &stat, link_path->path, NULL, NULL, dir_hdl->export);
	if(hdl == NULL) {
		retval = ENOMEM;
		goto errout;
	}
	*handle = &hdl->obj_handle;
	ReturnCode(ERR_FSAL_NO_ERROR, 0);

linkerr:
	retval = errno;
	unlinkat(dir_fd, name->name, 0);
	goto errout;

direrr:
	retval = errno;
	close(dir_fd);
errout:
	if(retval == ENOENT)
		fsal_error = ERR_FSAL_STALE;
	else
		fsal_error = posix2fsal_error(retval);
	ReturnCode(fsal_error, retval);	
}

static fsal_status_t readsymlink(struct fsal_obj_handle *obj_hdl,
				 char *link_content,
				 uint32_t *link_len,
				 fsal_boolean_t refresh)
{
	struct vfs_fsal_obj_handle *myself;
	int fd, mntfd;
	int retval = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;

	if(obj_hdl->type != SYMBOLIC_LINK) {
		fsal_error = ERR_FSAL_FAULT;
		goto out;
	}
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	if(refresh) { /* lazy load or LRU'd storage */
		ssize_t retlink;
		char link_buff[FSAL_MAX_PATH_LEN];

		if(myself->u.symlink.link_content != NULL) {
			free(myself->u.symlink.link_content);
			myself->u.symlink.link_content = NULL;
			myself->u.symlink.link_size = 0;
		}
		mntfd = vfs_get_root_fd(obj_hdl->export);
		fd = open_by_handle_at(mntfd, myself->handle, (O_PATH|O_NOACCESS));
		if(fd < 0) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
			goto out;
		}
		retlink = readlinkat(fd, "", link_buff, FSAL_MAX_PATH_LEN);
		if(retlink < 0) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
			close(fd);
			goto out;
		}
		close(fd);

		myself->u.symlink.link_content = malloc(retlink + 1);
		if(myself->u.symlink.link_content == NULL) {
			fsal_error = ERR_FSAL_NOMEM;
			goto out;
		}
		memcpy(myself->u.symlink.link_content, link_buff, retlink);
		myself->u.symlink.link_content[retlink] = '\0';
		myself->u.symlink.link_size = retlink + 1;
	}
	if(myself->u.symlink.link_content == NULL
	   || *link_len <= myself->u.symlink.link_size) {
		fsal_error = ERR_FSAL_FAULT; /* probably a better error?? */
		goto out;
	}
	memcpy(link_content,
	       myself->u.symlink.link_content,
	       myself->u.symlink.link_size);

out:
	*link_len = myself->u.symlink.link_size;
	ReturnCode(fsal_error, retval);	
}

static fsal_status_t linkfile(struct fsal_obj_handle *obj_hdl,
			      struct fsal_obj_handle *destdir_hdl,
			      fsal_name_t *name)
{
	struct vfs_fsal_obj_handle *myself, *destdir;
	int srcfd, destdirfd, mntfd;
	int retval = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;

	if( !obj_hdl->export->ops->fs_supports(obj_hdl->export, link_support)) {
		fsal_error = ERR_FSAL_NOTSUPP;
		goto out;
	}
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mntfd = vfs_get_root_fd(obj_hdl->export);
	srcfd = open_by_handle_at(mntfd, myself->handle, (O_PATH|O_NOACCESS));
	if(srcfd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto out;
	}
	destdir = container_of(destdir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	destdirfd = open_by_handle_at(mntfd, destdir->handle, (O_PATH|O_NOACCESS));
	if(destdirfd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(srcfd);
		goto out;
	}
	retval = linkat(srcfd, "", destdirfd, name->name, AT_EMPTY_PATH);
	if(retval == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
	}

	close(srcfd);
	close(destdirfd);
out:
	ReturnCode(fsal_error, retval);	
}

/* not defined in linux headers so we do it here
 */

struct linux_dirent {
	unsigned long  d_ino;     /* Inode number */
	unsigned long  d_off;     /* Offset to next linux_dirent */
	unsigned short d_reclen;  /* Length of this linux_dirent */
	char           d_name[];  /* Filename (null-terminated) */
	/* length is actually (d_reclen - 2 -
	 * offsetof(struct linux_dirent, d_name)
	 */
	/*
	  char           pad;       // Zero padding byte
	  char           d_type;    // File type (only since Linux 2.6.4;
	  // offset is (d_reclen - 1))
	  */
};

#define BUF_SIZE 1024
/**
 * read_dirents
 * read the directory and call through the callback function for
 * each entry.
 * @param dir_hdl [IN] the directory to read
 * @param entry_cnt [IN] limit of entries. 0 implies no limit
 * @param whence [IN] where to start (next)
 * @param dir_state [IN] pass thru of state to callback
 * @param cb [IN] callback function
 * @param eof [OUT] eof marker TRUE == end of dir
 */

static fsal_status_t read_dirents(struct fsal_obj_handle *dir_hdl,
				  uint32_t entry_cnt,
				  struct fsal_cookie *whence,
				  void *dir_state,
				  fsal_status_t (*cb)(
					  const char *name,
					  unsigned int dtype,
					  struct fsal_obj_handle *dir_hdl,
					  void *dir_state,
					  struct fsal_cookie *cookie),
				  fsal_boolean_t *eof)
{
	struct vfs_fsal_obj_handle *myself;
	int dirfd, mntfd;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	fsal_status_t status;
	int retval = 0;
	off_t seekloc = 0;
	int bpos, cnt, nread;
	unsigned char d_type;
	struct linux_dirent *dentry;
	struct fsal_cookie *entry_cookie;
	char buf[BUF_SIZE];

	if(whence != NULL) {
		if(whence->size != sizeof(off_t)) {
			fsal_error = posix2fsal_error(EINVAL);
			retval = errno;
			goto out;
		}
		memcpy(&seekloc, whence->cookie, sizeof(off_t));
	}
	entry_cookie = alloca(sizeof(struct fsal_cookie) + sizeof(off_t));
	myself = container_of(dir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mntfd = vfs_get_root_fd(dir_hdl->export);
	dirfd = open_by_handle_at(mntfd, myself->handle, (O_RDONLY|O_DIRECTORY));
	if(dirfd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto out;
	}
	seekloc = lseek(dirfd, seekloc, SEEK_SET);
	if(seekloc < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto done;
	}
	cnt = 0;
	do {
		nread = syscall(SYS_getdents, dirfd, buf, BUF_SIZE);
		if(nread < 0) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
			goto done;
		}
		if(nread == 0)
			break;
		for(bpos = 0; bpos < nread;) {
			dentry = (struct linux_dirent *)(buf + bpos);
			if(strcmp(dentry->d_name, ".") == 0 ||
			   strcmp(dentry->d_name, "..") == 0)
				goto skip; /* must skip '.' and '..' */
			d_type = *(buf + bpos + dentry->d_reclen - 1);
			entry_cookie->size = sizeof(off_t);
			memcpy(&entry_cookie->cookie, &dentry->d_off, sizeof(off_t));

			/* callback to cache inode */
			status = cb(dentry->d_name,
				    d_type,
				    dir_hdl,
				    dir_state, entry_cookie);
			if(FSAL_IS_ERROR(status)) {
				fsal_error = status.major;
				retval = status.minor;
				goto done;
			}
		skip:
			bpos += dentry->d_reclen;
			cnt++;
			if(entry_cnt > 0 && cnt >= entry_cnt)
				goto done;
		}
	} while(nread > 0);

done:
	close(dirfd);
	*eof = nread == 0 ? TRUE : FALSE;
	
out:
	ReturnCode(fsal_error, retval);	
}


static fsal_status_t renamefile(struct fsal_obj_handle *olddir_hdl,
				fsal_name_t *old_name,
				struct fsal_obj_handle *newdir_hdl,
				fsal_name_t *new_name)
{
	struct vfs_fsal_obj_handle *olddir, *newdir;
	int oldfd, newfd, mntfd;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	olddir = container_of(olddir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mntfd = vfs_get_root_fd(olddir_hdl->export);
	oldfd = open_by_handle_at(mntfd, olddir->handle, (O_PATH|O_NOACCESS));
	if(oldfd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto out;
	}
	newdir = container_of(newdir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	newfd = open_by_handle_at(mntfd, newdir->handle, (O_PATH|O_NOACCESS));
	if(newfd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(oldfd);
		goto out;
	}
	retval = renameat(oldfd, old_name->name, newfd, new_name->name);
	if(retval < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
	}
	close(oldfd);
	close(newfd);
out:
	ReturnCode(fsal_error, retval);	
}

/* FIXME:  attributes are now merged into fsal_obj_handle.  This
 * spreads everywhere these methods are used.
 * eventually deprecate everywhere except where we explicitly want to
 * to refresh them.
 * NOTE: this is done under protection of the attributes rwlock in the cache entry.
 */

static fsal_status_t getattrs(struct fsal_obj_handle *obj_hdl,
			      fsal_attrib_list_t *obj_attr)
{
	struct vfs_fsal_obj_handle *myself;
	int fd = -1, mntfd;
	int open_flags = O_RDONLY;
	struct stat stat;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	fsal_status_t st;
	int retval = 0;

	if( !is_supported_attribute(obj_hdl, obj_attr)) {
		fsal_error = ERR_FSAL_ATTRNOTSUPP;
		goto out;
	}
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mntfd = vfs_get_root_fd(obj_hdl->export);
	if(obj_hdl->type == SOCKET_FILE) {
		fd = open_by_handle_at(mntfd,
				       myself->u.sock.sock_dir,
				       (O_PATH|O_NOACCESS));
		if(fd < 0) {
			goto errout;
		}
		retval = fstatat(fd,
				 myself->u.sock.sock_name,
				 &stat,
				 AT_SYMLINK_NOFOLLOW);
		if(retval < 0) {
			goto errout;
		}
	} else {
		if(obj_hdl->type == SYMBOLIC_LINK)
			open_flags |= O_PATH;
		else if(obj_hdl->type == FIFO_FILE)
			open_flags |= O_NONBLOCK;
		fd = open_by_handle_at(mntfd, myself->handle, open_flags);
		if(fd < 0) {
			goto errout;
		}
		retval = fstatat(fd,
				 "",
				 &stat,
				 (AT_SYMLINK_NOFOLLOW|AT_EMPTY_PATH));
		if(retval < 0) {
			goto errout;
		}
	}

	/* convert attributes */
	obj_hdl->attributes.asked_attributes = obj_attr->asked_attributes;
	st = posix2fsal_attributes(&stat, &obj_hdl->attributes);
	if(FSAL_IS_ERROR(st)) {
		FSAL_CLEAR_MASK(obj_attr->asked_attributes);
		FSAL_SET_MASK(obj_attr->asked_attributes,
			      FSAL_ATTR_RDATTR_ERR);
		fsal_error = st.major;  retval = st.minor;
		goto out;
	}
	memcpy(obj_attr, &obj_hdl->attributes, sizeof(fsal_attrib_list_t));
	goto out;

errout:
	retval = errno;
	if(retval == ENOENT)
		fsal_error = ERR_FSAL_STALE;
	else
		fsal_error = posix2fsal_error(retval);
out:
	if(fd >= 0)
		close(fd);
	ReturnCode(fsal_error, retval);	
}

/*
 * NOTE: this is done under protection of the attributes rwlock in the cache entry.
 */

static fsal_status_t setattrs(struct fsal_obj_handle *obj_hdl,
			      fsal_attrib_list_t *attrs)
{
	struct vfs_fsal_obj_handle *myself;
	int fd, mntfd;
	int open_flags = O_RDONLY;
	struct stat stat;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	if( !is_supported_attribute(obj_hdl, attrs)) {
		fsal_error = ERR_FSAL_ATTRNOTSUPP;
		goto out;
	}
	/* apply umask, if mode attribute is to be changed */
	if(FSAL_TEST_MASK(attrs->asked_attributes, FSAL_ATTR_MODE)) {
		attrs->mode
			&= ~obj_hdl->export->ops->fs_umask(obj_hdl->export);
	}
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mntfd = vfs_get_root_fd(obj_hdl->export);

	/* This is yet another "you can't get there from here".  If this object
	 * is a socket (AF_UNIX), an fd on the socket s useless _period_.
	 * If it is for a symlink, without O_PATH, you will get an ELOOP error
	 * and (f)chmod doesn't work for a symlink anyway - not that it matters
	 * because access checking is not done on the symlink but the final target.
	 * AF_UNIX sockets are also ozone material.  If the socket is already active
	 * listeners et al, you can manipulate the mode etc.  If it is just sitting
	 * there as in you made it with a mknod (one of those leaky abstractions...)
	 * or the listener forgot to unlink it, it is lame duck.
	 */

	if(obj_hdl->type == SOCKET_FILE) {
		fd = open_by_handle_at(mntfd,
				       myself->u.sock.sock_dir,
				       (O_PATH|O_NOACCESS));
		if(fd < 0) {
			retval = errno;
			if(retval == ENOENT)
				fsal_error = ERR_FSAL_STALE;
			else
				fsal_error = posix2fsal_error(retval);
			goto out;
		}
		retval = fstatat(fd,
				 myself->u.sock.sock_name,
				 &stat,
				 AT_SYMLINK_NOFOLLOW);
	} else {
		if(obj_hdl->type == SYMBOLIC_LINK)
			open_flags |= O_PATH;
		else if(obj_hdl->type == FIFO_FILE)
			open_flags |= O_NONBLOCK;
		fd = open_by_handle_at(mntfd, myself->handle, open_flags);
		if(fd < 0) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
			goto out;
		}
		retval = fstatat(fd, "", &stat, (AT_SYMLINK_NOFOLLOW|AT_EMPTY_PATH));
	}
	if(retval < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(fd);
		goto out;
	}
	/** CHMOD **/
	if(FSAL_TEST_MASK(attrs->asked_attributes, FSAL_ATTR_MODE)) {
		/* The POSIX chmod call doesn't affect the symlink object, but
		 * the entry it points to. So we must ignore it.
		 */
		if(!S_ISLNK(stat.st_mode)) {
			if(obj_hdl->type == SOCKET_FILE)
				retval = fchmodat(fd,
						  myself->u.sock.sock_name,
						  fsal2unix_mode(attrs->mode), 0);
			else
				retval = fchmod(fd, fsal2unix_mode(attrs->mode));

			if(retval != 0) {
				goto fileerr;
			}
		}
	}
		
	/**  CHOWN  **/
	if(FSAL_TEST_MASK(attrs->asked_attributes,
			  FSAL_ATTR_OWNER | FSAL_ATTR_GROUP)) {
		uid_t user = FSAL_TEST_MASK(attrs->asked_attributes,
					    FSAL_ATTR_OWNER) ? (int)attrs->owner : -1;
		gid_t group = FSAL_TEST_MASK(attrs->asked_attributes,
					     FSAL_ATTR_GROUP) ? (int)attrs->group : -1;

		if(obj_hdl->type == SOCKET_FILE)
			retval = fchownat(fd,
					  myself->u.sock.sock_name,
					  user,
					  group,
					  AT_SYMLINK_NOFOLLOW);
		else
			retval = fchown(fd, user, group);

		if(retval) {
			goto fileerr;
		}
	}
		
	/**  UTIME  **/
	if(FSAL_TEST_MASK(attrs->asked_attributes,
			  FSAL_ATTR_ATIME | FSAL_ATTR_MTIME)) {
		struct timeval timebuf[2];

		/* Atime */
		timebuf[0].tv_sec =
			(FSAL_TEST_MASK(attrs->asked_attributes, FSAL_ATTR_ATIME) ?
			 (time_t) attrs->atime.seconds : stat.st_atime);
		timebuf[0].tv_usec = 0;

		/* Mtime */
		timebuf[1].tv_sec =
			(FSAL_TEST_MASK(attrs->asked_attributes, FSAL_ATTR_MTIME) ?
			 (time_t) attrs->mtime.seconds : stat.st_mtime);
		timebuf[1].tv_usec = 0;
		if(obj_hdl->type == SOCKET_FILE)
			retval = futimesat(fd,
					   myself->u.sock.sock_name,
					   timebuf);
		else
			retval = futimes(fd, timebuf);
		if(retval != 0) {
			goto fileerr;
		}
	}
	close(fd);
	ReturnCode(fsal_error, retval);	

fileerr:	
	retval = errno;
	close(fd);
	fsal_error = posix2fsal_error(retval);
out:
	ReturnCode(fsal_error, retval);	
}

/* handle_is
 * test the type of this handle
 */

static fsal_boolean_t handle_is(struct fsal_obj_handle *obj_hdl,
				object_file_type_t type)
{
	return obj_hdl->type == type;
}

/* compare
 * compare two handles.
 * return 0 for equal, -1 for anything else
 */
static fsal_boolean_t compare(struct fsal_obj_handle *obj_hdl,
			      struct fsal_obj_handle *other_hdl)
{
	struct vfs_fsal_obj_handle *myself, *other;

	if( !other_hdl)
		return FALSE;
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	other = container_of(other_hdl, struct vfs_fsal_obj_handle, obj_handle);
	if((obj_hdl->type != other_hdl->type) ||
	   (myself->handle->handle_type != other->handle->handle_type) ||
	   (myself->handle->handle_bytes != other->handle->handle_bytes))
		return FALSE;
	return memcmp(myself->handle->f_handle,
		      other->handle->f_handle,
		      myself->handle->handle_bytes) ? FALSE : TRUE;
}

/* file_truncate
 * truncate a file to the size specified.
 * size should really be off_t...
 */

static fsal_status_t file_truncate(struct fsal_obj_handle *obj_hdl,
				   fsal_size_t length)
{
	struct vfs_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int fd, mount_fd;
	int retval = 0;

	if(obj_hdl->type != REGULAR_FILE) {
		fsal_error = ERR_FSAL_INVAL;
		goto errout;
	}
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mount_fd = vfs_get_root_fd(obj_hdl->export);
	fd = open_by_handle_at(mount_fd, myself->handle, O_PATH|O_RDWR);
	if(fd < 0) {
		retval = errno;
		if(retval == ENOENT)
			fsal_error = ERR_FSAL_STALE;
		else
			fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = ftruncate(fd, length);
	if(retval < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
	}
	close(fd);
	
errout:
	ReturnCode(fsal_error, retval);	
}

/* file_unlink
 * unlink the named file in the directory
 */

static fsal_status_t file_unlink(struct fsal_obj_handle *dir_hdl,
				 fsal_name_t *name)
{
	struct vfs_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	struct stat stat;
	int fd, mount_fd;
	int retval = 0;

	myself = container_of(dir_hdl, struct vfs_fsal_obj_handle, obj_handle);
	mount_fd = vfs_get_root_fd(dir_hdl->export);
	fd = open_by_handle_at(mount_fd, myself->handle, O_PATH|O_RDWR);
	if(fd < 0) {
		retval = errno;
		if(retval == ENOENT)
			fsal_error = ERR_FSAL_STALE;
		else
			fsal_error = posix2fsal_error(retval);
		goto out;
	}
	retval = fstatat(fd, name->name, &stat, AT_SYMLINK_NOFOLLOW);
	if(retval < 0) {
		retval = errno;
		if(retval == ENOENT)
			fsal_error = ERR_FSAL_STALE;
		else
			fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = unlinkat(fd, name->name,
			  (S_ISDIR(stat.st_mode)) ? AT_REMOVEDIR : 0);
	if(retval < 0) {
		retval = errno;
		if(retval == ENOENT)
			fsal_error = ERR_FSAL_STALE;
		else
			fsal_error = posix2fsal_error(retval);
	}
	
errout:
	close(fd);
out:
	ReturnCode(fsal_error, retval);	
}


/* handle_digest
 * fill in the opaque f/s file handle part.
 * we zero the buffer to length first.  This MAY already be done above
 * at which point, remove memset here because the caller is zeroing
 * the whole struct.
 */

static fsal_status_t handle_digest(struct fsal_obj_handle *obj_hdl,
				   fsal_digesttype_t output_type,
				   struct fsal_handle_desc *fh_desc)
{
	uint32_t ino32;
	uint64_t ino64;
	struct vfs_fsal_obj_handle *myself;
	struct file_handle *fh;
	size_t fh_size;

	/* sanity checks */
	if( !fh_desc || !fh_desc->start)
		ReturnCode(ERR_FSAL_FAULT, 0);
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	fh = myself->handle;

	switch(output_type) {
	case FSAL_DIGEST_NFSV2:
	case FSAL_DIGEST_NFSV3:
	case FSAL_DIGEST_NFSV4:
		fh_size = vfs_sizeof_handle(fh);
		if(fh_desc->len < fh_size)
			goto errout;
		memcpy(fh_desc->start, fh, fh_size);
		break;
	case FSAL_DIGEST_FILEID2:
		fh_size = FSAL_DIGEST_SIZE_FILEID2;
		if(fh_desc->len < fh_size)
			goto errout;
		memcpy(fh_desc->start, fh->f_handle, fh_size);
		break;
	case FSAL_DIGEST_FILEID3:
		fh_size = FSAL_DIGEST_SIZE_FILEID3;
		if(fh_desc->len < fh_size)
			goto errout;
		memcpy(&ino32, fh->f_handle, sizeof(ino32));
		ino64 = ino32;
		memcpy(fh_desc->start, &ino64, fh_size);
		break;
	case FSAL_DIGEST_FILEID4:
		fh_size = FSAL_DIGEST_SIZE_FILEID4;
		if(fh_desc->len < fh_size)
			goto errout;
		memcpy(&ino32, fh->f_handle, sizeof(ino32));
		ino64 = ino32;
		memcpy(fh_desc->start, &ino64, fh_size);
		break;
	default:
		ReturnCode(ERR_FSAL_SERVERFAULT, 0);
	}
	fh_desc->len = fh_size;
	ReturnCode(ERR_FSAL_NO_ERROR, 0);

errout:
	LogMajor(COMPONENT_FSAL,
		 "Space too small for handle.  need %lu, have %lu",
		 fh_size, fh_desc->len);
	ReturnCode(ERR_FSAL_TOOSMALL, 0);
}

/**
 * handle_to_key
 * return a handle descriptor into the handle in this object handle
 * @TODO reminder.  make sure things like hash keys don't point here
 * after the handle is released.
 */

static void handle_to_key(struct fsal_obj_handle *obj_hdl,
				  struct fsal_handle_desc *fh_desc)
{
	struct vfs_fsal_obj_handle *myself;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	fh_desc->start = (caddr_t)myself->handle;
	fh_desc->len = vfs_sizeof_handle(myself->handle);
}

/*
 * release
 * release our export first so they know we are gone
 */

static fsal_status_t release(struct fsal_obj_handle *obj_hdl)
{
	struct fsal_export *exp = obj_hdl->export;
	struct vfs_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);
	pthread_mutex_lock(&obj_hdl->lock);
	obj_hdl->refs--;  /* subtract the reference when we were created */
	if(obj_hdl->refs != 0 || (obj_hdl->type == REGULAR_FILE
				  && (myself->u.file.lock_status != 0
				      || myself->u.file.fd >=0
				      || myself->u.file.openflags != FSAL_O_CLOSED))) {
		pthread_mutex_unlock(&obj_hdl->lock);
		retval = obj_hdl->refs > 0 ? EBUSY : EINVAL;
		LogCrit(COMPONENT_FSAL,
			"Tried to release busy handle, "
			"hdl = 0x%p->refs = %d, fd = %d, openflags = 0x%x, lock = %d",
			obj_hdl, obj_hdl->refs,
			myself->u.file.fd, myself->u.file.openflags,
			myself->u.file.lock_status);
		ReturnCode(posix2fsal_error(retval), retval);
	}
	fsal_detach_handle(exp, &obj_hdl->handles);
	pthread_mutex_unlock(&obj_hdl->lock);
	pthread_mutex_destroy(&obj_hdl->lock);
	myself->obj_handle.ops = NULL; /*poison myself */
	myself->obj_handle.export = NULL;
	if(myself->u.symlink.link_content != NULL)
		free(myself->u.symlink.link_content);
	if(myself->u.sock.sock_name != NULL)
		free(myself->u.sock.sock_name);
	if(myself->u.sock.sock_dir != NULL)
		free(myself->u.sock.sock_dir);
	free(myself);
	ReturnCode(fsal_error, 0);
}

static struct fsal_obj_ops obj_ops = {
	.get = fsal_handle_get,
	.put = fsal_handle_put,
	.release = release,
	.lookup = lookup,
	.readdir = read_dirents,
	.create = create,
	.mkdir = makedir,
	.mknode = makenode,
	.symlink = makesymlink,
	.readlink = readsymlink,
	.test_access = fsal_test_access,
	.getattrs = getattrs,
	.setattrs = setattrs,
	.link = linkfile,
	.rename = renamefile,
	.unlink = file_unlink,
	.truncate = file_truncate,
	.open = vfs_open,
	.status = vfs_status,
	.read = vfs_read,
	.write = vfs_write,
	.commit = vfs_commit,
	.lock_op = vfs_lock_op,
	.share_op = vfs_share_op,
	.close = vfs_close,
	.rcp = vfs_rcp,
	.getextattrs = vfs_getextattrs,
	.list_ext_attrs = vfs_list_ext_attrs,
	.getextattr_id_by_name = vfs_getextattr_id_by_name,
	.getextattr_value_by_name = vfs_getextattr_value_by_name,
	.getextattr_value_by_id = vfs_getextattr_value_by_id,
	.setextattr_value = vfs_setextattr_value,
	.setextattr_value_by_id = vfs_setextattr_value_by_id,
	.getextattr_attrs = vfs_getextattr_attrs,
	.remove_extattr_by_id = vfs_remove_extattr_by_id,
	.remove_extattr_by_name = vfs_remove_extattr_by_name,
	.handle_is = handle_is,
	.lru_cleanup = vfs_lru_cleanup,
	.compare = compare,
	.handle_digest = handle_digest,
	.handle_to_key = handle_to_key
};

/* export methods that create object handles
 */

/* lookup_path
 * modeled on old api except we don't stuff attributes.
 * KISS
 */

fsal_status_t vfs_lookup_path(struct fsal_export *exp_hdl,
			      const char *path,
			      struct fsal_obj_handle **handle)
{
	int dir_fd;
	int mnt_id = 0;
	struct stat stat;
	struct vfs_fsal_obj_handle *hdl;
	char *basepart;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	char *link_content = NULL;
	ssize_t retlink;
	struct file_handle *dir_fh = NULL;
	char *sock_name = NULL;
	struct file_handle *fh
		= alloca(sizeof(struct file_handle) + MAX_HANDLE_SZ);

	memset(fh, 0, sizeof(struct file_handle) + MAX_HANDLE_SZ);
	fh->handle_bytes = MAX_HANDLE_SZ;
	if(path == NULL
	   || path[0] != '/'
	   || strlen(path) > FSAL_MAX_PATH_LEN
	   || strlen(path) < 2) {
		fsal_error = ERR_FSAL_INVAL;
		goto errout;
	}
	basepart = rindex(path, '/');
	if(basepart[1] == '\0') {
		fsal_error = ERR_FSAL_INVAL;
		goto errout;
	}
	if(basepart == path) {
		dir_fd = open("/", O_RDONLY);
	} else {
		char *dirpart = alloca(basepart - path + 1);

		memcpy(dirpart, path, basepart - path);
		dirpart[basepart - path] = '\0';
		dir_fd = open(dirpart, O_RDONLY, 0600);
	}
	if(dir_fd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = fstat(dir_fd, &stat);
	if( !S_ISDIR(stat.st_mode)) {  /* this had better be a DIR! */
		goto fileerr;
	}
	basepart++;
	retval = name_to_handle_at(dir_fd, basepart, fh, &mnt_id, 0);
	if(retval < 0) {
		goto fileerr;
	}

	/* what about the file? Do no symlink chasing here. */
	retval = fstatat(dir_fd, basepart, &stat, AT_SYMLINK_NOFOLLOW);
	if(retval < 0) {
		goto fileerr;
	}
	if(S_ISLNK(stat.st_mode)) {
		char *link_content = malloc(FSAL_MAX_PATH_LEN);

		retlink = readlinkat(dir_fd, basepart,
				     link_content, FSAL_MAX_PATH_LEN);
		if(retlink < 0 || retlink == FSAL_MAX_PATH_LEN) {
			retval = errno;
			if(retlink == FSAL_MAX_PATH_LEN)
				retval = ENAMETOOLONG;
			goto linkerr;
		}
		link_content[retlink] = '\0';
	} else if(S_ISSOCK(stat.st_mode)) { /* AF_UNIX sockets require craziness */
		dir_fh = malloc(sizeof(struct file_handle) + MAX_HANDLE_SZ);
		memset(dir_fh, 0, sizeof(struct file_handle) + MAX_HANDLE_SZ);
		dir_fh->handle_bytes = MAX_HANDLE_SZ;
		retval = name_to_handle_at(dir_fd,
					   "",
					   dir_fh,
					   &mnt_id,
					   AT_EMPTY_PATH);
		if(retval < 0) {
			goto fileerr;
		}
		sock_name = basepart;
	}
	close(dir_fd);

	/* allocate an obj_handle and fill it up */
	hdl = alloc_handle(fh, &stat, link_content, dir_fh, sock_name, exp_hdl);
	if(link_content != NULL)
		free(link_content);
	if(dir_fh != NULL)
		free(dir_fh);
	if(hdl == NULL) {
		fsal_error = ERR_FSAL_NOMEM;
		*handle = NULL; /* poison it */
		goto errout;
	}
	*handle = &hdl->obj_handle;
	ReturnCode(ERR_FSAL_NO_ERROR, 0);

fileerr:
	retval = errno;
linkerr:
	if(link_content != NULL)
		free(link_content);
	if(dir_fh != NULL)
		free(dir_fh);
	close(dir_fd);
	fsal_error = posix2fsal_error(retval);

errout:
	ReturnCode(fsal_error, retval);	
}

/* create_handle
 * Does what original FSAL_ExpandHandle did (sort of)
 * returns a ref counted handle to be later used in cache_inode etc.
 * NOTE! you must release this thing when done with it!
 * BEWARE! Thanks to some holes in the *AT syscalls implementation,
 * we cannot get an fd on an AF_UNIX socket.  Sorry, it just doesn't...
 * we could if we had the handle of the dir it is in, but this method
 * is for getting handles off the wire for cache entries that have LRU'd.
 * Ideas and/or clever hacks are welcome...
 */

fsal_status_t vfs_create_handle(struct fsal_export *exp_hdl,
				struct fsal_handle_desc *hdl_desc,
				struct fsal_obj_handle **handle)
{
	struct vfs_fsal_obj_handle *hdl;
	struct stat stat;
	struct file_handle  *fh;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;
	int fd;
	int mount_fd = vfs_get_root_fd(exp_hdl);
	char *link_content = NULL;
	ssize_t retlink;
	char link_buff[FSAL_MAX_PATH_LEN];

	

	*handle = NULL; /* poison it first */
	if((hdl_desc->len > (sizeof(struct file_handle) + MAX_HANDLE_SZ)) ||
	   (((struct file_handle *)(hdl_desc->start))->handle_bytes >  MAX_HANDLE_SZ))
		ReturnCode(ERR_FSAL_FAULT, 0);

	fh = alloca(hdl_desc->len);
	memcpy(fh, hdl_desc->start, hdl_desc->len);  /* struct aligned copy */
	fd = open_by_handle_at(mount_fd, fh, O_PATH|O_NOACCESS);
	if(fd < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto errout;
	}
	retval = fstatat(fd, "", &stat, AT_EMPTY_PATH);
	if(retval < 0) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		close(fd);
		goto errout;
	}
	if(S_ISLNK(stat.st_mode)) { /* I could lazy eval this... */
		retlink = readlinkat(fd, "", link_buff, FSAL_MAX_PATH_LEN);
		if(retlink < 0 || retlink == FSAL_MAX_PATH_LEN) {
			retval = errno;
			if(retlink == FSAL_MAX_PATH_LEN)
				retval = ENAMETOOLONG;
			fsal_error = posix2fsal_error(retval);
			close(fd);
			goto errout;
		}
		link_buff[retlink] = '\0';
		link_content = &link_buff[0];
	}
	close(fd);

	hdl = alloc_handle(fh, &stat, link_content, NULL, NULL, exp_hdl);
	if(hdl == NULL) {
		fsal_error = ERR_FSAL_NOMEM;
		goto errout;
	}
	*handle = &hdl->obj_handle;
	
errout:
	ReturnCode(fsal_error, retval);	
}
