/*
 * Read-write ext2 filesystem for XNU (4096-byte blocks, POSIX permissions,
 * symlinks, hardlinks, sockets/FIFOs, 64-bit file sizes, and VFS xattrs).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/ubc.h>
#include <sys/unistd.h>
#include <sys/errno.h>
#include <sys/xattr.h>
#include <sys/malloc.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>

#define EXT2_MAGIC              0xEF53u
#define EXT2_ROOT_INO           2u
#define EXT2_BLK_SIZE           4096u
#define EXT2_BLK_SHIFT          12u
#define EXT2_PTRS_PER_BLK       (EXT2_BLK_SIZE / 4u)
#define EXT2_NDIR_BLOCKS        12u
#define EXT2_IND_BLOCK          12u
#define EXT2_DIND_BLOCK         13u
#define EXT2_TIND_BLOCK         14u
#define EXT2_N_BLOCKS           15u
#define EXT2_HASH_SIZE          256u
#define EXT2_MAX_IO             (128u * 1024u)
#define EXT2_MAX_XATTR          (64u * 1024u)

#define EXT2_FT_UNKNOWN         0
#define EXT2_FT_REG_FILE        1
#define EXT2_FT_DIR             2
#define EXT2_FT_CHRDEV          3
#define EXT2_FT_BLKDEV          4
#define EXT2_FT_FIFO            5
#define EXT2_FT_SOCK            6
#define EXT2_FT_SYMLINK         7

#define EXT2_N_CREATING         0x1
#define EXT2_N_WANTED           0x2

struct ext2_super_block {
	uint32_t s_inodes_count;
	uint32_t s_blocks_count;
	uint32_t s_r_blocks_count;
	uint32_t s_free_blocks_count;
	uint32_t s_free_inodes_count;
	uint32_t s_first_data_block;
	uint32_t s_log_block_size;
	uint32_t s_log_frag_size;
	uint32_t s_blocks_per_group;
	uint32_t s_frags_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;
	uint16_t s_mnt_count;
	uint16_t s_max_mnt_count;
	uint16_t s_magic;
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;
	uint16_t s_def_resgid;
	uint32_t s_first_ino;
	uint16_t s_inode_size;
	uint16_t s_block_group_nr;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	uint8_t  s_uuid[16];
	char     s_volume_name[16];
	char     s_last_mounted[64];
	uint32_t s_algorithm_usage_bitmap;
	uint8_t  s_prealloc_blocks;
	uint8_t  s_prealloc_dir_blocks;
	uint16_t s_padding1;
	uint8_t  s_reserved[816];
} __attribute__((packed));

struct ext2_group_desc {
	uint32_t bg_block_bitmap;
	uint32_t bg_inode_bitmap;
	uint32_t bg_inode_table;
	uint16_t bg_free_blocks_count;
	uint16_t bg_free_inodes_count;
	uint16_t bg_used_dirs_count;
	uint16_t bg_pad;
	uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
	uint16_t i_mode;
	uint16_t i_uid;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_ctime;
	uint32_t i_mtime;
	uint32_t i_dtime;
	uint16_t i_gid;
	uint16_t i_links_count;
	uint32_t i_blocks;
	uint32_t i_flags;
	uint32_t i_osd1;
	uint32_t i_block[EXT2_N_BLOCKS];
	uint32_t i_generation;
	uint32_t i_file_acl;
	uint32_t i_size_high;
	uint32_t i_faddr;
	uint8_t  i_osd2[12];
} __attribute__((packed));

struct ext2_dir_entry {
	uint32_t inode;
	uint16_t rec_len;
	uint8_t  name_len;
	uint8_t  file_type;
	char     name[];
} __attribute__((packed));

struct ext2_xattr {
	TAILQ_ENTRY(ext2_xattr) ex_link;
	char   *ex_name;
	size_t  ex_namelen;
	void   *ex_data;
	size_t  ex_size;
};

struct ext2_node {
	uint32_t            en_ino;
	vnode_t             en_vp;
	uint32_t            en_vid;
	uint32_t            en_flags;
	struct ext2_mount  *en_mount;
	struct ext2_node   *en_hash_next;
	struct ext2_inode   en_di;
	bool                en_dirty;
	uint32_t            en_parent_ino;
	uint32_t            en_last_blk;
	char                en_name[256];
};

struct ext2_xattr_bucket {
	uint32_t                 xb_ino;
	struct ext2_xattr_bucket *xb_next;
	TAILQ_HEAD(, ext2_xattr) xb_list;
};

#define EXT2_BCACHE_MAX   8192
#define EXT2_BCACHE_HASH  8192
#define EXT2_IO_BATCH     32
#define EXT2_RA_BLOCKS    8

struct ext2_bcache_entry {
	uint32_t                     bc_blk;
	uint32_t                     bc_ino;
	bool                         bc_dirty;
	TAILQ_ENTRY(ext2_bcache_entry) bc_lru;
	struct ext2_bcache_entry    *bc_hnext;
	uint8_t                     *bc_data;
};

struct ext2_mount {
	mount_t                 em_mp;
	vnode_t                 em_devvp;
	uint32_t                em_devblock;
	lck_mtx_t               em_lock;
	struct ext2_super_block em_sb;
	struct ext2_group_desc *em_gd;
	uint32_t                em_ngroups;
	uint32_t                em_gd_blocks;
	uint16_t                em_inode_size;
	bool                    em_sb_dirty;
	bool                    em_gd_dirty;
	struct ext2_node       *em_hash[EXT2_HASH_SIZE];
	struct ext2_xattr_bucket *em_xattr_hash[EXT2_HASH_SIZE];
	struct ext2_bcache_entry **em_bcache_hash;
	TAILQ_HEAD(, ext2_bcache_entry) em_bcache_lru;
	uint32_t                em_bcache_count;
	uint32_t                em_bcache_dirty;
	uint8_t                *em_io_buf;
	uint8_t                *em_ra_buf;
	uint16_t               *em_grp_hint;
	uint8_t                *em_bmap_buf;
	bool                    em_sync_stop;
	bool                    em_sync_running;
	bool                    em_sync_done;
};

#define EXT2_MP(mp)   ((struct ext2_mount *)vfs_fsprivate(mp))
#define EXT2_NODE(vp) ((struct ext2_node *)vnode_fsnode(vp))

static LCK_GRP_DECLARE(ext2_lck_grp, "ext2");
extern const struct vnodeopv_desc ext2_vnodeop_opv_desc;
int (**ext2_vnodeop_p)(void *);
static void ext2_xattr_purge_ino(struct ext2_mount *em, uint32_t ino);

static uint64_t
ext2_now_sec(void)
{
	struct timespec ts;
	nanotime(&ts);
	return (uint64_t)ts.tv_sec;
}

static uint64_t
ext2_inode_size_get(const struct ext2_inode *di)
{
	if ((di->i_mode & S_IFMT) == S_IFREG) {
		return ((uint64_t)di->i_size_high << 32) | di->i_size;
	}
	return di->i_size;
}

static void
ext2_inode_size_set(struct ext2_inode *di, uint64_t sz)
{
	di->i_size = (uint32_t)(sz & 0xffffffffULL);
	if ((di->i_mode & S_IFMT) == S_IFREG) {
		di->i_size_high = (uint32_t)(sz >> 32);
	}
}

static enum vtype
ext2_mode_to_vtype(uint16_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:  return VREG;
	case S_IFDIR:  return VDIR;
	case S_IFLNK:  return VLNK;
	case S_IFSOCK: return VSOCK;
	case S_IFIFO:  return VFIFO;
	case S_IFCHR:  return VCHR;
	case S_IFBLK:  return VBLK;
	default:       return VNON;
	}
}

static uint8_t
ext2_mode_to_ft(uint16_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:  return EXT2_FT_REG_FILE;
	case S_IFDIR:  return EXT2_FT_DIR;
	case S_IFLNK:  return EXT2_FT_SYMLINK;
	case S_IFSOCK: return EXT2_FT_SOCK;
	case S_IFIFO:  return EXT2_FT_FIFO;
	case S_IFCHR:  return EXT2_FT_CHRDEV;
	case S_IFBLK:  return EXT2_FT_BLKDEV;
	default:       return EXT2_FT_UNKNOWN;
	}
}

/* ---- Block Device I/O ---- */

static int
ext2_dev_io(struct ext2_mount *em, uint64_t byte_off, void *buf, size_t len, bool write)
{
	char *p = (char *)buf;

	if ((byte_off % em->em_devblock) != 0 || (len % em->em_devblock) != 0) {
		return EINVAL;
	}
	while (len > 0) {
		size_t n = len < EXT2_MAX_IO ? len : EXT2_MAX_IO;
		buf_t bp = buf_alloc(em->em_devvp);
		int error;

		if (bp == NULL) {
			return ENOMEM;
		}
		buf_reset(bp, write ? B_WRITE : B_READ);
		buf_setblkno(bp, (daddr64_t)(byte_off / em->em_devblock));
		buf_setlblkno(bp, (daddr64_t)(byte_off / em->em_devblock));
		buf_setcount(bp, (uint32_t)n);
		buf_setdataptr(bp, (uintptr_t)p);
		if (write) {
			vnode_startwrite(em->em_devvp);
		}
		error = VNOP_STRATEGY(bp);
		if (error == 0) {
			error = (int)buf_biowait(bp);
		}
		if (error == 0 && (buf_resid(bp) != 0 || buf_count(bp) != n)) {
			error = EIO;
		}
		buf_free(bp);
		if (error) {
			return error;
		}
		p += n;
		byte_off += n;
		len -= n;
	}
	return 0;
}

static struct ext2_bcache_entry *
ext2_bcache_find(struct ext2_mount *em, uint32_t blk)
{
	uint32_t h = blk & (EXT2_BCACHE_HASH - 1);
	for (struct ext2_bcache_entry *e = em->em_bcache_hash[h]; e != NULL; e = e->bc_hnext) {
		if (e->bc_blk == blk) {
			return e;
		}
	}
	return NULL;
}

static void
ext2_bcache_unhash(struct ext2_mount *em, struct ext2_bcache_entry *e)
{
	uint32_t h = e->bc_blk & (EXT2_BCACHE_HASH - 1);
	struct ext2_bcache_entry **pp = &em->em_bcache_hash[h];
	while (*pp != NULL) {
		if (*pp == e) {
			*pp = e->bc_hnext;
			e->bc_hnext = NULL;
			return;
		}
		pp = &(*pp)->bc_hnext;
	}
}

static void
ext2_bcache_inval(struct ext2_mount *em, uint32_t blk)
{
	struct ext2_bcache_entry *e = ext2_bcache_find(em, blk);
	if (e == NULL) {
		return;
	}
	if (e->bc_dirty) {
		em->em_bcache_dirty--;
	}
	ext2_bcache_unhash(em, e);
	TAILQ_REMOVE(&em->em_bcache_lru, e, bc_lru);
	em->em_bcache_count--;
	kfree_data(e->bc_data, EXT2_BLK_SIZE);
	kfree_type(struct ext2_bcache_entry, e);
}

/* Flush a contiguous run of dirty blocks around `e` (up to EXT2_IO_BATCH = 128 KB). */
static int
ext2_bcache_flush_run(struct ext2_mount *em, struct ext2_bcache_entry *e)
{
	struct ext2_bcache_entry *run_ents[EXT2_IO_BATCH];
	uint32_t start = e->bc_blk;
	uint32_t back = 0;

	while (start > 1 && back + 1 < EXT2_IO_BATCH) {
		struct ext2_bcache_entry *prev = ext2_bcache_find(em, start - 1);
		if (prev == NULL || !prev->bc_dirty) {
			break;
		}
		start--;
		back++;
	}

	uint32_t n = 0;
	for (uint32_t blk = start; n < EXT2_IO_BATCH; blk++) {
		struct ext2_bcache_entry *cur = (blk == e->bc_blk) ? e : ext2_bcache_find(em, blk);
		if (cur == NULL || !cur->bc_dirty) {
			break;
		}
		run_ents[n] = cur;
		memcpy(em->em_io_buf + (size_t)n * EXT2_BLK_SIZE, cur->bc_data, EXT2_BLK_SIZE);
		n++;
	}

	int error = ext2_dev_io(em, (uint64_t)start * EXT2_BLK_SIZE, em->em_io_buf,
	    (size_t)n * EXT2_BLK_SIZE, true);
	if (error == 0) {
		for (uint32_t i = 0; i < n; i++) {
			if (run_ents[i]->bc_dirty) {
				run_ents[i]->bc_dirty = false;
				em->em_bcache_dirty--;
			}
		}
	}
	return error;
}

static int
ext2_bcache_flush_ino(struct ext2_mount *em, uint32_t ino)
{
	struct ext2_bcache_entry *e;
	int first_err = 0;

	if (em->em_bcache_dirty == 0) {
		return 0;
	}
	TAILQ_FOREACH(e, &em->em_bcache_lru, bc_lru) {
		if (!e->bc_dirty) {
			continue;
		}
		if (ino != 0 && e->bc_ino != ino && e->bc_ino != 0) {
			continue;
		}
		int error = ext2_bcache_flush_run(em, e);
		if (error != 0 && first_err == 0) {
			first_err = error;
		}
	}
	return first_err;
}

static int
ext2_bcache_flush_all(struct ext2_mount *em)
{
	return ext2_bcache_flush_ino(em, 0);
}

static bool
ext2_bcache_put_ino(struct ext2_mount *em, uint32_t blk, const void *buf, bool dirty, uint32_t ino)
{
	struct ext2_bcache_entry *e = ext2_bcache_find(em, blk);
	if (e != NULL) {
		memcpy(e->bc_data, buf, EXT2_BLK_SIZE);
		if (dirty && !e->bc_dirty) {
			e->bc_dirty = true;
			em->em_bcache_dirty++;
		}
		if (ino != 0) {
			e->bc_ino = ino;
		}
		TAILQ_REMOVE(&em->em_bcache_lru, e, bc_lru);
		TAILQ_INSERT_TAIL(&em->em_bcache_lru, e, bc_lru);
		return true;
	}
	if (em->em_bcache_count >= EXT2_BCACHE_MAX) {
		e = TAILQ_FIRST(&em->em_bcache_lru);
		if (e != NULL) {
			if (e->bc_dirty && ext2_bcache_flush_run(em, e) != 0) {
				return false;
			}
			ext2_bcache_unhash(em, e);
			TAILQ_REMOVE(&em->em_bcache_lru, e, bc_lru);
			em->em_bcache_count--;
		}
	} else {
		e = kalloc_type(struct ext2_bcache_entry, Z_NOWAIT | Z_ZERO);
		if (e != NULL) {
			e->bc_data = kalloc_data(EXT2_BLK_SIZE, Z_NOWAIT);
			if (e->bc_data == NULL) {
				kfree_type(struct ext2_bcache_entry, e);
				e = NULL;
			}
		}
	}
	if (e == NULL) {
		return false;
	}
	e->bc_blk = blk;
	e->bc_ino = ino;
	e->bc_dirty = dirty;
	if (dirty) {
		em->em_bcache_dirty++;
	}
	memcpy(e->bc_data, buf, EXT2_BLK_SIZE);
	uint32_t h = blk & (EXT2_BCACHE_HASH - 1);
	e->bc_hnext = em->em_bcache_hash[h];
	em->em_bcache_hash[h] = e;
	TAILQ_INSERT_TAIL(&em->em_bcache_lru, e, bc_lru);
	em->em_bcache_count++;
	return true;
}

static bool
ext2_bcache_put(struct ext2_mount *em, uint32_t blk, const void *buf, bool dirty)
{
	return ext2_bcache_put_ino(em, blk, buf, dirty, 0);
}

static void
ext2_bcache_free_all(struct ext2_mount *em)
{
	struct ext2_bcache_entry *e;
	while ((e = TAILQ_FIRST(&em->em_bcache_lru)) != NULL) {
		TAILQ_REMOVE(&em->em_bcache_lru, e, bc_lru);
		kfree_data(e->bc_data, EXT2_BLK_SIZE);
		kfree_type(struct ext2_bcache_entry, e);
	}
	if (em->em_bcache_hash != NULL) {
		bzero(em->em_bcache_hash, (size_t)EXT2_BCACHE_HASH * sizeof(struct ext2_bcache_entry *));
	}
	em->em_bcache_count = 0;
	em->em_bcache_dirty = 0;
}

static int
ext2_read_block(struct ext2_mount *em, uint32_t blk, void *buf)
{
	if (blk == 0 || blk >= em->em_sb.s_blocks_count) {
		return EIO;
	}
	struct ext2_bcache_entry *e = ext2_bcache_find(em, blk);
	if (e != NULL) {
		memcpy(buf, e->bc_data, EXT2_BLK_SIZE);
		TAILQ_REMOVE(&em->em_bcache_lru, e, bc_lru);
		TAILQ_INSERT_TAIL(&em->em_bcache_lru, e, bc_lru);
		return 0;
	}
	uint32_t avail = em->em_sb.s_blocks_count - blk;
	uint32_t ra = avail < EXT2_RA_BLOCKS ? avail : EXT2_RA_BLOCKS;
	int error = ext2_dev_io(em, (uint64_t)blk * EXT2_BLK_SIZE, em->em_ra_buf,
	    (size_t)ra * EXT2_BLK_SIZE, false);
	if (error != 0) {
		error = ext2_dev_io(em, (uint64_t)blk * EXT2_BLK_SIZE, buf, EXT2_BLK_SIZE, false);
		if (error == 0) {
			(void)ext2_bcache_put(em, blk, buf, false);
		}
		return error;
	}
	memcpy(buf, em->em_ra_buf, EXT2_BLK_SIZE);
	for (uint32_t i = 0; i < ra; i++) {
		if (ext2_bcache_find(em, blk + i) == NULL) {
			(void)ext2_bcache_put(em, blk + i,
			    em->em_ra_buf + (size_t)i * EXT2_BLK_SIZE, false);
		}
	}
	return 0;
}

static int
ext2_write_block_ino(struct ext2_mount *em, uint32_t blk, const void *buf, uint32_t ino)
{
	if (blk == 0 || blk >= em->em_sb.s_blocks_count) {
		return EIO;
	}
	struct ext2_bcache_entry *e = ext2_bcache_find(em, blk);
	if (e != NULL && memcmp(e->bc_data, buf, EXT2_BLK_SIZE) == 0) {
		if (ino != 0) {
			e->bc_ino = ino;
		}
		TAILQ_REMOVE(&em->em_bcache_lru, e, bc_lru);
		TAILQ_INSERT_TAIL(&em->em_bcache_lru, e, bc_lru);
		return 0;
	}
	if (ext2_bcache_put_ino(em, blk, buf, true, ino)) {
		return 0;
	}
	int error = ext2_dev_io(em, (uint64_t)blk * EXT2_BLK_SIZE, __DECONST(void *, buf), EXT2_BLK_SIZE, true);
	if (error != 0) {
		ext2_bcache_inval(em, blk);
	}
	return error;
}

static int
ext2_write_block(struct ext2_mount *em, uint32_t blk, const void *buf)
{
	return ext2_write_block_ino(em, blk, buf, 0);
}

static int
ext2_zero_block(struct ext2_mount *em, uint32_t blk)
{
	void *z = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	int error = ext2_write_block(em, blk, z);
	kfree_data(z, EXT2_BLK_SIZE);
	return error;
}

static int
ext2_flush_meta(struct ext2_mount *em)
{
	int error = 0;

	if (em->em_sb_dirty) {
		void *blk0 = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
		error = ext2_dev_io(em, 0, blk0, EXT2_BLK_SIZE, false);
		if (!error) {
			memcpy((char *)blk0 + 1024, &em->em_sb, sizeof(em->em_sb));
			error = ext2_dev_io(em, 0, blk0, EXT2_BLK_SIZE, true);
			if (!error) {
				em->em_sb_dirty = false;
			}
		}
		kfree_data(blk0, EXT2_BLK_SIZE);
	}
	if (!error && em->em_gd_dirty) {
		error = ext2_dev_io(em, EXT2_BLK_SIZE, em->em_gd,
		    (size_t)em->em_gd_blocks * EXT2_BLK_SIZE, true);
		if (!error) {
			em->em_gd_dirty = false;
		}
	}
	return error;
}

/* ---- Inode Disk Read/Write ---- */

static int
ext2_inode_rw(struct ext2_mount *em, uint32_t ino, struct ext2_inode *di, bool write)
{
	uint32_t grp, idx, ipb, blk, off;
	uint8_t *buf;
	int error;

	if (ino < 1 || ino > em->em_sb.s_inodes_count) {
		return EINVAL;
	}
	grp = (ino - 1) / em->em_sb.s_inodes_per_group;
	idx = (ino - 1) % em->em_sb.s_inodes_per_group;
	if (grp >= em->em_ngroups) {
		return EINVAL;
	}
	ipb = EXT2_BLK_SIZE / em->em_inode_size;
	blk = em->em_gd[grp].bg_inode_table + idx / ipb;
	off = (idx % ipb) * em->em_inode_size;

	buf = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	error = ext2_read_block(em, blk, buf);
	if (!error) {
		if (write) {
			memcpy(buf + off, di, sizeof(*di));
			error = ext2_write_block(em, blk, buf);
		} else {
			memcpy(di, buf + off, sizeof(*di));
		}
	}
	kfree_data(buf, EXT2_BLK_SIZE);
	return error;
}

static int
ext2_sync_node(struct ext2_node *node)
{
	if (!node->en_dirty) {
		return 0;
	}
	int error = ext2_inode_rw(node->en_mount, node->en_ino, &node->en_di, true);
	if (!error) {
		node->en_dirty = false;
	}
	return error;
}

/* ---- Bitmap Allocators ---- */

static int
ext2_alloc_block_hint(struct ext2_mount *em, uint32_t pref_grp, uint32_t pref_blk, uint32_t *out_blk)
{
	uint8_t *bmap = em->em_bmap_buf;
	uint32_t g;

	if (em->em_sb.s_free_blocks_count == 0) {
		return ENOSPC;
	}
	/* Fast path: try the requested contiguous block first for 128 KB eMMC batching */
	if (pref_blk >= em->em_sb.s_first_data_block && pref_blk < em->em_sb.s_blocks_count) {
		uint32_t rel = pref_blk - em->em_sb.s_first_data_block;
		uint32_t pg = rel / em->em_sb.s_blocks_per_group;
		uint32_t idx = rel % em->em_sb.s_blocks_per_group;
		if (pg < em->em_ngroups && em->em_gd[pg].bg_free_blocks_count > 0 &&
		    ext2_read_block(em, em->em_gd[pg].bg_block_bitmap, bmap) == 0) {
			uint32_t byte = idx >> 3;
			uint8_t mask = (uint8_t)(1u << (idx & 7));
			if (!(bmap[byte] & mask)) {
				bmap[byte] |= mask;
				if (ext2_write_block(em, em->em_gd[pg].bg_block_bitmap, bmap) != 0) {
					return EIO;
				}
				if (em->em_grp_hint) {
					em->em_grp_hint[pg] = (uint16_t)byte;
				}
				em->em_gd[pg].bg_free_blocks_count--;
				em->em_sb.s_free_blocks_count--;
				em->em_gd_dirty = true;
				em->em_sb_dirty = true;
				*out_blk = pref_blk;
				return 0;
			}
		}
		if (pg < em->em_ngroups) {
			pref_grp = pg;
		}
	}
	for (uint32_t pass = 0; pass < em->em_ngroups; pass++) {
		g = (pref_grp + pass) % em->em_ngroups;
		if (em->em_gd[g].bg_free_blocks_count == 0) {
			continue;
		}
		if (ext2_read_block(em, em->em_gd[g].bg_block_bitmap, bmap) != 0) {
			continue;
		}
		uint32_t limit = em->em_sb.s_blocks_per_group;
		uint32_t grp_start = em->em_sb.s_first_data_block + g * em->em_sb.s_blocks_per_group;
		if (grp_start + limit > em->em_sb.s_blocks_count) {
			limit = em->em_sb.s_blocks_count - grp_start;
		}
		uint32_t start_byte = em->em_grp_hint ? em->em_grp_hint[g] : 0;
		if (start_byte * 8 >= limit) {
			start_byte = 0;
		}
		for (uint32_t phase = 0; phase < 2; phase++) {
			uint32_t begin = (phase == 0) ? start_byte : 0;
			uint32_t end = (phase == 0) ? ((limit + 7) / 8) : start_byte;
			uint32_t byte = begin;
			while (byte + 8 <= end && ((uintptr_t)(bmap + byte) & 7) == 0) {
				uint64_t w = *(const uint64_t *)(bmap + byte);
				if (w != ~0ULL) {
					int bit64 = __builtin_ctzll(~w);
					uint32_t rel_bit = byte * 8 + (uint32_t)bit64;
					if (rel_bit < limit) {
						uint32_t b_idx = rel_bit >> 3;
						bmap[b_idx] |= (uint8_t)(1u << (rel_bit & 7));
						if (ext2_write_block(em, em->em_gd[g].bg_block_bitmap, bmap) != 0) {
							return EIO;
						}
						if (em->em_grp_hint) {
							em->em_grp_hint[g] = (uint16_t)b_idx;
						}
						em->em_gd[g].bg_free_blocks_count--;
						em->em_sb.s_free_blocks_count--;
						em->em_gd_dirty = true;
						em->em_sb_dirty = true;
						*out_blk = grp_start + rel_bit;
						return 0;
					}
				}
				byte += 8;
			}
			for (; byte < end; byte++) {
				if (bmap[byte] == 0xff) {
					continue;
				}
				for (uint32_t bit = 0; bit < 8 && byte * 8 + bit < limit; bit++) {
					if (!(bmap[byte] & (1u << bit))) {
						uint32_t blk = grp_start + byte * 8 + bit;
						bmap[byte] |= (uint8_t)(1u << bit);
						if (ext2_write_block(em, em->em_gd[g].bg_block_bitmap, bmap) != 0) {
							return EIO;
						}
						if (em->em_grp_hint) {
							em->em_grp_hint[g] = (uint16_t)byte;
						}
						em->em_gd[g].bg_free_blocks_count--;
						em->em_sb.s_free_blocks_count--;
						em->em_gd_dirty = true;
						em->em_sb_dirty = true;
						*out_blk = blk;
						return 0;
					}
				}
			}
		}
	}
	return ENOSPC;
}

static int
ext2_alloc_block(struct ext2_mount *em, uint32_t pref_grp, uint32_t *out_blk)
{
	return ext2_alloc_block_hint(em, pref_grp, 0, out_blk);
}

static void
ext2_free_block(struct ext2_mount *em, uint32_t blk)
{
	uint32_t g, bit, byte;
	uint8_t *bmap = em->em_bmap_buf;

	if (blk < em->em_sb.s_first_data_block || blk >= em->em_sb.s_blocks_count) {
		return;
	}
	g = (blk - em->em_sb.s_first_data_block) / em->em_sb.s_blocks_per_group;
	bit = (blk - em->em_sb.s_first_data_block) % em->em_sb.s_blocks_per_group;
	byte = bit / 8;
	if (g >= em->em_ngroups) {
		return;
	}
	if (ext2_read_block(em, em->em_gd[g].bg_block_bitmap, bmap) == 0) {
		if (bmap[byte] & (1u << (bit & 7))) {
			bmap[byte] &= (uint8_t)~(1u << (bit & 7));
			if (ext2_write_block(em, em->em_gd[g].bg_block_bitmap, bmap) == 0) {
				if (em->em_grp_hint && byte < em->em_grp_hint[g]) {
					em->em_grp_hint[g] = (uint16_t)byte;
				}
				em->em_gd[g].bg_free_blocks_count++;
				em->em_sb.s_free_blocks_count++;
				em->em_gd_dirty = true;
				em->em_sb_dirty = true;
			}
		}
	}
}

static int
ext2_alloc_inode(struct ext2_mount *em, uint32_t pref_grp, bool is_dir, uint32_t *out_ino)
{
	uint8_t *imap;

	if (em->em_sb.s_free_inodes_count == 0) {
		return ENOSPC;
	}
	imap = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	for (uint32_t pass = 0; pass < em->em_ngroups; pass++) {
		uint32_t g = (pref_grp + pass) % em->em_ngroups;
		if (em->em_gd[g].bg_free_inodes_count == 0) {
			continue;
		}
		if (ext2_read_block(em, em->em_gd[g].bg_inode_bitmap, imap) != 0) {
			continue;
		}
		uint32_t limit = em->em_sb.s_inodes_per_group;
		for (uint32_t byte = 0; byte * 8 < limit; byte++) {
			if (imap[byte] == 0xff) {
				continue;
			}
			for (uint32_t bit = 0; bit < 8 && byte * 8 + bit < limit; bit++) {
				if (!(imap[byte] & (1u << bit))) {
					uint32_t ino = g * em->em_sb.s_inodes_per_group + byte * 8 + bit + 1;
					if (ino < em->em_sb.s_first_ino && ino != EXT2_ROOT_INO) {
						continue;
					}
					imap[byte] |= (uint8_t)(1u << bit);
					if (ext2_write_block(em, em->em_gd[g].bg_inode_bitmap, imap) != 0) {
						kfree_data(imap, EXT2_BLK_SIZE);
						return EIO;
					}
					em->em_gd[g].bg_free_inodes_count--;
					if (is_dir) {
						em->em_gd[g].bg_used_dirs_count++;
					}
					em->em_sb.s_free_inodes_count--;
					em->em_gd_dirty = true;
					em->em_sb_dirty = true;
					kfree_data(imap, EXT2_BLK_SIZE);
					*out_ino = ino;
					return 0;
				}
			}
		}
	}
	kfree_data(imap, EXT2_BLK_SIZE);
	return ENOSPC;
}

static void
ext2_free_inode(struct ext2_mount *em, uint32_t ino, bool is_dir)
{
	uint32_t g, bit, byte;
	uint8_t *imap;

	if (ino < 1 || ino > em->em_sb.s_inodes_count) {
		return;
	}
	g = (ino - 1) / em->em_sb.s_inodes_per_group;
	bit = (ino - 1) % em->em_sb.s_inodes_per_group;
	byte = bit / 8;
	if (g >= em->em_ngroups) {
		return;
	}
	imap = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	if (ext2_read_block(em, em->em_gd[g].bg_inode_bitmap, imap) == 0) {
		if (imap[byte] & (1u << (bit & 7))) {
			imap[byte] &= (uint8_t)~(1u << (bit & 7));
			if (ext2_write_block(em, em->em_gd[g].bg_inode_bitmap, imap) == 0) {
				em->em_gd[g].bg_free_inodes_count++;
				if (is_dir && em->em_gd[g].bg_used_dirs_count > 0) {
					em->em_gd[g].bg_used_dirs_count--;
				}
				em->em_sb.s_free_inodes_count++;
				em->em_gd_dirty = true;
				em->em_sb_dirty = true;
			}
		}
	}
	kfree_data(imap, EXT2_BLK_SIZE);
}

/* ---- Block Mapping (Direct + 1/2/3 Indirect) ---- */

static int
ext2_ind_get(struct ext2_mount *em, struct ext2_node *node, uint32_t *ptr,
    uint32_t idx, bool alloc, bool zero_leaf, uint32_t *out_blk)
{
	uint32_t pref = (node->en_ino - 1) / em->em_sb.s_inodes_per_group;
	uint32_t *table;
	int error = 0;

	if (*ptr == 0) {
		uint32_t nb = 0;
		if (!alloc) {
			*out_blk = 0;
			return 0;
		}
		error = ext2_alloc_block(em, pref, &nb);
		if (error) {
			return error;
		}
		error = ext2_zero_block(em, nb);
		if (error) {
			ext2_free_block(em, nb);
			return error;
		}
		*ptr = nb;
		node->en_di.i_blocks += EXT2_BLK_SIZE / 512u;
		node->en_dirty = true;
	}
	table = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	error = ext2_read_block(em, *ptr, table);
	if (!error) {
		if (table[idx] == 0 && alloc) {
			uint32_t nb = 0;
			error = ext2_alloc_block(em, pref, &nb);
			if (!error) {
				if (zero_leaf) {
					error = ext2_zero_block(em, nb);
					if (error) {
						ext2_free_block(em, nb);
					}
				}
				if (!error) {
					table[idx] = nb;
					error = ext2_write_block(em, *ptr, table);
					if (!error) {
						node->en_di.i_blocks += EXT2_BLK_SIZE / 512u;
						node->en_dirty = true;
					} else {
						ext2_free_block(em, nb);
					}
				}
			}
		}
		if (!error) {
			*out_blk = table[idx];
		}
	}
	kfree_data(table, EXT2_BLK_SIZE);
	return error;
}

static int
ext2_bmap(struct ext2_node *node, uint64_t lbn, bool alloc, bool zero_new, uint32_t *out_blk)
{
	struct ext2_mount *em = node->en_mount;
	uint32_t pref = (node->en_ino - 1) / em->em_sb.s_inodes_per_group;
	const uint64_t p = EXT2_PTRS_PER_BLK;
	const uint64_t p2 = p * p;

	*out_blk = 0;
	if (lbn < EXT2_NDIR_BLOCKS) {
		uint32_t blk = node->en_di.i_block[lbn];
		if (blk == 0 && alloc) {
			uint32_t hint = node->en_last_blk ? node->en_last_blk + 1 : 0;
			int error = ext2_alloc_block_hint(em, pref, hint, &blk);
			if (error) {
				return error;
			}
			if (zero_new) {
				error = ext2_zero_block(em, blk);
				if (error) {
					ext2_free_block(em, blk);
					return error;
				}
			}
			node->en_di.i_block[lbn] = blk;
			node->en_di.i_blocks += EXT2_BLK_SIZE / 512u;
			node->en_last_blk = blk;
			node->en_dirty = true;
		}
		*out_blk = blk;
		return 0;
	}
	lbn -= EXT2_NDIR_BLOCKS;
	if (lbn < p) {
		return ext2_ind_get(em, node, &node->en_di.i_block[EXT2_IND_BLOCK],
		    (uint32_t)lbn, alloc, zero_new, out_blk);
	}
	lbn -= p;
	if (lbn < p2) {
		uint32_t mid = 0;
		int error = ext2_ind_get(em, node, &node->en_di.i_block[EXT2_DIND_BLOCK],
		    (uint32_t)(lbn / p), alloc, true, &mid);
		if (error || mid == 0) {
			return error;
		}
		uint32_t tmp = mid;
		return ext2_ind_get(em, node, &tmp, (uint32_t)(lbn % p), alloc, zero_new, out_blk);
	}
	lbn -= p2;
	if (lbn < p2 * p) {
		uint32_t dind = 0, mid = 0;
		int error = ext2_ind_get(em, node, &node->en_di.i_block[EXT2_TIND_BLOCK],
		    (uint32_t)(lbn / p2), alloc, true, &dind);
		if (error || dind == 0) {
			return error;
		}
		uint32_t rem = (uint32_t)(lbn % p2);
		uint32_t tmp = dind;
		error = ext2_ind_get(em, node, &tmp, rem / (uint32_t)p, alloc, true, &mid);
		if (error || mid == 0) {
			return error;
		}
		tmp = mid;
		return ext2_ind_get(em, node, &tmp, rem % (uint32_t)p, alloc, zero_new, out_blk);
	}
	return EFBIG;
}

static void
ext2_free_indirect_tree(struct ext2_mount *em, uint32_t blk, int level)
{
	if (blk == 0) {
		return;
	}
	if (level > 0) {
		uint32_t *table = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
		if (ext2_read_block(em, blk, table) == 0) {
			for (uint32_t i = 0; i < EXT2_PTRS_PER_BLK; i++) {
				if (table[i] != 0) {
					ext2_free_indirect_tree(em, table[i], level - 1);
				}
			}
		}
		kfree_data(table, EXT2_BLK_SIZE);
	}
	ext2_free_block(em, blk);
}

static void
ext2_free_all_blocks(struct ext2_node *node)
{
	struct ext2_mount *em = node->en_mount;

	for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
		if (node->en_di.i_block[i] != 0) {
			ext2_free_block(em, node->en_di.i_block[i]);
			node->en_di.i_block[i] = 0;
		}
	}
	ext2_free_indirect_tree(em, node->en_di.i_block[EXT2_IND_BLOCK], 1);
	node->en_di.i_block[EXT2_IND_BLOCK] = 0;
	ext2_free_indirect_tree(em, node->en_di.i_block[EXT2_DIND_BLOCK], 2);
	node->en_di.i_block[EXT2_DIND_BLOCK] = 0;
	ext2_free_indirect_tree(em, node->en_di.i_block[EXT2_TIND_BLOCK], 3);
	node->en_di.i_block[EXT2_TIND_BLOCK] = 0;
	node->en_di.i_blocks = 0;
	ext2_inode_size_set(&node->en_di, 0);
	node->en_dirty = true;
}

/* ---- Vnode Hash & Lookup ---- */

static struct ext2_node *
ext2_hash_find(struct ext2_mount *em, uint32_t ino)
{
	struct ext2_node *n = em->em_hash[ino % EXT2_HASH_SIZE];
	while (n != NULL && n->en_ino != ino) {
		n = n->en_hash_next;
	}
	return n;
}

static void
ext2_hash_insert(struct ext2_mount *em, struct ext2_node *n)
{
	uint32_t b = n->en_ino % EXT2_HASH_SIZE;
	n->en_hash_next = em->em_hash[b];
	em->em_hash[b] = n;
}

static void
ext2_hash_remove(struct ext2_mount *em, struct ext2_node *n)
{
	uint32_t b = n->en_ino % EXT2_HASH_SIZE;
	struct ext2_node **pp = &em->em_hash[b];
	while (*pp != NULL && *pp != n) {
		pp = &(*pp)->en_hash_next;
	}
	if (*pp == n) {
		*pp = n->en_hash_next;
	}
}

static int
ext2_vget_locked(struct ext2_mount *em, uint32_t ino, uint32_t parent_ino, vnode_t *vpp)
{
	struct ext2_node *node;
	struct vnode_fsparam param;
	vnode_t vp;
	uint32_t vid;
	int error;

	for (;;) {
		node = ext2_hash_find(em, ino);
		if (node != NULL) {
			if (node->en_flags & EXT2_N_CREATING) {
				node->en_flags |= EXT2_N_WANTED;
				msleep(node, &em->em_lock, PINOD, "ext2_vget", NULL);
				continue;
			}
			vp = node->en_vp;
			vid = node->en_vid;
			if (vp != NULLVP) {
				lck_mtx_unlock(&em->em_lock);
				error = vnode_getwithvid(vp, vid);
				lck_mtx_lock(&em->em_lock);
				if (error == 0) {
					if (parent_ino != 0) {
						node->en_parent_ino = parent_ino;
					}
					*vpp = vp;
					return 0;
				}
				continue;
			}
			break;
		}
		node = kalloc_type(struct ext2_node, Z_WAITOK | Z_ZERO | Z_NOFAIL);
		node->en_ino = ino;
		node->en_mount = em;
		node->en_parent_ino = parent_ino ? parent_ino : EXT2_ROOT_INO;
		node->en_flags = EXT2_N_CREATING;
		ext2_hash_insert(em, node);
		error = ext2_inode_rw(em, ino, &node->en_di, false);
		if (error) {
			ext2_hash_remove(em, node);
			if (node->en_flags & EXT2_N_WANTED) {
				wakeup(node);
			}
			kfree_type(struct ext2_node, node);
			return error;
		}
		break;
	}

	node->en_flags |= EXT2_N_CREATING;
	lck_mtx_unlock(&em->em_lock);

	bzero(&param, sizeof(param));
	param.vnfs_mp = em->em_mp;
	param.vnfs_vtype = ext2_mode_to_vtype(node->en_di.i_mode);
	param.vnfs_str = "ext2";
	param.vnfs_dvp = NULLVP;
	param.vnfs_fsnode = node;
	param.vnfs_vops = ext2_vnodeop_p;
	param.vnfs_filesize = (off_t)ext2_inode_size_get(&node->en_di);
	param.vnfs_flags = VNFS_NOCACHE | VNFS_CANTCACHE;
	param.vnfs_markroot = (ino == EXT2_ROOT_INO);

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &param, &vp);

	lck_mtx_lock(&em->em_lock);
	node->en_flags &= ~EXT2_N_CREATING;
	if (!error) {
		node->en_vp = vp;
		node->en_vid = vnode_vid(vp);
		vnode_addfsref(vp);
		if (parent_ino != 0) {
			node->en_parent_ino = parent_ino;
		}
		*vpp = vp;
	}
	if (node->en_flags & EXT2_N_WANTED) {
		node->en_flags &= ~EXT2_N_WANTED;
		wakeup(node);
	}
	return error;
}

/* ---- File Data Fetch / Store ---- */

static int
ext2_fetch(struct ext2_node *node, uint64_t off, void *buf, size_t len)
{
	struct ext2_mount *em = node->en_mount;
	uint8_t *blkbuf = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	uint8_t *out = (uint8_t *)buf;
	int error = 0;

	while (len > 0 && !error) {
		uint64_t lbn = off >> EXT2_BLK_SHIFT;
		size_t boff = (size_t)(off & (EXT2_BLK_SIZE - 1));
		size_t chunk = MIN(len, EXT2_BLK_SIZE - boff);
		uint32_t pblk = 0;

		error = ext2_bmap(node, lbn, false, false, &pblk);
		if (!error) {
			if (pblk == 0) {
				bzero(out, chunk);
			} else {
				error = ext2_read_block(em, pblk, blkbuf);
				if (!error) {
					memcpy(out, blkbuf + boff, chunk);
				}
			}
		}
		out += chunk;
		off += chunk;
		len -= chunk;
	}
	kfree_data(blkbuf, EXT2_BLK_SIZE);
	return error;
}

static int
ext2_store(struct ext2_node *node, uint64_t off, const void *buf, size_t len)
{
	struct ext2_mount *em = node->en_mount;
	uint8_t *blkbuf = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	const uint8_t *in = (const uint8_t *)buf;
	int error = 0;

	while (len > 0 && !error) {
		uint64_t lbn = off >> EXT2_BLK_SHIFT;
		size_t boff = (size_t)(off & (EXT2_BLK_SIZE - 1));
		size_t chunk = MIN(len, EXT2_BLK_SIZE - boff);
		uint32_t pblk = 0;

		error = ext2_bmap(node, lbn, true, (boff != 0 || chunk < EXT2_BLK_SIZE), &pblk);
		if (!error) {
			if (boff != 0 || chunk < EXT2_BLK_SIZE) {
				error = ext2_read_block(em, pblk, blkbuf);
			}
			if (!error) {
				memcpy(blkbuf + boff, in, chunk);
				error = ext2_write_block_ino(em, pblk, blkbuf, node->en_ino);
			}
		}
		in += chunk;
		off += chunk;
		len -= chunk;
	}
	kfree_data(blkbuf, EXT2_BLK_SIZE);
	if (!error && off > ext2_inode_size_get(&node->en_di)) {
		ext2_inode_size_set(&node->en_di, off);
		node->en_dirty = true;
	}
	return error;
}

/* ---- Directory Operations ---- */

static int
ext2_dir_find(struct ext2_node *dir, const char *name, size_t namelen, uint32_t *out_ino)
{
	uint64_t dir_sz = ext2_inode_size_get(&dir->en_di);
	uint8_t *blk = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);

	for (uint64_t off = 0; off < dir_sz; off += EXT2_BLK_SIZE) {
		uint32_t pblk = 0;
		int error = ext2_bmap(dir, off >> EXT2_BLK_SHIFT, false, false, &pblk);
		if (error) {
			kfree_data(blk, EXT2_BLK_SIZE);
			return error;
		}
		if (pblk == 0 || ext2_read_block(dir->en_mount, pblk, blk) != 0) {
			continue;
		}
		uint32_t pos = 0;
		while (pos + 8 <= EXT2_BLK_SIZE) {
			struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + pos);
			if (de->rec_len < 8 || pos + de->rec_len > EXT2_BLK_SIZE) {
				break;
			}
			if (de->inode != 0 && de->name_len == namelen &&
			    memcmp(de->name, name, namelen) == 0) {
				*out_ino = de->inode;
				kfree_data(blk, EXT2_BLK_SIZE);
				return 0;
			}
			pos += de->rec_len;
		}
	}
	kfree_data(blk, EXT2_BLK_SIZE);
	return ENOENT;
}

static int
ext2_dir_add(struct ext2_node *dir, const char *name, size_t namelen, uint32_t ino, uint8_t ft)
{
	uint16_t need = (uint16_t)((8 + namelen + 3) & ~3u);
	uint64_t dir_sz = ext2_inode_size_get(&dir->en_di);
	uint8_t *blk = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);

	for (uint64_t off = 0; off < dir_sz; off += EXT2_BLK_SIZE) {
		uint32_t pblk = 0;
		int error = ext2_bmap(dir, off >> EXT2_BLK_SHIFT, false, false, &pblk);
		if (error || pblk == 0 || ext2_read_block(dir->en_mount, pblk, blk) != 0) {
			continue;
		}
		uint32_t pos = 0;
		while (pos + 8 <= EXT2_BLK_SIZE) {
			struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + pos);
			if (de->rec_len < 8 || pos + de->rec_len > EXT2_BLK_SIZE) {
				break;
			}
			uint16_t actual = de->inode ? (uint16_t)((8 + de->name_len + 3) & ~3u) : 0;
			if (de->rec_len - actual >= need) {
				struct ext2_dir_entry *nde;
				if (de->inode == 0) {
					nde = de;
				} else {
					uint16_t rem = de->rec_len - actual;
					de->rec_len = actual;
					nde = (struct ext2_dir_entry *)(blk + pos + actual);
					nde->rec_len = rem;
				}
				nde->inode = ino;
				nde->name_len = (uint8_t)namelen;
				nde->file_type = ft;
				memcpy(nde->name, name, namelen);
				error = ext2_write_block(dir->en_mount, pblk, blk);
				kfree_data(blk, EXT2_BLK_SIZE);
				return error;
			}
			pos += de->rec_len;
		}
	}

	uint32_t pblk = 0;
	int error = ext2_bmap(dir, dir_sz >> EXT2_BLK_SHIFT, true, true, &pblk);
	if (!error) {
		bzero(blk, EXT2_BLK_SIZE);
		struct ext2_dir_entry *de = (struct ext2_dir_entry *)blk;
		de->inode = ino;
		de->rec_len = EXT2_BLK_SIZE;
		de->name_len = (uint8_t)namelen;
		de->file_type = ft;
		memcpy(de->name, name, namelen);
		error = ext2_write_block(dir->en_mount, pblk, blk);
		if (!error) {
			ext2_inode_size_set(&dir->en_di, dir_sz + EXT2_BLK_SIZE);
			dir->en_dirty = true;
		}
	}
	kfree_data(blk, EXT2_BLK_SIZE);
	return error;
}

static int
ext2_dir_remove(struct ext2_node *dir, const char *name, size_t namelen)
{
	uint64_t dir_sz = ext2_inode_size_get(&dir->en_di);
	uint8_t *blk = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);

	for (uint64_t off = 0; off < dir_sz; off += EXT2_BLK_SIZE) {
		uint32_t pblk = 0;
		if (ext2_bmap(dir, off >> EXT2_BLK_SHIFT, false, false, &pblk) != 0 || pblk == 0) {
			continue;
		}
		if (ext2_read_block(dir->en_mount, pblk, blk) != 0) {
			continue;
		}
		uint32_t pos = 0;
		struct ext2_dir_entry *prev = NULL;
		while (pos + 8 <= EXT2_BLK_SIZE) {
			struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + pos);
			if (de->rec_len < 8 || pos + de->rec_len > EXT2_BLK_SIZE) {
				break;
			}
			if (de->inode != 0 && de->name_len == namelen &&
			    memcmp(de->name, name, namelen) == 0) {
				if (prev != NULL) {
					prev->rec_len += de->rec_len;
				} else {
					de->inode = 0;
				}
				int error = ext2_write_block(dir->en_mount, pblk, blk);
				kfree_data(blk, EXT2_BLK_SIZE);
				return error;
			}
			prev = de;
			pos += de->rec_len;
		}
	}
	kfree_data(blk, EXT2_BLK_SIZE);
	return ENOENT;
}

static bool
ext2_dir_is_empty(struct ext2_node *dir)
{
	uint64_t dir_sz = ext2_inode_size_get(&dir->en_di);
	uint8_t *blk = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	bool empty = true;

	for (uint64_t off = 0; off < dir_sz && empty; off += EXT2_BLK_SIZE) {
		uint32_t pblk = 0;
		if (ext2_bmap(dir, off >> EXT2_BLK_SHIFT, false, false, &pblk) != 0 || pblk == 0) {
			continue;
		}
		if (ext2_read_block(dir->en_mount, pblk, blk) != 0) {
			continue;
		}
		uint32_t pos = 0;
		while (pos + 8 <= EXT2_BLK_SIZE) {
			struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + pos);
			if (de->rec_len < 8 || pos + de->rec_len > EXT2_BLK_SIZE) {
				break;
			}
			if (de->inode != 0) {
				if (!((de->name_len == 1 && de->name[0] == '.') ||
				    (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'))) {
					empty = false;
					break;
				}
			}
			pos += de->rec_len;
		}
	}
	kfree_data(blk, EXT2_BLK_SIZE);
	return empty;
}

/* ---- Vnode Operations ---- */

/* Bring already-resident UBC pages up to date after write() so mmap() readers see new data. */
static void
ext2_refresh_cache(vnode_t vp, struct ext2_node *node, uint64_t start, uint64_t end)
{
	struct ext2_mount *em = node->en_mount;

	for (uint64_t page = trunc_page_64(start); page < end; page += PAGE_SIZE) {
		upl_t upl = NULL;
		upl_page_info_t *pl = NULL;
		vm_offset_t addr;
		int flags = 0;

		if (ubc_page_op(vp, (off_t)page, 0, NULL, &flags) != KERN_SUCCESS) {
			continue;
		}
		if (ubc_create_upl_kernel(vp, (off_t)page, PAGE_SIZE, &upl, &pl, UPL_SET_LITE | UPL_WILL_MODIFY,
		    VM_KERN_MEMORY_FILE) != KERN_SUCCESS || upl == NULL) {
			continue;
		}
		if (!upl_page_present(pl, 0) || ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
			ubc_upl_abort_range(upl, 0, PAGE_SIZE, UPL_ABORT_FREE_ON_EMPTY);
			continue;
		}
		lck_mtx_lock(&em->em_lock);
		uint64_t sz = ext2_inode_size_get(&node->en_di);
		if (page < sz) {
			size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, sz - page);
			if (ext2_fetch(node, page, (void *)addr, len) != 0) {
				bzero((void *)addr, PAGE_SIZE);
			} else if (len < PAGE_SIZE) {
				bzero((void *)(addr + len), PAGE_SIZE - len);
			}
		} else {
			bzero((void *)addr, PAGE_SIZE);
		}
		lck_mtx_unlock(&em->em_lock);
		ubc_upl_unmap(upl);
		ubc_upl_commit_range(upl, 0, PAGE_SIZE, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
	}
}

static void
ext2_sync_mapping(vnode_t vp, struct ext2_node *node)
{
	uint64_t sz = ext2_inode_size_get(&node->en_di);
	if (UBCINFOEXISTS(vp) && sz > 0) {
		(void)ubc_msync(vp, 0, (off_t)round_page_64(sz), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}
}

static int
ext2_create_node_common(vnode_t dvp, struct componentname *cnp, struct vnode_attr *vap,
    vnode_t *vpp, enum vtype vtype, const char *link_target)
{
	struct ext2_node *dir = EXT2_NODE(dvp);
	struct ext2_mount *em = dir->en_mount;
	vfs_context_t ctx = vfs_context_current();
	kauth_cred_t cred = vfs_context_ucred(ctx);
	uint32_t ino = 0, existing = 0;
	struct ext2_node *child = NULL;
	vnode_t vp = NULLVP;
	uint16_t mode;
	uint64_t now = ext2_now_sec();
	int error;

	if (cnp->cn_namelen > 255) {
		return ENAMETOOLONG;
	}
	lck_mtx_lock(&em->em_lock);
	if (ext2_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen, &existing) == 0) {
		lck_mtx_unlock(&em->em_lock);
		return EEXIST;
	}
	error = ext2_alloc_inode(em, (dir->en_ino - 1) / em->em_sb.s_inodes_per_group, vtype == VDIR, &ino);
	if (error) {
		lck_mtx_unlock(&em->em_lock);
		return error;
	}
	mode = VATTR_IS_ACTIVE(vap, va_mode) ? (uint16_t)(vap->va_mode & 07777) : (vtype == VDIR ? 0755 : 0644);
	switch (vtype) {
	case VDIR:  mode |= S_IFDIR; break;
	case VLNK:  mode |= S_IFLNK; break;
	case VSOCK: mode |= S_IFSOCK; break;
	case VFIFO: mode |= S_IFIFO; break;
	default:    mode |= S_IFREG; break;
	}

	struct ext2_inode di;
	bzero(&di, sizeof(di));
	di.i_mode = mode;
	di.i_uid = VATTR_IS_ACTIVE(vap, va_uid) ? (uint16_t)vap->va_uid : (uint16_t)kauth_cred_getuid(cred);
	di.i_gid = VATTR_IS_ACTIVE(vap, va_gid) ? (uint16_t)vap->va_gid : (uint16_t)dir->en_di.i_gid;
	di.i_atime = di.i_ctime = di.i_mtime = (uint32_t)now;
	di.i_links_count = (vtype == VDIR) ? 2 : 1;

	error = ext2_inode_rw(em, ino, &di, true);
	if (!error) {
		error = ext2_vget_locked(em, ino, dir->en_ino, &vp);
	}
	if (error) {
		ext2_free_inode(em, ino, vtype == VDIR);
		lck_mtx_unlock(&em->em_lock);
		return error;
	}
	child = EXT2_NODE(vp);
	if (vtype == VDIR) {
		uint32_t pblk = 0;
		error = ext2_bmap(child, 0, true, true, &pblk);
		if (!error) {
			uint8_t *blk = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_ZERO | Z_NOFAIL);
			struct ext2_dir_entry *dot = (struct ext2_dir_entry *)blk;
			dot->inode = ino;
			dot->rec_len = 12;
			dot->name_len = 1;
			dot->file_type = EXT2_FT_DIR;
			dot->name[0] = '.';
			struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *)(blk + 12);
			dotdot->inode = dir->en_ino;
			dotdot->rec_len = EXT2_BLK_SIZE - 12;
			dotdot->name_len = 2;
			dotdot->file_type = EXT2_FT_DIR;
			dotdot->name[0] = '.';
			dotdot->name[1] = '.';
			error = ext2_write_block(em, pblk, blk);
			kfree_data(blk, EXT2_BLK_SIZE);
			if (!error) {
				ext2_inode_size_set(&child->en_di, EXT2_BLK_SIZE);
				child->en_dirty = true;
			}
		}
	} else if (vtype == VLNK && link_target != NULL) {
		size_t tlen = strlen(link_target);
		if (tlen < sizeof(child->en_di.i_block)) {
			memcpy(child->en_di.i_block, link_target, tlen);
			ext2_inode_size_set(&child->en_di, tlen);
			child->en_dirty = true;
		} else {
			error = ext2_store(child, 0, link_target, tlen);
		}
	}
	if (!error) {
		error = ext2_dir_add(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen, ino, ext2_mode_to_ft(mode));
	}
	if (!error) {
		if (vtype == VDIR) {
			dir->en_di.i_links_count++;
		}
		size_t nlen = (size_t)cnp->cn_namelen;
		if (nlen >= sizeof(child->en_name)) {
			nlen = sizeof(child->en_name) - 1;
		}
		memcpy(child->en_name, cnp->cn_nameptr, nlen);
		child->en_name[nlen] = '\0';
		dir->en_di.i_mtime = dir->en_di.i_ctime = (uint32_t)now;
		dir->en_dirty = true;
		(void)ext2_sync_node(child);
		(void)ext2_sync_node(dir);
	}
	lck_mtx_unlock(&em->em_lock);
	if (error) {
		vnode_put(vp);
		return error;
	}
	*vpp = vp;
	return 0;
}

static int
ext2_lookup(struct vnop_lookup_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct ext2_node *dir = EXT2_NODE(dvp);
	struct ext2_mount *em = dir->en_mount;
	struct componentname *cnp = ap->a_cnp;
	uint32_t ino = 0;
	vnode_t vp = NULLVP;
	int error;

	if (vnode_vtype(dvp) != VDIR) {
		return ENOTDIR;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		error = vnode_get(dvp);
		if (!error) {
			*ap->a_vpp = dvp;
		}
		return error;
	}
	lck_mtx_lock(&em->em_lock);
	if (cnp->cn_flags & ISDOTDOT) {
		ino = dir->en_parent_ino ? dir->en_parent_ino : EXT2_ROOT_INO;
		error = ext2_vget_locked(em, ino, 0, &vp);
		lck_mtx_unlock(&em->em_lock);
		if (!error) {
			*ap->a_vpp = vp;
		}
		return error;
	}
	error = ext2_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen, &ino);
	if (error == ENOENT) {
		lck_mtx_unlock(&em->em_lock);
		if ((cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME) && (cnp->cn_flags & ISLASTCN)) {
			return EJUSTRETURN;
		}
		return ENOENT;
	}
	if (!error) {
		error = ext2_vget_locked(em, ino, dir->en_ino, &vp);
		if (!error && !(cnp->cn_flags & ISDOTDOT) &&
		    !(cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.')) {
			struct ext2_node *child = EXT2_NODE(vp);
			size_t nlen = (size_t)cnp->cn_namelen;
			if (nlen >= sizeof(child->en_name)) {
				nlen = sizeof(child->en_name) - 1;
			}
			memcpy(child->en_name, cnp->cn_nameptr, nlen);
			child->en_name[nlen] = '\0';
		}
	}
	lck_mtx_unlock(&em->em_lock);
	if (!error) {
		*ap->a_vpp = vp;
	}
	return error;
}

static int
ext2_create(struct vnop_create_args *ap)
{
	return ext2_create_node_common(ap->a_dvp, ap->a_cnp, ap->a_vap, ap->a_vpp, VREG, NULL);
}

static int
ext2_mknod(struct vnop_mknod_args *ap)
{
	enum vtype vt = VATTR_IS_ACTIVE(ap->a_vap, va_type) ? ap->a_vap->va_type : VREG;
	if (vt != VREG && vt != VSOCK && vt != VFIFO) {
		return EINVAL;
	}
	return ext2_create_node_common(ap->a_dvp, ap->a_cnp, ap->a_vap, ap->a_vpp, vt, NULL);
}

static int
ext2_mkdir(struct vnop_mkdir_args *ap)
{
	return ext2_create_node_common(ap->a_dvp, ap->a_cnp, ap->a_vap, ap->a_vpp, VDIR, NULL);
}

static int
ext2_symlink(struct vnop_symlink_args *ap)
{
	return ext2_create_node_common(ap->a_dvp, ap->a_cnp, ap->a_vap, ap->a_vpp, VLNK, ap->a_target);
}

static int
ext2_open(__unused struct vnop_open_args *ap)
{
	return 0;
}

static int
ext2_close(__unused struct vnop_close_args *ap)
{
	return 0;
}

static int
ext2_getattr(struct vnop_getattr_args *ap)
{
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = node->en_mount;
	struct vnode_attr *vap = ap->a_vap;
	struct timespec ts;

	lck_mtx_lock(&em->em_lock);
	VATTR_RETURN(vap, va_type, ext2_mode_to_vtype(node->en_di.i_mode));
	VATTR_RETURN(vap, va_mode, node->en_di.i_mode & 07777);
	VATTR_RETURN(vap, va_nlink, node->en_di.i_links_count);
	VATTR_RETURN(vap, va_uid, node->en_di.i_uid);
	VATTR_RETURN(vap, va_gid, node->en_di.i_gid);
	VATTR_RETURN(vap, va_fsid, vfs_statfs(em->em_mp)->f_fsid.val[0]);
	VATTR_RETURN(vap, va_fileid, node->en_ino);
	VATTR_RETURN(vap, va_rdev, 0);
	uint64_t dsize = ext2_inode_size_get(&node->en_di);
	uint64_t dalloc = (uint64_t)node->en_di.i_blocks * 512ULL;
	VATTR_RETURN(vap, va_data_size, dsize);
	VATTR_RETURN(vap, va_total_size, dsize);
	VATTR_RETURN(vap, va_data_alloc, dalloc);
	VATTR_RETURN(vap, va_total_alloc, dalloc);
	VATTR_RETURN(vap, va_iosize, EXT2_BLK_SIZE);
	VATTR_RETURN(vap, va_flags, 0);
	VATTR_RETURN(vap, va_gen, node->en_di.i_generation);
	VATTR_RETURN(vap, va_linkid, node->en_ino);
	VATTR_RETURN(vap, va_parentid,
	    node->en_parent_ino ? node->en_parent_ino : (node->en_ino == EXT2_ROOT_INO ? 1 : 0));
	VATTR_RETURN(vap, va_filerev, 0);
	if (VATTR_IS_ACTIVE(vap, va_name)) {
		if (node->en_ino == EXT2_ROOT_INO) {
			strlcpy(vap->va_name, "/", MAXPATHLEN);
			VATTR_SET_SUPPORTED(vap, va_name);
		} else if (node->en_name[0] != '\0') {
			strlcpy(vap->va_name, node->en_name, MAXPATHLEN);
			VATTR_SET_SUPPORTED(vap, va_name);
		}
	}
	ts.tv_nsec = 0;
	ts.tv_sec = node->en_di.i_atime;
	VATTR_RETURN(vap, va_access_time, ts);
	ts.tv_sec = node->en_di.i_mtime;
	VATTR_RETURN(vap, va_modify_time, ts);
	ts.tv_sec = node->en_di.i_ctime;
	VATTR_RETURN(vap, va_change_time, ts);
	VATTR_RETURN(vap, va_create_time, ts);
	lck_mtx_unlock(&em->em_lock);
	return 0;
}

static int
ext2_setattr(struct vnop_setattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;
	struct vnode_attr *vap = ap->a_vap;
	uint64_t now = ext2_now_sec();

	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		if (vnode_vtype(vp) != VREG) {
			return EINVAL;
		}
		ext2_sync_mapping(vp, node);
		lck_mtx_lock(&em->em_lock);
		uint64_t old_sz = ext2_inode_size_get(&node->en_di);
		uint64_t new_sz = vap->va_data_size;
		if (new_sz == 0 && old_sz > 0) {
			ext2_free_all_blocks(node);
		}
		ext2_inode_size_set(&node->en_di, new_sz);
		node->en_di.i_mtime = node->en_di.i_ctime = (uint32_t)now;
		node->en_dirty = true;
		VATTR_SET_SUPPORTED(vap, va_data_size);
		lck_mtx_unlock(&em->em_lock);
		if (UBCINFOEXISTS(vp)) {
			ubc_setsize(vp, (off_t)new_sz);
			if (new_sz < old_sz && (new_sz & PAGE_MASK) != 0 && ubc_pages_resident(vp)) {
				ext2_refresh_cache(vp, node, trunc_page_64(new_sz), round_page_64(new_sz));
			}
		}
	}

	lck_mtx_lock(&em->em_lock);
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		node->en_di.i_mode = (node->en_di.i_mode & S_IFMT) | (uint16_t)(vap->va_mode & 07777);
		node->en_dirty = true;
		VATTR_SET_SUPPORTED(vap, va_mode);
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		node->en_di.i_uid = (uint16_t)vap->va_uid;
		node->en_dirty = true;
		VATTR_SET_SUPPORTED(vap, va_uid);
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		node->en_di.i_gid = (uint16_t)vap->va_gid;
		node->en_dirty = true;
		VATTR_SET_SUPPORTED(vap, va_gid);
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		VATTR_SET_SUPPORTED(vap, va_flags);
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		node->en_di.i_atime = (uint32_t)vap->va_access_time.tv_sec;
		node->en_dirty = true;
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		node->en_di.i_mtime = (uint32_t)vap->va_modify_time.tv_sec;
		node->en_dirty = true;
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		node->en_di.i_ctime = (uint32_t)vap->va_change_time.tv_sec;
		node->en_dirty = true;
		VATTR_SET_SUPPORTED(vap, va_change_time);
	}
	lck_mtx_unlock(&em->em_lock);
	return 0;
}

static int
ext2_read(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;
	char *bounce;
	int error = 0;

	if (vnode_vtype(vp) == VDIR) {
		return EISDIR;
	}
	if (vnode_vtype(vp) != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	ext2_sync_mapping(vp, node);

	bounce = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0 && !error) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len;

		lck_mtx_lock(&em->em_lock);
		uint64_t sz = ext2_inode_size_get(&node->en_di);
		if (off >= sz) {
			lck_mtx_unlock(&em->em_lock);
			break;
		}
		len = (size_t)MIN((uint64_t)uio_resid(uio), sz - off);
		len = MIN(len, EXT2_BLK_SIZE - (size_t)(off & (EXT2_BLK_SIZE - 1)));
		error = ext2_fetch(node, off, bounce, len);
		lck_mtx_unlock(&em->em_lock);
		if (!error) {
			error = uiomove(bounce, (int)len, uio);
		}
	}
	kfree_data(bounce, EXT2_BLK_SIZE);
	return error;
}

static int
ext2_write(struct vnop_write_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;
	char *bounce;
	uint64_t old_sz;
	int error = 0;

	if (vnode_vtype(vp) == VDIR) {
		return EISDIR;
	}
	if (vnode_vtype(vp) != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	if (uio_resid(uio) == 0) {
		return 0;
	}
	ext2_sync_mapping(vp, node);

	lck_mtx_lock(&em->em_lock);
	old_sz = ext2_inode_size_get(&node->en_di);
	if (ap->a_ioflag & IO_APPEND) {
		uio_setoffset(uio, (off_t)old_sz);
	}
	lck_mtx_unlock(&em->em_lock);
	uint64_t start = (uint64_t)uio_offset(uio);

	bounce = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len = MIN((size_t)uio_resid(uio), EXT2_BLK_SIZE - (size_t)(off & (EXT2_BLK_SIZE - 1)));

		error = uiomove(bounce, (int)len, uio);
		if (error) {
			break;
		}
		lck_mtx_lock(&em->em_lock);
		error = ext2_store(node, off, bounce, len);
		lck_mtx_unlock(&em->em_lock);
		if (error) {
			uio_setoffset(uio, (off_t)off);
			break;
		}
	}
	kfree_data(bounce, EXT2_BLK_SIZE);

	lck_mtx_lock(&em->em_lock);
	uint64_t new_sz = ext2_inode_size_get(&node->en_di);
	node->en_di.i_mtime = node->en_di.i_ctime = (uint32_t)ext2_now_sec();
	node->en_dirty = true;
	if (ap->a_ioflag & IO_SYNC) {
		(void)ext2_sync_node(node);
		(void)ext2_bcache_flush_ino(em, node->en_ino);
		(void)ext2_flush_meta(em);
	}
	lck_mtx_unlock(&em->em_lock);

	if (UBCINFOEXISTS(vp) && new_sz != old_sz) {
		ubc_setsize(vp, (off_t)new_sz);
	}
	uint64_t end_off = (uint64_t)uio_offset(uio);
	if (end_off > start && UBCINFOEXISTS(vp) && ubc_pages_resident(vp)) {
		ext2_refresh_cache(vp, node, start, end_off);
	}
	return error;
}

static int
ext2_remove(struct vnop_remove_args *ap)
{
	struct ext2_node *dir = EXT2_NODE(ap->a_dvp);
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = dir->en_mount;
	struct componentname *cnp = ap->a_cnp;
	int error;

	lck_mtx_lock(&em->em_lock);
	error = ext2_dir_remove(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen);
	if (!error) {
		if (node->en_di.i_links_count > 0) {
			node->en_di.i_links_count--;
		}
		node->en_di.i_ctime = (uint32_t)ext2_now_sec();
		node->en_dirty = true;
		dir->en_di.i_mtime = dir->en_di.i_ctime = (uint32_t)ext2_now_sec();
		dir->en_dirty = true;
		(void)ext2_sync_node(node);
		(void)ext2_sync_node(dir);
	}
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_rmdir(struct vnop_rmdir_args *ap)
{
	struct ext2_node *dir = EXT2_NODE(ap->a_dvp);
	struct ext2_node *sub = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = dir->en_mount;
	struct componentname *cnp = ap->a_cnp;
	int error;

	if (sub == dir || sub->en_ino == EXT2_ROOT_INO) {
		return EINVAL;
	}
	lck_mtx_lock(&em->em_lock);
	if (!ext2_dir_is_empty(sub)) {
		lck_mtx_unlock(&em->em_lock);
		return ENOTEMPTY;
	}
	error = ext2_dir_remove(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen);
	if (!error) {
		sub->en_di.i_links_count = 0;
		sub->en_dirty = true;
		if (dir->en_di.i_links_count > 1) {
			dir->en_di.i_links_count--;
		}
		dir->en_di.i_mtime = dir->en_di.i_ctime = (uint32_t)ext2_now_sec();
		dir->en_dirty = true;
		(void)ext2_sync_node(sub);
		(void)ext2_sync_node(dir);
	}
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_link(struct vnop_link_args *ap)
{
	struct ext2_node *dir = EXT2_NODE(ap->a_tdvp);
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = dir->en_mount;
	struct componentname *cnp = ap->a_cnp;
	uint32_t dummy;
	int error;

	if (vnode_vtype(ap->a_vp) == VDIR) {
		return EPERM;
	}
	lck_mtx_lock(&em->em_lock);
	if (ext2_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen, &dummy) == 0) {
		lck_mtx_unlock(&em->em_lock);
		return EEXIST;
	}
	error = ext2_dir_add(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen,
	    node->en_ino, ext2_mode_to_ft(node->en_di.i_mode));
	if (!error) {
		node->en_di.i_links_count++;
		node->en_di.i_ctime = (uint32_t)ext2_now_sec();
		node->en_dirty = true;
		dir->en_di.i_mtime = dir->en_di.i_ctime = (uint32_t)ext2_now_sec();
		dir->en_dirty = true;
		(void)ext2_sync_node(node);
		(void)ext2_sync_node(dir);
	}
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_rename(struct vnop_rename_args *ap)
{
	struct ext2_node *fdir = EXT2_NODE(ap->a_fdvp);
	struct ext2_node *fnode = EXT2_NODE(ap->a_fvp);
	struct ext2_node *tdir = EXT2_NODE(ap->a_tdvp);
	vnode_t tvp = ap->a_tvp;
	struct ext2_mount *em = fdir->en_mount;
	uint32_t existing = 0;
	int error = 0;

	if (fnode->en_ino == EXT2_ROOT_INO) {
		return EINVAL;
	}
	lck_mtx_lock(&em->em_lock);
	if (tvp != NULLVP) {
		struct ext2_node *tnode = EXT2_NODE(tvp);
		if (tnode == fnode) {
			lck_mtx_unlock(&em->em_lock);
			return 0;
		}
		if (vnode_vtype(tvp) == VDIR && !ext2_dir_is_empty(tnode)) {
			lck_mtx_unlock(&em->em_lock);
			return ENOTEMPTY;
		}
		error = ext2_dir_remove(tdir, ap->a_tcnp->cn_nameptr, (size_t)ap->a_tcnp->cn_namelen);
		if (!error) {
			tnode->en_di.i_links_count = (vnode_vtype(tvp) == VDIR) ? 0 :
			    (tnode->en_di.i_links_count > 0 ? tnode->en_di.i_links_count - 1 : 0);
			tnode->en_dirty = true;
			if (vnode_vtype(tvp) == VDIR && tdir->en_di.i_links_count > 1) {
				tdir->en_di.i_links_count--;
			}
			(void)ext2_sync_node(tnode);
		}
	} else if (ext2_dir_find(tdir, ap->a_tcnp->cn_nameptr, (size_t)ap->a_tcnp->cn_namelen, &existing) == 0) {
		lck_mtx_unlock(&em->em_lock);
		return EEXIST;
	}
	if (!error) {
		error = ext2_dir_add(tdir, ap->a_tcnp->cn_nameptr, (size_t)ap->a_tcnp->cn_namelen,
		    fnode->en_ino, ext2_mode_to_ft(fnode->en_di.i_mode));
	}
	if (!error) {
		error = ext2_dir_remove(fdir, ap->a_fcnp->cn_nameptr, (size_t)ap->a_fcnp->cn_namelen);
	}
	if (!error) {
		size_t tlen = (size_t)ap->a_tcnp->cn_namelen;
		if (tlen >= sizeof(fnode->en_name)) {
			tlen = sizeof(fnode->en_name) - 1;
		}
		memcpy(fnode->en_name, ap->a_tcnp->cn_nameptr, tlen);
		fnode->en_name[tlen] = '\0';
		if (vnode_vtype(ap->a_fvp) == VDIR && fdir != tdir) {
			uint32_t pblk = 0;
			if (fdir->en_di.i_links_count > 1) {
				fdir->en_di.i_links_count--;
			}
			tdir->en_di.i_links_count++;
			fnode->en_parent_ino = tdir->en_ino;
			if (ext2_bmap(fnode, 0, false, false, &pblk) == 0 && pblk != 0) {
				uint8_t *blk = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK);
				if (blk != NULL) {
					if (ext2_read_block(em, pblk, blk) == 0) {
						uint32_t off = 0;
						while (off + 8 <= EXT2_BLK_SIZE) {
							struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + off);
							if (de->rec_len < 8 || off + de->rec_len > EXT2_BLK_SIZE) {
								break;
							}
							if (de->inode != 0 && de->name_len == 2 &&
							    de->name[0] == '.' && de->name[1] == '.') {
								de->inode = tdir->en_ino;
								(void)ext2_write_block(em, pblk, blk);
								break;
							}
							off += de->rec_len;
						}
					}
					kfree_data(blk, EXT2_BLK_SIZE);
				}
			}
			(void)ext2_sync_node(fnode);
		}
		fdir->en_di.i_mtime = fdir->en_di.i_ctime = (uint32_t)ext2_now_sec();
		tdir->en_di.i_mtime = tdir->en_di.i_ctime = (uint32_t)ext2_now_sec();
		fdir->en_dirty = true;
		tdir->en_dirty = true;
		(void)ext2_sync_node(fdir);
		(void)ext2_sync_node(tdir);
	}
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_readdir(struct vnop_readdir_args *ap)
{
#define EXT2_DIRSIZ(namelen) ((__offsetof(struct dirent, d_name) + (namelen) + 1 + 3) & ~3)
	vnode_t vp = ap->a_vp;
	struct ext2_node *dir = EXT2_NODE(vp);
	struct ext2_mount *em = dir->en_mount;
	struct uio *uio = ap->a_uio;
	off_t start = uio_offset(uio), pos = 0;
	size_t room = (size_t)MIN(uio_resid(uio), PAGE_SIZE), used = 0;
	int count = 0, eof = 0, error = 0;
	char *out;
	uint8_t *blk;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}
	if (vnode_vtype(vp) != VDIR) {
		return ENOTDIR;
	}
	out = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);
	blk = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);

	lck_mtx_lock(&em->em_lock);
	uint64_t dir_sz = ext2_inode_size_get(&dir->en_di);
	for (uint64_t off = 0; off < dir_sz; off += EXT2_BLK_SIZE) {
		uint32_t pblk = 0;
		if (ext2_bmap(dir, off >> EXT2_BLK_SHIFT, false, false, &pblk) != 0 || pblk == 0) {
			continue;
		}
		if (ext2_read_block(em, pblk, blk) != 0) {
			continue;
		}
		uint32_t bpos = 0;
		while (bpos + 8 <= EXT2_BLK_SIZE) {
			struct ext2_dir_entry *de = (struct ext2_dir_entry *)(blk + bpos);
			if (de->rec_len < 8 || bpos + de->rec_len > EXT2_BLK_SIZE) {
				break;
			}
			if (de->inode != 0 && de->name_len > 0) {
				struct dirent d;
				bzero(&d, sizeof(d));
				d.d_fileno = de->inode;
				d.d_namlen = de->name_len;
				d.d_reclen = (uint16_t)EXT2_DIRSIZ(d.d_namlen);
				switch (de->file_type) {
				case EXT2_FT_REG_FILE: d.d_type = DT_REG; break;
				case EXT2_FT_DIR:      d.d_type = DT_DIR; break;
				case EXT2_FT_SYMLINK:  d.d_type = DT_LNK; break;
				case EXT2_FT_FIFO:     d.d_type = DT_FIFO; break;
				case EXT2_FT_SOCK:     d.d_type = DT_SOCK; break;
				default:               d.d_type = DT_UNKNOWN; break;
				}
				if (pos >= start) {
					if (used + d.d_reclen > room) {
						goto full;
					}
					memcpy(d.d_name, de->name, d.d_namlen);
					memcpy(out + used, &d, d.d_reclen);
					used += d.d_reclen;
					count++;
				}
				pos += d.d_reclen;
			}
			bpos += de->rec_len;
		}
	}
	eof = 1;
full:
	lck_mtx_unlock(&em->em_lock);
	error = used ? uiomove(out, (int)used, uio) : 0;
	kfree_data(blk, EXT2_BLK_SIZE);
	kfree_data(out, PAGE_SIZE);
	if (!error) {
		uio_setoffset(uio, pos);
		if (ap->a_eofflag) {
			*ap->a_eofflag = eof;
		}
		if (ap->a_numdirent) {
			*ap->a_numdirent = count;
		}
	}
	return error;
#undef EXT2_DIRSIZ
}

static int
ext2_readlink(struct vnop_readlink_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;
	uint64_t sz;
	int error;

	if (vnode_vtype(vp) != VLNK) {
		return EINVAL;
	}
	lck_mtx_lock(&em->em_lock);
	sz = ext2_inode_size_get(&node->en_di);
	if (sz < sizeof(node->en_di.i_block)) {
		char tmp[64];
		memcpy(tmp, node->en_di.i_block, (size_t)sz);
		lck_mtx_unlock(&em->em_lock);
		return uiomove(tmp, (int)sz, ap->a_uio);
	}
	char *buf = kalloc_data((size_t)sz, Z_WAITOK | Z_NOFAIL);
	error = ext2_fetch(node, 0, buf, (size_t)sz);
	lck_mtx_unlock(&em->em_lock);
	if (!error) {
		error = uiomove(buf, (int)sz, ap->a_uio);
	}
	kfree_data(buf, (size_t)sz);
	return error;
}

static int
ext2_fsync(struct vnop_fsync_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;

	ext2_sync_mapping(vp, node);
	lck_mtx_lock(&em->em_lock);
	int error = ext2_sync_node(node);
	if (ap->a_waitfor != MNT_NOWAIT) {
		if (!error) {
			error = ext2_bcache_flush_ino(em, node->en_ino);
		}
		if (!error) {
			error = ext2_flush_meta(em);
		}
	}
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_inactive(struct vnop_inactive_args *ap)
{
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = node->en_mount;

	lck_mtx_lock(&em->em_lock);
	(void)ext2_sync_node(node);
	bool dead = (node->en_di.i_links_count == 0);
	lck_mtx_unlock(&em->em_lock);
	if (dead) {
		vnode_recycle(ap->a_vp);
	}
	return 0;
}

static int
ext2_reclaim(struct vnop_reclaim_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;

	lck_mtx_lock(&em->em_lock);
	node->en_vp = NULLVP;
	node->en_vid = 0;
	if (node->en_di.i_links_count == 0 && node->en_ino != EXT2_ROOT_INO) {
		bool is_dir = (node->en_di.i_mode & S_IFMT) == S_IFDIR;
		ext2_free_all_blocks(node);
		node->en_di.i_dtime = (uint32_t)ext2_now_sec();
		node->en_dirty = true;
		(void)ext2_sync_node(node);
		ext2_free_inode(em, node->en_ino, is_dir);
		ext2_xattr_purge_ino(em, node->en_ino);
		(void)ext2_flush_meta(em);
	} else {
		(void)ext2_sync_node(node);
	}
	ext2_hash_remove(em, node);
	lck_mtx_unlock(&em->em_lock);

	vnode_clearfsnode(vp);
	vnode_removefsref(vp);
	kfree_type(struct ext2_node, node);
	return 0;
}

static int
ext2_mmap(__unused struct vnop_mmap_args *ap)
{
	return 0;
}

static int
ext2_mnomap(__unused struct vnop_mnomap_args *ap)
{
	return 0;
}

static int
ext2_pagein(struct vnop_pagein_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	upl_size_t valid = 0;
	vm_offset_t addr;

	if (upl == NULL || vnode_vtype(vp) != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit && upl != NULL) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	lck_mtx_lock(&em->em_lock);
	uint64_t sz = ext2_inode_size_get(&node->en_di);
	for (upl_size_t done = 0; done < size && off + done < sz; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, sz - (off + done));
		if (ext2_fetch(node, off + done, (void *)(addr + done), len) != 0) {
			break;
		}
		if (len < PAGE_SIZE) {
			bzero((void *)(addr + done + len), PAGE_SIZE - len);
		}
		valid = done + PAGE_SIZE;
	}
	lck_mtx_unlock(&em->em_lock);
	ubc_upl_unmap(upl);

	if (commit) {
		if (valid != 0) {
			ubc_upl_commit_range(upl, ap->a_pl_offset, valid, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
		if (valid < size) {
			ubc_upl_abort_range(upl, ap->a_pl_offset + valid, size - valid, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
	}
	return valid != 0 ? 0 : EINVAL;
}

static int
ext2_pageout(struct vnop_pageout_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ext2_node *node = EXT2_NODE(vp);
	struct ext2_mount *em = node->en_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	vm_offset_t addr;
	int error = 0;

	if (upl == NULL) {
		return EINVAL;
	}
	if (vnode_vtype(vp) != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	lck_mtx_lock(&em->em_lock);
	uint64_t sz = ext2_inode_size_get(&node->en_di);
	for (upl_size_t done = 0; done < size && off + done < sz && !error; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, sz - (off + done));
		error = ext2_store(node, off + done, (const void *)(addr + done), len);
	}
	if (!error) {
		node->en_di.i_mtime = node->en_di.i_ctime = (uint32_t)ext2_now_sec();
		node->en_dirty = true;
	}
	lck_mtx_unlock(&em->em_lock);
	ubc_upl_unmap(upl);

	if (commit) {
		if (error) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		} else {
			ubc_upl_commit_range(upl, ap->a_pl_offset, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

static int
ext2_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:        *ap->a_retval = 32000; return 0;
	case _PC_NAME_MAX:        *ap->a_retval = 255; return 0;
	case _PC_PATH_MAX:        *ap->a_retval = MAXPATHLEN; return 0;
	case _PC_PIPE_BUF:        *ap->a_retval = PIPE_BUF; return 0;
	case _PC_CHOWN_RESTRICTED:*ap->a_retval = 200112; return 0;
	case _PC_NO_TRUNC:        *ap->a_retval = 200112; return 0;
	case _PC_CASE_SENSITIVE:  *ap->a_retval = 1; return 0;
	case _PC_CASE_PRESERVING: *ap->a_retval = 1; return 0;
	case _PC_FILESIZEBITS:    *ap->a_retval = 64; return 0;
	default:                  return EINVAL;
	}
}

static int
ext2_ioctl(__unused struct vnop_ioctl_args *ap)
{
	return ENOTTY;
}

static int
ext2_blktooff(struct vnop_blktooff_args *ap)
{
	*ap->a_offset = (off_t)ap->a_lblkno * EXT2_BLK_SIZE;
	return 0;
}

static int
ext2_offtoblk(struct vnop_offtoblk_args *ap)
{
	*ap->a_lblkno = (daddr64_t)(ap->a_offset / EXT2_BLK_SIZE);
	return 0;
}

static void
ext2_xattr_free(struct ext2_xattr *xa)
{
	if (xa == NULL) {
		return;
	}
	if (xa->ex_name != NULL) {
		kfree_data(xa->ex_name, xa->ex_namelen + 1);
	}
	if (xa->ex_data != NULL) {
		kfree_data(xa->ex_data, xa->ex_size);
	}
	kfree_type(struct ext2_xattr, xa);
}

static struct ext2_xattr_bucket *
ext2_xattr_bucket_find(struct ext2_mount *em, uint32_t ino, bool create)
{
	uint32_t h = ino % EXT2_HASH_SIZE;
	for (struct ext2_xattr_bucket *xb = em->em_xattr_hash[h]; xb != NULL; xb = xb->xb_next) {
		if (xb->xb_ino == ino) {
			return xb;
		}
	}
	if (!create) {
		return NULL;
	}
	struct ext2_xattr_bucket *xb = kalloc_type(struct ext2_xattr_bucket, Z_NOWAIT | Z_ZERO);
	if (xb == NULL) {
		return NULL;
	}
	xb->xb_ino = ino;
	TAILQ_INIT(&xb->xb_list);
	xb->xb_next = em->em_xattr_hash[h];
	em->em_xattr_hash[h] = xb;
	return xb;
}

static void
ext2_xattr_purge_ino(struct ext2_mount *em, uint32_t ino)
{
	uint32_t h = ino % EXT2_HASH_SIZE;
	struct ext2_xattr_bucket **pp = &em->em_xattr_hash[h];
	while (*pp != NULL) {
		if ((*pp)->xb_ino == ino) {
			struct ext2_xattr_bucket *xb = *pp;
			*pp = xb->xb_next;
			struct ext2_xattr *xa, *xa_tmp;
			TAILQ_FOREACH_SAFE(xa, &xb->xb_list, ex_link, xa_tmp) {
				TAILQ_REMOVE(&xb->xb_list, xa, ex_link);
				ext2_xattr_free(xa);
			}
			kfree_type(struct ext2_xattr_bucket, xb);
			return;
		}
		pp = &(*pp)->xb_next;
	}
}

static void
ext2_xattr_free_all(struct ext2_mount *em)
{
	for (uint32_t i = 0; i < EXT2_HASH_SIZE; i++) {
		struct ext2_xattr_bucket *xb;
		while ((xb = em->em_xattr_hash[i]) != NULL) {
			em->em_xattr_hash[i] = xb->xb_next;
			struct ext2_xattr *xa, *xa_tmp;
			TAILQ_FOREACH_SAFE(xa, &xb->xb_list, ex_link, xa_tmp) {
				TAILQ_REMOVE(&xb->xb_list, xa, ex_link);
				ext2_xattr_free(xa);
			}
			kfree_type(struct ext2_xattr_bucket, xb);
		}
	}
}

static struct ext2_xattr *
ext2_xattr_find(struct ext2_node *node, const char *name)
{
	struct ext2_xattr_bucket *xb = ext2_xattr_bucket_find(node->en_mount, node->en_ino, false);
	if (xb == NULL) {
		return NULL;
	}
	struct ext2_xattr *xa;
	size_t len = strlen(name);

	TAILQ_FOREACH(xa, &xb->xb_list, ex_link) {
		if (xa->ex_namelen == len && memcmp(xa->ex_name, name, len) == 0) {
			return xa;
		}
	}
	return NULL;
}

static int
ext2_getxattr(struct vnop_getxattr_args *ap)
{
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = node->en_mount;
	struct ext2_xattr *xa;
	void *copy = NULL;
	size_t size = 0;
	int error = 0;

	lck_mtx_lock(&em->em_lock);
	xa = ext2_xattr_find(node, ap->a_name);
	if (xa == NULL) {
		error = ENOATTR;
	} else {
		size = xa->ex_size;
		if (ap->a_uio != NULL) {
			if ((size_t)uio_resid(ap->a_uio) < size) {
				error = ERANGE;
			} else if (size != 0) {
				copy = kalloc_data(size, Z_NOWAIT);
				if (copy == NULL) {
					error = ENOMEM;
				} else {
					memcpy(copy, xa->ex_data, size);
				}
			}
		}
	}
	lck_mtx_unlock(&em->em_lock);

	if (!error) {
		if (ap->a_uio == NULL) {
			*ap->a_size = size;
		} else if (copy != NULL) {
			error = uiomove(copy, (int)size, ap->a_uio);
		}
	}
	if (copy != NULL) {
		kfree_data(copy, size);
	}
	return error;
}

static int
ext2_setxattr(struct vnop_setxattr_args *ap)
{
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = node->en_mount;
	struct ext2_xattr *xa, *fresh, *old = NULL;
	size_t size = (size_t)uio_resid(ap->a_uio);
	size_t namelen = strlen(ap->a_name);
	void *data = NULL;
	int error = 0;

	if (size > EXT2_MAX_XATTR || namelen > XATTR_MAXNAMELEN) {
		return E2BIG;
	}
	if (size != 0) {
		data = kalloc_data(size, Z_WAITOK);
		if (data == NULL) {
			return ENOMEM;
		}
		error = uiomove(data, (int)size, ap->a_uio);
		if (error) {
			kfree_data(data, size);
			return error;
		}
	}
	fresh = kalloc_type(struct ext2_xattr, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	fresh->ex_name = kalloc_data(namelen + 1, Z_WAITOK | Z_NOFAIL);
	memcpy(fresh->ex_name, ap->a_name, namelen + 1);
	fresh->ex_namelen = namelen;
	fresh->ex_data = data;
	fresh->ex_size = size;

	lck_mtx_lock(&em->em_lock);
	struct ext2_xattr_bucket *xb = ext2_xattr_bucket_find(em, node->en_ino, true);
	if (xb == NULL) {
		error = ENOMEM;
	} else {
		xa = ext2_xattr_find(node, ap->a_name);
		if (xa != NULL && (ap->a_options & XATTR_CREATE)) {
			error = EEXIST;
		} else if (xa == NULL && (ap->a_options & XATTR_REPLACE)) {
			error = ENOATTR;
		} else {
			if (xa != NULL) {
				TAILQ_REMOVE(&xb->xb_list, xa, ex_link);
				old = xa;
			}
			TAILQ_INSERT_TAIL(&xb->xb_list, fresh, ex_link);
			fresh = NULL;
			node->en_di.i_ctime = (uint32_t)ext2_now_sec();
			node->en_dirty = true;
		}
	}
	lck_mtx_unlock(&em->em_lock);

	if (fresh != NULL) {
		ext2_xattr_free(fresh);
	}
	if (old != NULL) {
		ext2_xattr_free(old);
	}
	return error;
}

static int
ext2_removexattr(struct vnop_removexattr_args *ap)
{
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = node->en_mount;
	struct ext2_xattr *xa;

	lck_mtx_lock(&em->em_lock);
	struct ext2_xattr_bucket *xb = ext2_xattr_bucket_find(em, node->en_ino, false);
	xa = ext2_xattr_find(node, ap->a_name);
	if (xa != NULL && xb != NULL) {
		TAILQ_REMOVE(&xb->xb_list, xa, ex_link);
		node->en_di.i_ctime = (uint32_t)ext2_now_sec();
		node->en_dirty = true;
	}
	lck_mtx_unlock(&em->em_lock);

	if (xa == NULL) {
		return ENOATTR;
	}
	ext2_xattr_free(xa);
	return 0;
}

static int
ext2_listxattr(struct vnop_listxattr_args *ap)
{
	struct ext2_node *node = EXT2_NODE(ap->a_vp);
	struct ext2_mount *em = node->en_mount;
	struct ext2_xattr *xa;
	char *copy = NULL;
	size_t total = 0, at = 0;
	int error = 0;

	lck_mtx_lock(&em->em_lock);
	struct ext2_xattr_bucket *xb = ext2_xattr_bucket_find(em, node->en_ino, false);
	if (xb != NULL) {
		TAILQ_FOREACH(xa, &xb->xb_list, ex_link) {
			total += xa->ex_namelen + 1;
		}
	}
	if (ap->a_uio != NULL && total != 0 && xb != NULL) {
		if ((size_t)uio_resid(ap->a_uio) < total) {
			error = ERANGE;
		} else if ((copy = kalloc_data(total, Z_NOWAIT)) == NULL) {
			error = ENOMEM;
		} else {
			TAILQ_FOREACH(xa, &xb->xb_list, ex_link) {
				memcpy(copy + at, xa->ex_name, xa->ex_namelen + 1);
				at += xa->ex_namelen + 1;
			}
		}
	}
	lck_mtx_unlock(&em->em_lock);

	if (!error) {
		if (ap->a_uio == NULL) {
			*ap->a_size = total;
		} else if (copy != NULL) {
			error = uiomove(copy, (int)total, ap->a_uio);
		}
	}
	if (copy != NULL) {
		kfree_data(copy, total);
	}
	return error;
}

#define VOPFUNC int (*)(void *)

const struct vnodeopv_entry_desc ext2_vnodeop_entries[] = {
	{ .opve_op = &vnop_default_desc,  .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_lookup_desc,   .opve_impl = (VOPFUNC)ext2_lookup },
	{ .opve_op = &vnop_create_desc,   .opve_impl = (VOPFUNC)ext2_create },
	{ .opve_op = &vnop_mknod_desc,    .opve_impl = (VOPFUNC)ext2_mknod },
	{ .opve_op = &vnop_open_desc,     .opve_impl = (VOPFUNC)ext2_open },
	{ .opve_op = &vnop_close_desc,    .opve_impl = (VOPFUNC)ext2_close },
	{ .opve_op = &vnop_getattr_desc,  .opve_impl = (VOPFUNC)ext2_getattr },
	{ .opve_op = &vnop_setattr_desc,  .opve_impl = (VOPFUNC)ext2_setattr },
	{ .opve_op = &vnop_read_desc,     .opve_impl = (VOPFUNC)ext2_read },
	{ .opve_op = &vnop_write_desc,    .opve_impl = (VOPFUNC)ext2_write },
	{ .opve_op = &vnop_ioctl_desc,    .opve_impl = (VOPFUNC)ext2_ioctl },
	{ .opve_op = &vnop_fsync_desc,    .opve_impl = (VOPFUNC)ext2_fsync },
	{ .opve_op = &vnop_remove_desc,   .opve_impl = (VOPFUNC)ext2_remove },
	{ .opve_op = &vnop_link_desc,     .opve_impl = (VOPFUNC)ext2_link },
	{ .opve_op = &vnop_rename_desc,   .opve_impl = (VOPFUNC)ext2_rename },
	{ .opve_op = &vnop_mkdir_desc,    .opve_impl = (VOPFUNC)ext2_mkdir },
	{ .opve_op = &vnop_rmdir_desc,    .opve_impl = (VOPFUNC)ext2_rmdir },
	{ .opve_op = &vnop_symlink_desc,  .opve_impl = (VOPFUNC)ext2_symlink },
	{ .opve_op = &vnop_readdir_desc,  .opve_impl = (VOPFUNC)ext2_readdir },
	{ .opve_op = &vnop_readlink_desc, .opve_impl = (VOPFUNC)ext2_readlink },
	{ .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)ext2_inactive },
	{ .opve_op = &vnop_reclaim_desc,  .opve_impl = (VOPFUNC)ext2_reclaim },
	{ .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)ext2_pathconf },
	{ .opve_op = &vnop_mmap_desc,     .opve_impl = (VOPFUNC)ext2_mmap },
	{ .opve_op = &vnop_mnomap_desc,   .opve_impl = (VOPFUNC)ext2_mnomap },
	{ .opve_op = &vnop_pagein_desc,   .opve_impl = (VOPFUNC)ext2_pagein },
	{ .opve_op = &vnop_pageout_desc,  .opve_impl = (VOPFUNC)ext2_pageout },
	{ .opve_op = &vnop_blktooff_desc, .opve_impl = (VOPFUNC)ext2_blktooff },
	{ .opve_op = &vnop_offtoblk_desc, .opve_impl = (VOPFUNC)ext2_offtoblk },
	{ .opve_op = &vnop_getxattr_desc, .opve_impl = (VOPFUNC)ext2_getxattr },
	{ .opve_op = &vnop_setxattr_desc, .opve_impl = (VOPFUNC)ext2_setxattr },
	{ .opve_op = &vnop_removexattr_desc, .opve_impl = (VOPFUNC)ext2_removexattr },
	{ .opve_op = &vnop_listxattr_desc, .opve_impl = (VOPFUNC)ext2_listxattr },
	{ .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};

const struct vnodeopv_desc ext2_vnodeop_opv_desc = {
	.opv_desc_vector_p = &ext2_vnodeop_p,
	.opv_desc_ops = ext2_vnodeop_entries
};

/* ---- VFS Operations ---- */

static void
ext2_sync_thread(void *arg, __unused wait_result_t result)
{
	struct ext2_mount *em = (struct ext2_mount *)arg;

	lck_mtx_lock(&em->em_lock);
	while (!em->em_sync_stop) {
		uint64_t deadline;

		if (em->em_bcache_dirty > 0 || em->em_sb_dirty || em->em_gd_dirty) {
			(void)ext2_bcache_flush_all(em);
			(void)ext2_flush_meta(em);
		}
		clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
		(void)lck_mtx_sleep_deadline(&em->em_lock, LCK_SLEEP_DEFAULT, (event_t)&em->em_sync_stop, THREAD_UNINT, deadline);
	}
	em->em_sync_done = true;
	wakeup(&em->em_sync_done);
	lck_mtx_unlock(&em->em_lock);
}

static int
ext2_mount(mount_t mp, __unused vnode_t devvp, user_addr_t data, vfs_context_t ctx)
{
	char *path;
	vnode_t vp = NULLVP;
	struct ext2_mount *em = NULL;
	struct vfsstatfs *sp;
	uint8_t *blk0 = NULL;
	size_t len = 0;
	int error;

	if (vfs_isupdate(mp)) {
		return 0;
	}
	if (data == USER_ADDR_NULL) {
		return EINVAL;
	}
	path = kalloc_data(MAXPATHLEN, Z_WAITOK);
	if (path == NULL) {
		return ENOMEM;
	}
	if (vfs_iskernelmount(mp)) {
		error = (strlcpy(path, CAST_DOWN(char *, data), MAXPATHLEN) < MAXPATHLEN) ? 0 : ENAMETOOLONG;
	} else {
		error = copyinstr(data, path, MAXPATHLEN, &len);
	}
	if (!error) {
		error = vnode_lookup(path, 0, &vp, ctx);
	}
	if (!error && !vnode_isblk(vp)) {
		error = ENOTBLK;
	}
	if (!error) {
		error = VNOP_OPEN(vp, FREAD | FWRITE, ctx);
	}
	if (error) {
		if (vp != NULLVP) {
			vnode_put(vp);
		}
		kfree_data(path, MAXPATHLEN);
		return error;
	}

	em = kalloc_type(struct ext2_mount, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	em->em_mp = mp;
	em->em_devvp = vp;
	em->em_devblock = 512;
	lck_mtx_init(&em->em_lock, &ext2_lck_grp, LCK_ATTR_NULL);
	TAILQ_INIT(&em->em_bcache_lru);

	blk0 = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
	error = ext2_dev_io(em, 0, blk0, EXT2_BLK_SIZE, false);
	if (!error) {
		memcpy(&em->em_sb, blk0 + 1024, sizeof(em->em_sb));
		if (em->em_sb.s_magic != EXT2_MAGIC || em->em_sb.s_log_block_size != 2 ||
		    em->em_sb.s_blocks_per_group == 0 || em->em_sb.s_inodes_per_group == 0) {
			printf("ext2: invalid superblock (magic=%#x log_bs=%u)\n",
			    em->em_sb.s_magic, em->em_sb.s_log_block_size);
			error = EINVAL;
		}
	}
	kfree_data(blk0, EXT2_BLK_SIZE);

	if (!error) {
		em->em_inode_size = em->em_sb.s_inode_size ? em->em_sb.s_inode_size : 128;
		em->em_ngroups = (em->em_sb.s_blocks_count - em->em_sb.s_first_data_block +
		    em->em_sb.s_blocks_per_group - 1) / em->em_sb.s_blocks_per_group;
		uint32_t gd_bytes = em->em_ngroups * (uint32_t)sizeof(struct ext2_group_desc);
		em->em_gd_blocks = (gd_bytes + EXT2_BLK_SIZE - 1) / EXT2_BLK_SIZE;
		em->em_gd = kalloc_data((size_t)em->em_gd_blocks * EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
		em->em_bcache_hash = kalloc_data((size_t)EXT2_BCACHE_HASH * sizeof(struct ext2_bcache_entry *), Z_WAITOK | Z_ZERO);
		em->em_io_buf = kalloc_data((size_t)EXT2_IO_BATCH * EXT2_BLK_SIZE, Z_WAITOK);
		em->em_ra_buf = kalloc_data((size_t)EXT2_RA_BLOCKS * EXT2_BLK_SIZE, Z_WAITOK);
		em->em_grp_hint = kalloc_data((size_t)em->em_ngroups * sizeof(uint16_t), Z_WAITOK | Z_ZERO | Z_NOFAIL);
		em->em_bmap_buf = kalloc_data(EXT2_BLK_SIZE, Z_WAITOK | Z_NOFAIL);
		if (em->em_bcache_hash == NULL || em->em_io_buf == NULL || em->em_ra_buf == NULL) {
			error = ENOMEM;
		} else {
			error = ext2_dev_io(em, EXT2_BLK_SIZE, em->em_gd,
			    (size_t)em->em_gd_blocks * EXT2_BLK_SIZE, false);
		}
	}
	if (error) {
		if (em->em_gd != NULL) {
			kfree_data(em->em_gd, (size_t)em->em_gd_blocks * EXT2_BLK_SIZE);
		}
		if (em->em_bcache_hash != NULL) {
			kfree_data(em->em_bcache_hash, (size_t)EXT2_BCACHE_HASH * sizeof(struct ext2_bcache_entry *));
		}
		if (em->em_io_buf != NULL) {
			kfree_data(em->em_io_buf, (size_t)EXT2_IO_BATCH * EXT2_BLK_SIZE);
		}
		if (em->em_ra_buf != NULL) {
			kfree_data(em->em_ra_buf, (size_t)EXT2_RA_BLOCKS * EXT2_BLK_SIZE);
		}
		if (em->em_grp_hint != NULL) {
			kfree_data(em->em_grp_hint, (size_t)em->em_ngroups * sizeof(uint16_t));
		}
		if (em->em_bmap_buf != NULL) {
			kfree_data(em->em_bmap_buf, EXT2_BLK_SIZE);
		}
		lck_mtx_destroy(&em->em_lock, &ext2_lck_grp);
		kfree_type(struct ext2_mount, em);
		VNOP_CLOSE(vp, FREAD | FWRITE, ctx);
		vnode_put(vp);
		kfree_data(path, MAXPATHLEN);
		return error;
	}

	vfs_setfsprivate(mp, em);
	vfs_setlocklocal(mp);
	vfs_setextendedsecurity(mp);
	vfs_getnewfsid(mp);

	sp = vfs_statfs(mp);
	sp->f_bsize = EXT2_BLK_SIZE;
	sp->f_iosize = EXT2_BLK_SIZE;
	sp->f_blocks = em->em_sb.s_blocks_count;
	sp->f_bfree = em->em_sb.s_free_blocks_count;
	sp->f_bavail = em->em_sb.s_free_blocks_count;
	sp->f_bused = em->em_sb.s_blocks_count - em->em_sb.s_free_blocks_count;
	sp->f_files = em->em_sb.s_inodes_count;
	sp->f_ffree = em->em_sb.s_free_inodes_count;
	strlcpy(sp->f_mntfromname, path, sizeof(sp->f_mntfromname));
	kfree_data(path, MAXPATHLEN);

	printf("ext2: mounted %s (%llu MB, %u groups, %u free blocks)\n",
	    sp->f_mntfromname, ((uint64_t)em->em_sb.s_blocks_count * EXT2_BLK_SIZE) >> 20,
	    em->em_ngroups, em->em_sb.s_free_blocks_count);
	{
		thread_t sth = NULL;
		if (kernel_thread_start(ext2_sync_thread, em, &sth) == KERN_SUCCESS) {
			em->em_sync_running = true;
			thread_deallocate(sth);
		}
	}
	return 0;
}

static int
ext2_unmount(mount_t mp, int mntflags, vfs_context_t ctx)
{
	struct ext2_mount *em = EXT2_MP(mp);
	int error;

	error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error) {
		return error;
	}
	lck_mtx_lock(&em->em_lock);
	if (em->em_sync_running) {
		em->em_sync_stop = true;
		wakeup(&em->em_sync_stop);
		while (!em->em_sync_done) {
			(void)lck_mtx_sleep(&em->em_lock, LCK_SLEEP_DEFAULT, (event_t)&em->em_sync_done, THREAD_UNINT);
		}
	}
	(void)ext2_bcache_flush_all(em);
	(void)ext2_flush_meta(em);
	ext2_bcache_free_all(em);
	ext2_xattr_free_all(em);
	lck_mtx_unlock(&em->em_lock);

	VNOP_CLOSE(em->em_devvp, FREAD | FWRITE, ctx);
	vnode_put(em->em_devvp);
	kfree_data(em->em_gd, (size_t)em->em_gd_blocks * EXT2_BLK_SIZE);
	kfree_data(em->em_bcache_hash, (size_t)EXT2_BCACHE_HASH * sizeof(struct ext2_bcache_entry *));
	kfree_data(em->em_io_buf, (size_t)EXT2_IO_BATCH * EXT2_BLK_SIZE);
	kfree_data(em->em_ra_buf, (size_t)EXT2_RA_BLOCKS * EXT2_BLK_SIZE);
	kfree_data(em->em_grp_hint, (size_t)em->em_ngroups * sizeof(uint16_t));
	kfree_data(em->em_bmap_buf, EXT2_BLK_SIZE);
	lck_mtx_destroy(&em->em_lock, &ext2_lck_grp);
	kfree_type(struct ext2_mount, em);
	vfs_setfsprivate(mp, NULL);
	return 0;
}

static int
ext2_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct ext2_mount *em = EXT2_MP(mp);
	lck_mtx_lock(&em->em_lock);
	int error = ext2_vget_locked(em, EXT2_ROOT_INO, EXT2_ROOT_INO, vpp);
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_vfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct ext2_mount *em = EXT2_MP(mp);
	lck_mtx_lock(&em->em_lock);
	int error = ext2_vget_locked(em, (uint32_t)ino, 0, vpp);
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct ext2_mount *em = EXT2_MP(mp);

	lck_mtx_lock(&em->em_lock);
	uint64_t total_b = em->em_sb.s_blocks_count;
	uint64_t free_b = em->em_sb.s_free_blocks_count;
	uint64_t total_i = em->em_sb.s_inodes_count;
	uint64_t free_i = em->em_sb.s_free_inodes_count;
	struct vfsstatfs *sp = vfs_statfs(mp);
	sp->f_bfree = free_b;
	sp->f_bavail = free_b;
	sp->f_bused = total_b - free_b;
	sp->f_ffree = free_i;
	lck_mtx_unlock(&em->em_lock);

	VFSATTR_RETURN(fsap, f_objcount, total_i - free_i);
	VFSATTR_RETURN(fsap, f_bsize, EXT2_BLK_SIZE);
	VFSATTR_RETURN(fsap, f_iosize, EXT2_BLK_SIZE);
	VFSATTR_RETURN(fsap, f_blocks, total_b);
	VFSATTR_RETURN(fsap, f_bfree, free_b);
	VFSATTR_RETURN(fsap, f_bavail, free_b);
	VFSATTR_RETURN(fsap, f_bused, total_b - free_b);
	VFSATTR_RETURN(fsap, f_files, total_i);
	VFSATTR_RETURN(fsap, f_ffree, free_i);

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;
		bzero(cap, sizeof(*cap));
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_SYMBOLICLINKS | VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES | VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING | VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE | VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_PATH_FROM_ID;
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK | VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_SECURITY | VOL_CAP_INT_EXTENDED_ATTR;
		cap->valid[VOL_CAPABILITIES_FORMAT] = cap->capabilities[VOL_CAPABILITIES_FORMAT];
		cap->valid[VOL_CAPABILITIES_INTERFACES] = cap->capabilities[VOL_CAPABILITIES_INTERFACES];
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	VFSATTR_RETURN(fsap, f_filecount, total_i - free_i);
	VFSATTR_RETURN(fsap, f_dircount, 0);
	VFSATTR_RETURN(fsap, f_maxobjcount, total_i);
	VFSATTR_RETURN(fsap, f_owner, 0);
	VFSATTR_RETURN(fsap, f_fssubtype, 0);
	VFSATTR_RETURN(fsap, f_signature, 0x4244);
	VFSATTR_RETURN(fsap, f_carbon_fsid, 0);
	VFSATTR_RETURN(fsap, f_quota, 0);
	VFSATTR_RETURN(fsap, f_reserved, 0);
	if (VFSATTR_IS_ACTIVE(fsap, f_fsid)) {
		fsap->f_fsid = vfs_statfs(mp)->f_fsid;
		VFSATTR_SET_SUPPORTED(fsap, f_fsid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_create_time) || VFSATTR_IS_ACTIVE(fsap, f_modify_time) ||
	    VFSATTR_IS_ACTIVE(fsap, f_access_time) || VFSATTR_IS_ACTIVE(fsap, f_backup_time)) {
		struct timespec since = { .tv_sec = (time_t)em->em_sb.s_wtime, .tv_nsec = 0 };
		VFSATTR_RETURN(fsap, f_create_time, since);
		VFSATTR_RETURN(fsap, f_modify_time, since);
		VFSATTR_RETURN(fsap, f_access_time, since);
		since.tv_sec = 0;
		VFSATTR_RETURN(fsap, f_backup_time, since);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		memcpy(fsap->f_uuid, em->em_sb.s_uuid, sizeof(uuid_t));
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_attributes)) {
		vol_attributes_attr_t *attr = &fsap->f_attributes;
		attr->validattr.commonattr = ATTR_CMN_VALIDMASK;
		attr->validattr.volattr = ATTR_VOL_VALIDMASK;
		attr->validattr.dirattr = ATTR_DIR_VALIDMASK;
		attr->validattr.fileattr = ATTR_FILE_VALIDMASK;
		attr->validattr.forkattr = 0;
		attr->nativeattr = attr->validattr;
		VFSATTR_SET_SUPPORTED(fsap, f_attributes);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		strlcpy(fsap->f_vol_name, "iosdata", MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	return 0;
}

static int
ext2_sync(mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
	struct ext2_mount *em = EXT2_MP(mp);
	lck_mtx_lock(&em->em_lock);
	int error = ext2_bcache_flush_all(em);
	int meta_err = ext2_flush_meta(em);
	if (error == 0) {
		error = meta_err;
	}
	lck_mtx_unlock(&em->em_lock);
	return error;
}

static int
ext2_init(__unused struct vfsconf *vfsc)
{
	return 0;
}

struct vfsops ext2_vfsops = {
	.vfs_mount   = ext2_mount,
	.vfs_unmount = ext2_unmount,
	.vfs_root    = ext2_root,
	.vfs_vget    = ext2_vfs_vget,
	.vfs_getattr = ext2_vfs_getattr,
	.vfs_sync    = ext2_sync,
	.vfs_init    = ext2_init,
};
