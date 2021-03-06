/*
 * fs/f2fs/gc.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/blkdev.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "gc.h"
#include <trace/events/f2fs.h>

static int gc_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	wait_queue_head_t *wq = &sbi->gc_thread->gc_wait_queue_head;
	long wait_ms;

	wait_ms = gc_th->min_sleep_time;

	do {
		if (try_to_freeze())
			continue;
		else
			wait_event_interruptible_timeout(*wq,
						kthread_should_stop(),
						msecs_to_jiffies(wait_ms));
		if (kthread_should_stop())
			break;

		if (sbi->sb->s_writers.frozen >= SB_FREEZE_WRITE) {
			increase_sleep_time(gc_th, &wait_ms);
			continue;
		}

		/*
		 * [GC triggering condition]
		 * 0. GC is not conducted currently.
		 * 1. There are enough dirty segments.
		 * 2. IO subsystem is idle by checking the # of writeback pages.
		 * 3. IO subsystem is idle by checking the # of requests in
		 *    bdev's request list.
		 *
		 * Note) We have to avoid triggering GCs frequently.
		 * Because it is possible that some segments can be
		 * invalidated soon after by user update or deletion.
		 * So, I'd like to wait some time to collect dirty segments.
		 */
		if (!mutex_trylock(&sbi->gc_mutex))
			continue;

		if (!is_idle(sbi)) {
			increase_sleep_time(gc_th, &wait_ms);
			mutex_unlock(&sbi->gc_mutex);
			continue;
		}

		if (has_enough_invalid_blocks(sbi))
			decrease_sleep_time(gc_th, &wait_ms);
		else
			increase_sleep_time(gc_th, &wait_ms);

		stat_inc_bggc_count(sbi);

		/* if return value is not zero, no victim was selected */
		if (f2fs_gc(sbi, test_opt(sbi, FORCE_FG_GC)))
			wait_ms = gc_th->no_gc_sleep_time;

		trace_f2fs_background_gc(sbi->sb, wait_ms,
				prefree_segments(sbi), free_segments(sbi));

		/* balancing f2fs's metadata periodically */
		f2fs_balance_fs_bg(sbi);

	} while (!kthread_should_stop());
	return 0;
}

int start_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	gc_th = kmalloc(sizeof(struct f2fs_gc_kthread), GFP_KERNEL);
	if (!gc_th) {
		err = -ENOMEM;
		goto out;
	}

	gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	gc_th->gc_idle = 0;

	sbi->gc_thread = gc_th;
	init_waitqueue_head(&sbi->gc_thread->gc_wait_queue_head);
	sbi->gc_thread->f2fs_gc_task = kthread_run(gc_thread_func, sbi,
			"f2fs_gc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		err = PTR_ERR(gc_th->f2fs_gc_task);
		kfree(gc_th);
		sbi->gc_thread = NULL;
	}
out:
	return err;
}

void stop_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	if (!gc_th)
		return;
	kthread_stop(gc_th->f2fs_gc_task);
	kfree(gc_th);
	sbi->gc_thread = NULL;
}

static int select_gc_type(struct f2fs_gc_kthread *gc_th, int gc_type)
{
	int gc_mode = (gc_type == BG_GC) ? GC_CB : GC_GREEDY;

	if (gc_th && gc_th->gc_idle) {
		if (gc_th->gc_idle == 1)
			gc_mode = GC_CB;
		else if (gc_th->gc_idle == 2)
			gc_mode = GC_GREEDY;
	}
	return gc_mode;
}

static void select_policy(struct f2fs_sb_info *sbi, int gc_type,
			int type, struct victim_sel_policy *p)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	if (p->alloc_mode == SSR) {
		p->gc_mode = GC_GREEDY;
		p->dirty_segmap = dirty_i->dirty_segmap[type];
		p->max_search = dirty_i->nr_dirty[type];
		p->ofs_unit = 1;
	} else {
		p->gc_mode = select_gc_type(sbi->gc_thread, gc_type);
		p->dirty_segmap = dirty_i->dirty_segmap[DIRTY];
		p->max_search = dirty_i->nr_dirty[DIRTY];
		p->ofs_unit = sbi->segs_per_sec;
	}

	if (p->max_search > sbi->max_victim_search)
		p->max_search = sbi->max_victim_search;

	p->offset = sbi->last_victim[p->gc_mode];
}

static unsigned int get_max_cost(struct f2fs_sb_info *sbi,
				struct victim_sel_policy *p)
{
	/* SSR allocates in a segment unit */
	if (p->alloc_mode == SSR)
		return 1 << sbi->log_blocks_per_seg;
	if (p->gc_mode == GC_GREEDY)
		return (1 << sbi->log_blocks_per_seg) * p->ofs_unit;
	else if (p->gc_mode == GC_CB)
		return UINT_MAX;
	else /* No other gc_mode */
		return 0;
}

static unsigned int check_bg_victims(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int secno;

	/*
	 * If the gc_type is FG_GC, we can select victim segments
	 * selected by background GC before.
	 * Those segments guarantee they have small valid blocks.
	 */
	for_each_set_bit(secno, dirty_i->victim_secmap, MAIN_SECS(sbi)) {
		if (sec_usage_check(sbi, secno))
			continue;
		clear_bit(secno, dirty_i->victim_secmap);
		return secno * sbi->segs_per_sec;
	}
	return NULL_SEGNO;
}

static unsigned int get_cb_cost(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SECNO(sbi, segno);
	unsigned int start = secno * sbi->segs_per_sec;
	unsigned long long mtime = 0;
	unsigned int vblocks;
	unsigned char age = 0;
	unsigned char u;
	unsigned int i;

	for (i = 0; i < sbi->segs_per_sec; i++)
		mtime += get_seg_entry(sbi, start + i)->mtime;
	vblocks = get_valid_blocks(sbi, segno, sbi->segs_per_sec);

	mtime = div_u64(mtime, sbi->segs_per_sec);
	vblocks = div_u64(vblocks, sbi->segs_per_sec);

	u = (vblocks * 100) >> sbi->log_blocks_per_seg;

	/* Handle if the system time has changed by the user */
	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;
	if (sit_i->max_mtime != sit_i->min_mtime)
		age = 100 - div64_u64(100 * (mtime - sit_i->min_mtime),
				sit_i->max_mtime - sit_i->min_mtime);

	return UINT_MAX - ((100 * (100 - u) * age) / (100 + u));
}

static inline unsigned int get_gc_cost(struct f2fs_sb_info *sbi,
			unsigned int segno, struct victim_sel_policy *p)
{
	if (p->alloc_mode == SSR)
		return get_seg_entry(sbi, segno)->ckpt_valid_blocks;

	/* alloc_mode == LFS */
	if (p->gc_mode == GC_GREEDY)
		return get_valid_blocks(sbi, segno, sbi->segs_per_sec);
	else
		return get_cb_cost(sbi, segno);
}

/*
 * This function is called from two paths.
 * One is garbage collection and the other is SSR segment selection.
 * When it is called during GC, it just gets a victim segment
 * and it does not remove it from dirty seglist.
 * When it is called from SSR segment selection, it finds a segment
 * which has minimum valid blocks and removes it from dirty seglist.
 */
static int get_victim_by_default(struct f2fs_sb_info *sbi,
		unsigned int *result, int gc_type, int type, char alloc_mode)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct victim_sel_policy p;
	unsigned int secno, max_cost;
	unsigned int last_segment = MAIN_SEGS(sbi);
	int nsearched = 0;

	mutex_lock(&dirty_i->seglist_lock);

	p.alloc_mode = alloc_mode;
	select_policy(sbi, gc_type, type, &p);

	p.min_segno = NULL_SEGNO;
	p.min_cost = max_cost = get_max_cost(sbi, &p);

	if (p.max_search == 0)
		goto out;

	if (p.alloc_mode == LFS && gc_type == FG_GC) {
		p.min_segno = check_bg_victims(sbi);
		if (p.min_segno != NULL_SEGNO)
			goto got_it;
	}

	while (1) {
		unsigned long cost;
		unsigned int segno;

		segno = find_next_bit(p.dirty_segmap, last_segment, p.offset);
		if (segno >= last_segment) {
			if (sbi->last_victim[p.gc_mode]) {
				last_segment = sbi->last_victim[p.gc_mode];
				sbi->last_victim[p.gc_mode] = 0;
				p.offset = 0;
				continue;
			}
			break;
		}

		p.offset = segno + p.ofs_unit;
		if (p.ofs_unit > 1)
			p.offset -= segno % p.ofs_unit;

		secno = GET_SECNO(sbi, segno);

		if (sec_usage_check(sbi, secno))
			continue;
		if (gc_type == BG_GC && test_bit(secno, dirty_i->victim_secmap))
			continue;

		cost = get_gc_cost(sbi, segno, &p);

		if (p.min_cost > cost) {
			p.min_segno = segno;
			p.min_cost = cost;
		} else if (unlikely(cost == max_cost)) {
			continue;
		}

		if (nsearched++ >= p.max_search) {
			sbi->last_victim[p.gc_mode] = segno;
			break;
		}
	}
	if (p.min_segno != NULL_SEGNO) {
got_it:
		if (p.alloc_mode == LFS) {
			secno = GET_SECNO(sbi, p.min_segno);
			if (gc_type == FG_GC)
				sbi->cur_victim_sec = secno;
			else
				set_bit(secno, dirty_i->victim_secmap);
		}
		*result = (p.min_segno / p.ofs_unit) * p.ofs_unit;

		trace_f2fs_get_victim(sbi->sb, type, gc_type, &p,
				sbi->cur_victim_sec,
				prefree_segments(sbi), free_segments(sbi));
	}
out:
	mutex_unlock(&dirty_i->seglist_lock);

	return (p.min_segno == NULL_SEGNO) ? 0 : 1;
}

static const struct victim_selection default_v_ops = {
	.get_victim = get_victim_by_default,
};

static struct inode *find_gc_inode(struct gc_inode_list *gc_list, nid_t ino)
{
	struct inode_entry *ie;

	ie = radix_tree_lookup(&gc_list->iroot, ino);
	if (ie)
		return ie->inode;
	return NULL;
}

static void add_gc_inode(struct gc_inode_list *gc_list, struct inode *inode)
{
	struct inode_entry *new_ie;

	if (inode == find_gc_inode(gc_list, inode->i_ino)) {
		iput(inode);
		return;
	}
	new_ie = f2fs_kmem_cache_alloc(inode_entry_slab, GFP_NOFS);
	new_ie->inode = inode;

	f2fs_radix_tree_insert(&gc_list->iroot, inode->i_ino, new_ie);
	list_add_tail(&new_ie->list, &gc_list->ilist);
}

static void put_gc_inode(struct gc_inode_list *gc_list)
{
	struct inode_entry *ie, *next_ie;
	list_for_each_entry_safe(ie, next_ie, &gc_list->ilist, list) {
		radix_tree_delete(&gc_list->iroot, ie->inode->i_ino);
		iput(ie->inode);
		list_del(&ie->list);
		kmem_cache_free(inode_entry_slab, ie);
	}
}

static int check_valid_map(struct f2fs_sb_info *sbi,
				unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

/*
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */

/*
struct f2fs_summary { // a summary entry for a 4KB-sized block in a segment
	__le32 nid;		// parent node id
	union {
		__u8 reserved[3];
		struct {
			__u8 version;		//node version number
			__le16 ofs_in_node;	// block index in parent node
		} __packed;
	};
} __packed;
*/
/*
 * @ Aim to know the situation of the SSD when trigger GC.
 * This function used to let the SSD known when to do clean.
 * The only thing need to do is to tell the start and then end address 
 * in this segment clean.
 */
#include <linux/types.h>
//#include "/home/willow/fxl/linux-4.4.4/drivers/nvme/host/nvme.h"
extern int nvme_set_features(struct nvme_dev *dev, unsigned fid, unsigned dword11,dma_addr_t dma_addr, u32 *result);
int sendtoSSD(unsigned int lba, unsigned int s_e){ // s_e = 0,means start; s_e = 1,means end;

    struct file *filp = NULL;
    mm_segment_t oldfs;
    int err = 0;

    oldfs = get_fs();
    set_fs(get_ds());
    filp = filp_open("/dev/nvme0n1", O_RDONLY, 0);
    set_fs(oldfs);
    if (IS_ERR(filp)) {
        err = PTR_ERR(filp);
		printk("Unable to open stats file to write\n");
        return -1;
    }
	struct nvme_ns *ns = filp->f_inode->i_bdev->bd_disk->private_data;
	u32 result;
	int err2;
	int count = 20;
	int opcode;
    if(s_e) // end
		opcode = 0x13;
	else
        opcode = 0x12;
	err2 = nvme_set_features(ns->dev, opcode, lba, 0, &result);
    filp_close(filp, NULL);
	printk("err2:%d\n",err2);
	printk("result:%d\n",result);
	int ret = min(result & 0xffff,result >> 16) + 1;
	if(ret>=0){
		printk("ioctl success!\n");	
		return 0;
	} else {
		printk("ioctl failed!\n");
		return -1;
	}
}
#define START_ADDR_GC 0
#define END_ADDR_GC 1

// static int gc_node_segment(struct f2fs_sb_info *sbi,
// 		struct f2fs_summary *sum, unsigned int segno, int gc_type)
// {
// 	bool initial = true;
// 	struct f2fs_summary *entry;
// 	block_t start_addr;
// 	int off;

// 	start_addr = START_BLOCK(sbi, segno); // start logical block address.
// 	printk(KERN_EMERG "gc_node::%x\n",start_addr);  // start_addr means block number.
// next_step:
// 	entry = sum;

// 	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
// 		nid_t nid = le32_to_cpu(entry->nid); 
// 		struct page *node_page;
// 		struct node_info ni;

// 		/* stop BG_GC if there is not enough free sections. */
// 		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0))
// 			return 0;

// 		if (check_valid_map(sbi, segno, off) == 0){ // check if the block is valid.
// 			continue;
// 		} 

// 		if (initial) {
// //			printk(KERN_EMERG "%d:",off); // Show how many nodes to read.
// 			ra_node_page_gc(sbi, nid); // ?????????nid?????????node page?????????????????????,?????????????????????????????????
// 			continue;
// 		}
// 		node_page = get_node_page_gc(sbi, nid); // This function or the ra_node_page is used to read page.
// 		if (IS_ERR(node_page))
// 			continue;
// /*		if(PageDirty(node_page)){
// 			printk(KERN_EMERG "2:%d\n",off); 
// 		}*/
// 		/* block may become invalid during get_node_page */
// 		if (check_valid_map(sbi, segno, off) == 0) { // Why block may become invalid?
// 			f2fs_put_page(node_page, 1); // minus the used page count.
// 			continue;
// 		}

// 		get_node_info(sbi, nid, &ni); 
// 		if (ni.blk_addr != start_addr + off) {
// 			f2fs_put_page(node_page, 1);
// 			continue;
// 		}
// 		/* set page dirty and write it */
// 		if (gc_type == FG_GC) {
// 			f2fs_wait_on_page_writeback(node_page, NODE); // Why wait for write back? What kind of pages to write back?
// 			set_page_dirty(node_page); // This page is dirty.
// 		} else {
// 			if (!PageWriteback(node_page))
// 				set_page_dirty(node_page);
// 		}
// 		f2fs_put_page(node_page, 1);
// 		stat_inc_node_blk_count(sbi, 1, gc_type); // Incress block count for what? For background.
// 	}

// 	if (initial) {
// 		initial = false;
// 		goto next_step;
// 	}

// 	if (gc_type == FG_GC) {
// 		struct writeback_control wbc = {
// 			.sync_mode = WB_SYNC_ALL,
// 			.nr_to_write = LONG_MAX,
// 			.for_reclaim = 0,
// 		};
// //		printk(KERN_EMERG "FG_GC\n",off); 
// 		sync_node_pages_gc(sbi, 0, &wbc); // ???????????????????????????????????????????????????

// 		/* return 1 only if FG_GC succefully reclaimed one */
// 		if (get_valid_blocks(sbi, segno, 1) == 0) {
// 			sendtoSSD(start_addr+sbi->blocks_per_seg, END_ADDR_GC); 
// 			return 1;
// 		}	
// 	}
// 	sendtoSSD(start_addr+sbi->blocks_per_seg, END_ADDR_GC); 
// 	return 0;
// }
static int gc_node_segment(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	bool initial = true;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;

	start_addr = START_BLOCK(sbi, segno); // start logical block address.
	printk(KERN_EMERG "gc_node::%x\n",start_addr);  // start_addr means block number.
//	sendtoSSD(start_addr, START_ADDR_GC); 
next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid); 
		struct page *node_page;
		struct node_info ni;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0))
			return 0;

		if (check_valid_map(sbi, segno, off) == 0){ // check if the block is valid.
			continue;
		} 

		if (initial) {
//			printk(KERN_EMERG "%d:",off); // Show how many nodes to read.
			ra_node_page(sbi, nid); // ?????????nid?????????node page?????????????????????,?????????????????????????????????
			continue;
		}
		node_page = get_node_page(sbi, nid); // This function or the ra_node_page is used to read page.
		if (IS_ERR(node_page))
			continue;
/*		if(PageDirty(node_page)){
			printk(KERN_EMERG "2:%d\n",off); 
		}*/
		/* block may become invalid during get_node_page */
		if (check_valid_map(sbi, segno, off) == 0) { // Why block may become invalid?
			f2fs_put_page(node_page, 1);
			continue;
		}

		get_node_info(sbi, nid, &ni); 
		if (ni.blk_addr != start_addr + off) {
			f2fs_put_page(node_page, 1);
			continue;
		}
		// set page dirty and write it 
		if (gc_type == FG_GC) {
			f2fs_wait_on_page_writeback(node_page, NODE); // Why wait for write back? What kind of pages to write back?
			set_page_dirty(node_page); // This page is dirty.
		} else {
			if (!PageWriteback(node_page))
				set_page_dirty(node_page);
		}
		f2fs_put_page(node_page, 1);
		stat_inc_node_blk_count(sbi, 1, gc_type);
	}

	if (initial) {
		initial = false;
		goto next_step;
	}

	if (gc_type == FG_GC) {
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_ALL,
			.nr_to_write = LONG_MAX,
			.for_reclaim = 0,
		};
//		printk(KERN_EMERG "FG_GC\n",off); 
		sync_node_pages(sbi, 0, &wbc); // ???????????????????????????????????????????????????

		/* return 1 only if FG_GC succefully reclaimed one */
		if (get_valid_blocks(sbi, segno, 1) == 0) {
//			sendtoSSD(start_addr+sbi->blocks_per_seg, END_ADDR_GC); 
			return 1;
		}	
	}
//	sendtoSSD(start_addr+sbi->blocks_per_seg, END_ADDR_GC); 
	return 0;
}
/*
 * Calculate start block index indicating the given node offset.
 * Be careful, caller should give this node offset only indicating direct node
 * blocks. If any node offsets, which point the other types of node blocks such
 * as indirect or double indirect node blocks, are given, it must be a caller's
 * bug.
 */
block_t start_bidx_of_node(unsigned int node_ofs, struct f2fs_inode_info *fi)
{
	unsigned int indirect_blks = 2 * NIDS_PER_BLOCK + 4;
	unsigned int bidx;

	if (node_ofs == 0)
		return 0;

	if (node_ofs <= 2) {
		bidx = node_ofs - 1;
	} else if (node_ofs <= indirect_blks) {
		int dec = (node_ofs - 4) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 2 - dec;
	} else {
		int dec = (node_ofs - indirect_blks - 3) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 5 - dec;
	}
	return bidx * ADDRS_PER_BLOCK + ADDRS_PER_INODE(fi);
}

static bool is_alive(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

	node_page = get_node_page(sbi, nid);
	if (IS_ERR(node_page))
		return false;

	get_node_info(sbi, nid, dni);

	if (sum->version != dni->version) {
		f2fs_put_page(node_page, 1);
		return false;
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = datablock_addr(node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr)
		return false;
	return true;
}

static void move_encrypted_block(struct inode *inode, block_t bidx)
{
	struct f2fs_io_info fio = {
		.sbi = F2FS_I_SB(inode),
		.type = DATA,
		.rw = READ_SYNC,
		.encrypted_page = NULL,
	};
	struct dnode_of_data dn;
	struct f2fs_summary sum;
	struct node_info ni;
	struct page *page;
	int err;

	/* do not read out */
	page = f2fs_grab_cache_page(inode->i_mapping, bidx, false);
	if (!page)
		return;

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = get_dnode_of_data(&dn, bidx, LOOKUP_NODE);
	if (err)
		goto out;

	if (unlikely(dn.data_blkaddr == NULL_ADDR)) {
		ClearPageUptodate(page);
		goto put_out;
	}

	/*
	 * don't cache encrypted data into meta inode until previous dirty
	 * data were writebacked to avoid racing between GC and flush.
	 */
	f2fs_wait_on_page_writeback(page, DATA);

	get_node_info(fio.sbi, dn.nid, &ni);
	set_summary(&sum, dn.nid, dn.ofs_in_node, ni.version);

	/* read page */
	fio.page = page;
	fio.blk_addr = dn.data_blkaddr;

	fio.encrypted_page = pagecache_get_page(META_MAPPING(fio.sbi),
					fio.blk_addr,
					FGP_LOCK|FGP_CREAT,
					GFP_NOFS);
	if (!fio.encrypted_page)
		goto put_out;

	err = f2fs_submit_page_bio(&fio);
	if (err)
		goto put_page_out;

	/* write page */
	lock_page(fio.encrypted_page);

	if (unlikely(!PageUptodate(fio.encrypted_page)))
		goto put_page_out;
	if (unlikely(fio.encrypted_page->mapping != META_MAPPING(fio.sbi)))
		goto put_page_out;

	set_page_dirty(fio.encrypted_page);
	f2fs_wait_on_page_writeback(fio.encrypted_page, DATA);
	if (clear_page_dirty_for_io(fio.encrypted_page))
		dec_page_count(fio.sbi, F2FS_DIRTY_META);

	set_page_writeback(fio.encrypted_page);

	/* allocate block address */
	f2fs_wait_on_page_writeback(dn.node_page, NODE);
	allocate_data_block(fio.sbi, NULL, fio.blk_addr,
					&fio.blk_addr, &sum, CURSEG_COLD_DATA);
	fio.rw = WRITE_SYNC;
	f2fs_submit_page_mbio(&fio);

	dn.data_blkaddr = fio.blk_addr;
	set_data_blkaddr(&dn);
	f2fs_update_extent_cache(&dn);
	set_inode_flag(F2FS_I(inode), FI_APPEND_WRITE);
	if (page->index == 0)
		set_inode_flag(F2FS_I(inode), FI_FIRST_BLOCK_WRITTEN);
put_page_out:
	f2fs_put_page(fio.encrypted_page, 1);
put_out:
	f2fs_put_dnode(&dn);
out:
	f2fs_put_page(page, 1);
}
static void remap_data_page(struct inode *inode, block_t bidx, int gc_type) { // ?????????????????????????????????
	// write something.
	struct page *page = fio->page;
	struct inode *inode = page->mapping->host;
	struct dnode_of_data dn;
	int err = 0;

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = get_dnode_of_data(&dn, bidx, LOOKUP_NODE);
	if (err)
		return err;

	/* This page is already truncated */
	if (dn.data_blkaddr == NULL_ADDR) {
		ClearPageUptodate(page);
		goto out_writepage;
	}

	write_data_page(&dn, fio);
	set_data_blkaddr(&dn);
	f2fs_update_extent_cache(&dn);
	trace_f2fs_do_write_data_page(page, OPU);
	set_inode_flag(F2FS_I(inode), FI_APPEND_WRITE);
	if (page->index == 0)
		set_inode_flag(F2FS_I(inode), FI_FIRST_BLOCK_WRITTEN);

out_writepage:
	f2fs_put_dnode(&dn);
	return err;

}
static void move_data_page(struct inode *inode, block_t bidx, int gc_type)
{
	struct page *page;

	page = get_lock_data_page(inode, bidx, true); // This time to get the block_t of the index.
	if (IS_ERR(page))
		return;

	if (gc_type == BG_GC) {
		if (PageWriteback(page)) // ????????????????????????
			goto out;
		set_page_dirty(page);
		set_cold_data(page); // Background GC will not write the page back immediately.
	} else {
		struct f2fs_io_info fio = {
			.sbi = F2FS_I_SB(inode),
			.type = DATA,
			.rw = WRITE_SYNC,
			.page = page,
			.encrypted_page = NULL,
		};
		set_page_dirty(page);
		f2fs_wait_on_page_writeback(page, DATA);
		if (clear_page_dirty_for_io(page))
			inode_dec_dirty_pages(inode);
		set_cold_data(page);
		do_write_data_page(&fio); // This time to write data page. Know the new logical address.
		clear_cold_data(page);
	}
	// get the bloct_t of the inode and the bidx.

out:
	f2fs_put_page(page, 1);
}
static void change_data_page(struct inode *inode, block_t bidx, int gc_type)
{
	struct page *page;
	// change the code: get cached page.
	page = get_cached_data_page(inode, 
					start_bidx + ofs_in_node, READA, true,off);
	/* wait for read completion */
	lock_page(page);
	if (unlikely(!PageUptodate(page))) {
		f2fs_put_page(page, 1);
		return;
	}
	if (unlikely(page->mapping != mapping)) {
		f2fs_put_page(page, 1);
		return;
	}
	//page = get_lock_data_page(inode, bidx, true); // This time to get the block_t of the index. 
	if (IS_ERR(page))
		return;

	if (gc_type == BG_GC) {
		if (PageWriteback(page)) //????????????????????????
			goto out;
		set_page_dirty(page);
		set_cold_data(page); // Background GC will not write the page back immediately.
	} else {
		struct f2fs_io_info fio = {
			.sbi = F2FS_I_SB(inode),
			.type = DATA,
			.rw = WRITE_SYNC,
			.page = page,
			.encrypted_page = NULL,
		};
		if (clear_page_dirty_for_io(page)) // What is the aim? Because this page is ready to write, so it is not dirty anymore?
			inode_dec_dirty_pages(inode);  // decrease dirty pages?
		set_cold_data(page);
		do_remap_data_page(&fio);
		clear_cold_data(page);
	}
	// get the bloct_t of the inode and the bidx.

out:
	f2fs_put_page(page, 1);
}
/*
 * This function tries to get parent node of victim data block, and identifies
 * data block validity. If the block is valid, copy that with cold status and
 * modify parent node.
 * If the parent node is not valid or the data block address is different,
 * the victim data block is ignored.
 */
#define EFFICIENT 1
static int gc_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;
	unsigned int ori_lba = 0;
	unsigned int new_lba = 0;
	unsigned int len = -1;

	// get the bitmap of the segment. 512*32.
	#ifdef EFFICIENT
		int * array,i;
        array = kzalloc(sizeof(int)*512,GFP_KERNEL);
	#endif
	start_addr = START_BLOCK(sbi, segno);
	printk(KERN_EMERG "gc_data::%x\n",start_addr); 
next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0))
			return 0;

		if (check_valid_map(sbi, segno, off) == 0) // check if the block is valid.
			continue;

		if (phase == 0) {
			ra_node_page(sbi, le32_to_cpu(entry->nid)); // read the node page of the moving block.
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs)) // get ino number by reading NAT.
			continue;

		if (phase == 1) {
			ra_node_page(sbi, dni.ino); // read the inode of the moving block.
			continue;
		}

		ofs_in_node = le16_to_cpu(entry->ofs_in_node); // the offset in the dnode.

		if (phase == 2) {
			inode = f2fs_iget(sb, dni.ino); // get the inode(already in page cache) , which is in the cache. ???
			if (IS_ERR(inode) || is_bad_inode(inode))
				continue;

			/* if encrypted??????????????? inode, let's go phase 3 */
			if (f2fs_encrypted_inode(inode) &&
						S_ISREG(inode->i_mode)) {
				add_gc_inode(gc_list, inode);
				continue;
			}
			
			start_bidx = start_bidx_of_node(nofs, F2FS_I(inode)); // ??????node block???start_bidx.
			
			
			/*change : 1. judge the page in the page cache but clean or not in the cache. 2. judge the page is dirty.
				1. just remap.
				2. just move data.
			*/
			
			data_page = get_cached_data_page(inode, 
					start_bidx + ofs_in_node, READA, true,off); // read the block in data_page.
			if (PageUptodate(data_page)) { // means data already in the page cache.
				if(!PageDirty(data_page)){ // if page is not dirty.
					printk("2\n");
					array[off] = 2;
				} else { // if page is dirty.
					// 1. At first, I will remove the page. I think the overhead is the smallest.
					// 2. Then, what need to do is syncing to the ssd. But I don't know how to sync.
					#ifdef EFFICIENT
						array[off] = 1;
						// *** write the page back to the .
					#endif
					printk("1\n");
				}
			} else {
				// page haven't read. add the remap table.
				array[off] = 3;
			}
			if (IS_ERR(data_page)) { //#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
				printk("erro:IS_ERR(data_page);\n");
				iput(inode); // The function is used to reduce the usage count of inode.
				continue;
			} 

			f2fs_put_page(data_page, 0); // ?????????????????????????????????grab_page_cache????????????????????????????????????
			add_gc_inode(gc_list, inode);
			continue;
		}

		/* phase 3 */
		inode = find_gc_inode(gc_list, dni.ino); // ??????GC??????inode??????????????????inode?????????radix tree??????
		if (inode) {
			start_bidx = start_bidx_of_node(nofs, F2FS_I(inode))
								+ ofs_in_node;
			if (f2fs_encrypted_inode(inode) && S_ISREG(inode->i_mode))
				move_encrypted_block(inode, start_bidx);
			else{
				
				if(array[off]==1 || gc_type == BG_GC) // if set bit , means dirty page.
					move_data_page(inode, start_bidx, gc_type); // write data to ?
				else	
					change_data_page(inode, start_bidx, gc_type); // write data to ?
			}
				
			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}

	if (++phase < 4)
		goto next_step;

	if (gc_type == FG_GC) {
		f2fs_submit_merged_bio(sbi, DATA, WRITE);

		/* return 1 only if FG_GC succefully reclaimed one */
		if (get_valid_blocks(sbi, segno, 1) == 0){
			return 1;
		}	
	}
	return 0;
}
static int gc_data_segment_FG(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct gc_inode_list *gc_list, unsigned int segno, int gc_type)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;

	start_addr = START_BLOCK(sbi, segno);
	printk(KERN_EMERG "gc_data::%x\n",start_addr); 
//	sendtoSSD(start_addr, START_ADDR_GC); 
next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0))
			return 0;

		if (check_valid_map(sbi, segno, off) == 0) // check if the block is valid.
			continue;

		if (phase == 0) {
			ra_node_page(sbi, le32_to_cpu(entry->nid)); // read the node page of the moving block.
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (!is_alive(sbi, entry, &dni, start_addr + off, &nofs)) // get ino number by reading NAT.
			continue;

		if (phase == 1) {
			ra_node_page(sbi, dni.ino); // read the inode of the moving block.
			continue;
		}

		ofs_in_node = le16_to_cpu(entry->ofs_in_node); // the offset in the dnode.

		if (phase == 2) {
			inode = f2fs_iget(sb, dni.ino); // get the inode(already in page cache) , which is in the cache. ???
			if (IS_ERR(inode) || is_bad_inode(inode))
				continue;

			/* if encrypted??????????????? inode, let's go phase 3 */
			if (f2fs_encrypted_inode(inode) &&
						S_ISREG(inode->i_mode)) {
				add_gc_inode(gc_list, inode);
				continue;
			}
			
			start_bidx = start_bidx_of_node(nofs, F2FS_I(inode));
			// First, record the block number of the page.
			data_page = get_read_data_page(inode, 
					start_bidx + ofs_in_node, READA, true,off); // read the block in data_page.
			if (IS_ERR(data_page)) {
				iput(inode); // The function is used to reduce the usage count of inode.
				continue;
			} 
		/*	if(PageDirty(data_page))
				printk(KERN_EMERG "2:%d\n",off); */// print wheater the page cached is dirty.

			f2fs_put_page(data_page, 0); // What is the meaning of page cache release?
			add_gc_inode(gc_list, inode);
			continue;
		}

		/* phase 3 */
		inode = find_gc_inode(gc_list, dni.ino);
		if (inode) {
			start_bidx = start_bidx_of_node(nofs, F2FS_I(inode))
								+ ofs_in_node;
			if (f2fs_encrypted_inode(inode) && S_ISREG(inode->i_mode))
				move_encrypted_block(inode, start_bidx);
			else
				move_data_page(inode, start_bidx, gc_type); // write data to ?
			stat_inc_data_blk_count(sbi, 1, gc_type);
		}
	}

	if (++phase < 4)
		goto next_step;

	if (gc_type == FG_GC) {
		f2fs_submit_merged_bio(sbi, DATA, WRITE);

		/* return 1 only if FG_GC succefully reclaimed one */
		if (get_valid_blocks(sbi, segno, 1) == 0){
//			sendtoSSD(start_addr+sbi->blocks_per_seg, END_ADDR_GC); 
			return 1;
		}	
	}
//	sendtoSSD(start_addr+sbi->blocks_per_seg, END_ADDR_GC); 
	return 0;
}
static int __get_victim(struct f2fs_sb_info *sbi, unsigned int *victim,
			int gc_type)
{
	struct sit_info *sit_i = SIT_I(sbi);
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	ret = DIRTY_I(sbi)->v_ops->get_victim(sbi, victim, gc_type,
					      NO_CHECK_TYPE, LFS);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

static int do_garbage_collect(struct f2fs_sb_info *sbi, unsigned int segno,
				struct gc_inode_list *gc_list, int gc_type)
{
	if(gc_type==FG_GC) {
		printk("FG_GC:--------\n");
	} else {
		printk("BG_GC:--------\n");
	}
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;
	int nfree = 0;

	/* read segment summary of victim */
	sum_page = get_sum_page(sbi, segno);

	blk_start_plug(&plug);

	sum = page_address(sum_page);

	/*
	 * this is to avoid deadlock:
	 * - lock_page(sum_page)         - f2fs_replace_block
	 *  - check_valid_map()            - mutex_lock(sentry_lock)
	 *   - mutex_lock(sentry_lock)     - change_curseg()
	 *                                  - lock_page(sum_page)
	 */
	unlock_page(sum_page);

	switch (GET_SUM_TYPE((&sum->footer))) {
	case SUM_TYPE_NODE:
		nfree = gc_node_segment(sbi, sum->entries, segno, gc_type);
		break;
	case SUM_TYPE_DATA:
		if(gc_type==FG_GC)
			nfree = gc_data_segment_FG(sbi, sum->entries, gc_list,
							segno, gc_type);
		else
			nfree = gc_data_segment(sbi, sum->entries, gc_list,
							segno, gc_type);
		break;
	}
	blk_finish_plug(&plug);

	stat_inc_seg_count(sbi, GET_SUM_TYPE((&sum->footer)), gc_type);
	stat_inc_call_count(sbi->stat_info);

	f2fs_put_page(sum_page, 0);
	return nfree;
}

int f2fs_gc(struct f2fs_sb_info *sbi, bool sync)
{	
	unsigned int segno, i;
	int gc_type = sync ? FG_GC : BG_GC;
	int sec_freed = 0;
	int ret = -EINVAL;
	struct cp_control cpc;
	struct gc_inode_list gc_list = {
		.ilist = LIST_HEAD_INIT(gc_list.ilist),
		.iroot = RADIX_TREE_INIT(GFP_NOFS),
	};

	cpc.reason = __get_cp_reason(sbi);
gc_more:
	segno = NULL_SEGNO;

	if (unlikely(!(sbi->sb->s_flags & MS_ACTIVE)))
		goto stop;
	if (unlikely(f2fs_cp_error(sbi)))
		goto stop;

	if (gc_type == BG_GC && has_not_enough_free_secs(sbi, sec_freed)) {
		gc_type = FG_GC;
		if (__get_victim(sbi, &segno, gc_type) || prefree_segments(sbi))
			write_checkpoint(sbi, &cpc);
	}

	if (segno == NULL_SEGNO && !__get_victim(sbi, &segno, gc_type))
		goto stop;
	ret = 0;

	/* readahead multi ssa blocks those have contiguous address */
	if (sbi->segs_per_sec > 1)
		ra_meta_pages(sbi, GET_SUM_BLOCK(sbi, segno), sbi->segs_per_sec,
							META_SSA, true);

	for (i = 0; i < sbi->segs_per_sec; i++) {
		/*
		 * for FG_GC case, halt gcing left segments once failed one
		 * of segments in selected section to avoid long latency.
		 */
		if (!do_garbage_collect(sbi, segno + i, &gc_list, gc_type) &&
				gc_type == FG_GC)
			break;
	}

	if (i == sbi->segs_per_sec && gc_type == FG_GC)
		sec_freed++;

	if (gc_type == FG_GC)
		sbi->cur_victim_sec = NULL_SEGNO;

	if (!sync) {
		if (has_not_enough_free_secs(sbi, sec_freed))
			goto gc_more;

		if (gc_type == FG_GC)
			write_checkpoint(sbi, &cpc);
	}
stop:
	mutex_unlock(&sbi->gc_mutex);

	put_gc_inode(&gc_list);

	if (sync)
		ret = sec_freed ? 0 : -EAGAIN;
	return ret;
}

void build_gc_manager(struct f2fs_sb_info *sbi)
{
	DIRTY_I(sbi)->v_ops = &default_v_ops;
}
