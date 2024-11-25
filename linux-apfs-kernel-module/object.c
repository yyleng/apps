// SPDX-License-Identifier: GPL-2.0
/*
 * Checksum routines for an APFS object
 */

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include "apfs.h"

/*
 * Note that this is not a generic implementation of fletcher64, as it assumes
 * a message length that doesn't overflow sum1 and sum2.  This constraint is ok
 * for apfs, though, since the block size is limited to 2^16.  For a more
 * generic optimized implementation, see Nakassis (1988).
 */
static u64 apfs_fletcher64(void *addr, size_t len)
{
	__le32 *buff = addr;
	u64 sum1 = 0;
	u64 sum2 = 0;
	u64 c1, c2;
	int i;

	for (i = 0; i < len/sizeof(u32); i++) {
		sum1 += le32_to_cpu(buff[i]);
		sum2 += sum1;
	}

	c1 = sum1 + sum2;
	c1 = 0xFFFFFFFF - do_div(c1, 0xFFFFFFFF);
	c2 = sum1 + c1;
	c2 = 0xFFFFFFFF - do_div(c2, 0xFFFFFFFF);

	return (c2 << 32) | c1;
}

int apfs_obj_verify_csum(struct super_block *sb, struct apfs_obj_phys *obj)
{
	return  (le64_to_cpu(obj->o_cksum) ==
		 apfs_fletcher64((char *) obj + APFS_MAX_CKSUM_SIZE,
				 sb->s_blocksize - APFS_MAX_CKSUM_SIZE));
}

/**
 * apfs_obj_set_csum - Set the fletcher checksum in an object header
 * @sb:		superblock structure
 * @obj:	the object header
 */
void apfs_obj_set_csum(struct super_block *sb, struct apfs_obj_phys *obj)
{
	u64 cksum = apfs_fletcher64((char *)obj + APFS_MAX_CKSUM_SIZE,
				    sb->s_blocksize - APFS_MAX_CKSUM_SIZE);

	obj->o_cksum = cpu_to_le64(cksum);
}

/**
 * apfs_cpm_lookup_oid - Search a checkpoint-mapping block for a given oid
 * @sb:		superblock structure
 * @cpm:	checkpoint-mapping block (on disk)
 * @oid:	the ephemeral object id to look up
 * @bno:	on return, the block number for the object
 *
 * Returns -EFSCORRUPTED in case of corruption, or -EAGAIN if @oid is not
 * listed in @cpm; returns 0 on success.
 */
static int apfs_cpm_lookup_oid(struct super_block *sb,
			       struct apfs_checkpoint_map_phys *cpm,
			       u64 oid, u64 *bno)
{
	u32 map_count = le32_to_cpu(cpm->cpm_count);
	int i;

	if (map_count > apfs_max_maps_per_block(sb))
		return -EFSCORRUPTED;

	for (i = 0; i < map_count; ++i) {
		struct apfs_checkpoint_mapping *map = &cpm->cpm_map[i];

		if (le64_to_cpu(map->cpm_oid) == oid) {
			*bno = le64_to_cpu(map->cpm_paddr);
			return 0;
		}
	}
	return -EAGAIN; /* The mapping may still be in the next block */
}

/**
 * apfs_read_cpm_block - Read the checkpoint mapping block
 * @sb:	super block structure
 *
 * Only a single cpm block is supported for now. Returns the buffer head for
 * the block on success, or NULL in case of failure.
 */
static struct buffer_head *apfs_read_cpm_block(struct super_block *sb)
{
	struct apfs_nx_superblock *raw_sb = APFS_NXI(sb)->nx_raw;
	u64 desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	u32 desc_index = le32_to_cpu(raw_sb->nx_xp_desc_index);
	u32 desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	u32 desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);
	u64 cpm_bno;

	if (!desc_blks || desc_len < 2)
		return NULL;

	/* Last block in area is superblock; we want the last mapping block */
	cpm_bno = desc_base + (desc_index + desc_len - 2) % desc_blks;
	return apfs_sb_bread(sb, cpm_bno);
}

/**
 * apfs_create_cpoint_map - Create a checkpoint mapping
 * @sb:		filesystem superblock
 * @oid:	ephemeral object id
 * @bno:	block number
 *
 * Only mappings for free queue nodes are supported for now.  Returns 0 on
 * success or a negative error code in case of failure.
 */
int apfs_create_cpoint_map(struct super_block *sb, u64 oid, u64 bno)
{
	struct buffer_head *bh;
	struct apfs_checkpoint_map_phys *cpm;
	struct apfs_checkpoint_mapping *map;
	u32 cpm_count;
	int err = 0;

	bh = apfs_read_cpm_block(sb);
	if (!bh)
		return -EIO;
	cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;
	apfs_assert_in_transaction(sb, &cpm->cpm_o);

	cpm_count = le32_to_cpu(cpm->cpm_count);
	if (cpm_count >= apfs_max_maps_per_block(sb)) { /* TODO */
		apfs_warn(sb, "creation of cpm blocks not yet supported");
		err = -EOPNOTSUPP;
		goto fail;
	}
	map = &cpm->cpm_map[cpm_count];
	le32_add_cpu(&cpm->cpm_count, 1);

	map->cpm_type = cpu_to_le32(APFS_OBJ_EPHEMERAL |
				    APFS_OBJECT_TYPE_BTREE_NODE);
	map->cpm_subtype = cpu_to_le32(APFS_OBJECT_TYPE_SPACEMAN_FREE_QUEUE);
	map->cpm_size = cpu_to_le32(sb->s_blocksize);
	map->cpm_pad = 0;
	map->cpm_fs_oid = 0;
	map->cpm_oid = cpu_to_le64(oid);
	map->cpm_paddr = cpu_to_le64(bno);

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_index_in_data_area - Get position of block in current checkpoint's data
 * @sb:		superblock structure
 * @bno:	block number
 *
 * TODO: reuse this function and apfs_data_index_to_bno(), and do the same for
 * the descriptor area.
 */
static inline u32 apfs_index_in_data_area(struct super_block *sb, u64 bno)
{
	struct apfs_nx_superblock *raw_sb = APFS_NXI(sb)->nx_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_index = le32_to_cpu(raw_sb->nx_xp_data_index);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);

	return (bno - data_base + data_blks - data_index) % data_blks;
}

/**
 * apfs_data_index_to_bno - Convert index in data area to block number
 * @sb:		superblock structure
 * @index:	index of the block in the current checkpoint's data area
 */
static inline u64 apfs_data_index_to_bno(struct super_block *sb, u32 index)
{
	struct apfs_nx_superblock *raw_sb = APFS_NXI(sb)->nx_raw;
	u64 data_base = le64_to_cpu(raw_sb->nx_xp_data_base);
	u32 data_index = le32_to_cpu(raw_sb->nx_xp_data_index);
	u32 data_blks = le32_to_cpu(raw_sb->nx_xp_data_blocks);

	return data_base + (index + data_index) % data_blks;
}

/**
 * apfs_remove_cpoint_map - Remove a checkpoint mapping
 * @sb:		filesystem superblock
 * @bno:	block number to delete
 *
 * Only mappings for free queue nodes are supported for now. Blocks that come
 * after the deleted one are assumed to shift back one place. Returns 0 on
 * success or a negative error code in case of failure.
 */
int apfs_remove_cpoint_map(struct super_block *sb, u64 bno)
{
	struct buffer_head *bh;
	struct apfs_checkpoint_map_phys *cpm;
	struct apfs_checkpoint_mapping *map, *maps_start, *maps_end;
	struct apfs_checkpoint_mapping *bno_map = NULL;
	u32 cpm_count;
	u32 bno_off;
	int err = 0;

	bh = apfs_read_cpm_block(sb);
	if (!bh)
		return -EIO;
	cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;
	apfs_assert_in_transaction(sb, &cpm->cpm_o);

	/* TODO: multiple cpm blocks? */
	cpm_count = le32_to_cpu(cpm->cpm_count);
	if (cpm_count > apfs_max_maps_per_block(sb)) {
		err = -EFSCORRUPTED;
		goto fail;
	}
	maps_start = &cpm->cpm_map[0];
	maps_end = &cpm->cpm_map[cpm_count];

	bno_off = apfs_index_in_data_area(sb, bno);
	for (map = maps_start; map < maps_end; ++map) {
		u32 curr_off;

		if (le64_to_cpu(map->cpm_paddr) == bno)
			bno_map = map;

		curr_off = apfs_index_in_data_area(sb, le64_to_cpu(map->cpm_paddr));
		if (curr_off > bno_off)
			map->cpm_paddr = cpu_to_le64(apfs_data_index_to_bno(sb, curr_off - 1));
	}
	if (!bno_map) {
		err = -EFSCORRUPTED;
		goto fail;
	}
	memmove(bno_map, bno_map + 1, (maps_end - bno_map - 1) * sizeof(*bno_map));
	le32_add_cpu(&cpm->cpm_count, -1);

fail:
	brelse(bh);
	return err;
}

/**
 * apfs_read_ephemeral_object - Find and map an ephemeral object
 * @sb:		superblock structure
 * @oid:	ephemeral object id
 *
 * Returns the mapped buffer head for the object, or an error pointer in case
 * of failure.
 */
struct buffer_head *apfs_read_ephemeral_object(struct super_block *sb, u64 oid)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_nx_superblock *raw_sb = nxi->nx_raw;
	u64 desc_base = le64_to_cpu(raw_sb->nx_xp_desc_base);
	u32 desc_index = le32_to_cpu(raw_sb->nx_xp_desc_index);
	u32 desc_blks = le32_to_cpu(raw_sb->nx_xp_desc_blocks);
	u32 desc_len = le32_to_cpu(raw_sb->nx_xp_desc_len);
	u32 i;

	if (!desc_blks || !desc_len)
		return ERR_PTR(-EFSCORRUPTED);

	/* Last block in the area is superblock; the rest are mapping blocks */
	for (i = 0; i < desc_len - 1; ++i) {
		struct buffer_head *bh;
		struct apfs_checkpoint_map_phys *cpm;
		u64 cpm_bno = desc_base + (desc_index + i) % desc_blks;
		u64 obj_bno;
		int err;

		bh = apfs_sb_bread(sb, cpm_bno);
		if (!bh)
			return ERR_PTR(-EIO);
		cpm = (struct apfs_checkpoint_map_phys *)bh->b_data;

		err = apfs_cpm_lookup_oid(sb, cpm, oid, &obj_bno);
		brelse(bh);
		cpm = NULL;
		if (err == -EAGAIN) /* Search the next mapping block */
			continue;
		if (err)
			return ERR_PTR(err);

		bh = apfs_sb_bread(sb, obj_bno);
		if (!bh)
			return ERR_PTR(-EIO);
		return bh;
	}
	return ERR_PTR(-EFSCORRUPTED); /* The mapping is missing */
}

/**
 * apfs_read_object_block - Map a non-ephemeral object block
 * @sb:		superblock structure
 * @bno:	block number for the object
 * @write:	request write access?
 *
 * On success returns the mapped buffer head for the object, which may now be
 * in a new location if write access was requested.  Returns an error pointer
 * in case of failure.
 */
struct buffer_head *apfs_read_object_block(struct super_block *sb, u64 bno,
					   bool write)
{
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct buffer_head *bh, *new_bh;
	struct apfs_obj_phys *obj;
	u32 type;
	u64 new_bno;
	int err;

	bh = apfs_sb_bread(sb, bno);
	if (!bh)
		return ERR_PTR(-EIO);

	obj = (struct apfs_obj_phys *)bh->b_data;
	type = le32_to_cpu(obj->o_type);
	ASSERT(!(type & APFS_OBJ_EPHEMERAL));
	if (nxi->nx_flags & APFS_CHECK_NODES && !apfs_obj_verify_csum(sb, obj)) {
		err = -EFSBADCRC;
		goto fail;
	}

	if (!write)
		return bh;
	ASSERT(!(sb->s_flags & SB_RDONLY));

	/* Is the object already part of the current transaction? */
	if (obj->o_xid == cpu_to_le64(nxi->nx_xid))
		return bh;

	err = apfs_spaceman_allocate_block(sb, &new_bno, true /* backwards */);
	if (err)
		goto fail;
	new_bh = apfs_sb_bread(sb, new_bno);
	if (!new_bh) {
		err = -EIO;
		goto fail;
	}
	memcpy(new_bh->b_data, bh->b_data, sb->s_blocksize);

	err = apfs_free_queue_insert(sb, bh->b_blocknr, 1);
	brelse(bh);
	bh = new_bh;
	new_bh = NULL;
	if (err)
		goto fail;
	obj = (struct apfs_obj_phys *)bh->b_data;

	if (type & APFS_OBJ_PHYSICAL)
		obj->o_oid = cpu_to_le64(new_bno);
	obj->o_xid = cpu_to_le64(nxi->nx_xid);
	err = apfs_transaction_join(sb, bh);
	if (err)
		goto fail;

	set_buffer_csum(bh);
	return bh;

fail:
	brelse(bh);
	return ERR_PTR(err);
}
