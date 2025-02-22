/*
 *  linux/fs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

/* [Feb 1997 T. Schoebel-Theuer] Complete rewrite of the pathname
 * lookup logic.
 */
/* [Feb-Apr 2000, AV] Rewrite to the new namespace architecture.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/quotaops.h>
#include <linux/pagemap.h>
#include <linux/fsnotify.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/ima.h>
#include <linux/syscalls.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/device_cgroup.h>
#include <linux/fs_struct.h>
#include <asm/uaccess.h>

#include "internal.h"

#define ACC_MODE(x) ("\000\004\002\006"[(x)&O_ACCMODE])

/* [Feb-1997 T. Schoebel-Theuer]
 * Fundamental changes in the pathname lookup mechanisms (namei)
 * were necessary because of omirr.  The reason is that omirr needs
 * to know the _real_ pathname, not the user-supplied one, in case
 * of symlinks (and also when transname replacements occur).
 *
 * The new code replaces the old recursive symlink resolution with
 * an iterative one (in case of non-nested symlink chains).  It does
 * this with calls to <fs>_follow_link().
 * As a side effect, dir_namei(), _namei() and follow_link() are now 
 * replaced with a single function lookup_dentry() that can handle all 
 * the special cases of the former code.
 *
 * With the new dcache, the pathname is stored at each inode, at least as
 * long as the refcount of the inode is positive.  As a side effect, the
 * size of the dcache depends on the inode cache and thus is dynamic.
 *
 * [29-Apr-1998 C. Scott Ananian] Updated above description of symlink
 * resolution to correspond with current state of the code.
 *
 * Note that the symlink resolution is not *completely* iterative.
 * There is still a significant amount of tail- and mid- recursion in
 * the algorithm.  Also, note that <fs>_readlink() is not used in
 * lookup_dentry(): lookup_dentry() on the result of <fs>_readlink()
 * may return different results than <fs>_follow_link().  Many virtual
 * filesystems (including /proc) exhibit this behavior.
 */

/* [24-Feb-97 T. Schoebel-Theuer] Side effects caused by new implementation:
 * New symlink semantics: when open() is called with flags O_CREAT | O_EXCL
 * and the name already exists in form of a symlink, try to create the new
 * name indicated by the symlink. The old code always complained that the
 * name already exists, due to not following the symlink even if its target
 * is nonexistent.  The new semantics affects also mknod() and link() when
 * the name is a symlink pointing to a non-existant name.
 *
 * I don't know which semantics is the right one, since I have no access
 * to standards. But I found by trial that HP-UX 9.0 has the full "new"
 * semantics implemented, while SunOS 4.1.1 and Solaris (SunOS 5.4) have the
 * "old" one. Personally, I think the new semantics is much more logical.
 * Note that "ln old new" where "new" is a symlink pointing to a non-existing
 * file does succeed in both HP-UX and SunOs, but not in Solaris
 * and in the old Linux semantics.
 */

/* [16-Dec-97 Kevin Buhr] For security reasons, we change some symlink
 * semantics.  See the comments in "open_namei" and "do_link" below.
 *
 * [10-Sep-98 Alan Modra] Another symlink change.
 */

/* [Feb-Apr 2000 AV] Complete rewrite. Rules for symlinks:
 *	inside the path - always follow.
 *	in the last component in creation/removal/renaming - never follow.
 *	if LOOKUP_FOLLOW passed - follow.
 *	if the pathname has trailing slashes - follow.
 *	otherwise - don't follow.
 * (applied in that order).
 *
 * [Jun 2000 AV] Inconsistent behaviour of open() in case if flags==O_CREAT
 * restored for 2.4. This is the last surviving part of old 4.2BSD bug.
 * During the 2.4 we need to fix the userland stuff depending on it -
 * hopefully we will be able to get rid of that wart in 2.5. So far only
 * XEmacs seems to be relying on it...
 */
/*
 * [Sep 2001 AV] Single-semaphore locking scheme (kudos to David Holland)
 * implemented.  Let's see if raised priority of ->s_vfs_rename_mutex gives
 * any extra contention...
 */

static int __link_path_walk(struct filename *filename, struct nameidata *nd);

void final_putname(struct filename *name)
{
	if (name->separate) {
		__putname(name->name);
		kfree(name);
	} else {
		__putname(name);
	}
}

/* In order to reduce some races, while at the same time doing additional
 * checking and hopefully speeding things up, we copy filenames to the
 * kernel data space before using them..
 *
 * POSIX.1 2.4: an empty pathname is invalid (ENOENT).
 * PATH_MAX includes the nul terminator --RR.
 */
static int do_getname(const char __user *filename, char *page,
			unsigned long len)
{
	if (!segment_eq(get_fs(), KERNEL_DS)) {
		if ((unsigned long) filename >= TASK_SIZE)
			return -EFAULT;
		if (TASK_SIZE - (unsigned long) filename < len)
			len = TASK_SIZE - (unsigned long) filename;
	}

	return strncpy_from_user(page, filename, len);
}

#define EMBEDDED_NAME_MAX      (PATH_MAX - sizeof(struct filename))

struct filename *
getname(const char __user * filename)
{
	int len;
	struct filename *result, *err;
	unsigned long max;
	char *kname;

	result = audit_reusename(filename);
	if (result)
		return result;

	result = __getname();
	if (unlikely(!result))
		return ERR_PTR(-ENOMEM);

	kname = (char *)result + sizeof(*result);
	result->name = kname;
	result->separate = false;
	max = EMBEDDED_NAME_MAX;

recopy:
	len = do_getname(filename, kname, max);
	if (len < 0) {
		err = ERR_PTR(len);
		goto error;
	}

	/*
	 * Uh-oh. We have a name that's approaching PATH_MAX. Allocate a
	 * separate struct filename so we can dedicate the entire
	 * names_cache allocation for the pathname, and re-do the copy from
	 * userland.
	 */
	if (len == EMBEDDED_NAME_MAX && max == EMBEDDED_NAME_MAX) {
		kname = (char *)result;

		result = kzalloc(sizeof(*result), GFP_KERNEL);
		if (!result) {
			err = ERR_PTR(-ENOMEM);
			result = (struct filename *)kname;
			goto error;
		}
		result->name = kname;
		result->separate = true;
		max = PATH_MAX;
		goto recopy;
	}

	err = ERR_PTR(-ENOENT);
	if (unlikely(!len))
		goto error;

	err = ERR_PTR(-ENAMETOOLONG);
	if (unlikely(len >= PATH_MAX))
		goto error;

	result->uptr = filename;
	audit_getname(result);
	return result;

error:
	final_putname(result);
	return err;
}
EXPORT_SYMBOL(getname);

#ifdef CONFIG_AUDITSYSCALL
void putname(struct filename *name)
{
	if (unlikely(!audit_dummy_context()))
		return audit_putname(name);
	final_putname(name);
}
EXPORT_SYMBOL(putname);
#endif

/*
 * This does basic POSIX ACL permission checking
 */
static int acl_permission_check(struct inode *inode, int mask,
		int (*check_acl)(struct inode *inode, int mask))
{
	umode_t			mode = inode->i_mode;

	mask &= MAY_READ | MAY_WRITE | MAY_EXEC;

	if (current_fsuid() == inode->i_uid)
		mode >>= 6;
	else {
		if (IS_POSIXACL(inode) && (mode & S_IRWXG) && check_acl) {
			int error = check_acl(inode, mask);
			if (error != -EAGAIN)
				return error;
		}

		if (in_group_p(inode->i_gid))
			mode >>= 3;
	}

	/*
	 * If the DACs are ok we don't need any capability check.
	 */
	if ((mask & ~mode) == 0)
		return 0;
	return -EACCES;
}

/**
 * generic_permission  -  check for access rights on a Posix-like filesystem
 * @inode:	inode to check access rights for
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 * @check_acl:	optional callback to check for Posix ACLs
 *
 * Used to check for read/write/execute permissions on a file.
 * We use "fsuid" for this, letting us set arbitrary permissions
 * for filesystem access without changing the "normal" uids which
 * are used for other things..
 */
int generic_permission(struct inode *inode, int mask,
		int (*check_acl)(struct inode *inode, int mask))
{
	int ret;

	/*
	 * Do the basic POSIX ACL permission checks.
	 */
	ret = acl_permission_check(inode, mask, check_acl);
	if (ret != -EACCES)
		return ret;

	/*
	 * Read/write DACs are always overridable.
	 * Executable DACs are overridable if at least one exec bit is set.
	 */
	if (!(mask & MAY_EXEC) || execute_ok(inode))
		if (capable(CAP_DAC_OVERRIDE))
			return 0;

	/*
	 * Searching includes executable on directories, else just read.
	 */
	mask &= MAY_READ | MAY_WRITE | MAY_EXEC;
	if (mask == MAY_READ || (S_ISDIR(inode->i_mode) && !(mask & MAY_WRITE)))
		if (capable(CAP_DAC_READ_SEARCH))
			return 0;

	return -EACCES;
}

/**
 * inode_permission  -  check for access rights to a given inode
 * @inode:	inode to check permission on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Used to check for read/write/execute permissions on an inode.
 * We use "fsuid" for this, letting us set arbitrary permissions
 * for filesystem access without changing the "normal" uids which
 * are used for other things.
 */
int inode_permission(struct inode *inode, int mask)
{
	int retval;

	if (mask & MAY_WRITE) {
		umode_t mode = inode->i_mode;

		/*
		 * Nobody gets write access to a read-only fs.
		 */
		if (IS_RDONLY(inode) &&
		    (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))
			return -EROFS;

		/*
		 * Nobody gets write access to an immutable file.
		 */
		if (IS_IMMUTABLE(inode))
			return -EACCES;
	}

	if (inode->i_op->permission)
		retval = inode->i_op->permission(inode, mask);
	else
		retval = generic_permission(inode, mask, inode->i_op->check_acl);

	if (retval)
		return retval;

	retval = devcgroup_inode_permission(inode, mask);
	if (retval)
		return retval;

	return security_inode_permission(inode,
			mask & (MAY_READ|MAY_WRITE|MAY_EXEC|MAY_APPEND));
}

/**
 * file_permission  -  check for additional access rights to a given file
 * @file:	file to check access rights for
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Used to check for read/write/execute permissions on an already opened
 * file.
 *
 * Note:
 *	Do not use this function in new code.  All access checks should
 *	be done using inode_permission().
 */
int file_permission(struct file *file, int mask)
{
	return inode_permission(file->f_path.dentry->d_inode, mask);
}

/*
 * get_write_access() gets write permission for a file.
 * put_write_access() releases this write permission.
 * This is used for regular files.
 * We cannot support write (and maybe mmap read-write shared) accesses and
 * MAP_DENYWRITE mmappings simultaneously. The i_writecount field of an inode
 * can have the following values:
 * 0: no writers, no VM_DENYWRITE mappings
 * < 0: (-i_writecount) vm_area_structs with VM_DENYWRITE set exist
 * > 0: (i_writecount) users are writing to the file.
 *
 * Normally we operate on that counter with atomic_{inc,dec} and it's safe
 * except for the cases where we don't hold i_writecount yet. Then we need to
 * use {get,deny}_write_access() - these functions check the sign and refuse
 * to do the change if sign is wrong. Exclusion between them is provided by
 * the inode->i_lock spinlock.
 */

int get_write_access(struct inode * inode)
{
	spin_lock(&inode->i_lock);
	if (atomic_read(&inode->i_writecount) < 0) {
		spin_unlock(&inode->i_lock);
		return -ETXTBSY;
	}
	atomic_inc(&inode->i_writecount);
	spin_unlock(&inode->i_lock);

	return 0;
}

int deny_write_access(struct file * file)
{
	struct inode *inode = file->f_path.dentry->d_inode;

	spin_lock(&inode->i_lock);
	if (atomic_read(&inode->i_writecount) > 0) {
		spin_unlock(&inode->i_lock);
		return -ETXTBSY;
	}
	atomic_dec(&inode->i_writecount);
	spin_unlock(&inode->i_lock);

	return 0;
}

/**
 * path_get - get a reference to a path
 * @path: path to get the reference to
 *
 * Given a path increment the reference count to the dentry and the vfsmount.
 */
void path_get(struct path *path)
{
	mntget(path->mnt);
	dget(path->dentry);
}
EXPORT_SYMBOL(path_get);

/**
 * path_put - put a reference to a path
 * @path: path to put the reference to
 *
 * Given a path decrement the reference count to the dentry and the vfsmount.
 */
void path_put(struct path *path)
{
	dput(path->dentry);
	mntput(path->mnt);
}
EXPORT_SYMBOL(path_put);

/**
 * release_open_intent - free up open intent resources
 * @nd: pointer to nameidata
 */
void release_open_intent(struct nameidata *nd)
{
	if (nd->intent.open.file->f_path.dentry == NULL)
		put_filp(nd->intent.open.file);
	else
		fput(nd->intent.open.file);
}

static inline struct dentry *
do_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	int status = dentry->d_op->d_revalidate(dentry, nd);
	if (unlikely(status <= 0)) {
		/*
		 * The dentry failed validation.
		 * If d_revalidate returned 0 attempt to invalidate
		 * the dentry otherwise d_revalidate is asking us
		 * to return a fail status.
		 */
		if (!status) {
			if (!d_invalidate(dentry)) {
				dput(dentry);
				dentry = NULL;
			}
		} else {
			dput(dentry);
			dentry = ERR_PTR(status);
		}
	}
	return dentry;
}

/*
 * force_reval_path - force revalidation of a dentry
 *
 * In some situations the path walking code will trust dentries without
 * revalidating them. This causes problems for filesystems that depend on
 * d_revalidate to handle file opens (e.g. NFSv4). When FS_REVAL_DOT is set
 * (which indicates that it's possible for the dentry to go stale), force
 * a d_revalidate call before proceeding.
 *
 * Returns 0 if the revalidation was successful. If the revalidation fails,
 * either return the error returned by d_revalidate or -ESTALE if the
 * revalidation it just returned 0. If d_revalidate returns 0, we attempt to
 * invalidate the dentry. It's up to the caller to handle putting references
 * to the path if necessary.
 */
static int
force_reval_path(struct path *path, struct nameidata *nd)
{
	int status;
	struct dentry *dentry = path->dentry;

	/*
	 * only check on filesystems where it's possible for the dentry to
	 * become stale. It's assumed that if this flag is set then the
	 * d_revalidate op will also be defined.
	 */
	if (!(dentry->d_sb->s_type->fs_flags & FS_REVAL_DOT))
		return 0;

	status = dentry->d_op->d_revalidate(dentry, nd);
	if (status > 0)
		return 0;

	if (!status) {
		d_invalidate(dentry);
		status = -ESTALE;
	}
	return status;
}

/*
 * Short-cut version of permission(), for calling by
 * path_walk(), when dcache lock is held.  Combines parts
 * of permission() and generic_permission(), and tests ONLY for
 * MAY_EXEC permission.
 *
 * If appropriate, check DAC only.  If not appropriate, or
 * short-cut DAC fails, then call permission() to do more
 * complete permission check.
 */
static int exec_permission_lite(struct inode *inode)
{
	int ret;

	if (inode->i_op->permission) {
		ret = inode->i_op->permission(inode, MAY_EXEC);
		if (!ret)
			goto ok;
		return ret;
	}
	ret = acl_permission_check(inode, MAY_EXEC, inode->i_op->check_acl);
	if (!ret)
		goto ok;

	if (capable(CAP_DAC_OVERRIDE) || capable(CAP_DAC_READ_SEARCH))
		goto ok;

	return ret;
ok:
	return security_inode_permission(inode, MAY_EXEC);
}

/*
 * Wrapper to retry pathname resolution whenever the underlying
 * file system returns an ESTALE.
 *
 * Retry the whole path once, forcing real lookup requests
 * instead of relying on the dcache.
 */
static __always_inline int link_path_walk(struct filename *name,
					  struct nameidata *nd)
{
	struct path save = nd->path;
	int result;

	/* make sure the stuff we saved doesn't go away */
	path_get(&save);

	result = __link_path_walk(name, nd);
	if (result == -ESTALE) {
		/* nd->path had been dropped */
		nd->path = save;
		path_get(&nd->path);
		nd->flags |= LOOKUP_REVAL;
		result = __link_path_walk(name, nd);
	}

	path_put(&save);

	return result;
}

static __always_inline void set_root(struct nameidata *nd)
{
	if (!nd->root.mnt) {
		struct fs_struct *fs = current->fs;
		read_lock(&fs->lock);
		nd->root = fs->root;
		path_get(&nd->root);
		read_unlock(&fs->lock);
	}
}

static __always_inline int __vfs_follow_link(struct nameidata *nd, const char *link)
{
	int res = 0;
	char *name;
	struct filename filename = { .name = link };

	if (IS_ERR(link))
		goto fail;

	if (*link == '/') {
		set_root(nd);
		path_put(&nd->path);
		nd->path = nd->root;
		path_get(&nd->root);
	}

	res = link_path_walk(&filename, nd);
	if (nd->depth || res || nd->last_type!=LAST_NORM)
		return res;
	/*
	 * If it is an iterative symlinks resolution in open_namei() we
	 * have to copy the last component. And all that crap because of
	 * bloody create() on broken symlinks. Furrfu...
	 */
	name = __getname();
	if (unlikely(!name)) {
		path_put(&nd->path);
		return -ENOMEM;
	}
	strcpy(name, nd->last.name);
	nd->last.name = name;
	return 0;
fail:
	path_put(&nd->path);
	return PTR_ERR(link);
}

static void path_put_conditional(struct path *path, struct nameidata *nd)
{
	dput(path->dentry);
	if (path->mnt != nd->path.mnt)
		mntput(path->mnt);
}

static inline void path_to_nameidata(struct path *path, struct nameidata *nd)
{
	dput(nd->path.dentry);
	if (nd->path.mnt != path->mnt)
		mntput(nd->path.mnt);
	nd->path.mnt = path->mnt;
	nd->path.dentry = path->dentry;
}

static __always_inline int __do_follow_link(struct path *path, struct nameidata *nd)
{
	int error;
	void *cookie;
	struct dentry *dentry = path->dentry;

	touch_atime(path->mnt, dentry);
	nd_set_link(nd, NULL);

	if (path->mnt == nd->path.mnt)
		mntget(path->mnt);
	nd->last_type = LAST_BIND;
	cookie = dentry->d_inode->i_op->follow_link(dentry, nd);
	error = PTR_ERR(cookie);
	if (!IS_ERR(cookie)) {
		char *s = nd_get_link(nd);
		error = 0;
		if (s)
			error = __vfs_follow_link(nd, s);
		else if (nd->last_type == LAST_BIND) {
			error = force_reval_path(&nd->path, nd);
			if (error) {
				if (unlikely(!audit_dummy_context()))
					audit_inode(NULL, path->dentry,
						nd->flags & LOOKUP_PARENT);
				path_put(&nd->path);
			}
		}
		if (dentry->d_inode->i_op->put_link)
			dentry->d_inode->i_op->put_link(dentry, nd, cookie);
	} else {
		if (unlikely(!audit_dummy_context()))
			audit_inode(NULL, path->dentry,
					nd->flags & LOOKUP_PARENT);
	}
	path_put(path);

	return error;
}

/*
 * This limits recursive symlink follows to 8, while
 * limiting consecutive symlinks to 40.
 *
 * Without that kind of total limit, nasty chains of consecutive
 * symlinks can cause almost arbitrarily long lookups. 
 */
static inline int do_follow_link(struct path *path, struct nameidata *nd)
{
	int err = -ELOOP;
	if (current->link_count >= MAX_NESTED_LINKS)
		goto loop;
	if (current->total_link_count >= 40)
		goto loop;
	BUG_ON(nd->depth >= MAX_NESTED_LINKS);
	cond_resched();
	err = security_inode_follow_link(path->dentry, nd);
	if (err)
		goto loop;
	current->link_count++;
	current->total_link_count++;
	nd->depth++;
	err = __do_follow_link(path, nd);
	current->link_count--;
	nd->depth--;
	return err;
loop:
	path_put_conditional(path, nd);
	path_put(&nd->path);
	return err;
}

int follow_up(struct path *path)
{
	struct vfsmount *parent;
	struct dentry *mountpoint;
	spin_lock(&vfsmount_lock);
	parent = path->mnt->mnt_parent;
	if (parent == path->mnt) {
		spin_unlock(&vfsmount_lock);
		return 0;
	}
	mntget(parent);
	mountpoint = dget(path->mnt->mnt_mountpoint);
	spin_unlock(&vfsmount_lock);
	dput(path->dentry);
	path->dentry = mountpoint;
	mntput(path->mnt);
	path->mnt = parent;
	return 1;
}

/*
 * Perform an automount
 * - return -EISDIR to tell follow_managed() to stop and return the path we
 *   were called with.
 */
static int follow_automount(struct path *path, unsigned flags,
			    bool *need_mntput)
{
	struct vfsmount *mnt;
	int err;

	if (!path->dentry->d_op || !path->dentry->d_op->d_automount)
		return -EREMOTE;

	/* We don't want to mount if someone's just doing a stat -
	 * unless they're stat'ing a directory and appended a '/' to
	 * the name.
	 *
	 * We do, however, want to mount if someone wants to open or
	 * create a file of any type under the mountpoint, wants to
	 * traverse through the mountpoint or wants to open the
	 * mounted directory.  Also, autofs may mark negative dentries
	 * as being automount points.  These will need the attentions
	 * of the daemon to instantiate them before they can be used.
	 */
	if (!(flags & (LOOKUP_CONTINUE | LOOKUP_DIRECTORY |
		     LOOKUP_OPEN | LOOKUP_CREATE | LOOKUP_AUTOMOUNT)) &&
	    path->dentry->d_inode)
		return -EISDIR;

	current->total_link_count++;
	if (current->total_link_count >= 40)
		return -ELOOP;

	mnt = path->dentry->d_op->d_automount(path);
	if (IS_ERR(mnt)) {
		/*
		 * The filesystem is allowed to return -EISDIR here to indicate
		 * it doesn't want to automount.  For instance, autofs would do
		 * this so that its userspace daemon can mount on this dentry.
		 *
		 * However, we can only permit this if it's a terminal point in
		 * the path being looked up; if it wasn't then the remainder of
		 * the path is inaccessible and we should say so.
		 */
		if (PTR_ERR(mnt) == -EISDIR && (flags & LOOKUP_CONTINUE))
			return -EREMOTE;
		return PTR_ERR(mnt);
	}

	if (!mnt) /* mount collision */
		return 0;

	if (!*need_mntput) {
		/* lock_mount() may release path->mnt on error */
		mntget(path->mnt);
		*need_mntput = true;
	}
	err = finish_automount(mnt, path);

	switch (err) {
	case -EBUSY:
		/* Someone else made a mount here whilst we were busy */
		return 0;
	case 0:
		path_put(path);
		path->mnt = mnt;
		path->dentry = dget(mnt->mnt_root);
		return 0;
	default:
		return err;
	}

}

static int managed_d_mountpoint(struct dentry *dentry)
{
	return d_mountpoint(dentry) ? DCACHE_MOUNTED : 0;
}

/*
 * Handle a dentry that is managed in some way.
 * - Flagged for transit management (autofs)
 * - Flagged as mountpoint
 * - Flagged as automount point
 */
static int follow_managed(struct path *path, unsigned flags)
{
	struct vfsmount *mnt = path->mnt; /* held by caller, must be left alone */
	unsigned managed;
	bool need_mntput = false;
	int ret = 0;

	/* Given that we're not holding a lock here, we retain the value in a
	 * local variable for each dentry as we look at it so that we don't see
	 * the components of that value change under us */
	while (managed = ACCESS_ONCE(path->dentry->d_flags)|
		managed_d_mountpoint(path->dentry),
	       managed &= DCACHE_MANAGED_DENTRY,
	       unlikely(managed != 0)) {
		/* Allow the filesystem to manage the transit without i_mutex
		 * being held. */
		if (managed & DCACHE_MANAGE_TRANSIT) {
			BUG_ON(!path->dentry->d_op);
			BUG_ON(!path->dentry->d_op->d_manage);
			ret = path->dentry->d_op->d_manage(path->dentry, false);
			if (ret < 0)
				break;
		}

		/* Transit to a mounted filesystem. */
		if (managed & DCACHE_MOUNTED) {
			struct vfsmount *mounted = lookup_mnt(path);
			if (mounted) {
				dput(path->dentry);
				if (need_mntput)
					mntput(path->mnt);
				path->mnt = mounted;
				path->dentry = dget(mounted->mnt_root);
				need_mntput = true;
				continue;
			}

			/* Something is mounted on this dentry in another
			 * namespace and/or whatever was mounted there in this
			 * namespace got unmounted before we managed to get the
			 * vfsmount_lock */
		}

		/* Handle an automount point */
		if (managed & DCACHE_NEED_AUTOMOUNT) {
			ret = follow_automount(path, flags, &need_mntput);
			if (ret < 0)
				break;
			continue;
		}

		/* We didn't change the current path point */
		break;
	}

	if (need_mntput && path->mnt == mnt)
		mntput(path->mnt);
	if (ret == -EISDIR)
		ret = 0;
	return ret;
}

/*
 * Skip to top of mountpoint pile for follow_dotdot()
 */
static void follow_mount(struct path *path)
{
	while (d_mountpoint(path->dentry)) {
		struct vfsmount *mounted = lookup_mnt(path);
		if (!mounted)
			break;
		dput(path->dentry);
		mntput(path->mnt);
		path->mnt = mounted;
		path->dentry = dget(mounted->mnt_root);
	}
}

/* no need for dcache_lock, as serialization is taken care in
 * namespace.c
 */
int follow_down(struct path *path)
{
	struct vfsmount *mounted;

	mounted = lookup_mnt(path);
	if (mounted) {
		dput(path->dentry);
		mntput(path->mnt);
		path->mnt = mounted;
		path->dentry = dget(mounted->mnt_root);
		return 1;
	}
	return 0;
}

/*
 * Follow down to the covering mount currently visible to userspace.  At each
 * point, the filesystem owning that dentry may be queried as to whether the
 * caller is permitted to proceed or not.
 *
 * Care must be taken as namespace_sem may be held (indicated by mounting_here
 * being true).
 */
int __follow_down(struct path *path, bool mounting_here)
{
	unsigned managed;
	int ret;

	while (managed = ACCESS_ONCE(path->dentry->d_flags)|
		managed_d_mountpoint(path->dentry),
	       unlikely(managed & DCACHE_MANAGED_DENTRY)) {
		/* Allow the filesystem to manage the transit without i_mutex
		 * being held.
		 *
		 * We indicate to the filesystem if someone is trying to mount
		 * something here.  This gives autofs the chance to deny anyone
		 * other than its daemon the right to mount on its
		 * superstructure.
		 *
		 * The filesystem may sleep at this point.
		 */
		if (managed & DCACHE_MANAGE_TRANSIT) {
			BUG_ON(!path->dentry->d_op);
			BUG_ON(!path->dentry->d_op->d_manage);
			ret = path->dentry->d_op->d_manage(path->dentry, mounting_here);
			if (ret < 0)
				return ret == -EISDIR ? 0 : ret;
		}

		/* Transit to a mounted filesystem. */
		if (managed & DCACHE_MOUNTED) {
			struct vfsmount *mounted = lookup_mnt(path);
			if (!mounted)
				break;
			dput(path->dentry);
			mntput(path->mnt);
			path->mnt = mounted;
			path->dentry = dget(mounted->mnt_root);
			continue;
		}

		/* Don't handle automount points here */
		break;
	}
	return 0;
}

static __always_inline void follow_dotdot(struct nameidata *nd)
{
	set_root(nd);

	while(1) {
		struct vfsmount *parent;
		struct dentry *old = nd->path.dentry;

		if (nd->path.dentry == nd->root.dentry &&
		    nd->path.mnt == nd->root.mnt) {
			break;
		}
		spin_lock(&dcache_lock);
		if (nd->path.dentry != nd->path.mnt->mnt_root) {
			nd->path.dentry = dget(nd->path.dentry->d_parent);
			spin_unlock(&dcache_lock);
			dput(old);
			break;
		}
		spin_unlock(&dcache_lock);
		spin_lock(&vfsmount_lock);
		parent = nd->path.mnt->mnt_parent;
		if (parent == nd->path.mnt) {
			spin_unlock(&vfsmount_lock);
			break;
		}
		mntget(parent);
		nd->path.dentry = dget(nd->path.mnt->mnt_mountpoint);
		spin_unlock(&vfsmount_lock);
		dput(old);
		mntput(nd->path.mnt);
		nd->path.mnt = parent;
	}
	follow_mount(&nd->path);
}

/*
 *  It's more convoluted than I'd like it to be, but... it's still fairly
 *  small and for now I'd prefer to have fast path as straight as possible.
 *  It _is_ time-critical.
 */
static int do_lookup(struct nameidata *nd, struct qstr *name,
		     struct path *path)
{
	struct vfsmount *mnt = nd->path.mnt;
	struct dentry *dentry = __d_lookup(nd->path.dentry, name);
	int flags = nd->flags;
	struct dentry *parent;
	struct inode *dir;
	int err;

	if (!dentry)
		goto need_lookup;
found:
	if (dentry->d_op && dentry->d_op->d_revalidate)
		goto need_revalidate;
done:
	path->mnt = mnt;
	path->dentry = dentry;
	/*
	 * Make sure follow_automount() knows about the trailing
	 * "/" but only for the real last path component.
	 */
	if (!(nd->flags & LOOKUP_CONTINUE) && name->name[name->len] == '/')
		flags |= LOOKUP_DIRECTORY;
	err = follow_managed(path, flags);
	if (unlikely(err < 0))
		path_put_conditional(path, nd);
	return err;

need_lookup:
	parent = nd->path.dentry;
	dir = parent->d_inode;

	mutex_lock(&dir->i_mutex);
	/*
	 * First re-do the cached lookup just in case it was created
	 * while we waited for the directory semaphore..
	 *
	 * FIXME! This could use version numbering or similar to
	 * avoid unnecessary cache lookups.
	 *
	 * The "dcache_lock" is purely to protect the RCU list walker
	 * from concurrent renames at this point (we mustn't get false
	 * negatives from the RCU list walk here, unlike the optimistic
	 * fast walk).
	 *
	 * so doing d_lookup() (with seqlock), instead of lockfree __d_lookup
	 */
	dentry = d_lookup(parent, name);
	if (!dentry) {
		struct dentry *new;

		/* Don't create child dentry for a dead directory. */
		dentry = ERR_PTR(-ENOENT);
		if (IS_DEADDIR(dir))
			goto out_unlock;

		new = d_alloc(parent, name);
		dentry = ERR_PTR(-ENOMEM);
		if (new) {
			dentry = dir->i_op->lookup(dir, new, nd);
			if (dentry)
				dput(new);
			else
				dentry = new;
		}
out_unlock:
		mutex_unlock(&dir->i_mutex);
		if (IS_ERR(dentry))
			goto fail;
		goto done;
	}

	/*
	 * Uhhuh! Nasty case: the cache was re-populated while
	 * we waited on the semaphore. Need to revalidate.
	 */
	mutex_unlock(&dir->i_mutex);
	goto found;

need_revalidate:
	dentry = do_revalidate(dentry, nd);
	if (!dentry)
		goto need_lookup;
	if (IS_ERR(dentry))
		goto fail;
	goto done;

fail:
	return PTR_ERR(dentry);
}

/*
 * This is a temporary kludge to deal with "automount" symlinks; proper
 * solution is to trigger them on follow_mount(), so that do_lookup()
 * would DTRT.  To be killed before 2.6.34-final.
 */
static inline int follow_on_final(struct inode *inode, unsigned lookup_flags)
{
	return inode && unlikely(inode->i_op->follow_link) &&
		((lookup_flags & LOOKUP_FOLLOW) || S_ISDIR(inode->i_mode));
}

/*
 * Name resolution.
 * This is the basic name resolution function, turning a pathname into
 * the final dentry. We expect 'base' to be positive and a directory.
 *
 * Returns 0 and nd will have valid dentry and mnt on success.
 * Returns error and drops reference to input namei data on failure.
 */
static int __link_path_walk(struct filename *filename, struct nameidata *nd)
{
	struct path next;
	struct inode *inode;
	int err;
	unsigned int lookup_flags = nd->flags;
	const char *name = filename->name;
	
	while (*name=='/')
		name++;
	if (!*name)
		goto return_reval;

	inode = nd->path.dentry->d_inode;
	if (nd->depth)
		lookup_flags = LOOKUP_FOLLOW | (nd->flags & LOOKUP_CONTINUE);

	/* At this point we know we have a real path component. */
	for(;;) {
		unsigned long hash;
		struct qstr this;
		unsigned int c;

		nd->flags |= LOOKUP_CONTINUE;
		err = exec_permission_lite(inode);
 		if (err)
			break;

		this.name = name;
		c = *(const unsigned char *)name;

		hash = init_name_hash();
		do {
			name++;
			hash = partial_name_hash(c, hash);
			c = *(const unsigned char *)name;
		} while (c && (c != '/'));
		this.len = name - (const char *) this.name;
		this.hash = end_name_hash(hash);

		/* remove trailing slashes? */
		if (!c)
			goto last_component;
		while (*++name == '/');
		if (!*name)
			goto last_with_slashes;

		/*
		 * "." and ".." are special - ".." especially so because it has
		 * to be able to know about the current root directory and
		 * parent relationships.
		 */
		if (this.name[0] == '.') switch (this.len) {
			default:
				break;
			case 2:	
				if (this.name[1] != '.')
					break;
				follow_dotdot(nd);
				inode = nd->path.dentry->d_inode;
				/* fallthrough */
			case 1:
				continue;
		}
		/*
		 * See if the low-level filesystem might want
		 * to use its own hash..
		 */
		if (nd->path.dentry->d_op && nd->path.dentry->d_op->d_hash) {
			err = nd->path.dentry->d_op->d_hash(nd->path.dentry,
							    &this);
			if (err < 0)
				break;
		}
		/* This does the actual lookups.. */
		err = do_lookup(nd, &this, &next);
		if (err)
			break;

		err = -ENOENT;
		inode = next.dentry->d_inode;
		if (!inode)
			goto out_dput;

		if (inode->i_op->follow_link) {
			err = do_follow_link(&next, nd);
			if (err)
				goto return_err;
			err = -ENOENT;
			inode = nd->path.dentry->d_inode;
			if (!inode)
				break;
		} else
			path_to_nameidata(&next, nd);
		err = -ENOTDIR; 
		if (!inode->i_op->lookup)
			break;
		continue;
		/* here ends the main loop */

last_with_slashes:
		lookup_flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
last_component:
		/* Clear LOOKUP_CONTINUE iff it was previously unset */
		nd->flags &= lookup_flags | ~LOOKUP_CONTINUE;
		if (lookup_flags & LOOKUP_PARENT)
			goto lookup_parent;
		if (this.name[0] == '.') switch (this.len) {
			default:
				break;
			case 2:	
				if (this.name[1] != '.')
					break;
				follow_dotdot(nd);
				inode = nd->path.dentry->d_inode;
				/* fallthrough */
			case 1:
				goto return_reval;
		}
		if (nd->path.dentry->d_op && nd->path.dentry->d_op->d_hash) {
			err = nd->path.dentry->d_op->d_hash(nd->path.dentry,
							    &this);
			if (err < 0)
				break;
		}
		err = do_lookup(nd, &this, &next);
		if (err)
			break;
		inode = next.dentry->d_inode;
		if (follow_on_final(inode, lookup_flags)) {
			err = do_follow_link(&next, nd);
			if (err)
				goto return_err;
			inode = nd->path.dentry->d_inode;
		} else
			path_to_nameidata(&next, nd);
		err = -ENOENT;
		if (!inode)
			break;
		if (lookup_flags & LOOKUP_DIRECTORY) {
			err = -ENOTDIR; 
			if (!inode->i_op->lookup)
				break;
		}
		goto return_base;
lookup_parent:
		nd->last = this;
		nd->last_type = LAST_NORM;
		if (this.name[0] != '.')
			goto return_base;
		if (this.len == 1)
			nd->last_type = LAST_DOT;
		else if (this.len == 2 && this.name[1] == '.')
			nd->last_type = LAST_DOTDOT;
		else
			goto return_base;
return_reval:
		/*
		 * We bypassed the ordinary revalidation routines.
		 * We may need to check the cached dentry for staleness.
		 */
		if (nd->path.dentry && nd->path.dentry->d_sb) {
			int fs_flags = nd->path.dentry->d_sb->s_type->fs_flags;

			if (fs_flags & FS_REVAL_DOT) {
				err = -ESTALE;

				/* Note: we do not d_invalidate() */
				if (fs_flags & FS_WEAK_REVALIDATE) {
					if (!nd->path.dentry->d_op->d_weak_revalidate(nd->path.dentry, nd))
						break;
				} else {
					if (!nd->path.dentry->d_op->d_revalidate(nd->path.dentry, nd))
						break;
				}
			}
		}
return_base:
		return 0;
out_dput:
		path_put_conditional(&next, nd);
		break;
	}
	if (unlikely(!audit_dummy_context()) && nd->path.dentry->d_inode)
		audit_inode(filename, nd->path.dentry,
				nd->flags & LOOKUP_PARENT);
	path_put(&nd->path);
return_err:
	return err;
}

static int path_walk(struct filename *name, struct nameidata *nd)
{
	current->total_link_count = 0;
	return link_path_walk(name, nd);
}

static int path_init(int dfd, const char *name, unsigned int flags, struct nameidata *nd)
{
	int retval = 0;
	int fput_needed;
	struct file *file;

	nd->last_type = LAST_ROOT; /* if there are only slashes... */
	nd->flags = flags;
	nd->depth = 0;
	nd->root.mnt = NULL;

	if (*name=='/') {
		set_root(nd);
		nd->path = nd->root;
		path_get(&nd->root);
	} else if (dfd == AT_FDCWD) {
		struct fs_struct *fs = current->fs;
		read_lock(&fs->lock);
		nd->path = fs->pwd;
		path_get(&fs->pwd);
		read_unlock(&fs->lock);
	} else {
		struct dentry *dentry;

		file = fget_light(dfd, &fput_needed);
		retval = -EBADF;
		if (!file)
			goto out_fail;

		dentry = file->f_path.dentry;

		retval = -ENOTDIR;
		if (!S_ISDIR(dentry->d_inode->i_mode))
			goto fput_fail;

		/*
		 * __link_path_walk will shortcut the permissions check if the
		 * pathname is empty, so we must handle it here.
		 */
		if (unlikely(*name == '\0')) {
			retval = file_permission(file, MAY_EXEC);
			if (retval)
				goto fput_fail;
		}

		nd->path = file->f_path;
		path_get(&file->f_path);

		fput_light(file, fput_needed);
	}
	return 0;

fput_fail:
	fput_light(file, fput_needed);
out_fail:
	return retval;
}

/* Returns 0 and nd will be valid on success; Retuns error, otherwise. */
static int filename_lookup(int dfd, struct filename *filename,
				unsigned int flags, struct nameidata *nd)
{
	int retval = path_init(dfd, filename->name, flags, nd);
	if (!retval)
		retval = path_walk(filename, nd);
	if (unlikely(!retval && !audit_dummy_context() && nd->path.dentry &&
				nd->path.dentry->d_inode))
		audit_inode(filename, nd->path.dentry, flags & LOOKUP_PARENT);
	if (nd->root.mnt) {
		path_put(&nd->root);
		nd->root.mnt = NULL;
	}
	return retval;
}

static int do_path_lookup(int dfd, const char *name,
				unsigned int flags, struct nameidata *nd)
{
	struct filename filename = { .name = name };

	return filename_lookup(dfd, &filename, flags, nd);
}

int path_lookup(const char *name, unsigned int flags,
			struct nameidata *nd)
{
	return do_path_lookup(AT_FDCWD, name, flags, nd);
}

int kern_path(const char *name, unsigned int flags, struct path *path)
{
	struct nameidata nd;
	int res = do_path_lookup(AT_FDCWD, name, flags, &nd);
	if (!res)
		*path = nd.path;
	return res;
}

/**
 * vfs_path_lookup - lookup a file path relative to a dentry-vfsmount pair
 * @dentry:  pointer to dentry of the base directory
 * @mnt: pointer to vfs mount of the base directory
 * @name: pointer to file name
 * @flags: lookup flags
 * @nd: pointer to nameidata
 */
int vfs_path_lookup(struct dentry *dentry, struct vfsmount *mnt,
		    const char *name, unsigned int flags,
		    struct nameidata *nd)
{
	int retval;
	struct filename filename = { .name = name };

	/* same as do_path_lookup */
	nd->last_type = LAST_ROOT;
	nd->flags = flags;
	nd->depth = 0;

	nd->path.dentry = dentry;
	nd->path.mnt = mnt;
	path_get(&nd->path);
	nd->root = nd->path;
	path_get(&nd->root);

	retval = path_walk(&filename, nd);
	if (unlikely(!retval && !audit_dummy_context() && nd->path.dentry &&
				nd->path.dentry->d_inode))
		audit_inode(&filename, nd->path.dentry, flags & LOOKUP_PARENT);

	path_put(&nd->root);
	nd->root.mnt = NULL;

	return retval;
}

static struct dentry *__lookup_hash(struct qstr *name,
		struct dentry *base, struct nameidata *nd)
{
	struct dentry *dentry;
	struct inode *inode;
	int err;

	inode = base->d_inode;

	/*
	 * See if the low-level filesystem might want
	 * to use its own hash..
	 */
	if (base->d_op && base->d_op->d_hash) {
		err = base->d_op->d_hash(base, name);
		dentry = ERR_PTR(err);
		if (err < 0)
			goto out;
	}

	dentry = __d_lookup(base, name);

	/* lockess __d_lookup may fail due to concurrent d_move()
	 * in some unrelated directory, so try with d_lookup
	 */
	if (!dentry)
		dentry = d_lookup(base, name);

	if (dentry && dentry->d_op && dentry->d_op->d_revalidate)
		dentry = do_revalidate(dentry, nd);


	if (!dentry) {
		struct dentry *new;

		/* Don't create child dentry for a dead directory. */
		dentry = ERR_PTR(-ENOENT);
		if (IS_DEADDIR(inode))
			goto out;

		new = d_alloc(base, name);
		dentry = ERR_PTR(-ENOMEM);
		if (!new)
			goto out;
		dentry = inode->i_op->lookup(inode, new, nd);
		if (!dentry)
			dentry = new;
		else
			dput(new);
	}
out:
	return dentry;
}

/*
 * Restricted form of lookup. Doesn't follow links, single-component only,
 * needs parent already locked. Doesn't follow mounts.
 * SMP-safe.
 */
static struct dentry *lookup_hash(struct nameidata *nd)
{
	int err;

	err = inode_permission(nd->path.dentry->d_inode, MAY_EXEC);
	if (err)
		return ERR_PTR(err);
	return __lookup_hash(&nd->last, nd->path.dentry, nd);
}

static int __lookup_one_len(const char *name, struct qstr *this,
		struct dentry *base, int len)
{
	unsigned long hash;
	unsigned int c;

	this->name = name;
	this->len = len;
	if (!len)
		return -EACCES;

	hash = init_name_hash();
	while (len--) {
		c = *(const unsigned char *)name++;
		if (c == '/' || c == '\0')
			return -EACCES;
		hash = partial_name_hash(c, hash);
	}
	this->hash = end_name_hash(hash);
	return 0;
}

/**
 * lookup_one_len - filesystem helper to lookup single pathname component
 * @name:	pathname component to lookup
 * @base:	base directory to lookup from
 * @len:	maximum length @len should be interpreted to
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code.  Also note that by using this function the
 * nameidata argument is passed to the filesystem methods and a filesystem
 * using this helper needs to be prepared for that.
 */
struct dentry *lookup_one_len(const char *name, struct dentry *base, int len)
{
	int err;
	struct qstr this;

	WARN_ON_ONCE(!mutex_is_locked(&base->d_inode->i_mutex));

	err = __lookup_one_len(name, &this, base, len);
	if (err)
		return ERR_PTR(err);

	err = inode_permission(base->d_inode, MAY_EXEC);
	if (err)
		return ERR_PTR(err);
	return __lookup_hash(&this, base, NULL);
}

/**
 * lookup_one_noperm - bad hack for sysfs
 * @name:	pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This is a variant of lookup_one_len that doesn't perform any permission
 * checks.   It's a horrible hack to work around the braindead sysfs
 * architecture and should not be used anywhere else.
 *
 * DON'T USE THIS FUNCTION EVER, thanks.
 */
struct dentry *lookup_one_noperm(const char *name, struct dentry *base)
{
	int err;
	struct qstr this;

	err = __lookup_one_len(name, &this, base, strlen(name));
	if (err)
		return ERR_PTR(err);
	return __lookup_hash(&this, base, NULL);
}

int user_path_at(int dfd, const char __user *name, unsigned flags,
		 struct path *path)
{
	struct nameidata nd;
	struct filename *tmp = getname(name);
	int err = PTR_ERR(tmp);
	if (!IS_ERR(tmp)) {

		BUG_ON(flags & LOOKUP_PARENT);

		err = filename_lookup(dfd, tmp, flags, &nd);
		putname(tmp);
		if (!err)
			*path = nd.path;
	}
	return err;
}

static struct filename *
user_path_parent(int dfd, const char __user *path, struct nameidata *nd,
		 unsigned int flags)
{
	struct filename *s = getname(path);
	int error;

	/* only LOOKUP_REVAL is allowed in extra flags */
	flags &= LOOKUP_REVAL;

	if (IS_ERR(s))
		return s;

	error = filename_lookup(dfd, s, flags | LOOKUP_PARENT, nd);
	if (error) {
		putname(s);
		return ERR_PTR(error);
	}

	return s;
}

/**
 * mountpoint_last - look up last component for umount
 * @nd:   pathwalk nameidata - currently pointing at parent directory of "last"
 * @path: pointer to container for result
 *
 * This is a special lookup_last function just for umount. In this case, we
 * need to resolve the path without doing any revalidation.
 *
 * The nameidata should be the result of doing a LOOKUP_PARENT pathwalk. Since
 * mountpoints are always pinned in the dcache, their ancestors are too. Thus,
 * in almost all cases, this lookup will be served out of the dcache. The only
 * cases where it won't are if nd->last refers to a symlink or the path is
 * bogus and it doesn't exist.
 *
 * Returns:
 * -error: if there was an error during lookup. This includes -ENOENT if the
 *         lookup found a negative dentry. The nd->path reference will also be
 *         put in this case.
 *
 * 0:      if we successfully resolved nd->path and found it to not to be a
 *         symlink that needs to be followed. "path" will also be populated.
 *         The nd->path reference will also be put.
 *
 * 1:      if we successfully resolved nd->last and found it to be a symlink
 *         that needs to be followed. "path" will be populated with the path
 *         to the link, and nd->path will *not* be put.
 */
static int
mountpoint_last(struct nameidata *nd, struct path *path)
{
	int error = 0;
	struct dentry *dentry;
	struct dentry *parent = nd->path.dentry;
	struct inode *dir = parent->d_inode;

	nd->flags &= ~LOOKUP_PARENT;

	if (unlikely(nd->last_type != LAST_NORM)) {
		follow_dotdot(nd);
		dentry = dget(nd->path.dentry);
		goto done;
	}

	mutex_lock(&dir->i_mutex);
	dentry = d_lookup(parent, &nd->last);
	if (!dentry) {
		struct dentry *new;
		/*
		 * No cached dentry. Mounted dentries are pinned in the cache,
		 * so that means that this dentry is probably a symlink or the
		 * path doesn't actually point to a mounted dentry.
		 */
		if (IS_DEADDIR(dir)) {
			error = -ENOENT;
			goto out_unlock;
		}

		new = d_alloc(parent, &nd->last);
		if (!new) {
			error = -ENOMEM;
			goto out_unlock;
		}
		dentry = dir->i_op->lookup(dir, new, nd);
		if (IS_ERR(dentry)) {
			error = PTR_ERR(dentry);
			goto out_unlock;
		}
		if (dentry)
			dput(new);
		else
			dentry = new;
	}
	mutex_unlock(&dir->i_mutex);

done:
	if (!dentry->d_inode) {
		error = -ENOENT;
		dput(dentry);
		goto out;
	}
	path->dentry = dentry;
	path->mnt = mntget(nd->path.mnt);
	if (nd->flags & LOOKUP_FOLLOW) {
		if (unlikely(dentry->d_inode->i_op->follow_link))
			return 1;
	}
	follow_mount(path);
	error = 0;
	goto out;

out_unlock:
	mutex_unlock(&dir->i_mutex);
out:
	path_put(&nd->path);
	return error;
}

/**
 * path_mountpoint - look up a path to a mount point
 * @dfd:	directory file descriptor to start walk from
 * @name:	full pathname to walk
 * @flags:	lookup flags
 * @nd:		pathwalk nameidata
 *
 * Look up the given name, but don't attempt to revalidate the last component.
 * Returns 0 and "path" will be valid on success; Retuns error otherwise.
 */
static int
path_mountpoint(int dfd, struct filename *s, struct path *path, unsigned int flags)
{
	struct nameidata nd;
	int err;

	err = path_init(dfd, s->name, flags | LOOKUP_PARENT, &nd);
	if (unlikely(err))
		return err;

	current->total_link_count = 0;
	err = __link_path_walk(s, &nd);
	if (err)
		goto out;

	err = mountpoint_last(&nd, path);
	while (err > 0) {
		struct path link = *path;
		err = -ELOOP;
		current->total_link_count++;
		if (current->total_link_count >= 40)
			break;
		nd.flags |= LOOKUP_PARENT;
		err = __do_follow_link(&link, &nd);
		if (err)
			break;
		err = mountpoint_last(&nd, path);
	}
out:
	if (nd.root.mnt)
		path_put(&nd.root);

	return err;
}

static int
filename_mountpoint(int dfd, struct filename *s, struct path *path,
			unsigned int flags)
{
	int error = path_mountpoint(dfd, s, path, flags);
	if (unlikely(error == -ESTALE))
		error = path_mountpoint(dfd, s, path, flags | LOOKUP_REVAL);
	if (likely(!error))
		audit_inode(s, path->dentry, 0);
	return error;
}

/**
 * user_path_mountpoint_at - lookup a path from userland in order to umount it
 * @dfd:	directory file descriptor
 * @name:	pathname from userland
 * @flags:	lookup flags
 * @path:	pointer to container to hold result
 *
 * A umount is a special case for path walking. We're not actually interested
 * in the inode in this situation, and ESTALE errors can be a problem. We
 * simply want track down the dentry and vfsmount attached at the mountpoint
 * and avoid revalidating the last component.
 *
 * Returns 0 and populates "path" on success.
 */
int
user_path_mountpoint_at(int dfd, const char __user *name, unsigned int flags,
			struct path *path)
{
	struct filename *s = getname(name);
	int error;
	if (IS_ERR(s))
		return PTR_ERR(s);
	error = filename_mountpoint(dfd, s, path, flags);
	putname(s);
	return error;
}

int
kern_path_mountpoint(int dfd, const char *name, struct path *path,
			unsigned int flags)
{
	struct filename s = {.name = name};
	return filename_mountpoint(dfd, &s, path, flags);
}
EXPORT_SYMBOL(kern_path_mountpoint);

/*
 * It's inline, so penalty for filesystems that don't use sticky bit is
 * minimal.
 */
static inline int check_sticky(struct inode *dir, struct inode *inode)
{
	uid_t fsuid = current_fsuid();

	if (!(dir->i_mode & S_ISVTX))
		return 0;
	if (inode->i_uid == fsuid)
		return 0;
	if (dir->i_uid == fsuid)
		return 0;
	return !capable(CAP_FOWNER);
}

/*
 *	Check whether we can remove a link victim from directory dir, check
 *  whether the type of victim is right.
 *  1. We can't do it if dir is read-only (done in permission())
 *  2. We should have write and exec permissions on dir
 *  3. We can't remove anything from append-only dir
 *  4. We can't do anything with immutable dir (done in permission())
 *  5. If the sticky bit on dir is set we should either
 *	a. be owner of dir, or
 *	b. be owner of victim, or
 *	c. have CAP_FOWNER capability
 *  6. If the victim is append-only or immutable we can't do antyhing with
 *     links pointing to it.
 *  7. If we were asked to remove a directory and victim isn't one - ENOTDIR.
 *  8. If we were asked to remove a non-directory and victim isn't one - EISDIR.
 *  9. We can't remove a root or mountpoint.
 * 10. We don't allow removal of NFS sillyrenamed files; it's handled by
 *     nfs_async_unlink().
 */
static int may_delete(struct inode *dir,struct dentry *victim,int isdir)
{
	int error;

	if (!victim->d_inode)
		return -ENOENT;

	BUG_ON(victim->d_parent->d_inode != dir);
	audit_inode_child(dir, victim, AUDIT_TYPE_CHILD_DELETE);

	error = inode_permission(dir, MAY_WRITE | MAY_EXEC);
	if (error)
		return error;
	if (IS_APPEND(dir))
		return -EPERM;
	if (check_sticky(dir, victim->d_inode)||IS_APPEND(victim->d_inode)||
	    IS_IMMUTABLE(victim->d_inode) || IS_SWAPFILE(victim->d_inode))
		return -EPERM;
	if (isdir) {
		if (!S_ISDIR(victim->d_inode->i_mode))
			return -ENOTDIR;
		if (IS_ROOT(victim))
			return -EBUSY;
	} else if (S_ISDIR(victim->d_inode->i_mode))
		return -EISDIR;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	if (victim->d_flags & DCACHE_NFSFS_RENAMED)
		return -EBUSY;
	return 0;
}

/*	Check whether we can create an object with dentry child in directory
 *  dir.
 *  1. We can't do it if child already exists (open has special treatment for
 *     this case, but since we are inlined it's OK)
 *  2. We can't do it if dir is read-only (done in permission())
 *  3. We should have write and exec permissions on dir
 *  4. We can't do it if dir is immutable (done in permission())
 */
static inline int may_create(struct inode *dir, struct dentry *child)
{
	audit_inode_child(dir, child, AUDIT_TYPE_CHILD_CREATE);
	if (child->d_inode)
		return -EEXIST;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	return inode_permission(dir, MAY_WRITE | MAY_EXEC);
}

/* 
 * O_DIRECTORY translates into forcing a directory lookup.
 */
static inline int lookup_flags(unsigned int f)
{
	unsigned long retval = LOOKUP_FOLLOW;

	if (f & O_NOFOLLOW)
		retval &= ~LOOKUP_FOLLOW;
	
	if (f & O_DIRECTORY)
		retval |= LOOKUP_DIRECTORY;

	return retval;
}

/*
 * p1 and p2 should be directories on the same fs.
 */
struct dentry *lock_rename(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	if (p1 == p2) {
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_PARENT);
		return NULL;
	}

	mutex_lock(&p1->d_inode->i_sb->s_vfs_rename_mutex);

	p = d_ancestor(p2, p1);
	if (p) {
		mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_PARENT);
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_CHILD);
		return p;
	}

	p = d_ancestor(p1, p2);
	if (p) {
		mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_PARENT);
		mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_CHILD);
		return p;
	}

	mutex_lock_nested(&p1->d_inode->i_mutex, I_MUTEX_PARENT);
	mutex_lock_nested(&p2->d_inode->i_mutex, I_MUTEX_CHILD);
	return NULL;
}

void unlock_rename(struct dentry *p1, struct dentry *p2)
{
	mutex_unlock(&p1->d_inode->i_mutex);
	if (p1 != p2) {
		mutex_unlock(&p2->d_inode->i_mutex);
		mutex_unlock(&p1->d_inode->i_sb->s_vfs_rename_mutex);
	}
}

int vfs_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
{
	int error = may_create(dir, dentry);

	if (error)
		return error;

	if (!dir->i_op->create)
		return -EACCES;	/* shouldn't it be ENOSYS? */
	mode &= S_IALLUGO;
	mode |= S_IFREG;
	error = security_inode_create(dir, dentry, mode);
	if (error)
		return error;
	vfs_dq_init(dir);
	error = dir->i_op->create(dir, dentry, mode, nd);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}

int may_open(struct path *path, int acc_mode, int flag)
{
	struct dentry *dentry = path->dentry;
	struct inode *inode = dentry->d_inode;
	int error;

	if (!inode)
		return -ENOENT;

	switch (inode->i_mode & S_IFMT) {
	case S_IFLNK:
		return -ELOOP;
	case S_IFDIR:
		if (acc_mode & MAY_WRITE)
			return -EISDIR;
		break;
	case S_IFBLK:
	case S_IFCHR:
		if (path->mnt->mnt_flags & MNT_NODEV)
			return -EACCES;
		/*FALLTHRU*/
	case S_IFIFO:
	case S_IFSOCK:
		flag &= ~O_TRUNC;
		break;
	}

	error = inode_permission(inode, acc_mode);
	if (error)
		return error;

	/*
	 * An append-only file must be opened in append mode for writing.
	 */
	if (IS_APPEND(inode)) {
		if  ((flag & O_ACCMODE) != O_RDONLY && !(flag & O_APPEND))
			return -EPERM;
		if (flag & O_TRUNC)
			return -EPERM;
	}

	/* O_NOATIME can only be set by the owner or superuser */
	if (flag & O_NOATIME && !is_owner_or_cap(inode))
		return -EPERM;

	/*
	 * Ensure there are no outstanding leases on the file.
	 */
	return break_lease(inode, flag);
}

static int handle_truncate(struct file *filp)
{
	struct path *path = &filp->f_path;
	struct inode *inode = path->dentry->d_inode;
	int error = get_write_access(inode);
	if (error)
		return error;
	/*
	 * Refuse to truncate files with mandatory locks held on them.
	 */
	error = locks_verify_locked(inode);
	if (!error)
		error = security_path_truncate(path, 0,
				       ATTR_MTIME|ATTR_CTIME|ATTR_OPEN);
	if (!error) {
		error = do_truncate(path->dentry, 0,
				    ATTR_MTIME|ATTR_CTIME|ATTR_OPEN,
				    filp);
	}
	put_write_access(inode);
	return error;
}

/*
 * Be careful about ever adding any more callers of this
 * function.  Its flags must be in the namei format, not
 * what get passed to sys_open().
 */
static int __open_namei_create(struct nameidata *nd, struct path *path,
				int open_flag, int mode)
{
	int error;
	struct dentry *dir = nd->path.dentry;

	if (!IS_POSIXACL(dir->d_inode))
		mode &= ~current_umask();
	error = security_path_mknod(&nd->path, path->dentry, mode, 0);
	if (error)
		goto out_unlock;
	error = vfs_create(dir->d_inode, path->dentry, mode, nd);
out_unlock:
	mutex_unlock(&dir->d_inode->i_mutex);
	dput(nd->path.dentry);
	nd->path.dentry = path->dentry;
	if (error)
		return error;
	/* Don't check for write permission, don't truncate */
	return may_open(&nd->path, 0, open_flag & ~O_TRUNC);
}

/*
 * Note that while the flag value (low two bits) for sys_open means:
 *	00 - read-only
 *	01 - write-only
 *	10 - read-write
 *	11 - special
 * it is changed into
 *	00 - no permissions needed
 *	01 - read-permission
 *	10 - write-permission
 *	11 - read-write
 * for the internal routines (ie open_namei()/follow_link() etc)
 * This is more logical, and also allows the 00 "no perm needed"
 * to be used for symlinks (where the permissions are checked
 * later).
 *
*/
static inline int open_to_namei_flags(int flag)
{
	if ((flag+1) & O_ACCMODE)
		flag++;
	return flag;
}

static int open_will_truncate(int flag, struct inode *inode)
{
	/*
	 * We'll never write to the fs underlying
	 * a device file.
	 */
	if (special_file(inode->i_mode))
		return 0;
	return (flag & O_TRUNC);
}

/*
 * Note that the low bits of the passed in "open_flag"
 * are not the same as in the local variable "flag". See
 * open_to_namei_flags() for more details.
 */
struct file *do_filp_open(int dfd, struct filename *filename,
		int open_flag, int mode, int acc_mode)
{
	struct file *filp;
	struct nameidata nd;
	int error;
	struct path path;
	struct dentry *dir;
	int count = 0;
	int will_truncate;
	int flag = open_to_namei_flags(open_flag);
	int got_write = false;
	const char *pathname = filename->name;

	if (!acc_mode)
		acc_mode = MAY_OPEN | ACC_MODE(flag);

	/* O_TRUNC implies we need access checks for write permissions */
	if (flag & O_TRUNC)
		acc_mode |= MAY_WRITE;

	/* Allow the LSM permission hook to distinguish append 
	   access from general write access. */
	if (flag & O_APPEND)
		acc_mode |= MAY_APPEND;

	/*
	 * The simplest case - just a plain lookup.
	 */
	if (!(flag & O_CREAT)) {
		filp = get_empty_filp();

		if (filp == NULL)
			return ERR_PTR(-ENFILE);
		nd.intent.open.file = filp;
		filp->f_flags = open_flag;
		nd.intent.open.flags = flag;
		nd.intent.open.create_mode = 0;
		error = filename_lookup(dfd, filename,
					lookup_flags(flag)|LOOKUP_OPEN, &nd);
		if (IS_ERR(nd.intent.open.file)) {
			if (error == 0) {
				error = PTR_ERR(nd.intent.open.file);
				path_put(&nd.path);
			}
		} else if (error)
			release_open_intent(&nd);
		if (error)
			return ERR_PTR(error);
		goto ok;
	}

	/*
	 * Create - we need to know the parent.
	 */
	error = path_init(dfd, pathname, LOOKUP_PARENT, &nd);
	if (error)
		return ERR_PTR(error);
	error = path_walk(filename, &nd);
	if (error) {
		if (nd.root.mnt)
			path_put(&nd.root);
		return ERR_PTR(error);
	}
	if (unlikely(!audit_dummy_context()))
		audit_inode(filename, nd.path.dentry, LOOKUP_PARENT);

	/*
	 * We have the parent and last component. First of all, check
	 * that we are not asked to creat(2) an obvious directory - that
	 * will not do.
	 */
	error = -EISDIR;
	if (nd.last_type != LAST_NORM || nd.last.name[nd.last.len])
		goto exit_parent;

	error = -ENFILE;
	filp = get_empty_filp();
	if (filp == NULL)
		goto exit_parent;
	nd.intent.open.file = filp;
	filp->f_flags = open_flag;
	nd.intent.open.flags = flag;
	nd.intent.open.create_mode = mode;
	dir = nd.path.dentry;
	nd.flags &= ~LOOKUP_PARENT;
	nd.flags |= LOOKUP_CREATE | LOOKUP_OPEN;
	if (flag & O_EXCL)
		nd.flags |= LOOKUP_EXCL;

	/*
	 * This write is needed to ensure that a
	 * ro->rw transition does not occur between
	 * the time when the file is created and when
	 * a permanent write count is taken through
	 * the 'struct file' in nameidata_to_filp().
	 */
	error = mnt_want_write(nd.path.mnt);
	if (!error)
		got_write = true;

	mutex_lock(&dir->d_inode->i_mutex);
	path.dentry = lookup_hash(&nd);
	path.mnt = nd.path.mnt;

do_last:
	error = PTR_ERR(path.dentry);
	if (IS_ERR(path.dentry)) {
		mutex_unlock(&dir->d_inode->i_mutex);
		if (got_write)
			mnt_drop_write(nd.path.mnt);
		goto exit;
	}

	if (IS_ERR(nd.intent.open.file)) {
		error = PTR_ERR(nd.intent.open.file);
		goto exit_mutex_unlock;
	}

	/* Negative dentry, just create the file */
	if (!path.dentry->d_inode) {
		if (!got_write) {
			error = -EROFS;
			goto exit_mutex_unlock;
		}
		error = __open_namei_create(&nd, &path, open_flag, mode);
		if (error) {
			mnt_drop_write(nd.path.mnt);
			goto exit;
		}
		filp = nameidata_to_filp(&nd);
		mnt_drop_write(nd.path.mnt);
		if (nd.root.mnt)
			path_put(&nd.root);
		if (!IS_ERR(filp)) {
			error = ima_file_check(filp, acc_mode);
			if (error) {
				fput(filp);
				filp = ERR_PTR(error);
			}
		}
		return filp;
	}

	/*
	 * It already exists.
	 */
	mutex_unlock(&dir->d_inode->i_mutex);
	if (got_write) {
		mnt_drop_write(nd.path.mnt);
		got_write = false;
	}
	audit_inode(filename, path.dentry, 0);

	error = -EEXIST;
	if (flag & O_EXCL)
		goto exit_dput;

	error = follow_managed(&path, nd.flags);
	if (error < 0)
		goto exit_dput;
 

	error = -ENOENT;
	if (!path.dentry->d_inode)
		goto exit_dput;
	if (path.dentry->d_inode->i_op->follow_link)
		goto do_link;

	path_to_nameidata(&path, &nd);
	error = -EISDIR;
	if (path.dentry->d_inode && S_ISDIR(path.dentry->d_inode->i_mode))
		goto exit;
ok:
	/*
	 * Consider:
	 * 1. may_open() truncates a file
	 * 2. a rw->ro mount transition occurs
	 * 3. nameidata_to_filp() fails due to
	 *    the ro mount.
	 * That would be inconsistent, and should
	 * be avoided. Taking this mnt write here
	 * ensures that (2) can not occur.
	 */
	will_truncate = open_will_truncate(flag, nd.path.dentry->d_inode);
	if (will_truncate) {
		error = mnt_want_write(nd.path.mnt);
		if (error)
			goto exit;
	}
	error = may_open(&nd.path, acc_mode, open_flag);
	if (error) {
		if (will_truncate)
			mnt_drop_write(nd.path.mnt);
		goto exit;
	}
	filp = nameidata_to_filp(&nd);
	if (!IS_ERR(filp)) {
		error = ima_file_check(filp, acc_mode);
		if (error) {
			fput(filp);
			filp = ERR_PTR(error);
		}
	}
	if (!IS_ERR(filp)) {
		if (acc_mode & MAY_WRITE)
			vfs_dq_init(nd.path.dentry->d_inode);

		if (will_truncate) {
			error = handle_truncate(filp);
			if (error) {
				fput(filp);
				filp = ERR_PTR(error);
			}
		}
	}
	/*
	 * It is now safe to drop the mnt write
	 * because the filp has had a write taken
	 * on its behalf.
	 */
	if (will_truncate)
		mnt_drop_write(nd.path.mnt);
	if (nd.root.mnt)
		path_put(&nd.root);
	return filp;

exit_mutex_unlock:
	mutex_unlock(&dir->d_inode->i_mutex);
	if (got_write)
		mnt_drop_write(nd.path.mnt);
exit_dput:
	path_put_conditional(&path, &nd);
exit:
	if (!IS_ERR(nd.intent.open.file))
		release_open_intent(&nd);
exit_parent:
	if (nd.root.mnt)
		path_put(&nd.root);
	path_put(&nd.path);
	return ERR_PTR(error);

do_link:
	error = -ELOOP;
	if (flag & O_NOFOLLOW)
		goto exit_dput;
	/*
	 * This is subtle. Instead of calling do_follow_link() we do the
	 * thing by hands. The reason is that this way we have zero link_count
	 * and path_walk() (called from ->follow_link) honoring LOOKUP_PARENT.
	 * After that we have the parent and last component, i.e.
	 * we are in the same situation as after the first path_walk().
	 * Well, almost - if the last component is normal we get its copy
	 * stored in nd->last.name and we will have to putname() it when we
	 * are done. Procfs-like symlinks just set LAST_BIND.
	 */
	nd.flags |= LOOKUP_PARENT;
	error = security_inode_follow_link(path.dentry, &nd);
	if (error)
		goto exit_dput;
	error = __do_follow_link(&path, &nd);
	if (error) {
		/* Does someone understand code flow here? Or it is only
		 * me so stupid? Anathema to whoever designed this non-sense
		 * with "intent.open".
		 */
		release_open_intent(&nd);
		if (nd.root.mnt)
			path_put(&nd.root);
		return ERR_PTR(error);
	}
	nd.flags &= ~LOOKUP_PARENT;
	if (nd.last_type == LAST_BIND)
		goto ok;
	error = -EISDIR;
	if (nd.last_type != LAST_NORM)
		goto exit;
	if (nd.last.name[nd.last.len]) {
		__putname(nd.last.name);
		goto exit;
	}
	error = -ELOOP;
	if (count++==32) {
		__putname(nd.last.name);
		goto exit;
	}
	dir = nd.path.dentry;
	error = mnt_want_write(nd.path.mnt);
	if (!error)
		got_write = true;
	mutex_lock(&dir->d_inode->i_mutex);
	path.dentry = lookup_hash(&nd);
	path.mnt = nd.path.mnt;
	__putname(nd.last.name);
	goto do_last;
}

/**
 * filp_open - open file and return file pointer
 *
 * @filename:	path to open
 * @flags:	open flags as per the open(2) second argument
 * @mode:	mode for the new file if O_CREAT is set, else ignored
 *
 * This is the helper to open a file from kernelspace if you really
 * have to.  But in generally you should not do this, so please move
 * along, nothing to see here..
 */
struct file *filp_open(const char *filename, int flags, int mode)
{
	struct filename name = { .name = filename };

	return do_filp_open(AT_FDCWD, &name, flags, mode, 0);
}
EXPORT_SYMBOL(filp_open);

/**
 * lookup_create - lookup a dentry, creating it if it doesn't exist
 * @nd: nameidata info
 * @is_dir: directory flag
 *
 * Simple function to lookup and return a dentry and create it
 * if it doesn't exist.  Is SMP-safe.
 *
 * Returns with nd->path.dentry->d_inode->i_mutex locked.
 */
struct dentry *lookup_create(struct nameidata *nd, int is_dir)
{
	struct dentry *dentry = ERR_PTR(-EEXIST);

	mutex_lock_nested(&nd->path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);
	/*
	 * Yucky last component or no last component at all?
	 * (foo/., foo/.., /////)
	 */
	if (nd->last_type != LAST_NORM)
		goto fail;
	nd->flags &= ~LOOKUP_PARENT;
	nd->flags |= LOOKUP_CREATE | LOOKUP_EXCL;
	nd->intent.open.flags = O_EXCL;

	/*
	 * Do the final lookup.
	 */
	dentry = lookup_hash(nd);
	if (IS_ERR(dentry))
		goto fail;

	if (dentry->d_inode)
		goto eexist;
	/*
	 * Special case - lookup gave negative, but... we had foo/bar/
	 * From the vfs_mknod() POV we just have a negative dentry -
	 * all is fine. Let's be bastards - you had / on the end, you've
	 * been asking for (non-existent) directory. -ENOENT for you.
	 */
	if (unlikely(!is_dir && nd->last.name[nd->last.len])) {
		dput(dentry);
		dentry = ERR_PTR(-ENOENT);
	}
	return dentry;
eexist:
	dput(dentry);
	dentry = ERR_PTR(-EEXIST);
fail:
	return dentry;
}
EXPORT_SYMBOL_GPL(lookup_create);

int vfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	int error = may_create(dir, dentry);

	if (error)
		return error;

	if ((S_ISCHR(mode) || S_ISBLK(mode)) && !capable(CAP_MKNOD))
		return -EPERM;

	if (!dir->i_op->mknod)
		return -EPERM;

	error = devcgroup_inode_mknod(mode, dev);
	if (error)
		return error;

	error = security_inode_mknod(dir, dentry, mode, dev);
	if (error)
		return error;

	vfs_dq_init(dir);
	error = dir->i_op->mknod(dir, dentry, mode, dev);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}

static int may_mknod(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFIFO:
	case S_IFSOCK:
	case 0: /* zero mode translates to S_IFREG */
		return 0;
	case S_IFDIR:
		return -EPERM;
	default:
		return -EINVAL;
	}
}

SYSCALL_DEFINE4(mknodat, int, dfd, const char __user *, filename, int, mode,
		unsigned, dev)
{
	int error, err2;
	struct filename *tmp;
	struct dentry *dentry;
	struct nameidata nd;
	unsigned int lookup_flags = 0;

	if (S_ISDIR(mode))
		return -EPERM;
retry:
	tmp = user_path_parent(dfd, filename, &nd, lookup_flags);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	/* don't fail immediately if it's r/o, at least try to report other errors */
	err2 = mnt_want_write(nd.path.mnt);

	dentry = lookup_create(&nd, 0);
	if (IS_ERR(dentry)) {
		error = PTR_ERR(dentry);
		goto out_unlock;
	}

	if (unlikely(err2)) {
		error = err2;
		goto out_dput;
	}
	if (!IS_POSIXACL(nd.path.dentry->d_inode))
		mode &= ~current_umask();
	error = may_mknod(mode);
	if (error)
		goto out_dput;
	error = security_path_mknod(&nd.path, dentry, mode, dev);
	if (error)
		goto out_dput;
	switch (mode & S_IFMT) {
		case 0: case S_IFREG:
			error = vfs_create(nd.path.dentry->d_inode,dentry,mode,&nd);
			break;
		case S_IFCHR: case S_IFBLK:
			error = vfs_mknod(nd.path.dentry->d_inode,dentry,mode,
					new_decode_dev(dev));
			break;
		case S_IFIFO: case S_IFSOCK:
			error = vfs_mknod(nd.path.dentry->d_inode,dentry,mode,0);
			break;
	}
out_dput:
	dput(dentry);
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	if (!err2)
		mnt_drop_write(nd.path.mnt);
	path_put(&nd.path);
	putname(tmp);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE3(mknod, const char __user *, filename, int, mode, unsigned, dev)
{
	return sys_mknodat(AT_FDCWD, filename, mode, dev);
}

int vfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int error = may_create(dir, dentry);

	if (error)
		return error;

	if (!dir->i_op->mkdir)
		return -EPERM;

	mode &= (S_IRWXUGO|S_ISVTX);
	error = security_inode_mkdir(dir, dentry, mode);
	if (error)
		return error;

	vfs_dq_init(dir);
	error = dir->i_op->mkdir(dir, dentry, mode);
	if (!error)
		fsnotify_mkdir(dir, dentry);
	return error;
}

SYSCALL_DEFINE3(mkdirat, int, dfd, const char __user *, pathname, int, mode)
{
	int error = 0, err2;
	struct filename *tmp;
	struct dentry *dentry;
	struct nameidata nd;
	unsigned int lookup_flags = 0;

retry:
	tmp = user_path_parent(dfd, pathname, &nd, lookup_flags);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	/* don't fail immediately if it's r/o, at least try to report other errors */
	err2 = mnt_want_write(nd.path.mnt);

	dentry = lookup_create(&nd, 1);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out_unlock;

	if (unlikely(err2)) {
		error = err2;
		goto out_dput;
	}
	if (!IS_POSIXACL(nd.path.dentry->d_inode))
		mode &= ~current_umask();
	error = security_path_mkdir(&nd.path, dentry, mode);
	if (error)
		goto out_dput;
	error = vfs_mkdir(nd.path.dentry->d_inode, dentry, mode);
out_dput:
	dput(dentry);
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	if (!err2)
		mnt_drop_write(nd.path.mnt);
	path_put(&nd.path);
	putname(tmp);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE2(mkdir, const char __user *, pathname, int, mode)
{
	return sys_mkdirat(AT_FDCWD, pathname, mode);
}

/*
 * We try to drop the dentry early: we should have
 * a usage count of 2 if we're the only user of this
 * dentry, and if that is true (possibly after pruning
 * the dcache), then we drop the dentry now.
 *
 * A low-level filesystem can, if it choses, legally
 * do a
 *
 *	if (!d_unhashed(dentry))
 *		return -EBUSY;
 *
 * if it cannot handle the case of removing a directory
 * that is still in use by something else..
 */
void dentry_unhash(struct dentry *dentry)
{
	dget(dentry);
	shrink_dcache_parent(dentry);
	spin_lock(&dcache_lock);
	spin_lock(&dentry->d_lock);
	if (atomic_read(&dentry->d_count) == 2)
		__d_drop(dentry);
	spin_unlock(&dentry->d_lock);
	spin_unlock(&dcache_lock);
}

int vfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int error = may_delete(dir, dentry, 1);

	if (error)
		return error;

	if (!dir->i_op->rmdir)
		return -EPERM;

	vfs_dq_init(dir);

	mutex_lock(&dentry->d_inode->i_mutex);
	dentry_unhash(dentry);
	if (d_mountpoint(dentry))
		error = -EBUSY;
	else {
		error = security_inode_rmdir(dir, dentry);
		if (!error) {
			error = dir->i_op->rmdir(dir, dentry);
			if (!error)
				dentry->d_inode->i_flags |= S_DEAD;
		}
	}
	mutex_unlock(&dentry->d_inode->i_mutex);
	if (!error) {
		d_delete(dentry);
	}
	dput(dentry);

	return error;
}

static long do_rmdir(int dfd, const char __user *pathname)
{
	int error = 0;
	struct filename *name;
	struct dentry *dentry;
	struct nameidata nd;
	unsigned int lookup_flags = 0;
retry:
	name = user_path_parent(dfd, pathname, &nd, lookup_flags);
	if (IS_ERR(name))
		return PTR_ERR(name);

	switch(nd.last_type) {
	case LAST_DOTDOT:
		error = -ENOTEMPTY;
		goto exit1;
	case LAST_DOT:
		error = -EINVAL;
		goto exit1;
	case LAST_ROOT:
		error = -EBUSY;
		goto exit1;
	}

	nd.flags &= ~LOOKUP_PARENT;
	error = mnt_want_write(nd.path.mnt);
	if (error)
		goto exit1;

	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);
	dentry = lookup_hash(&nd);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto exit2;
	error = security_path_rmdir(&nd.path, dentry);
	if (error)
		goto exit3;
	error = vfs_rmdir(nd.path.dentry->d_inode, dentry);
exit3:
	dput(dentry);
exit2:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	mnt_drop_write(nd.path.mnt);
exit1:
	path_put(&nd.path);
	putname(name);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE1(rmdir, const char __user *, pathname)
{
	return do_rmdir(AT_FDCWD, pathname);
}

int vfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int error = may_delete(dir, dentry, 0);

	if (error)
		return error;

	if (!dir->i_op->unlink)
		return -EPERM;

	vfs_dq_init(dir);

	mutex_lock(&dentry->d_inode->i_mutex);
	if (d_mountpoint(dentry))
		error = -EBUSY;
	else {
		error = security_inode_unlink(dir, dentry);
		if (!error)
			error = dir->i_op->unlink(dir, dentry);
	}
	mutex_unlock(&dentry->d_inode->i_mutex);

	/* We don't d_delete() NFS sillyrenamed files--they still exist. */
	if (!error && !(dentry->d_flags & DCACHE_NFSFS_RENAMED)) {
		fsnotify_link_count(dentry->d_inode);
		d_delete(dentry);
	}

	return error;
}

/*
 * Make sure that the actual truncation of the file will occur outside its
 * directory's i_mutex.  Truncate can take a long time if there is a lot of
 * writeout happening, and we don't want to prevent access to the directory
 * while waiting on the I/O.
 */
static long do_unlinkat(int dfd, const char __user *pathname)
{
	int error;
	struct filename *name;
	struct dentry *dentry;
	struct nameidata nd;
	struct inode *inode = NULL;
	unsigned int lookup_flags = 0;
retry:
	name = user_path_parent(dfd, pathname, &nd, lookup_flags);
	if (IS_ERR(name))
		return PTR_ERR(name);

	error = -EISDIR;
	if (nd.last_type != LAST_NORM)
		goto exit1;

	nd.flags &= ~LOOKUP_PARENT;
	error = mnt_want_write(nd.path.mnt);
	if (error)
		goto exit1;

	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);
	dentry = lookup_hash(&nd);
	error = PTR_ERR(dentry);
	if (!IS_ERR(dentry)) {
		/* Why not before? Because we want correct error value */
		if (nd.last.name[nd.last.len])
			goto slashes;
		inode = dentry->d_inode;
		if (inode)
			atomic_inc(&inode->i_count);
		error = security_path_unlink(&nd.path, dentry);
		if (error)
			goto exit2;
		error = vfs_unlink(nd.path.dentry->d_inode, dentry);
exit2:
		dput(dentry);
	}
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	if (inode)
		iput(inode);	/* truncate the inode here */
	mnt_drop_write(nd.path.mnt);
exit1:
	path_put(&nd.path);
	putname(name);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		inode = NULL;
		goto retry;
	}
	return error;

slashes:
	error = !dentry->d_inode ? -ENOENT :
		S_ISDIR(dentry->d_inode->i_mode) ? -EISDIR : -ENOTDIR;
	goto exit2;
}

SYSCALL_DEFINE3(unlinkat, int, dfd, const char __user *, pathname, int, flag)
{
	if ((flag & ~AT_REMOVEDIR) != 0)
		return -EINVAL;

	if (flag & AT_REMOVEDIR)
		return do_rmdir(dfd, pathname);

	return do_unlinkat(dfd, pathname);
}

SYSCALL_DEFINE1(unlink, const char __user *, pathname)
{
	return do_unlinkat(AT_FDCWD, pathname);
}

int vfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname)
{
	int error = may_create(dir, dentry);

	if (error)
		return error;

	if (!dir->i_op->symlink)
		return -EPERM;

	error = security_inode_symlink(dir, dentry, oldname);
	if (error)
		return error;

	vfs_dq_init(dir);
	error = dir->i_op->symlink(dir, dentry, oldname);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}

SYSCALL_DEFINE3(symlinkat, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	int error, err2;
	struct filename *from;
	struct filename *to;
	struct dentry *dentry;
	struct nameidata nd;
	unsigned int lookup_flags = 0;

	from = getname(oldname);
	if (IS_ERR(from))
		return PTR_ERR(from);

retry:
	to = user_path_parent(newdfd, newname, &nd, lookup_flags);
	if (IS_ERR(to)) {
		error = PTR_ERR(to);
		goto out_putname;
	}

	/* don't fail immediately if it's r/o, at least try to report other errors */
	err2 = mnt_want_write(nd.path.mnt);

	dentry = lookup_create(&nd, 0);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto out_unlock;
	if (unlikely(err2)) {
		error = err2;
		goto out_dput;
	}
	error = security_path_symlink(&nd.path, dentry, from->name);
	if (error)
		goto out_dput;
	error = vfs_symlink(nd.path.dentry->d_inode, dentry, from->name);
out_dput:
	dput(dentry);
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	if (!err2)
		mnt_drop_write(nd.path.mnt);
	path_put(&nd.path);
	putname(to);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out_putname:
	putname(from);
	return error;
}

SYSCALL_DEFINE2(symlink, const char __user *, oldname, const char __user *, newname)
{
	return sys_symlinkat(oldname, AT_FDCWD, newname);
}

int vfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry)
{
	struct inode *inode = old_dentry->d_inode;
	int error;

	if (!inode)
		return -ENOENT;

	error = may_create(dir, new_dentry);
	if (error)
		return error;

	if (dir->i_sb != inode->i_sb)
		return -EXDEV;

	/*
	 * A link to an append-only or immutable file cannot be created.
	 */
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return -EPERM;
	if (!dir->i_op->link)
		return -EPERM;
	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	error = security_inode_link(old_dentry, dir, new_dentry);
	if (error)
		return error;

	mutex_lock(&inode->i_mutex);
	vfs_dq_init(dir);
	/* Make sure we don't allow creating hardlink to an unlinked file */
	if (inode->i_nlink == 0)
		error =  -ENOENT;
	else
		error = dir->i_op->link(old_dentry, dir, new_dentry);
	mutex_unlock(&inode->i_mutex);
	if (!error)
		fsnotify_link(dir, inode, new_dentry);
	return error;
}

/*
 * Hardlinks are often used in delicate situations.  We avoid
 * security-related surprises by not following symlinks on the
 * newname.  --KAB
 *
 * We don't follow them on the oldname either to be compatible
 * with linux 2.0, and to avoid hard-linking to directories
 * and other special files.  --ADM
 */
SYSCALL_DEFINE5(linkat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname, int, flags)
{
	struct dentry *new_dentry;
	struct nameidata nd;
	struct path old_path;
	int error;
	unsigned int how = 0;
	struct filename *to;

	if ((flags & ~AT_SYMLINK_FOLLOW) != 0)
		return -EINVAL;

	if (flags & AT_SYMLINK_FOLLOW)
		how |= LOOKUP_FOLLOW;

retry:
	error = user_path_at(olddfd, oldname, how, &old_path);
	if (error)
		return error;

	to = user_path_parent(newdfd, newname, &nd,
					(how & LOOKUP_REVAL));
	if (IS_ERR(to)) {
		error = PTR_ERR(to);
		goto out;
	}

	error = -EXDEV;
	if (old_path.mnt != nd.path.mnt)
		goto out_release;
	error = mnt_want_write(nd.path.mnt);
	if (error)
		goto out_release;
	new_dentry = lookup_create(&nd, 0);
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto out_unlock;
	error = security_path_link(old_path.dentry, &nd.path, new_dentry);
	if (error)
		goto out_dput;
	error = vfs_link(old_path.dentry, nd.path.dentry->d_inode, new_dentry);
out_dput:
	dput(new_dentry);
out_unlock:
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	mnt_drop_write(nd.path.mnt);
out_release:
	path_put(&nd.path);
	putname(to);
	if (retry_estale(error, how)) {
		path_put(&old_path);
		how |= LOOKUP_REVAL;
		goto retry;
	}
out:
	path_put(&old_path);

	return error;
}

SYSCALL_DEFINE2(link, const char __user *, oldname, const char __user *, newname)
{
	return sys_linkat(AT_FDCWD, oldname, AT_FDCWD, newname, 0);
}

/*
 * The worst of all namespace operations - renaming directory. "Perverted"
 * doesn't even start to describe it. Somebody in UCB had a heck of a trip...
 * Problems:
 *	a) we can get into loop creation. Check is done in is_subdir().
 *	b) race potential - two innocent renames can create a loop together.
 *	   That's where 4.4 screws up. Current fix: serialization on
 *	   sb->s_vfs_rename_mutex. We might be more accurate, but that's another
 *	   story.
 *	c) we have to lock _three_ objects - parents and victim (if it exists).
 *	   And that - after we got ->i_mutex on parents (until then we don't know
 *	   whether the target exists).  Solution: try to be smart with locking
 *	   order for inodes.  We rely on the fact that tree topology may change
 *	   only under ->s_vfs_rename_mutex _and_ that parent of the object we
 *	   move will be locked.  Thus we can rank directories by the tree
 *	   (ancestors first) and rank all non-directories after them.
 *	   That works since everybody except rename does "lock parent, lookup,
 *	   lock child" and rename is under ->s_vfs_rename_mutex.
 *	   HOWEVER, it relies on the assumption that any object with ->lookup()
 *	   has no more than 1 dentry.  If "hybrid" objects will ever appear,
 *	   we'd better make sure that there's no link(2) for them.
 *	d) some filesystems don't support opened-but-unlinked directories,
 *	   either because of layout or because they are not ready to deal with
 *	   all cases correctly. The latter will be fixed (taking this sort of
 *	   stuff into VFS), but the former is not going away. Solution: the same
 *	   trick as in rmdir().
 *	e) conversion from fhandle to dentry may come in the wrong moment - when
 *	   we are removing the target. Solution: we will have to grab ->i_mutex
 *	   in the fhandle_to_dentry code. [FIXME - current nfsfh.c relies on
 *	   ->i_mutex on parents, which works but leads to some truely excessive
 *	   locking].
 */
static int vfs_rename_dir(struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry)
{
	int error = 0;
	struct inode *target;

	/*
	 * If we are going to change the parent - check write permissions,
	 * we'll need to flip '..'.
	 */
	if (new_dir != old_dir) {
		error = inode_permission(old_dentry->d_inode, MAY_WRITE);
		if (error)
			return error;
	}

	error = security_inode_rename(old_dir, old_dentry, new_dir, new_dentry);
	if (error)
		return error;

	target = new_dentry->d_inode;
	if (target) {
		mutex_lock(&target->i_mutex);
		dentry_unhash(new_dentry);
	}
	if (d_mountpoint(old_dentry)||d_mountpoint(new_dentry))
		error = -EBUSY;
	else 
		error = old_dir->i_op->rename(old_dir, old_dentry, new_dir, new_dentry);
	if (target) {
		if (!error)
			target->i_flags |= S_DEAD;
		mutex_unlock(&target->i_mutex);
		if (d_unhashed(new_dentry))
			d_rehash(new_dentry);
		dput(new_dentry);
	}
	if (!error)
		if (!(old_dir->i_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE))
			d_move(old_dentry,new_dentry);
	return error;
}

static int vfs_rename_other(struct inode *old_dir, struct dentry *old_dentry,
			    struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *target;
	int error;

	error = security_inode_rename(old_dir, old_dentry, new_dir, new_dentry);
	if (error)
		return error;

	dget(new_dentry);
	target = new_dentry->d_inode;
	if (target)
		mutex_lock(&target->i_mutex);
	if (d_mountpoint(old_dentry)||d_mountpoint(new_dentry))
		error = -EBUSY;
	else
		error = old_dir->i_op->rename(old_dir, old_dentry, new_dir, new_dentry);
	if (!error) {
		if (!(old_dir->i_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE))
			d_move(old_dentry, new_dentry);
	}
	if (target)
		mutex_unlock(&target->i_mutex);
	dput(new_dentry);
	return error;
}

int vfs_rename(struct inode *old_dir, struct dentry *old_dentry,
	       struct inode *new_dir, struct dentry *new_dentry)
{
	int error;
	int is_dir = S_ISDIR(old_dentry->d_inode->i_mode);
	const char *old_name;

	if (old_dentry->d_inode == new_dentry->d_inode)
 		return 0;
 
	error = may_delete(old_dir, old_dentry, is_dir);
	if (error)
		return error;

	if (!new_dentry->d_inode)
		error = may_create(new_dir, new_dentry);
	else
		error = may_delete(new_dir, new_dentry, is_dir);
	if (error)
		return error;

	if (!old_dir->i_op->rename)
		return -EPERM;

	vfs_dq_init(old_dir);
	vfs_dq_init(new_dir);

	old_name = fsnotify_oldname_init(old_dentry->d_name.name);

	if (is_dir)
		error = vfs_rename_dir(old_dir,old_dentry,new_dir,new_dentry);
	else
		error = vfs_rename_other(old_dir,old_dentry,new_dir,new_dentry);
	if (!error) {
		const char *new_name = old_dentry->d_name.name;
		fsnotify_move(old_dir, new_dir, old_name, new_name, is_dir,
			      new_dentry->d_inode, old_dentry);
	}
	fsnotify_oldname_free(old_name);

	return error;
}

SYSCALL_DEFINE4(renameat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	struct dentry *old_dir, *new_dir;
	struct dentry *old_dentry, *new_dentry;
	struct dentry *trap;
	struct nameidata oldnd, newnd;
	struct filename *from;
	struct filename *to;
	unsigned int lookup_flags = 0;
	bool should_retry = false;
	int error;
retry:
	from = user_path_parent(olddfd, oldname, &oldnd, lookup_flags);
	if (IS_ERR(from)) {
		error = PTR_ERR(from);
		goto exit;
	}

	to = user_path_parent(newdfd, newname, &newnd, lookup_flags);
	if (IS_ERR(to)) {
		error = PTR_ERR(to);
		goto exit1;
	}

	error = -EXDEV;
	if (oldnd.path.mnt != newnd.path.mnt)
		goto exit2;

	old_dir = oldnd.path.dentry;
	error = -EBUSY;
	if (oldnd.last_type != LAST_NORM)
		goto exit2;

	new_dir = newnd.path.dentry;
	if (newnd.last_type != LAST_NORM)
		goto exit2;

	error = mnt_want_write(oldnd.path.mnt);
	if (error)
		goto exit2;

	oldnd.flags &= ~LOOKUP_PARENT;
	newnd.flags &= ~LOOKUP_PARENT;
	newnd.flags |= LOOKUP_RENAME_TARGET;

	trap = lock_rename(new_dir, old_dir);

	old_dentry = lookup_hash(&oldnd);
	error = PTR_ERR(old_dentry);
	if (IS_ERR(old_dentry))
		goto exit3;
	/* source must exist */
	error = -ENOENT;
	if (!old_dentry->d_inode)
		goto exit4;
	/* unless the source is a directory trailing slashes give -ENOTDIR */
	if (!S_ISDIR(old_dentry->d_inode->i_mode)) {
		error = -ENOTDIR;
		if (oldnd.last.name[oldnd.last.len])
			goto exit4;
		if (newnd.last.name[newnd.last.len])
			goto exit4;
	}
	/* source should not be ancestor of target */
	error = -EINVAL;
	if (old_dentry == trap)
		goto exit4;
	new_dentry = lookup_hash(&newnd);
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto exit4;
	/* target should not be an ancestor of source */
	error = -ENOTEMPTY;
	if (new_dentry == trap)
		goto exit5;

	error = security_path_rename(&oldnd.path, old_dentry,
				     &newnd.path, new_dentry);
	if (error)
		goto exit5;
	error = vfs_rename(old_dir->d_inode, old_dentry,
				   new_dir->d_inode, new_dentry);
exit5:
	dput(new_dentry);
exit4:
	dput(old_dentry);
exit3:
	unlock_rename(new_dir, old_dir);
	mnt_drop_write(oldnd.path.mnt);
exit2:
	if (retry_estale(error, lookup_flags))
		should_retry = true;
	path_put(&newnd.path);
	putname(to);
exit1:
	path_put(&oldnd.path);
	putname(from);
	if (should_retry) {
		should_retry = false;
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
exit:
	return error;
}

SYSCALL_DEFINE2(rename, const char __user *, oldname, const char __user *, newname)
{
	return sys_renameat(AT_FDCWD, oldname, AT_FDCWD, newname);
}

int vfs_readlink(struct dentry *dentry, char __user *buffer, int buflen, const char *link)
{
	int len;

	len = PTR_ERR(link);
	if (IS_ERR(link))
		goto out;

	len = strlen(link);
	if (len > (unsigned) buflen)
		len = buflen;
	if (copy_to_user(buffer, link, len))
		len = -EFAULT;
out:
	return len;
}

/*
 * A helper for ->readlink().  This should be used *ONLY* for symlinks that
 * have ->follow_link() touching nd only in nd_set_link().  Using (or not
 * using) it for any given inode is up to filesystem.
 */
int generic_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct nameidata nd;
	void *cookie;
	int res;

	nd.depth = 0;
	cookie = dentry->d_inode->i_op->follow_link(dentry, &nd);
	if (IS_ERR(cookie))
		return PTR_ERR(cookie);

	res = vfs_readlink(dentry, buffer, buflen, nd_get_link(&nd));
	if (dentry->d_inode->i_op->put_link)
		dentry->d_inode->i_op->put_link(dentry, &nd, cookie);
	return res;
}

int vfs_follow_link(struct nameidata *nd, const char *link)
{
	return __vfs_follow_link(nd, link);
}

/* get the link contents into pagecache */
static char *page_getlink(struct dentry * dentry, struct page **ppage)
{
	char *kaddr;
	struct page *page;
	struct address_space *mapping = dentry->d_inode->i_mapping;
	page = read_mapping_page(mapping, 0, NULL);
	if (IS_ERR(page))
		return (char*)page;
	*ppage = page;
	kaddr = kmap(page);
	nd_terminate_link(kaddr, dentry->d_inode->i_size, PAGE_SIZE - 1);
	return kaddr;
}

int page_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct page *page = NULL;
	char *s = page_getlink(dentry, &page);
	int res = vfs_readlink(dentry,buffer,buflen,s);
	if (page) {
		kunmap(page);
		page_cache_release(page);
	}
	return res;
}

void *page_follow_link_light(struct dentry *dentry, struct nameidata *nd)
{
	struct page *page = NULL;
	nd_set_link(nd, page_getlink(dentry, &page));
	return page;
}

void page_put_link(struct dentry *dentry, struct nameidata *nd, void *cookie)
{
	struct page *page = cookie;

	if (page) {
		kunmap(page);
		page_cache_release(page);
	}
}

/*
 * The nofs argument instructs pagecache_write_begin to pass AOP_FLAG_NOFS
 */
int __page_symlink(struct inode *inode, const char *symname, int len, int nofs)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page;
	void *fsdata;
	int err;
	char *kaddr;
	unsigned int flags = AOP_FLAG_UNINTERRUPTIBLE;
	if (nofs)
		flags |= AOP_FLAG_NOFS;

retry:
	err = pagecache_write_begin(NULL, mapping, 0, len-1,
				flags, &page, &fsdata);
	if (err)
		goto fail;

	kaddr = kmap_atomic(page, KM_USER0);
	memcpy(kaddr, symname, len-1);
	kunmap_atomic(kaddr, KM_USER0);

	err = pagecache_write_end(NULL, mapping, 0, len-1, len-1,
							page, fsdata);
	if (err < 0)
		goto fail;
	if (err < len-1)
		goto retry;

	mark_inode_dirty(inode);
	return 0;
fail:
	return err;
}

int page_symlink(struct inode *inode, const char *symname, int len)
{
	return __page_symlink(inode, symname, len,
			!(mapping_gfp_mask(inode->i_mapping) & __GFP_FS));
}

const struct inode_operations page_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= page_follow_link_light,
	.put_link	= page_put_link,
};

EXPORT_SYMBOL(user_path_at);
EXPORT_SYMBOL(follow_down);
EXPORT_SYMBOL(__follow_down);
EXPORT_SYMBOL(follow_up);
EXPORT_SYMBOL(get_write_access); /* binfmt_aout */
EXPORT_SYMBOL(lock_rename);
EXPORT_SYMBOL(lookup_one_len);
EXPORT_SYMBOL(page_follow_link_light);
EXPORT_SYMBOL(page_put_link);
EXPORT_SYMBOL(page_readlink);
EXPORT_SYMBOL(__page_symlink);
EXPORT_SYMBOL(page_symlink);
EXPORT_SYMBOL(page_symlink_inode_operations);
EXPORT_SYMBOL(path_lookup);
EXPORT_SYMBOL(kern_path);
EXPORT_SYMBOL(vfs_path_lookup);
EXPORT_SYMBOL(inode_permission);
EXPORT_SYMBOL(file_permission);
EXPORT_SYMBOL(unlock_rename);
EXPORT_SYMBOL(vfs_create);
EXPORT_SYMBOL(vfs_follow_link);
EXPORT_SYMBOL(vfs_link);
EXPORT_SYMBOL(vfs_mkdir);
EXPORT_SYMBOL(vfs_mknod);
EXPORT_SYMBOL(generic_permission);
EXPORT_SYMBOL(vfs_readlink);
EXPORT_SYMBOL(vfs_rename);
EXPORT_SYMBOL(vfs_rmdir);
EXPORT_SYMBOL(vfs_symlink);
EXPORT_SYMBOL(vfs_unlink);
EXPORT_SYMBOL(dentry_unhash);
EXPORT_SYMBOL(generic_readlink);
