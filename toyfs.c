/************************************************************************************
 *																					                                        *
 * LINUX TOY FILE SYSTEM												                              			*
 * 																				                                        	*
 * San Jose State University 														                            *
 * CMPE 142 : Operating Systems														                          *
 *																					                                        *
 * Authors : Anson Jiang															                              *
 *   		 Banks Lin																                                  *
 *			 Jonathon Hongpananon												                              	*
 *			 Phillip Tran															                                  *
 *			 Shangming Wang														                                	*
 *																				                                        	*
 * Description : This is a simple, flat file system that can create files and keep	*
 *				 its attributes in a root directory. This kernel module interfaces	      *
 *				 with the VFS in Linux and redirects all calls to this toy file 	        *
 *			     file system implemented in user space. The system can be mounted     	*
 *				 like any other file system in Linux. You are able to read/write to	      *
 *				 your files and the files reflect such operations. Basic metadata	        *
 *				 is maintained. This VFS was created with Linux kernel version 3.13	      *
 *				 																                                         	*
 * References : /linux/fs.h													                            		*
 *				/linux/fs/inode.c												                                	*
 *				/ramfs/inode.c													                                	*
 *				/linux/drivers/oprofile/oprofilefs.c					                       			*
 *				/linux/drivers/usb/core/inode.c									                        	*
 *				lxr.free-electrons.com											                            	*
 ************************************************************************************/

#include <linux/module.h>    // included for all kernel modules
#include <linux/kernel.h>    // included for KERN_INFO
#include <linux/init.h>      // included for __init and __exit macros
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>

#define PROCFS_MAX_SIZE 	1024             // max buffer length
static unsigned long procfs_buffer_size = 0; // default procfs buffer size
static char procfs_buffer[PROCFS_MAX_SIZE];  // procfs buffer

// LMK info
MODULE_LICENSE("GPL");
MODULE_AUTHOR("CMPE142 ");
MODULE_DESCRIPTION("A Toy File System module");

#define TOYFS_MAGIC 0x858458f6 //random number 

// Prototypes
static const struct file_operations toyfs_file_operations;
static const struct inode_operations toyfs_inode_operations;
static const struct super_operations toyfs_ops;
extern const struct file_operations simple_dir_operations;

// ~~~~~~~~~~~~~~~~~ READ/WRITE/OPEN FILE OPERATIONS ~~~~~~~~~~~~~~~~~~
// read file
static ssize_t toyfs_read_file (struct file *file, char *buffer, 
                                size_t length, loff_t *offset)
{
	static int finished = 0;

	/* 
	 * We return 0 to indicate end of file, that we have
	 * no more information. Otherwise, processes will
	 * continue to read from us in an endless loop. 
	 */
	if ( finished ) {
		printk(KERN_INFO "procfs_read: END\n");
		finished = 0;
		return 0;
	}
	
	finished = 1;
		
	/* 
	 * We use put_to_user to copy the string from the kernel's
	 * memory segment to the memory segment of the process
	 * that called us. get_from_user, BTW, is
	 * used for the reverse. 
	 */
	if ( copy_to_user(buffer, procfs_buffer, procfs_buffer_size) ) {
		return -EFAULT;
	}

	printk(KERN_INFO "procfs_read: read %lu bytes\n", procfs_buffer_size);

	return procfs_buffer_size;	/* Return the number of bytes "read" */
}

// write file
static ssize_t toyfs_write_file (struct file *file, const char *buffer, 
                                  size_t len, loff_t *off)
{
	//Create proc-_dir_entry struct 
	struct proc_dir_entry *procFile; //used to keep information about the /proc file
	
	/*
	 * static inline struct proc_dir_entry *proc_create(
	 * const char *name, umode_t mode, struct proc_dir_entry *parent,
	 * const struct file_operations *proc_fops)
	 */
	procFile = proc_create(file->f_path.dentry->d_iname, 0644, NULL,  
	                       &toyfs_file_operations);  //create proc entry

	// buffer length
	if ( len > PROCFS_MAX_SIZE )	{
		procfs_buffer_size = PROCFS_MAX_SIZE;
	}
	else	{
		procfs_buffer_size = len;
	}
	
	// User space to kernel space copy
	if ( copy_from_user(procfs_buffer, buffer, procfs_buffer_size) ) {
		return -EFAULT;		// Bad Address
	}

	printk(KERN_INFO "procfs_write: write %lu bytes\n", procfs_buffer_size);
	
	return procfs_buffer_size;
}

// open file
static int toyfs_open (struct inode *inode, struct file *file)
{
	try_module_get(THIS_MODULE);  //increment process counter
	return 0;
}

//checks read/write position of file
static loff_t toyfs_file_lseek (struct file *file, loff_t offset, int orig)
{
	loff_t retval = -EINVAL;  // invalid argument
	mutex_lock(&file->f_path.dentry->d_inode->i_mutex);
	switch(orig) {
	case 0:
		if (offset > 0) {
			file->f_pos = offset;
			retval = file->f_pos;
		} 
		break;
	case 1:
		if ((offset + file->f_pos) > 0) {
			file->f_pos += offset;
			retval = file->f_pos;
		} 
		break;
	default:
		break;
	}
	mutex_unlock(&file->f_path.dentry->d_inode->i_mutex);
	return retval;
}

// close file
int toyfs_close(struct inode *inode, struct file *file)
{
	module_put(THIS_MODULE);  //decrement the process counter, 
	                          //cannot close unless # of processes using module = 0
	return 0;		/* success */
}

static const struct file_operations toyfs_file_operations = {
	.read =		toyfs_read_file,
	.write =	toyfs_write_file,
	.open =		toyfs_open,
	.release = 	toyfs_close,
	.llseek =	toyfs_file_lseek,
};

// ~~~~~~~~~~~~~~~~~~~~~~~~~ CREATE INODE ~~~~~~~~~~~~~~~~~~~~~~~~~~
static struct inode *toyfs_get_inode (struct super_block *sb, const 
                                      struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);
	if (inode) {
		inode->i_ino  = get_next_ino();  //obtain inode number
		inode->i_mode = mode; 			//permissions
		inode->i_uid  = current_fsuid();	//user id
		inode->i_gid  = current_fsgid();	//group id
		//access time = modify time = current time
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;  
		// Determine if inode is file or directory
		switch (mode & S_IFMT) { 
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:	//regular file
			inode->i_op = &toyfs_inode_operations;	//inode operations table
			inode->i_fop = &toyfs_file_operations;	//default inode operations
			break;
		case S_IFDIR:	//dir file
			inode->i_op = &toyfs_inode_operations;
			inode->i_fop = &simple_dir_operations;
			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);	//increase inode link count
			break;
		}
	}
	return inode; 
}

// create symbolic link "symname" to file represented by dentry under dir
static int toyfs_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
{
	struct inode *inode; // inode to attach to this dentry
	int error = -ENOSPC;	// No space on drive

	inode = toyfs_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode); 				// file in inode information in dentry
			dget(dentry); 								// get reference to a dentry
			dir->i_mtime = dir->i_ctime = CURRENT_TIME; // set access time and modify time
		} else
			iput(inode); // put an inode, drop usage count
	}
	return error;
}

// make inode
static int toyfs_mknod (struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = toyfs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC; // no space on drive

	if (inode) {
		d_instantiate(dentry, inode); 				// file in inode information in dentry
		dget(dentry); 								// get reference to a dentry
		error = 0; 									// no error creating inode
		dir->i_mtime = dir->i_ctime = CURRENT_TIME; // set access time and modify time
	}
	return error;
}


// make directory
static int toyfs_mkdir (struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int res;
	mode = (mode & (S_IRWXUGO | S_ISVTX)) | S_IFDIR;  // set permissions
	res = toyfs_mknod (dir, dentry, mode, 0);		  // make inode
	if (!res)
		inc_nlink(dir);								  // increase inode link count
	return res;
}

// create inode
static int toyfs_create (struct inode *dir, struct dentry *dentry, umode_t mode)
{
	return toyfs_mknod (dir, dentry, mode | S_IFREG, 0);
}

static const struct inode_operations toyfs_inode_operations = {
	.create		= toyfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink 	= simple_unlink,
	.symlink 	= toyfs_symlink,
	.mkdir		= toyfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= toyfs_mknod,
	.rename		= simple_rename,
};

// set to simple super_operations struct
static struct super_operations toyfs_s_ops = {
    .statfs         = simple_statfs,
    .drop_inode     = generic_delete_inode,
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~ SUPERBLOCK ~~~~~~~~~~~~~~~~~~~~~~~~~
static int toyfs_fill_super (struct super_block *sb, 
                               void *data, int silent)
{
	struct inode *inode;

	//Basic parameters
	sb->s_blocksize 	 = PAGE_CACHE_SIZE;		// block size in bytes
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;	// block size in bits
	sb->s_magic 	     = TOYFS_MAGIC;			// filesystem's *magic* number
	sb->s_op 			 = &toyfs_s_ops;		// superblock methods
	sb->s_time_gran      = 1;					// granularity of timestamps
	
	//Make inode to represent root directory
	inode = toyfs_get_inode (sb, NULL, S_IFDIR | 0777, 0);
	inode->i_op = &toyfs_inode_operations;
	inode->i_fop = &simple_dir_operations;
	sb->s_root = d_make_root(inode);       
	if (!sb->s_root)
		return -ENOMEM; // out of memory

	return 0;
}

//get superblock
static struct dentry *toyfs_get_super(struct file_system_type *fst,
		int flags, const char *devname, void *data)
{
	//mount superblock
	return mount_single(fst, flags, data, toyfs_fill_super);
}

//file system struct
static struct file_system_type toyfs_type = {
	.owner 		= THIS_MODULE,
	.name		= "toyfs",           // file system name to be seen in /proc
	.mount 		= toyfs_get_super,	 
	.kill_sb	= kill_litter_super,
	.fs_flags	= FS_USERNS_MOUNT,
};

static int __init toyfs_init(void)
{
    return register_filesystem(&toyfs_type);
}

static void __exit toyfs_cleanup(void)
{
    unregister_filesystem(&toyfs_type);
}

module_init(toyfs_init);
module_exit(toyfs_cleanup);
