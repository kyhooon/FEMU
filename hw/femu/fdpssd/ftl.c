#include "ftl.h"

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
static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
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

// FIXME
static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
	ssd->maptbl[lpn] = *ppa;
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

// FIXME: 
/* move write pointer */

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
	if (n->fdp_params.nr_rg != 1) 
	{
		ftl_err("invalid RG count %d, only support 1\n", n->fdp_params.nr_rg);
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

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
	blk->npgs = spp->pgs_per_blk;
	blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
	for(int i = 0; i < blk->npgs; i++) {
		ssd_init_nand_page(&blk->pg[i], spp);
	}
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt = 0;
	blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp) 
{
	pl->nblks = spp->blks_per_pl;
	pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
	for(int i = 0; i < pl->nblks; i++) {
		ssd_init_nand_blk(&pl->blk[i], spp);
	}	
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
	lun->npls = spp->pls_per_lun;
	lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
	for(int i = 0; i < lun->npls; i++) {
		ssd_init_nand_plane(&lun->pl[i], spp);
	}
	lun->next_lun_avail_time = 0;
	lun->busy = false;
}

static void ssd_init_lines(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
	struct line_mgmt *lm = &ssd->lm;
	struct line *line;

	lm->tt_lines = spp->blks_per_pl;
	ftl_assert(lm->tt_lines == spp->tt_lines);
	lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

	QTAILQ_INIT(&lm->free_line_list);
	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
			victim_line_get_pri, victim_line_set_pri,
			victim_line_get_pos, victim_line_set_pos);
	QTAILQ_INIT(&lm->full_line_list);

	lm->free_line_cnt = 0;
	for (int i = 0; i < lm->tt_lines; i++) {
		line = &lm->lines[i];
		line->id = i;
		line->ipc = 0;
		line->vpc = 0;
		line->pos = 0;

		// FIXME
		/* line ruh type */
		line->ruht = 0;
		/* ruhid */
		line->ruhid = -1;

		/* initialize all the lines as free lines */
		QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
		lm->free_line_cnt++;
	}

	ftl_assert(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp) 
{
	ch->nluns = spp->luns_per_ch;
	ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
	for(int i = 0; i < ch->nluns; i++) {
		ssd_init_nand_lun(&ch->lun[i], spp);
	}
	ch->next_ch_avail_time = 0;
	ch->busy = 0;
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
static void ssd_init_write_pointer(struct ssd *ssd) 
{
	int i;
	int ii_cnt = 0;
	int pi_cnt = 0;

	int pi_wp_start = 1;
	int wp_idx = 0;
	int *map = NULL;

	struct line_mgmt *lm = &ssd->lm;
	struct ssdparams *spp = &ssd->sp;
	struct line *curline = NULL;
	struct NvmeRuHandle *ruh = NULL;
	struct write_pointer *wpp = NULL;
	
	for (i = 0; i < spp->nruh; i++) {
		ruh = &ssd->ruhs[i];
		if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) 
			pi_cnt++;
		else 
			ii_cnt++;
	}

	/* ruh_index <-> wp_index mapping */
	map = ssd->ruhmap = g_malloc0(sizeof(int) * spp->nruh);

	/* [0] ii_type_wp, [1 ~ nruh-1] pi_type_wp */
	ssd->wp = g_malloc0(sizeof(struct write_pointer) * (1 + pi_cnt));

	if (ii_cnt > 0) {
		wpp = &ssd->wp[0];
		curline = QTAILQ_FIRST(&lm->free_line_list);
		QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
		lm->free_line_cnt--;

		curline->ruht = 1;
		
		wpp->curline = curline;
		wpp->ch = 0;
		wpp->lun = 0;
		wpp->pg = 0;
		wpp->blk = wpp->curline->id;
		wpp->pl = 0;
		wpp->type = 1;
	}

	int pi_allocated = 0;
	for (i = 0; i < spp->nruh; i++) {

		ruh = &ssd->ruhs[i];

		if (ruh->ruht == NVME_RUHT_INITIALLY_ISOLATED) {
			// ii type is mapped to idx 0
			map[i] = 0;
			// wpp = &ssd->wpp[0];

		} else {
			// pi type allocates an independent write pointer
			wp_idx = pi_wp_start + pi_allocated; 

			wpp = NULL;
			wpp = &ssd->wp[wp_idx];
			curline = QTAILQ_FIRST(&lm->free_line_list);
			QTAILQ_REMOVE(&lm->free_line_list, curline, entry);

			/* pi type */
			curline->ruht = 2;
			curline->ruhid = i;		

			lm->free_line_cnt--;
			
			wpp->curline = curline;
			wpp->ch = 0;
			wpp->lun = 0;
			wpp->pg = 0;
			wpp->blk = wpp->curline->id;
			wpp->pl = 0;
			wpp->type = 2;

			map[i] = wp_idx;
			
			pi_allocated++;
		}
	} 
}

void fdp_ssd_init(FemuCtrl *n) 
{
	struct ssd *ssd = n->ssd;
	struct ssdparams *spp = &ssd->sp;
	struct NvmeNamespace *ns = n->namespaces; 
	struct NvmeEnduranceGroup *endgrp = ns->endgrp; 

	ssd_init_params(spp, n);
	
	/* initialize ssd internal layout architecture */
	ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
	for(int i = 0; i < spp->nchs; i++) {
		ssd_init_ch(&ssd->ch[i], spp);
	}

	/* initialize ruhs */
	ssd->ruhs = endgrp->fdp.ruhs;

	/* initialize maptbl */
	ssd_init_maptbl(ssd);

	/* initialize rmap */ 
	ssd_init_rmap(ssd);

	/* initialize all the lines */
	ssd_init_lines(ssd);

	/* initialize write pointer */
	/* ii / pi write pointer */
	ssd_init_write_pointer(ssd);

	/* initialize hostWrite, GCWrite */
	ssd->hostWrite = 0;
	ssd->GCWrite = 0;

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
		// FIXME
		nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
					lun->next_lun_avail_time;
		if (ncmd->type == USER_IO) {
			lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
		} else {
			lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
		}
		lat = lun->next_lun_avail_time - cmd_stime;
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

	if (ph >= spp->nruh) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	//NvmeRuHandle *ruh = &ssd->ruhs[ph];

	// FIXME
	// update EnduranceGroup hbmw/mbmw 
	written_bytes = (uint64_t) len * spp->secsz;
	nvme_fdp_stat_inc(&endgrp->fdp.hbmw, written_bytes);
	nvme_fdp_stat_inc(&endgrp->fdp.mbmw, written_bytes);

	//ruh->total_writes += written_bytes;

	if (end_lpn >= spp->tt_pgs) {
		ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
	}

	/* should gc */
	// while (should_gc_high(ssd)) ...

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		ppa = get_maptbl_ent(ssd, lpn);

		if (mapped_ppa(&ppa)) {
			// FIXME
			/* update old page information first */
			mark_page_invalid(ssd, &ppa);
			// set_rmap_ent(ssd, INVALID_LPN, &ppa);
		}

		// FIXME fdp_get_new_ppa
		// struct ppa new_ppa = fdp_get_new_ppa(ssd, ruh);
		struct ppa new_ppa;
		new_ppa.ppa = UNMAPPED_PPA;
	
		// FIXME ppa2pgidx
		ssd->maptbl[lpn] = new_ppa;
		ssd->rmap[ppa2pgidx(ssd, &new_ppa)] = lpn;
		
		// FIXME ssd_advance_status
		struct nand_cmd swr;
		swr.type = USER_IO;
		swr.cmd = NAND_WRITE;
		swr.stime = req->stime;
		uint64_t lat = ssd_advance_status(ssd, &new_ppa, &swr);
		maxlat = (lat > maxlat) ? lat : maxlat;

		/* hostWrite */
		ssd->hostWrite += 1;
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
