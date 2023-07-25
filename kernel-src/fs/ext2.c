#include <kernel/vfs.h>
#include <kernel/alloc.h>
#include <kernel/timekeeper.h>
#include <kernel/slab.h>
#include <hashtable.h>
#include <mutex.h>
#include <logging.h>
#include <util.h>

#define INODE_ROOT 2

#define SUPERBLOCK_OFFSET 1024
#define SB_SIGNATURE 0xef53
#define SB_STATE_CLEAN 1
#define SB_STATE_ERROR 2
#define SB_ERRORACTION_IGNORE 1
#define SB_ERRORACTION_READONLY 2
#define SB_ERRORACTION_PANIC 3

typedef struct {
	uint32_t inodecount;
	uint32_t blockcount;
	uint32_t reservedblocks;
	uint32_t unallocatedblocks;
	uint32_t unallocatedinodes;
	uint32_t superblockstart;
	uint32_t blocksize;
	uint32_t fragmentsize;
	uint32_t blockspergroup;
	uint32_t fragmentspergroup;
	uint32_t inodespergroup;
	uint32_t timeoflastmount;
	uint32_t timeoflastwrite;
	uint16_t mountsaftercheck;
	uint16_t maxmountsbeforecheck;
	uint16_t signature;
	uint16_t state;
	uint16_t erroraction;
	uint16_t versionminor;
	uint32_t checktime;
	uint32_t maxcheckinterval;
	uint32_t osid;
	uint32_t versionmajor;
	uint16_t reserveduid;
	uint16_t reservedgid;
	// version >= 1
	uint32_t firstusableinode;
	uint16_t inodesize;
	uint16_t blockgroup;
	uint32_t optionalfeatures;
	uint32_t requiredfeatures;
	uint32_t readonlyfeatures;
	uint8_t  fsid[16];
	char     volumename[16];
	char     volumepath[64];
	uint32_t compression;
	uint8_t  fileblockpreallocation;
	uint8_t  dirblockpreallocation;
	uint16_t unused;
	uint32_t journalinode;
	uint32_t journaldevice;
	uint32_t orphanlist;
} __attribute__((packed)) ext2superblock_t;

typedef struct {
	uint32_t blockbitmap;
	uint32_t inodebitmap;
	uint32_t inodetable;
	uint16_t freeblocks;
	uint16_t freeinodes;
	uint16_t dircount;
	uint8_t  reserved[14];
} __attribute__((packed)) blockgroupdesc_t;

typedef struct {
	uint32_t inode;
	uint16_t size;
	uint8_t namelen;
	uint8_t type;
	char name[];
} __attribute__((packed)) ext2dent_t;

typedef struct {
	uint16_t typeperm;
	uint16_t uid;
	uint32_t sizelow;
	uint32_t atime;
	uint32_t ctime;
	uint32_t mtime;
	uint32_t dtime;
	uint16_t gid;
	uint16_t links;
	uint32_t sectcount; // in 512 byte blocks
	uint32_t flags;
	uint32_t osvalue;
	uint32_t directpointer[12];
	uint32_t singlypointer;
	uint32_t doublypointer;
	uint32_t triplypointer;
	uint32_t generation;
	uint32_t fileacl;
	uint32_t sizehigh;
	uint32_t fragaddress;
	uint8_t  osvalue2[12];
} __attribute__((packed)) inode_t;

#define INODE_TYPE_FIFO 1
#define INODE_TYPE_CHDEV 2
#define INODE_TYPE_DIR 4
#define INODE_TYPE_BLKDEV 6
#define INODE_TYPE_REGULAR 8
#define INODE_TYPE_SYMLINK 0xa
#define INODE_TYPE_SOCKET 0xc

#define INODE_TYPEPERM_SETTYPE(typeperm, x) typeperm = ((typeperm) & ~0xf000) | (((x) & 0xf) << 12)
#define INODE_TYPEPERM_SETPERM(typeperm, x) typeperm = ((typeperm) & ~0x0fff) | ((x) & 0xfff)
#define INODE_TYPEPERM_PERM(x) ((x) & 0xfff)
#define INODE_TYPEPERM_TYPE(x) (((x) >> 12) & 0xf)
#define INODE_SIZE(x) (INODE_TYPEPERM_TYPE((x)->typeperm) == INODE_TYPE_DIR ? (x)->sizelow : (((uint64_t)(x)->sizehigh << 32) | (uint64_t)(x)->sizelow))
#define INODE_SETSIZE(x, size) { \
	(x)->sizelow = size & 0xffffffff; \
	if (INODE_TYPEPERM_TYPE((x)->typeperm) != INODE_TYPE_DIR) \
		(x)->sizehigh = (size >> 32) & 0xffffffff; \
}

#define INODE_SECTSIZE 512
#define EXT2NODE_INIT(vn, vop, f, t, v, i) \
	VOP_INIT(&(vn)->vnode, vop, f, t, v); \
	(vn)->id = i;

typedef struct {
	vnode_t vnode;
	inode_t inode;
	int id;
} ext2node_t;

typedef struct {
	vfs_t vfs;
	ext2superblock_t superblock;
	vnode_t *backing;
	size_t blocksize;
	size_t bgcount;
	ext2node_t *root;
	hashtable_t inodetable; // hashtable of in memory inodes (indexed with inode id)
	uintmax_t lowestfreeinodebg;
	uintmax_t lowestfreeblockbg;
	mutex_t rootlock; // protects the root variable
	mutex_t inodetablelock; // protects the inodetable hashtable
	mutex_t superblocklock; // protects the superblock
	mutex_t descriptorlock; // protects the block group descriptor table and related structures like the bitmaps. also protects the lowestfree*bg variables
	mutex_t inodewritelock; // protects on disk inode tables
} ext2fs_t;

typedef uint32_t blockptr_t;

#define GROUP_GETINODE(fs, x) ((x) * (fs)->superblock.inodespergroup + 1)
#define GROUP_GETBLOCK(fs, x) ((x) * (fs)->superblock.blockspergroup + (fs)->superblock.superblockstart)
#define BLOCK_GETDISKOFFSET(fs, x) ((fs)->blocksize * (x))
#define BLOCK_GETGROUP(fs, x) (((x) - (fs)->superblock.superblockstart) / (fs)->superblock.blockspergroup)
#define BLOCK_GETINDEX(fs, x) (((x) - (fs)->superblock.superblockstart) % (fs)->superblock.blockspergroup)
#define INODE_GETGROUP(fs, x) (((x) - 1) / (fs)->superblock.inodespergroup)
#define INODE_GETINDEX(fs, x) (((x) - 1) % (fs)->superblock.inodespergroup)
#define INODE_GETDISKOFFSET(fs, table, x) ((table) + INODE_GETINDEX(fs, x) * (fs)->superblock.inodesize)
#define INODE_SECTSPERBLOCK(fs) ((fs)->blocksize / INODE_SECTSIZE)
#define DESC_GETDISKOFFSET(fs, x) (BLOCK_GETDISKOFFSET(fs, (fs)->superblock.superblockstart + 1) + sizeof(blockgroupdesc_t) * (x))
#define BLOCKS_IN_INDIRECT(fs) ((fs)->blocksize / sizeof(blockptr_t))


static const int ext2tovfstypetable[] = {
	-1, V_TYPE_FIFO, V_TYPE_CHDEV, -1, V_TYPE_DIR, -1, V_TYPE_BLKDEV, -1, V_TYPE_REGULAR, -1, V_TYPE_LINK, -1, V_TYPE_SOCKET
};

static const int ext2denttovfstypetable[] = {
	-1, V_TYPE_REGULAR, V_TYPE_DIR, V_TYPE_CHDEV, V_TYPE_BLKDEV, V_TYPE_FIFO, V_TYPE_SOCKET, V_TYPE_LINK
};

static const int vfstoext2denttypetable[] = {
	[V_TYPE_REGULAR] = 1,
	[V_TYPE_DIR] = 2,
	[V_TYPE_CHDEV] = 3,
	[V_TYPE_BLKDEV] = 4,
	[V_TYPE_FIFO] = 5,
	[V_TYPE_SOCKET] = 6,
	[V_TYPE_LINK] = 7
};

static const int vfstoext2typetable[] = {
	[V_TYPE_FIFO] = 1,
	[V_TYPE_CHDEV] = 2,
	[V_TYPE_DIR] = 4,
	[V_TYPE_BLKDEV] = 6,
	[V_TYPE_REGULAR] = 8,
	[V_TYPE_LINK] = 10,
	[V_TYPE_SOCKET] = 12
};

static vops_t vnops;
static scache_t *nodecache;

static int syncsuperblock(ext2fs_t *fs) {
	size_t tmp;
	return vfs_write(fs->backing, &fs->superblock, sizeof(ext2superblock_t), SUPERBLOCK_OFFSET, &tmp, 0);
}

static int changedircount(ext2fs_t *fs, int bg, int change) {
	MUTEX_ACQUIRE(&fs->descriptorlock, false);
	blockgroupdesc_t desc;
	size_t count;
	int e = vfs_read(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, bg), &count, 0);
	if (e)
		goto cleanup;

	desc.dircount += change;

	e = vfs_write(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, bg), &count, 0);

	cleanup:
	MUTEX_RELEASE(&fs->descriptorlock);
	return e;
}

// allocates either a block or an inode
static int allocatestructure(ext2fs_t *fs, uintmax_t *retid, bool inode) {
	MUTEX_ACQUIRE(&fs->descriptorlock, false);
	uintmax_t bg = inode ? fs->lowestfreeinodebg : fs->lowestfreeblockbg;

	int e = 0;
	blockgroupdesc_t desc;

	// safe to check outside of the superblock lock, as nothing that doesn't already
	// hold the descriptorlock would change this variable
	if (inode ? fs->superblock.unallocatedinodes == 0 : fs->superblock.unallocatedblocks == 0) {
		e = ENOSPC;
		goto cleanup;
	}

	// iterate through block groups to find one with free structures
	for (; bg < fs->bgcount; ++bg) {
		size_t readc;
		e = vfs_read(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, bg), &readc, 0);
		if (e)
			goto cleanup;

		if (inode ? desc.freeinodes : desc.freeblocks)
			break;
	}

	if (inode)
		fs->lowestfreeinodebg = bg;
	else
		fs->lowestfreeblockbg = bg;

	if (bg == fs->bgcount) {
		e = ENOSPC;
		goto cleanup;
	}

	// XXX maybe it shouldn't be assumed this will always be a power of 2?
	size_t bmsize = (inode ? fs->superblock.inodespergroup : fs->superblock.blockspergroup) / 8;
	uint8_t *bm = alloc(bmsize);
	if (bm == NULL) {
		e = ENOMEM;
		goto cleanup;
	}

	// read bitmap into buffer
	size_t readc;
	e = vfs_read(fs->backing, bm, bmsize, BLOCK_GETDISKOFFSET(fs, inode ? desc.inodebitmap : desc.blockbitmap), &readc, 0);
	if (e)
		goto cleanup;

	uintmax_t structurefound = 0;
	int i;
	// find free block in it
	for (i = 0; i < bmsize; ++i) {
		// in ext2, a 1 means an used entry and a 0 means a free entry.
		// this is bad for iterating and finding free structures and getting a free structure using __builtin_ctz
		// therefore we do a logical NOT in it. now 0 means an used entry and 1 means a free entry.
		uint8_t v = ~bm[i];
		if (v) {
			int offset = __builtin_ctz(v);
			bm[i] |= 1 << offset; //set as used
			structurefound = (inode ? GROUP_GETINODE(fs, bg) : GROUP_GETBLOCK(fs, bg)) + i * 8 + offset;
			break;
		}
	}

	__assert(structurefound);

	// sync bitmap back into disk
	size_t writec;
	e = vfs_write(fs->backing, bm, bmsize, BLOCK_GETDISKOFFSET(fs, inode ? desc.inodebitmap : desc.blockbitmap), &writec, 0);
	if (e)
		goto cleanup;

	// update block group desc free structure count
	// TODO if an error happens here, the filesystem should probably be marked to be in an unclean state.
	if (inode)
		desc.freeinodes -= 1;
	else
		desc.freeblocks -= 1;

	e = vfs_write(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, bg), &writec, 0);
	if (e)
		goto cleanup;

	// update lowest free block group if there are no more free structures in the current one
	if ((inode ? desc.freeinodes : desc.freeblocks) == 0) {
		if (inode)
			fs->lowestfreeinodebg += 1;
		else
			fs->lowestfreeblockbg += 1;
	}

	*retid = structurefound;

	// update superblock unallocated structure count
	MUTEX_ACQUIRE(&fs->superblocklock, false);
	if (inode)
		fs->superblock.unallocatedinodes -= 1;
	else
		fs->superblock.unallocatedblocks -= 1;
	e = syncsuperblock(fs);
	MUTEX_RELEASE(&fs->superblocklock);

	cleanup:
	MUTEX_RELEASE(&fs->descriptorlock);
	return e;
}

// frees a block or an inode
static int freestructure(ext2fs_t *fs, uintmax_t id, bool inode) {
	int e = 0;
	int bg = inode ? INODE_GETGROUP(fs, id) : BLOCK_GETGROUP(fs, id);
	MUTEX_ACQUIRE(&fs->descriptorlock, false);

	blockgroupdesc_t desc;
	size_t readc;
	e = vfs_read(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, bg), &readc, 0);
	if (e)
		goto cleanup;

	// XXX maybe it shouldn't be assumed this will always be a power of 2?
	size_t bmsize = (inode ? fs->superblock.inodespergroup : fs->superblock.blockspergroup) / 8;
	uint8_t *bm = alloc(bmsize);
	if (bm == NULL) {
		e = ENOMEM;
		goto cleanup;
	}

	// read bitmap into buffer
	e = vfs_read(fs->backing, bm, bmsize, BLOCK_GETDISKOFFSET(fs, inode ? desc.inodebitmap : desc.blockbitmap), &readc, 0);
	if (e)
		goto cleanup;

	int index = inode ? INODE_GETINDEX(fs, id) : BLOCK_GETINDEX(fs, id);
	int bmoffset = index / 8;
	int bmindex = index % 8;

	// 0 means free
	bm[bmoffset] &= ~(1 << bmindex);

	// sync bitmap back into disk
	size_t writec;
	e = vfs_write(fs->backing, bm, bmsize, BLOCK_GETDISKOFFSET(fs, inode ? desc.inodebitmap : desc.blockbitmap), &writec, 0);
	if (e)
		goto cleanup;

	// update block group desc free structure count
	// TODO if an error happens here, the filesystem should probably be marked to be in an unclean state.
	if (inode)
		desc.freeinodes += 1;
	else
		desc.freeblocks += 1;

	e = vfs_write(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, bg), &writec, 0);
	if (e)
		goto cleanup;

	// update lowest free block group if block group is lower
	if (bg < (inode ? fs->lowestfreeinodebg : fs->lowestfreeblockbg)) {
		if (inode)
			fs->lowestfreeinodebg = bg;
		else
			fs->lowestfreeblockbg = bg;
	}

	// update superblock unallocated structure count
	MUTEX_ACQUIRE(&fs->superblocklock, false);
	if (inode)
		fs->superblock.unallocatedinodes += 1;
	else
		fs->superblock.unallocatedblocks += 1;
	e = syncsuperblock(fs);
	MUTEX_RELEASE(&fs->superblocklock);

	cleanup:
	MUTEX_RELEASE(&fs->descriptorlock);
	return e;
}

static int readinode(ext2fs_t *fs, inode_t *buffer, int inode) {
	// get inode table offset. descriptor lock not held because the position of the table is fixed
	blockgroupdesc_t desc;
	size_t readc;
	int e = vfs_read(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, INODE_GETGROUP(fs, inode)), &readc, 0);
	if (e)
		return e;

	if (readc != sizeof(blockgroupdesc_t))
		return EIO;

	// read inode into buffer. inode table lock not held because not a writing operation and the inode lock is already held
	e = vfs_read(fs->backing, buffer, sizeof(inode_t), INODE_GETDISKOFFSET(fs, BLOCK_GETDISKOFFSET(fs, desc.inodetable), inode), &readc, 0);
	if (e)
		return e;

	if (readc != sizeof(inode_t))
		return EIO;

	return e;
}

static int writeinode(ext2fs_t *fs, inode_t *buffer, int inode) {
	// get inode table offset. descriptor lock not held because the position of the table is fixed
	blockgroupdesc_t desc;
	size_t count;
	int e = vfs_read(fs->backing, &desc, sizeof(blockgroupdesc_t), DESC_GETDISKOFFSET(fs, INODE_GETGROUP(fs, inode)), &count, 0);
	if (e)
		return e;

	if (count != sizeof(blockgroupdesc_t))
		return EIO;

	MUTEX_ACQUIRE(&fs->inodewritelock, false);
	// write inode from buffer
	e = vfs_write(fs->backing, buffer, sizeof(inode_t), INODE_GETDISKOFFSET(fs, BLOCK_GETDISKOFFSET(fs, desc.inodetable), inode), &count, 0);
	MUTEX_RELEASE(&fs->inodewritelock);
	if (e)
		return e;

	if (count != sizeof(inode_t))
		return EIO;

	return e;
}

static int getinodeblock(ext2fs_t *fs, ext2node_t *node, uintmax_t index, blockptr_t *block) {
	// no indirection needed
	if (index < 12) {
		*block = node->inode.directpointer[index];
		return 0;
	}

	index -= 12;

	size_t blocksinindirect = BLOCKS_IN_INDIRECT(fs);
	int singlyidx = index % blocksinindirect;
	uintmax_t singlyoffset = singlyidx * sizeof(blockptr_t);
	int singly = index / blocksinindirect;

	// first singly indirect block
	if (singly == 0) {
		size_t readc;
		return vfs_read(fs->backing, block, sizeof(blockptr_t), BLOCK_GETDISKOFFSET(fs, node->inode.singlypointer) + singlyoffset, &readc, 0);
	}

	singly -= 1;
	int doublyidx = singly % blocksinindirect;
	uintmax_t doublyoffset = doublyidx * sizeof(blockptr_t);
	int doubly = singly / blocksinindirect;

	// first doubly indirect block
	if (doubly == 0) {
		size_t readc;
		blockptr_t singlyptr;
		int e = vfs_read(fs->backing, &singlyptr, sizeof(blockptr_t), BLOCK_GETDISKOFFSET(fs, node->inode.doublypointer) + doublyoffset, &readc, 0);
		if (e)
			return e;
		return vfs_read(fs->backing, block, sizeof(blockptr_t), BLOCK_GETDISKOFFSET(fs, singlyptr) + singlyoffset, &readc, 0);
	}

	doubly -= 1;
	int triplyidx = doubly % blocksinindirect;
	uintmax_t triplyoffset = triplyidx * sizeof(blockptr_t);

	// triply indirect block
	size_t readc;
	blockptr_t doublyptr;
	blockptr_t singlyptr;
	int e = vfs_read(fs->backing, &doublyptr, sizeof(blockptr_t), BLOCK_GETDISKOFFSET(fs, node->inode.triplypointer) + triplyoffset, &readc, 0);
	if (e)
		return e;

	e = vfs_read(fs->backing, &singlyptr, sizeof(blockptr_t), BLOCK_GETDISKOFFSET(fs, doublyptr) + doublyoffset, &readc, 0);
	if (e)
		return e;

	return vfs_read(fs->backing, block, sizeof(blockptr_t), BLOCK_GETDISKOFFSET(fs, singlyptr) + singlyoffset, &readc, 0);
}

static int allocandset(ext2fs_t *fs, ext2node_t *node, uintmax_t setoffset, blockptr_t *newvalue) {
	uintmax_t block = 0;
	int e = allocatestructure(fs, &block, false);
	if (e)
		return e;
	blockptr_t blockptr = block;
	size_t writec;
	e = vfs_write(fs->backing, &blockptr, sizeof(blockptr_t), setoffset, &writec, 0);
	if (e) {
		freestructure(fs, block, false); // TODO in case of error set filesystem as unclean?
		return e;
	}

	node->inode.sectcount += INODE_SECTSPERBLOCK(fs);

	return e;
}

static int inodeallocateindirect(ext2fs_t *fs, ext2node_t *node, int indirect) {
	uintmax_t v;
	int e = allocatestructure(fs, &v, false);
	if (e)
		return e;

	switch (indirect) {
		case 1:
			node->inode.singlypointer = v;
			break;
		case 2:
			node->inode.doublypointer = v;
			break;
		case 3:
			node->inode.triplypointer = v;
			break;
	}

	node->inode.sectcount += INODE_SECTSPERBLOCK(fs);

	return e;
}

static int setinodeblock(ext2fs_t *fs, ext2node_t *node, uintmax_t index, blockptr_t block) {
	// no indirection needed
	size_t blocksinindirect = BLOCKS_IN_INDIRECT(fs);
	bool usedirect = index < 12;
	bool usesingly = false;
	bool usedoubly = false;
	bool usetriply = false;
	uintmax_t singly = -1;
	uintmax_t doubly = -1;
	uintmax_t directindex = index;
	uintmax_t singlyindex = -1;
	uintmax_t doublyindex = -1;
	uintmax_t triplyindex = -1;
	uintmax_t singlyoffset = -1;
	uintmax_t doublyoffset = -1;
	uintmax_t triplyoffset = -1;

	if (usedirect == false) {
		index -= 12;
		usesingly = true;
		singly = index / blocksinindirect;
		singlyindex = index % blocksinindirect;
		singlyoffset = singlyindex * sizeof(blockptr_t);
	}

	if (usesingly && singly > 0) {
		singly -= 1;
		usedoubly = true;
		doubly = singly / blocksinindirect;
		doublyindex = singly % blocksinindirect;
		doublyoffset = doublyindex * sizeof(blockptr_t);
	}

	if (usedoubly && doubly > 0) {
		doubly -= 1;
		usetriply = true;
		triplyindex = doubly % blocksinindirect;
		triplyoffset = triplyindex * sizeof(blockptr_t);
	}

	blockptr_t oldblock = 0;

	if (usedirect) {
		oldblock = node->inode.directpointer[directindex];
		node->inode.directpointer[directindex] = block;
	} else {
		size_t count;
		blockptr_t doublyptr;
		blockptr_t singlyptr;

		int e = 0;

		// get doublyptr
		if (usetriply && node->inode.triplypointer == 0) {
			// allocate triply and doubly
			e = inodeallocateindirect(fs, node, 3);
			if (e)
				return e;

			e = allocandset(fs, node, BLOCK_GETDISKOFFSET(fs, node->inode.triplypointer) + triplyoffset, &doublyptr);
			if (e)
				return e;
		} else if (usetriply) {
			// use already allocated triply
			uintmax_t offset = BLOCK_GETDISKOFFSET(fs, node->inode.triplypointer) + triplyoffset;
			e = vfs_read(fs->backing, &doublyptr, sizeof(blockptr_t), offset, &count, 0);
			if (e)
				return e;

			// allocate doubly if needed
			if (doublyptr == 0)
				e = allocandset(fs, node, offset, &doublyptr);
			if (e)
				return e;
		} else {
			// no triply used, allocate doubly if needed
			if (node->inode.doublypointer == 0)
				e = inodeallocateindirect(fs, node, 2);
			if (e)
				return e;

			doublyptr = node->inode.doublypointer;
		}

		// doubly is guaranteed to be allocated
		// get singlyptr
		if (usedoubly) {
			uintmax_t offset = BLOCK_GETDISKOFFSET(fs, doublyptr) + doublyoffset;
			e = vfs_read(fs->backing, &singlyptr, sizeof(blockptr_t), offset, &count, 0);
			if (e)
				return e;

			// allocate singly if needed
			if (singlyptr == 0)
				e = allocandset(fs, node, offset, &singlyptr);
			if (e)
				return e;
		} else {
			// no doubly used, allocate singly if needed
			if (node->inode.singlypointer == 0)
				e = inodeallocateindirect(fs, node, 1);
			if (e)
				return e;

			singlyptr = node->inode.singlypointer;
		}

		// singly guaranteed to be allocated past this point
		// read old block and set new block
		uintmax_t offset = BLOCK_GETDISKOFFSET(fs, singlyptr) + singlyoffset;
		e = vfs_read(fs->backing, &oldblock, sizeof(blockptr_t), offset, &count, 0);
		if (e)
			return e;
		e = vfs_write(fs->backing, &block, sizeof(blockptr_t), offset, &count, 0);
		if (e)
			return e;
	}

	if (oldblock) {
		node->inode.sectcount -= INODE_SECTSPERBLOCK(fs);
		freestructure(fs, oldblock, false); // TODO set as unclean in case of error?
	}

	if (block)
		node->inode.sectcount += INODE_SECTSPERBLOCK(fs);

	return writeinode(fs, &node->inode, node->id); // TODO set fs as unclean if an error happened here?
}

static int resizeinode(ext2fs_t *fs, ext2node_t *node, size_t newsize) {
	size_t newblockcount = ROUND_UP(newsize, fs->blocksize) / fs->blocksize;
	size_t currentblockcount = ROUND_UP(INODE_SIZE(&node->inode), fs->blocksize) / fs->blocksize;

	if (newblockcount > currentblockcount) {
		// grow
		for (int i = currentblockcount; i < newblockcount; ++i) {
			uintmax_t newblock;
			int e = allocatestructure(fs, &newblock, false);
			if (e)
				return e;
			e = setinodeblock(fs, node, i, newblock);
			if (e)
				return e; // TODO set as unclean?
		}
	} else if (newblockcount < currentblockcount) {
		// shrink
		for (int i = newblockcount; i < currentblockcount; ++i) {
			int e = setinodeblock(fs, node, i, 0);
			if (e)
				return e;
		}
	}

	INODE_SETSIZE(&node->inode, newsize);

	writeinode(fs, &node->inode, node->id); // TODO if error mark fs as unclean
	return 0;
}

static int rwblocks(ext2fs_t *fs, ext2node_t *node, void *buffer, size_t count, uintmax_t index, bool write) {
	for (uintmax_t i = 0; i < count; ++i) {
		void *bufferp = (void *)((uintptr_t)buffer + i * fs->blocksize);
		blockptr_t block;
		int e = getinodeblock(fs, node, index + i, &block);
		if (e)
			return e;

		size_t donecount;
		e = write ?
			vfs_write(fs->backing, bufferp, fs->blocksize, BLOCK_GETDISKOFFSET(fs, block), &donecount, 0) :
			vfs_read(fs->backing, bufferp, fs->blocksize, BLOCK_GETDISKOFFSET(fs, block), &donecount, 0);

		if (e)
			return e;

		__assert(donecount == fs->blocksize);
	}
	return 0;
}

static int rwblock(ext2fs_t *fs, ext2node_t *node, void *buffer, size_t count, uintmax_t offset, uintmax_t index, bool write) {
	__assert(offset + count <= fs->blocksize);
	blockptr_t block;
	int e = getinodeblock(fs, node, index, &block);
	if (e)
		return e;

	size_t donecount;
	e = write ?
		vfs_write(fs->backing, buffer, count, BLOCK_GETDISKOFFSET(fs, block) + offset, &donecount, 0) :
		vfs_read(fs->backing, buffer, count, BLOCK_GETDISKOFFSET(fs, block) + offset, &donecount, 0);

	if (e)
		return e;

	__assert(donecount == count);
	return e;
}

static int rwbytes(ext2fs_t *fs, ext2node_t *node, void *buffer, size_t count, uintmax_t offset, bool write) {
	uintmax_t index = offset / fs->blocksize;
	uintmax_t startoffset = offset % fs->blocksize;

	// r/w the first block if there's an offset into it
	if (startoffset) {
		size_t blockremaining = fs->blocksize - startoffset;
		size_t docount = min(blockremaining, count);
		int e = rwblock(fs, node, buffer, docount, startoffset, index, write);
		if (e)
			return e;

		buffer = (void *)((uintptr_t)buffer + docount);
		count -= docount;
		index += 1;
	}

	// r/w all middle blocks
	size_t blocks = count / fs->blocksize;
	if (blocks) {
		int e = rwblocks(fs, node, buffer, blocks, index, write);
		if (e)
			return e;

		count -= blocks * fs->blocksize;
		buffer = (void *)((uintptr_t)buffer + blocks * fs->blocksize);
		index += blocks;
	}

	// r/w remaining block if there's any data left and return
	return count ? rwblock(fs, node, buffer, count, 0, index, write) : 0;
}

#define BUFFER_MAP_FLAGS (ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_NOEXEC)

static int findindir(ext2fs_t *fs, ext2node_t *node, char *name, int *inode) {
	// XXX loading the whole dir into memory at once isn't the best idea but works for now
	void *dirbuffer = vmm_map(NULL, INODE_SIZE(&node->inode) + 1, VMM_FLAGS_ALLOCATE, BUFFER_MAP_FLAGS, NULL); // + 1 to make sure there is at least one page mapped
	if (dirbuffer == NULL)
		return ENOMEM;

	int err = rwbytes(fs, node, dirbuffer, INODE_SIZE(&node->inode), 0, false);
	if (err)
		goto cleanup;

	uintmax_t offset = 0;
	int inodefound = 0;
	size_t namelen = strlen(name);

	// iterate through directory to find the inode number
	while (offset < INODE_SIZE(&node->inode)) {
		ext2dent_t *dent = (ext2dent_t *)((uintptr_t)dirbuffer + offset);

		if (namelen == dent->namelen && dent->inode && strncmp(name, dent->name, dent->namelen) == 0)
			inodefound = dent->inode;

		__assert(dent->size);
		offset += dent->size;
	}

	if (inodefound == 0)
		err = ENOENT;
	else
		*inode = inodefound;

	cleanup:
	if (dirbuffer)
		vmm_unmap(dirbuffer, INODE_SIZE(&node->inode) + 1, 0);

	return err;
}

static int insertdent(ext2fs_t *fs, ext2node_t *node, char *name, int inode, int type) {
	size_t namelen = strlen(name);
	size_t entlen = ROUND_UP(sizeof(ext2dent_t) + namelen, 4);
	ext2dent_t *dentbuffer = alloc(entlen);
	if (dentbuffer == NULL)
		return ENOMEM;

	dentbuffer->inode = inode;
	dentbuffer->namelen = namelen;
	dentbuffer->type = type;
	memcpy(dentbuffer->name, name, namelen);

	size_t inodesize = INODE_SIZE(&node->inode);
	// XXX mapping the whole dir into memory isn't the best idea
	void *dirbuffer = vmm_map(NULL, inodesize + 1, VMM_FLAGS_ALLOCATE, BUFFER_MAP_FLAGS, NULL); // + 1 to make sure there is at least one page mapped
	int err = dirbuffer ? 0 : ENOMEM;
	if (err)
		goto cleanup;

	err = rwbytes(fs, node, dirbuffer, inodesize, 0, false);
	if (err)
		goto cleanup;

	ext2dent_t *splitdent = NULL;
	uintmax_t offset = 0;
	size_t truesize, freesize;
	while (offset < inodesize) {
		splitdent = (ext2dent_t *)((uintptr_t)dirbuffer + offset);
		truesize = ROUND_UP(sizeof(ext2dent_t) + splitdent->namelen, 4);
		freesize = splitdent->size - truesize;

		if (entlen <= freesize)
			break;

		__assert(splitdent->size);
		offset += splitdent->size;
	}

	// need to grow dir?
	if (offset >= inodesize) {
		err = resizeinode(fs, node, inodesize + fs->blocksize); // directory size has to be block aligned
		if (err)
			goto cleanup;

		dentbuffer->size = fs->blocksize;
		// TODO if this fails set fs as unclean
		err = rwbytes(fs, node, dentbuffer, dentbuffer->size, inodesize, true);
	} else {
		size_t writesize = splitdent->size;
		splitdent->size = truesize;
		dentbuffer->size = freesize;
		memcpy((void *)((uintptr_t)splitdent + truesize), dentbuffer, entlen);
		err = rwbytes(fs, node, splitdent, writesize, offset, true);
	}

	cleanup:
	if (dirbuffer)
		vmm_unmap(dirbuffer, inodesize + 1, 0);
	free(dentbuffer);
	return err;
}

static int ext2_open(vnode_t **vnode, int flags, cred_t *cred) {
	// TODO device files
	return 0;
}

static int ext2_close(vnode_t *vnode, int flags, cred_t *cred) {
	return 0;
}

static int ext2_getattr(vnode_t *vnode, vattr_t *attr, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)vnode;
	VOP_LOCK(vnode);

	attr->rdevmajor = 0;
	attr->rdevminor = 0;
	attr->uid = node->inode.uid;
	attr->gid = node->inode.gid;
	attr->inode = node->id;
	attr->type = ext2tovfstypetable[INODE_TYPEPERM_TYPE(node->inode.typeperm)];
	attr->mode = INODE_TYPEPERM_PERM(node->inode.typeperm);
	attr->nlinks = node->inode.links;
	attr->size = INODE_SIZE(&node->inode);
	attr->fsblocksize = ((ext2fs_t *)vnode->vfs)->blocksize;
	attr->atime.s = node->inode.atime;
	attr->atime.ns = 0;
	attr->ctime.s = node->inode.ctime;
	attr->ctime.ns = 0;
	attr->mtime.s = node->inode.mtime;
	attr->mtime.ns = 0;
	attr->blocksused = ROUND_UP(attr->size, attr->fsblocksize) / attr->fsblocksize;

	VOP_UNLOCK(vnode);
	return 0;
}

static int ext2_setattr(vnode_t *vnode, vattr_t *attr, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)vnode;
	VOP_LOCK(vnode);

	INODE_TYPEPERM_SETPERM(node->inode.typeperm, attr->mode);
	node->inode.gid = attr->gid;
	node->inode.uid = attr->uid;

	int e = writeinode((ext2fs_t *)vnode->vfs, &node->inode, node->id);
	VOP_UNLOCK(vnode);

	return e;
}

static int ext2_lookup(vnode_t *vnode, char *name, vnode_t **result, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)vnode;
	ext2fs_t *fs = (ext2fs_t *)node->vnode.vfs;
	ext2node_t *newnode = NULL;
	VOP_LOCK(vnode);

	int inode = 0;
	int err = findindir(fs, node, name, &inode);
	if (err)
		goto cleanup;

	MUTEX_ACQUIRE(&fs->inodetablelock, false);

	void *tablev;
	// check if it isn't already in the inode table
	err = hashtable_get(&fs->inodetable, &tablev, &inode, sizeof(inode));
	if (err == ENOENT) {
		// it isn't, allocate new and read inode information
		newnode = slab_allocate(nodecache);
		if (newnode == NULL) {
			err = ENOMEM;
			MUTEX_RELEASE(&fs->inodetablelock);
			goto cleanup;
		}
	
		err = readinode(fs, &newnode->inode, inode);
		if (err)
			goto cleanup;

		EXT2NODE_INIT(newnode, &vnops, 0, ext2tovfstypetable[INODE_TYPEPERM_TYPE(newnode->inode.typeperm)], vnode->vfs, inode);

		// finally add it to the inode table
		err = hashtable_set(&fs->inodetable, newnode, &inode, sizeof(inode), true);
		if (err)
			goto cleanup;

		VOP_HOLD(&newnode->vnode);
	} else if (err) {
		MUTEX_RELEASE(&fs->inodetablelock);
		goto cleanup;
	} else {
		// it is, use the vnode from the table
		newnode = tablev;
	}

	VOP_HOLD(&newnode->vnode);

	MUTEX_RELEASE(&fs->inodetablelock);

	*result = &newnode->vnode;

	cleanup:
	if (err && newnode) {
		vnode_t *tmp = (vnode_t *)newnode;
		VOP_RELEASE(tmp);
	}

	VOP_UNLOCK(vnode);
	return err;
}

static int ext2_getdents(vnode_t *vnode, dent_t *buffer, size_t count, uintmax_t diroffset, size_t *readcount) {
	ext2node_t *node = (ext2node_t *)vnode;
	ext2fs_t *fs = (ext2fs_t *)node->vnode.vfs;

	VOP_LOCK(vnode);

	int err = 0;
	// XXX loading the whole dir into memory at once isn't the best idea but works for now
	void *dirbuffer = vmm_map(NULL, INODE_SIZE(&node->inode) + 1, VMM_FLAGS_ALLOCATE, BUFFER_MAP_FLAGS, NULL); // + 1 to make sure there is at least one page mapped
	if (dirbuffer == NULL) {
		err = ENOMEM;
		goto cleanup;
	}

	err = rwbytes(fs, node, dirbuffer, INODE_SIZE(&node->inode), 0, false);
	if (err)
		goto cleanup;

	uintmax_t currdiroffset = 0;
	uintmax_t offset = 0;
	int i = 0;

	// iterate through directory and build dent entries
	while (offset < INODE_SIZE(&node->inode) && i < count) {
		ext2dent_t *dent = (ext2dent_t *)((uintptr_t)dirbuffer + offset);

		if (dent->inode && currdiroffset++ >= diroffset) {
			buffer[i].d_ino = dent->inode;
			buffer[i].d_off = offset;
			buffer[i].d_reclen = sizeof(dent_t);
			buffer[i].d_type = vfs_getposixtype(ext2denttovfstypetable[dent->type]);
			memcpy(buffer[i].d_name, dent->name, dent->namelen);
			buffer[i].d_name[dent->namelen] = '\0';
			i += 1;
		}

		offset += dent->size;
	}

	*readcount = i;

	cleanup:
	if (dirbuffer)
		vmm_unmap(dirbuffer, INODE_SIZE(&node->inode) + 1, 0);

	VOP_UNLOCK(vnode);
	return err;
}

static int ext2_access(vnode_t *vnode, mode_t mode, cred_t *cred) {
	// TODO permission checks
	return 0;
}

static int ext2_readlink(vnode_t *vnode, char **link, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)vnode;
	ext2fs_t *fs = (ext2fs_t *)vnode->vfs;
	if (vnode->type != V_TYPE_LINK)
		return EINVAL;

	VOP_LOCK(vnode);
	int err = 0;

	size_t linksize = INODE_SIZE(&node->inode);
	char *buf = alloc(linksize + 1);

	// if the length of a symlink is larger than 60 bytes, its stored normally
	// otherwise, its stored in the inode strucutre itself
	if (linksize > 60)
		err = rwbytes(fs, node, buf, linksize, 0, false);
	else
		memcpy(buf, node->inode.directpointer, linksize);

	buf[linksize] = '\0';

	VOP_UNLOCK(vnode);
	if (err)
		free(buf);
	else
		*link = buf;

	return err;
}

static int ext2_read(vnode_t *vnode, void *buffer, size_t size, uintmax_t offset, int flags, size_t *readc, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)vnode;
	ext2fs_t *fs = (ext2fs_t *)vnode->vfs;
	uintmax_t endoffset = offset + size;
	int err = 0;
	VOP_LOCK(vnode);

	size_t inodesize = INODE_SIZE(&node->inode);

	if (offset >= inodesize) {
		*readc = 0;
		goto cleanup;
	}

	// check for overflow
	if (offset > endoffset)
		endoffset = -1ll;

	if (endoffset > inodesize) {
		endoffset = inodesize;
		size = endoffset - offset;
	}

	if (size == 0)
		goto cleanup;

	err = rwbytes(fs, node, buffer, size, offset, false);

	*readc = err ? -1 : size;

	cleanup:
	VOP_UNLOCK(vnode);
	return err;
}

int ext2_write(vnode_t *vnode, void *buffer, size_t size, uintmax_t offset, int flags, size_t *writec, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)vnode;
	ext2fs_t *fs = (ext2fs_t *)vnode->vfs;
	uintmax_t endoffset = offset + size;
	VOP_LOCK(vnode);
	int err = 0;
	size_t inodesize = INODE_SIZE(&node->inode);

	// overflow check
	if (offset > endoffset)
		endoffset = -1ll;

	if (endoffset > inodesize) {
		err = resizeinode(fs, node, endoffset);
		if (err)
			goto cleanup;
	}

	err = rwbytes(fs, node, buffer, size, offset, true);
	*writec = err ? -1 : size;
	if (err)
		goto cleanup;

	// TODO mtime

	cleanup:
	VOP_UNLOCK(vnode);
	return err;
}

int ext2_resize(vnode_t *vnode, size_t newsize, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)vnode;
	ext2fs_t *fs = (ext2fs_t *)vnode->vfs;
	VOP_LOCK(vnode);
	int e = resizeinode(fs, node, newsize);
	VOP_UNLOCK(vnode);
	return e;
}

static int ext2_link(vnode_t *vnode, vnode_t *dirvnode, char *name, cred_t *cred) {
	if (vnode->vfs != dirvnode->vfs)
		return EXDEV;

	ext2node_t *node = (ext2node_t *)vnode;
	ext2node_t *dirnode = (ext2node_t *)dirvnode;
	ext2fs_t *fs = (ext2fs_t *)dirvnode->vfs;
	VOP_LOCK(dirvnode);
	VOP_LOCK(vnode);
	int inode;
	int err = findindir(fs, dirnode, name, &inode);
	if (err == 0)
		err = EEXIST;
	if (err != ENOENT)
		goto cleanup;

	err = insertdent(fs, dirnode, name, node->id, vfstoext2denttypetable[vnode->type]);
	if (err)
		goto cleanup;

	node->inode.links += 1;

	writeinode(fs, &node->inode, node->id); // TODO mark as unclean if error

	cleanup:
	VOP_UNLOCK(vnode);
	VOP_UNLOCK(dirvnode);
	return err;
}

static int ext2_create(vnode_t *parent, char *name, vattr_t *attr, int type, vnode_t **result, cred_t *cred) {
	ext2node_t *node = (ext2node_t *)parent;
	ext2fs_t *fs = (ext2fs_t *)parent->vfs;
	VOP_LOCK(parent);
	ext2node_t *newnode = slab_allocate(nodecache);
	EXT2NODE_INIT(newnode, &vnops, 0, type, parent->vfs, 0);

	int inodetmp;
	int err = findindir(fs, node, name, &inodetmp);
	if (err == 0)
		err = EEXIST;
	if (err != ENOENT)
		goto cleanup;

	uintmax_t id = 0;
	err = allocatestructure(fs, &id, true);
	if (err)
		goto cleanup;

	newnode->id = id;
	memset(&newnode->inode, 0, sizeof(inode_t));
	newnode->inode = (inode_t){
		.typeperm = 0, // TODO
		.uid = attr->uid,
		.atime = timekeeper_time().s,
		.ctime = timekeeper_time().s,
		.mtime = timekeeper_time().s,
		.gid = attr->gid,
		.links = type == V_TYPE_DIR ? 2 : 1 // directories have 2 links at the beggining, the parent link and the . link
	};

	INODE_TYPEPERM_SETTYPE(newnode->inode.typeperm, vfstoext2typetable[type]);
	INODE_TYPEPERM_SETPERM(newnode->inode.typeperm, attr->mode);

	err = writeinode(fs, &newnode->inode, id);
	if (err)
		goto cleanup;

	err = insertdent(fs, node, name, newnode->id, vfstoext2denttypetable[type]);
	if (err)
		goto cleanup;

	MUTEX_ACQUIRE(&fs->inodetablelock, false);
	err = hashtable_set(&fs->inodetable, newnode, &newnode->id, sizeof(newnode->id), true);
	MUTEX_RELEASE(&fs->inodetablelock);
	if (err) // TODO if this fails set fs as unclean (directory entry is still there)
		goto cleanup;

	VOP_HOLD(&newnode->vnode);
	*result = &newnode->vnode;

	// create . and .. entries and increase block group dir count if dir TODO if any error set as unclean
	if (type == V_TYPE_DIR) {
		insertdent(fs, newnode, ".", newnode->id, vfstoext2denttypetable[V_TYPE_DIR]);
		insertdent(fs, newnode, "..", node->id, vfstoext2denttypetable[V_TYPE_DIR]);
		node->inode.links += 1;
		writeinode(fs, &node->inode, node->id);
		changedircount(fs, INODE_GETGROUP(fs, newnode->id), 1);
	}

	cleanup:
	if (err && id)
		freestructure(fs, id, true); // TODO if error set as unclean

	if (err)
		slab_free(nodecache, newnode);

	VOP_UNLOCK(parent);
	return err;
}

static int ext2_symlink(vnode_t *parentvnode, char *name, vattr_t *attr, char *path, cred_t *cred) {
	return ENOSYS;
}

// TODO move root variable to vfs
static int ext2_root(vfs_t *vfs, vnode_t **vnodep) {
	ext2fs_t *fs = (ext2fs_t *)vfs;
	int err = 0;
	MUTEX_ACQUIRE(&fs->rootlock, false);
	if (fs->root) {
		*vnodep = &fs->root->vnode;
		goto leave;
	}

	ext2node_t *node = slab_allocate(nodecache);
	if (node == NULL) {
		err = ENOMEM;
		goto leave;
	}

	err = readinode(fs, &node->inode, INODE_ROOT);
	if (err)
		goto leave;

	node->id = INODE_ROOT;

	err = hashtable_set(&fs->inodetable, node, &node->id, sizeof(node->id), true);
	if (err)
		goto leave;

	EXT2NODE_INIT(node, &vnops, V_FLAGS_ROOT, V_TYPE_DIR, vfs, INODE_ROOT);
	VOP_HOLD(&node->vnode);
	*vnodep = (vnode_t *)node;
	fs->root = node;

	leave:
	MUTEX_RELEASE(&fs->rootlock);
	return err;
}

static vfsops_t vfsops;
static int ext2_mount(vfs_t **vfs, vnode_t *mountpoint, vnode_t *backing, void *data) {
	if (backing == NULL)
		return EINVAL;

	ext2fs_t *fs = alloc(sizeof(ext2fs_t));
	if (fs == NULL)
		return ENOMEM;

	MUTEX_INIT(&fs->superblocklock);
	MUTEX_INIT(&fs->inodetablelock);
	MUTEX_INIT(&fs->rootlock);
	MUTEX_INIT(&fs->inodewritelock);
	MUTEX_INIT(&fs->descriptorlock);

	// read superblock

	size_t readcount;
	int err = vfs_read(backing, &fs->superblock, sizeof(ext2superblock_t), SUPERBLOCK_OFFSET, &readcount, 0);
	if (err)
		goto cleanup;

	// do a bunch of checks to make sure we can mount the filesystem

	err = EINVAL;
	if (readcount != sizeof(ext2superblock_t)) {
		printf("ext2: could not read superblock\n");
		goto cleanup;
	}

	if (fs->superblock.signature != SB_SIGNATURE) {
		printf("ext2: bad signature\n");
		goto cleanup;
	}

	if (fs->superblock.versionmajor == 0) {
		printf("ext2: no support for version 0\n");
		goto cleanup;
	}

	size_t inobgcount = ROUND_UP(fs->superblock.inodecount, fs->superblock.inodespergroup) / fs->superblock.inodespergroup;
	size_t blkbgcount = ROUND_UP(fs->superblock.blockcount, fs->superblock.blockspergroup) / fs->superblock.blockspergroup;

	if (inobgcount != blkbgcount) {
		printf("ext2: block group count from inode count and block count differ\n");
		goto cleanup;
	}

	// TODO features check

	if (fs->superblock.mountsaftercheck == fs->superblock.maxmountsbeforecheck)
		printf("ext2: exceeded the number of mounts allowed before a filesystem check\n");

	if (fs->superblock.maxcheckinterval && timekeeper_time().s > fs->superblock.checktime + fs->superblock.maxcheckinterval)
		printf("ext2: exceeded the time limit allowed between filesystem checks\n");

	// TODO path last mounted to

	err = hashtable_init(&fs->inodetable, 4096);
	if (err)
		goto cleanup;

	VFS_INIT(&fs->vfs, &vfsops, 0);
	fs->superblock.mountsaftercheck += 1;
	fs->superblock.timeoflastmount = timekeeper_time().s;
	fs->backing = backing;
	err = syncsuperblock(fs);

	VOP_HOLD(backing);

	fs->bgcount = inobgcount;
	fs->blocksize = 1024 << fs->superblock.blocksize;
	*vfs = &fs->vfs;
	err = 0;

	cleanup:
	if (err)
		free(fs);

	return err;
}

static vfsops_t vfsops = {
	.mount = ext2_mount,
	.root = ext2_root
};

	/*
	int (*unlink)(vnode_t *node, char *name, cred_t *cred);
	int (*inactive)(vnode_t *node);
	int (*mmap)(vnode_t *node, void *addr, uintmax_t offset, int flags, cred_t *cred);
	int (*munmap)(vnode_t *node, void *addr, uintmax_t offset, int flags, cred_t *cred);
	*/

static vops_t vnops = {
	.write = ext2_write,
	.read = ext2_read,
	.getdents = ext2_getdents,
	.lookup = ext2_lookup,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.open = ext2_open,
	.close = ext2_close,
	.poll = vfs_pollstub,
	.access = ext2_access,
	.readlink = ext2_readlink,
	.resize = ext2_resize,
	.link = ext2_link,
	.create = ext2_create,
	.symlink = ext2_symlink
};

void ext2_init() {
	__assert(vfs_register(&vfsops, "ext2") == 0);
	nodecache = slab_newcache(sizeof(ext2node_t), 0, NULL, NULL);
	__assert(nodecache);
}