#include "ftl.h"

// FDP support
//
// hw/nvme/ctrl.c
// nvme_do_write_fdp(NvmeCtrl *n, NvmeRequest *req, uint64_t slba, uint32_t nlb);
//

static void *ftl_thread(void *arg);

// FIXME
static inline bool nvme_ph_valid(NvmeNamespace *ns, uint16_t ph) 
{	
	return ph < ns->fdp.nphs;
}

// FIXME
static inline bool nvme_rg_valid(NvmeEnduranceGroup *endgrp, uint16_t rg) 
{
	return rg < endgrp->fdp.nrg;
}

// FIXME
static inline uint16_t nvme_pid2ph(NvmeNamespace *ns, uint16_t pid) 
{
	uint16_t rgif = ns->endgrp->fdp.rgif;
	
	if (!rgif) {
		return pid;
	}

	return pid & ((1 << (15 - rgif)) - 1);
}

// FIXME 
static inline uint16_t nvme_pid2rg(NvmeNamespace *ns, uint16_t pid)
{
	uint16_t rgif = ns->endgrp->fdp.rgif;

	if (!rgif) {
		return 0;
	}

	return pid >> (16 - rgif);
}

// FIXME
static inline bool nvme_parse_pid(NvmeNamespace *ns, uint16_t pid,
									uint16_t *ph, uint16_t *rg)
{
	*rg = nvme_pid2rg(ns, pid);
	*ph = nvme_pid2ph(ns, pid);

	return nvme_ph_valid(ns, *ph) && nvme_rg_valid(ns->endgrp, *rg);
}

// FIXME 
static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
	return &(ssd->ch[ppa->g.ch]);
}

// FIXME
static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa) 
{
	struct ssd_channel *ch = get_ch(ssd, ppa);
	return &(ch->lun[ppa->g.lun]);
}

// FIXME
static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa) 
{
	struct nand_lun *lun = get_lun(ssd, ppa);
	return &(lun->pl[ppa->g.pl]);
}

// FIXME
static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa) 
{
	struct nand_plane *pl = get_pl(ssd, ppa);
	return &(pl->blk[ppa->g.blk]);
}

// FIXME
static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
	struct nand_block *blk = get_blk(ssd, ppa);
	return &(blk->pg[ppa->g.pg]);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn) 
{
	return ssd->maptbl[lpn];
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)  
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a) 
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a) 
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos) 
{
	((struct line *)a)->pos = pos;
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa) 
{
	struct ssdparams *spp = &ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	int sec = ppa->g.sec;

	if( ch >= 0 && ch < spp->nchs 
		&& lun >= 0 && lun < spp->luns_per_ch 
		&& pl >= 0 && pl < spp->pls_per_lun 
		&& blk >= 0 && blk < spp->blks_per_pl 
		&& pg >= 0 && pg < spp->pgs_per_blk 
		&& sec >= 0 && sec < spp->secs_per_pg ) 
		return true;
	
	return false;
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n) 
{
	/* FDP support */
	spp->secsz = n->fdp_params.secsz; // 512
	spp->secs_per_pg = n->fdp_params.secs_per_pg; // 8
	spp->pgs_per_blk = n->fdp_params.pgs_per_blk; //256
	spp->blks_per_pl = n->fdp_params.blks_per_pl; /* 256 16GB */
	spp->pls_per_lun = n->fdp_params.pls_per_lun; // 1
	spp->luns_per_ch = n->fdp_params.luns_per_ch; // 8
	spp->nchs = n->fdp_params.nchs; // 8

	spp->pg_rd_lat = n->fdp_params.pg_rd_lat;
	spp->pg_wr_lat = n->fdp_params.pg_wr_lat;
	spp->blk_er_lat = n->fdp_params.blk_er_lat;
	spp->ch_xfer_lat = n->fdp_params.ch_xfer_lat;
	/* FDP feature */
	spp->nru = n->fdp_params.nr_ru;
	if (n->fdp_params.nr_rg != 2 
			&& n->fdp_params.nr_rg != 4
			&& n->fdp_params.nr_rg != 8)
	{
		ftl_err("invalid RG count %d, only support 2,4,8\n", n->fdp_params.nr_rg);
		abort();
	}
	spp->nrg = n->fdp_params.nr_rg;
	spp->nruh = n->fdp_params.nr_ruh;
	spp->ruh_type = n->fdp_params.ruh_type;

	/* calculated values */
	spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
	spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
	spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
	spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
	spp->tt_secs = spp->secs_per_ch * spp->nchs;

	spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
	spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
	spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
	spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

	spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
	spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;spp->tt_blks = spp->blks_per_ch * spp->nchs;

	spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
	spp->tt_pls = spp->pls_per_ch * spp->nchs;

	spp->tt_luns = spp->luns_per_ch * spp->nchs;

	/* line is special, put it at the end */
	spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
	spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
	spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
	spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

	spp->gc_thres_pcent = n->fdp_params.gc_thres_pcent/100.0;
	spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
	spp->gc_thres_pcent_high = n->fdp_params.gc_thres_pcent_high/100.0;
	spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
	spp->enable_gc_delay = true;

	return;
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp) 
{
	pg->nsecs = spp->secs_per_pg;
	pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
	for(int i = 0; i < pg->nsecs; i++) {
		pg->sec[i] = SEC_FREE;
	}
	pg->status = PG_FREE;
}

static void ssd_init_nand_block(struct nand_block *blk, struct ssdparams *spp)
{
	blk->npgs = spp->pgs_per_blk;
	blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
	for(int i = 0; i < blk->npgs; i++) {
		ssd_init_nand_page(&blk->pg[i], spp);
	}

	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp) 
{
	pl->nblks = spp->blks_per_pl;
	pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
	for(int i = 0; i < pl->nblks; i++) {
		ssd_init_nand_block(&pl->blk[i], spp);
	}	
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
	lun->npls = spp->pls_per_lun;
	lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
	for(int i = 0; i < lun->npls; i++) {
		ssd_init_nand_plane(&lun->pl[i], spp);
	}
}

// FIXME
static void ssd_init_lines(struct line *line, struct ssdparams *spp)
{
	for(int i = 0; i < spp->tt_lines; i++) {
		line[i].id = i;
		line[i].ipc = 0;
		line[i].vpc = 0;
	}
	return;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp) 
{
	struct nand_lun *lun = NULL;
	struct pool *pool = NULL;

	ch->nluns = spp->luns_per_ch;
	ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
	for(int i = 0; i < ch->nluns; i++) {
		//FIXME: 
		lun = &ch->lun[i];
		lun->next_lun_avail_time = 0;
		lun->busy = false;

		pool = lun->pool = g_malloc0(sizeof(struct pool));
		pool->tt_lines = spp->tt_lines;
		pool->free_line_cnt = spp->tt_lines;
		pool->victim_line_cnt = 0;
		pool->full_line_cnt = 0;
		pool->open_line = 0;

		pool->lines = g_malloc0(sizeof(struct line) * pool->tt_lines);

		ssd_init_lines(pool->lines, spp);
	
		ssd_init_nand_lun(&ch->lun[i], spp);
	}
}

static void ssd_init_maptbl(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;

	ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);

	for(int i = 0; i < spp->tt_pgs; i++) {
		ssd->maptbl[i].ppa = UNMAPPED_PPA;
	}
}

static void ssd_init_rmap(struct ssd *ssd) 
{
	struct ssdparams *spp = &ssd->sp;

	ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
	for(int i = 0; i < spp->tt_pgs; i++) {
		ssd->rmap[i] = INVALID_LPN;
	}
}

// FIXME
static bool fdp_ssd_setup(struct ssd *ssd, FemuCtrl *n) 
{
	struct ssdparams *spp = &ssd->sp;
	NvmeNamespace *ns = n->namespaces;
	NvmeEnduranceGroup *endgrp = ns->endgrp;
	pool *pool = NULL;
	struct ssd_channel *ch = NULL;
	struct nand_lun *lun = NULL;
	int lines_per_ruh;
	int i, j;

	if( !endgrp->fdp.enabled ) {
		femu_debug("Can't support FDP mode\n");
		return false;
	}

	lines_per_ruh = spp->tt_lines / endgrp->fdp.nruh; 	

	for(i = 0; i < spp->nchs; i++) {
		ch = &ssd->ch[i];
		for(j = 0; j < spp->luns_per_ch; j++ ) {
			lun = &ch->lun[j];
			pool = lun->pool;
			pool->open_line = lines_per_ruh;
		}
	}

	return true;
}

// FIXME
static void ssd_init_ruh_pools(struct ssd *ssd, FemuCtrl *n) 
{
	struct ssdparams *spp = &ssd->sp;
	int nruh = spp->nruh;
	int lines_per_ruh = spp->tt_lines / nruh;

	ssd->ruh_pool = g_malloc0(sizeof(pool) * nruh);
	
	// Add log
	
	for (int i = 0; i < nruh; i++) {
		pool *pool = &ssd->ruh_pool[i];
		pool->ruh_id = i;
		pool->rg_id = i / (nruh / spp->nrg);
		pool->tt_lines = spp->tt_lines;

		pool->lines = g_malloc0(sizeof(struct line) * lines_per_ruh);
		
		QTAILQ_INIT(&pool->free_line_list);
		QTAILQ_INIT(&pool->full_line_list);
		pool->victim_line_pq = pqueue_init(lines_per_ruh,
											victim_line_cmp_pri,
											victim_line_get_pri,
											victim_line_set_pri,
											victim_line_get_pos,
											victim_line_set_pos);

		pool->free_line_cnt = lines_per_ruh;
		for (int j = 0; j < lines_per_ruh; j++) {
			struct line *line = &pool->lines[j];
			line->id = i *lines_per_ruh + j;		
			line->ipc = 0;
			line->vpc = 0;
			QTAILQ_INSERT_TAIL(&pool->free_line_list, line, entry);
		}
		
	}	
}

void fdp_ssd_init(FemuCtrl *n) 
{
	struct ssd *ssd = n->ssd;
	struct ssdparams *spp = &ssd->sp;

	// FIXME
	// ftl_assert(ssd);

	ssd_init_params(spp, n);
	
	/* initialize ssd internal layout architecture */
	ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
	for(int i = 0; i < spp->nchs; i++) {
		ssd_init_ch(&ssd->ch[i], spp);
	}

	/* ruh pool */
	ssd_init_ruh_pools(ssd, n);

	/* RUH, RG, interface */
	fdp_ssd_setup(ssd, n);

	/* initialize maptbl */
	ssd_init_maptbl(ssd);

	/* initialize rmap */ 
	ssd_init_rmap(ssd);

	//ssd_init_write_pointer(ssd, n);

	qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n, QEMU_THREAD_JOINABLE);

}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, 
									struct nand_cmd *ncmd)
{
	int c = ncmd->cmd;
	uint64_t nand_stime;
	uint64_t lat = 0;
	/* request arrival time */
	uint64_t cmd_stime = (ncmd->stime == 0 ) ? \
				qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
	struct ssdparams *spp = &ssd->sp;
	struct nand_lun *lun = get_lun(ssd, ppa);

	switch (c) {
	case NAND_READ:
		/* read: perform NAND cmd first */
		nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
						lun->next_lun_avail_time;
		lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
		lat = lun->next_lun_avail_time - cmd_stime;
		break;
	case NAND_WRITE:
		break;
	case NAND_ERASE:
		break;
	default:
		ftl_err("Unsupported NAND command: 0x%x\n", c);
	}

	return lat;
}

// FIXME
/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa) 
{
	//struct ssdparams *spp = &ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;

	/* update corresponding page status */
	pg = get_pg(ssd, ppa);
	ftl_assert(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(ssd, ppa);
	//ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	//ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	
	return;
}

// FIXME
static inline void nvme_fdp_stat_inc(uint64_t *a, uint64_t b) 
{
	uint64_t ret = *a + b;
	*a = ret < *a ? UINT64_MAX : ret;
}

// FIXME
static uint64_t ssd_write(FemuCtrl *n, NvmeRequest *req)
{
	struct ssd *ssd = n->ssd;
	struct ssdparams *spp = &ssd->sp;
	struct ppa ppa;
	// NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
	struct NvmeNamespace *ns = n->namespaces;
	struct NvmeEnduranceGroup *endgrp = ns->endgrp;
	pool *pool = NULL;

	uint32_t dw12 = le32_to_cpu(req->cmd.cdw12);
	uint8_t dtype = (dw12  >> 20 ) & 0xf;
	//uint16_t pid = le16_to_cpu(rw->dspec);
	uint16_t pid = (uint16_t) (le32_to_cpu(req->cmd.cdw13) >> 16);
	uint16_t ph, rg;

	int len = req->nlb;
	uint64_t lba = req->slba;
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t written_bytes = 0;
	uint64_t maxlat = 0;

	if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT ||
		!nvme_parse_pid(ns, pid, &ph, &rg)) {
		ph = 0;	
		rg = 1;
	}

	pool = &ssd->ruh_pool[ph];
	if (pool->rg_id != rg) {
		ftl_err("RUH %d belongs to RG %d, but request specifies RG %d\n",
					ph, pool->rg_id, rg);
	}

	// FIXME
	// update EnduranceGroup hbmw/mbmw 
	written_bytes = (uint64_t) len * spp->secsz;
	nvme_fdp_stat_inc(&endgrp->fdp.hbmw, written_bytes);
	nvme_fdp_stat_inc(&endgrp->fdp.mbmw, written_bytes);

	if (end_lpn >= spp->tt_pgs) {
		ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
	}

	/* should gc */
	// while (should_gc_high(ssd)) ...

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		ppa = get_maptbl_ent(ssd, lpn);
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(ssd, &ppa);
			// set_rmap_ent(ssd, INVALID_LPN, &ppa);
		}
		
		//struct nand_cmd swr;
		//swr.type = USER_IO;
		//swr.cmd = NAND_WRITE;
		//swr.stime = req->stime;
		/* get latency statistics */
		// curlat = 
		// maxlat = 
	}

	return maxlat;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
	struct ssdparams *spp = &ssd->sp;
	uint64_t lba = req->slba;	
	int nsecs = req->nlb;
	struct ppa ppa;
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t sublat, maxlat = 0;
	
	if (end_lpn >= spp->tt_pgs) {
		ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
	}

	/* normal IO read path */ 
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		ppa = get_maptbl_ent(ssd, lpn);
		if( !mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa) ) {
			continue;
		}

		struct nand_cmd srd;
		srd.type = USER_IO;
		srd.cmd = NAND_READ;
		srd.stime = req->stime;
		sublat = ssd_advance_status(ssd, &ppa, &srd);
		maxlat = (sublat > maxlat) ? sublat : maxlat;
	}

	return maxlat;
}

static void *ftl_thread(void *arg)
{
	FemuCtrl *n = (FemuCtrl *)arg;
	struct ssd *ssd = n->ssd;
	NvmeRequest *req = NULL;
	uint64_t lat = 0;
	int rc;
	int i;

	while (!*(ssd->dataplane_started_ptr)) {
		usleep(100000);
	}

	ssd->to_ftl = n->to_ftl;
	ssd->to_poller = n->to_poller;

	while(1) {
		// FIXME
		for(i = 1; i <= n->nr_pollers; i++) {
			if( !ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]) )
				continue;
			
			rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
			if( rc != 1) {
				printf("FEMU: FTL to_ftl dequeue failed\n");
			}
			ftl_assert(req);
			switch (req->cmd.opcode) {
			case NVME_CMD_WRITE:
				lat = ssd_write(n, req);			
				break;
			case NVME_CMD_READ:
				lat = ssd_read(ssd, req);
				break;
			case NVME_CMD_DSM:
				//FIXME:
				break;
			default:
				;
			}
			
			req->reqlat = lat;
			req->expire_time += lat;

			rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
			if (rc != 1) {
				ftl_err("FTL to_poller enqueue failed\n");
			}

			/* check gc */
			//if (should_gc(ssd)) {
				//do_gc(ssd, false);
			//}
		}
	}

	return NULL;
}
