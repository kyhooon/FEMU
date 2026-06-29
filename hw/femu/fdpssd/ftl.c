#include "ftl.h"

/* WAF */
static FILE *WAFData = NULL;

// FIXME
/* per_ruh_WAF */
static FILE *ruhstat = NULL;

static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd)
{
	return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd) 
{
	return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline void check_addr(int a, int max) 
{
	ftl_assert(a >= 0 && a < max);
}

static inline bool nvme_ph_valid(NvmeNamespace *ns, uint16_t ph) 
{	
	return ph < ns->fdp.nphs;
}

static inline bool nvme_rg_valid(NvmeEnduranceGroup *endgrp, uint16_t rg) 
{
	return rg < endgrp->fdp.nrg;
}

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

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa) 
{
	uint64_t pgidx = ppa2pgidx(ssd, ppa);

	return ssd->rmap[pgidx];
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

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
	return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa) 
{
	struct ssd_channel *ch = get_ch(ssd, ppa);
	return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa) 
{
	struct nand_lun *lun = get_lun(ssd, ppa);
	return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa) 
{
	struct nand_plane *pl = get_pl(ssd, ppa);
	return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa) 
{
	return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
	struct nand_block *blk = get_blk(ssd, ppa);
	return &(blk->pg[ppa->g.pg]);
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
	ftl_assert(lpn < ssd->sp.tt_pgs);
	ssd->maptbl[lpn] = *ppa;
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn) 
{
	return ssd->maptbl[lpn];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa) 
{
	uint64_t pgidx = ppa2pgidx(ssd, ppa);
	
	ssd->rmap[pgidx] = lpn;
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

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn) 
{
	return (lpn < ssd->sp.tt_pgs);
}

static struct line *get_next_free_line(struct ssd *ssd) 
{
	struct line_mgmt *lm = &ssd->lm;
	struct line *curline = NULL;
	
	curline = QTAILQ_FIRST(&lm->free_line_list);
	if (!curline) {
		ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
		return NULL;
	}
	
	QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
	lm->free_line_cnt--;
	return curline;
}

static inline uint64_t fdp_get_timestamp(void) 
{
	struct timespec ts;
	uint64_t now;

	clock_gettime(CLOCK_REALTIME, &ts);

	now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	
	return cpu_to_le64(now);
}

static NvmeFdpEvent *ftl_fdp_alloc_event(struct ssd *ssd, 
											NvmeFdpEventBuffer *ebuf)
{
	NvmeFdpEvent *ret;
	bool is_full = ebuf->next == ebuf->start && ebuf->nelems;

	ret = &ebuf->events[ebuf->next++];
	if (unlikely(ebuf->next == NVME_FDP_MAX_EVENTS)) {
		ebuf->next = 0;
	}

	if (is_full) {
		ebuf->start = ebuf->next;
	} else {
		ebuf->nelems++;
	}

	memset(ret, 0, sizeof(NvmeFdpEvent));
	ret->timestamp = fdp_get_timestamp();

	return ret;
}

// FIXME: 
static void ssd_advance_write_pointer(struct ssd *ssd, uint16_t ph)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
	struct write_pointer *wpp = NULL;

	/* ruhmap -> wp_idx */
	//int wp_idx = ssd->ruhmap[ph];
	//wpp = &ssd->wp[wp_idx];
	wpp = &ssd->wp[ph];

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }

				wpp->curline->ruht = wpp->type;
				wpp->curline->ruhid = ph;
				
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd, int ruhid)
{
	struct write_pointer *wpp = NULL;
	struct ppa ppa;

	/* ph (ruh_id) to obtain the corresponding wp_idx */
	//wp_idx = ssd->ruhmap[ph];
	//wpp = &ssd->wp[wp_idx];
	wpp = &ssd->wp[ruhid];

	ppa.ppa = 0;
	ppa.g.ch = wpp->ch;
	ppa.g.lun = wpp->lun;
	ppa.g.pg = wpp->pg;
	ppa.g.blk = wpp->blk;
	ppa.g.pl = wpp->pl;
	ftl_assert(ppa.g.pl == 0);

	return ppa;
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
	if (n->fdp_params.nr_rg != 1) 
	{
		ftl_err("invalid RG count %d, only support 1\n", n->fdp_params.nr_rg);
		abort();
	}
	spp->nrg = n->fdp_params.nr_rg;
	spp->nruh = n->fdp_params.nr_ruh;
	spp->ruh_type = n->fdp_params.ruh_type;

	/* Mixed II/PI placement policy */
	spp->ruh_placement_policy = n->fdp_params.ruh_placement_policy;
	{
		int nruh = spp->nruh;
		switch (spp->ruh_placement_policy) {
		case FDP_POLICY_STATIC_HALF: 
			spp->ii_ruh_cnt = nruh / 2;
			break;

		case FDP_POLICY_RATIO_SPLIT: 
			int cnt = (nruh * n->fdp_params.ii_ruh_ratio + 99) / 100;
			spp->ii_ruh_cnt = (cnt < 0) ? 0 : (cnt > nruh) ? nruh : cnt;
			break; 
		
		case FDP_POLICY_WORKLOAD_AWARE: 
			spp->ii_ruh_cnt = nruh / 2;
			break; 
		
		default: 
			spp->ii_ruh_cnt = (spp->ruh_type == NVME_RUHT_INITIALLY_ISOLATED) 
							? nruh : 0;
			break;
		}
	}

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
	
	struct line_mgmt *lm = &ssd->lm;
	struct ssdparams *spp = &ssd->sp;
	struct line *curline = NULL;
	struct NvmeRuHandle *ruh = NULL;
	struct write_pointer *wpp = NULL;

	ssd->wp = g_malloc0(sizeof(struct write_pointer) * (spp->nruh));

	for (i = 0; i < spp->nruh; i++) {
		
		ruh = &ssd->ruhs[i];
		wpp = &ssd->wp[i];
		curline = QTAILQ_FIRST(&lm->free_line_list);
		QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
		lm->free_line_cnt--;
		
		wpp->curline = curline;
		if (ruh->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
			curline->ruht = 2;
			wpp->type = 2;
		} else {
			curline->ruht = 1;
			wpp->type = 1;
		}

		curline->ruhid = i;
		wpp->ch = 0;
		wpp->lun = 0;
		wpp->pg = 0;
		wpp->blk = wpp->curline->id;
		wpp->pl = 0;

	}
}

static void ssd_init_workload_stats(struct ssd *ssd) 
{
	struct workload_stats *wl = &ssd->wl_stats;

	wl->seq_writes 		= 0;
	wl->rand_writes 	= 0;
	wl->total_writes 	= 0;
	wl->overwrite_cnt 	= 0;
	wl->last_write_lpn	= 0;
	wl->last_lpn_valid	= false;
	wl->ii_rr			= 0;
	wl->pi_rr			= 0;
}

/* 
 * Policy A : Static Half
 * Policy B : Workload-Aware */
static void wl_update_stats(struct ssd *ssd, uint64_t start_lpn, 
												uint64_t end_lpn)
{
	struct workload_stats *wl = &ssd->wl_stats;
	bool is_sequential;

	is_sequential = wl->last_lpn_valid &&
		(start_lpn == wl->last_write_lpn + 1 ||
		 start_lpn == wl->last_write_lpn);

	wl->last_is_sequential = is_sequential;

	if (is_sequential)
		wl->seq_writes++;
	else
		wl->rand_writes++;
	wl->total_writes++;
	wl->last_write_lpn = end_lpn;
	wl->last_lpn_valid = true;

	if (ssd->lpnCount[start_lpn] > 0)
		wl->overwrite_cnt++;
}

/* Select which RUH to use when the host did not supply an explicit PID */
static uint16_t wl_select_ruh(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
	struct workload_stats *wl = &ssd->wl_stats;
	int ii_cnt = spp->ii_ruh_cnt;
	int pi_cnt = spp->nruh - ii_cnt;

	bool is_sequential = wl->last_is_sequential;

	if (spp->ruh_placement_policy == FDP_POLICY_WORKLOAD_AWARE &&
			wl->total_writes >= 1000) {
		uint64_t seq_pct = wl->seq_writes * 100 / wl->total_writes;

		if (seq_pct >= WL_SEQ_THRESHOLD_PCT) {
			if (ii_cnt > 0)
				return (uint16_t)(wl->ii_rr++ % ii_cnt);
			return (uint16_t)(wl->pi_rr++ % spp->nruh);
		}
		if (seq_pct <= WL_RAND_THRESHOLD_PCT) {
			if (pi_cnt > 0)
				return (uint16_t)(ii_cnt + wl->pi_rr++ % pi_cnt);
			return (uint16_t)(wl->ii_rr++ % spp->nruh);
		}
		return (uint16_t)(wl->total_writes % spp->nruh);
	}

	if (is_sequential) {
		if (ii_cnt > 0)
			return (uint16_t)(wl->ii_rr++ % ii_cnt);
		return (uint16_t)(wl->pi_rr++ % spp->nruh);
	}
	if (pi_cnt > 0)
		return (uint16_t)(ii_cnt + wl->pi_rr++ % pi_cnt);
	return (uint16_t)(wl->ii_rr++ % spp->nruh);
} 

static void ssd_init_lpn_tracking(struct ssd *ssd) 
{
	struct ssdparams *spp = &ssd->sp;
	
	ssd->lpnCount = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);

	for (int i = 0; i < spp->tt_pgs; i++) {
		ssd->lpnCount[i] = 0;
	}
}

void fdp_ssd_init(FemuCtrl *n) 
{
	struct ssd *ssd = n->ssd;
	struct ssdparams *spp = &ssd->sp;
	struct NvmeNamespace *ns = n->namespaces; 
	struct NvmeEnduranceGroup *endgrp = ns->endgrp; 

	// ctrl : back pointer to the FemuCtrl structure 
	ssd->ctrl = n;

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
	/* --- FDP --- */
	ssd_init_write_pointer(ssd);

	/* lpn tracking */
	ssd_init_lpn_tracking(ssd);

	/* Workload characterization stats */ 
	ssd_init_workload_stats(ssd);

	/* initialize WAF counters */
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

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa) 
{
	struct line_mgmt *lm = &ssd->lm;
	struct ssdparams *spp = &ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(ssd, ppa);
	ftl_assert(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(ssd, ppa);
	ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(ssd, ppa);
	ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		ftl_assert(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		QTAILQ_REMOVE(&lm->full_line_list, line, entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}

	if (line->ruhid >= 0 && ssd->ruhs[line->ruhid].valid_pages > 0)
		ssd->ruhs[line->ruhid].valid_pages--;
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa) 
{
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(ssd, ppa);
	ftl_assert(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(ssd, ppa);
	ftl_assert(blk->vpc >= 0 && blk->vpc < spp->sp.pgs_per_blk);
	blk->vpc++;
	
	/* update corresponding line status */
	line = get_line(ssd, ppa);
	ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
	line->vpc++;

	/* track current valid-page count per RUH */ 
	if (line->ruhid >= 0) 
		ssd->ruhs[line->ruhid].valid_pages++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa) 
{
	struct ssdparams *spp = &ssd->sp;
	struct nand_block *blk = get_blk(ssd, ppa);
	struct nand_page *pg = NULL;
	
	for (int i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		ftl_assert(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	ftl_assert(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

// FIXME
static inline void nvme_fdp_stat_inc(uint64_t *a, uint64_t b) 
{
	uint64_t ret = *a + b;
	*a = ret < *a ? UINT64_MAX : ret;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
	struct line_mgmt *lm = &ssd->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		return NULL;
	}

	if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa) 
{
	/* advance ssd status, we don't care about how long it takes */
	if (ssd->sp.enable_gc_delay) {
		struct nand_cmd gcr;
		gcr.cmd = NAND_READ;
		gcr.stime = 0;
		ssd_advance_status(ssd, ppa, &gcr);
	}
	return ;
}

// FIXME
/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, int ruhid)
{
	struct ppa new_ppa;
	struct nand_lun *new_lun;
	uint64_t lpn = get_rmap_ent(ssd, old_ppa);
	
	ftl_assert(valid_lpn(ssd, lpn));
	new_ppa = get_new_page(ssd, ruhid);
	/* update maptbl */
	set_maptbl_ent(ssd, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(ssd, lpn, &new_ppa);

	mark_page_valid(ssd, &new_ppa);

	/* need to advance the write pointer here */
	ssd_advance_write_pointer(ssd, ruhid);

	if (ssd->sp.enable_gc_delay) {
		struct nand_cmd gcw;
		gcw.type = GC_IO;
		gcw.cmd = NAND_WRITE;
		gcw.stime = 0;
		ssd_advance_status(ssd, &new_ppa, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(ssd, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

	new_lun = get_lun(ssd, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;

	return 0;
}

// FIXME
/* here ppa identifies the block we want to clean */
static int clean_one_block(struct ssd *ssd, struct ppa *ppa, int ruhid) 
{
	struct ssdparams *spp = &ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	
	for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		ftl_assert(pg_iter->status != PG_FREE); 
		if (pg_iter->status == PG_VALID) {
			gc_read_page(ssd, ppa);
			/* delay the maptbl update until "write" happens */
			gc_write_page(ssd, ppa, ruhid);
			cnt++;

			/* WAF */
			ssd->GCWrite++;
		}
	}

	ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
	return cnt;
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa) 
{
	struct line_mgmt *lm = &ssd->lm;
	struct line *line = get_line(ssd, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* fdp */
	line->ruht = 0;
	line->ruhid = -1;

	/* move this line to free line list */
	QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
	lm->free_line_cnt++;
}

// FIXME 
static int do_gc(struct ssd *ssd, bool force) 
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &ssd->sp;
	struct nand_lun *lunp;
	//NvmeRuHandle *ruh;
	FemuCtrl *n = ssd->ctrl;
	NvmeNamespace *ns = n->namespaces;
	NvmeEnduranceGroup *endgrp = ns->endgrp;

	struct ppa ppa;
	int ch, lun;
	int vpc_cnt = 0;
	int blk_cnt = 0;

	victim_line = select_victim_line(ssd, force);
	if (!victim_line) {
		return -1;
	}

	// FIXME: 
	// ruht == 1 (INITIALLY_ISOLATED)
	// ruht == 2 (PERSISTENTLY_ISOLATED)
	int dest_ruhid = 0;
	ftl_assert(victim_line->ruhid != -1);
	if (victim_line->ruht == NVME_RUHT_INITIALLY_ISOLATED) {
		/* II: GC data goes to the last II-type RUH */
		dest_ruhid = (spp->ii_ruh_cnt > 0) ? spp->ii_ruh_cnt - 1
											: spp->nruh - 1;
	} else if (victim_line->ruht == NVME_RUHT_PERSISTENTLY_ISOLATED) {
		dest_ruhid = victim_line->ruhid;
	} else {
		ftl_err("Underfined RUHT %d\n", victim_line->ruht);
		dest_ruhid = 0;
	}

	ppa.g.blk = victim_line->id;
	ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

	/* copy back valid data */
	for (ch = 0; ch < spp->nchs; ch++) {
		for(lun = 0; lun < spp->luns_per_ch; lun++) {
			ppa.g.ch = ch;
			ppa.g.lun = lun;
			ppa.g.pl = 0;
	
			lunp = get_lun(ssd, &ppa);
			// FDP: record media write operation 
			vpc_cnt += clean_one_block(ssd, &ppa, dest_ruhid);
			mark_block_free(ssd, &ppa);
			
			// FDP: record 'mbe' information for the ruh
			blk_cnt++;

			if (spp->enable_gc_delay) {
				struct nand_cmd gce;
				gce.type = GC_IO;
				gce.cmd = NAND_ERASE;
				gce.stime = 0;
				ssd_advance_status(ssd, &ppa, &gce);
			}
			lunp->gc_endtime = lunp->next_lun_avail_time;
		}
	}

	uint64_t gc_bytes = (uint64_t)vpc_cnt * spp->secsz * spp->secs_per_pg;
	//nvme_fdp_stat_inc(&ssd->ruhs[victim_line->ruhid].GCWrite, gc_bytes);
	/* per-RUH GC write in page (same unit as global ssd->GCWrite) */
	nvme_fdp_stat_inc(&ssd->ruhs[victim_line->ruhid].GCWrite, (uint64_t)vpc_cnt);

	uint64_t erase_bytes = (uint64_t)blk_cnt * spp->secs_per_blk * spp->secsz;
	nvme_fdp_stat_inc(&endgrp->fdp.mbmw, gc_bytes);
	nvme_fdp_stat_inc(&endgrp->fdp.mbe, erase_bytes);

	/* per-ruh GC metrics stored in NvmeRuHandle */
	ssd->ruhs[victim_line->ruhid].gc_count++;

	// FDP event
	NvmeRuHandle *ruh = &ssd->ruhs[victim_line->ruhid];
	if (ruh->event_filter >> nvme_fdp_evf_shifts[FDP_EVT_RUH_IMPLICIT_RU_CHANGE] & 0x1) {
		NvmeFdpEvent *e = ftl_fdp_alloc_event(ssd, &endgrp->fdp.ctrl_events);
		e->type = FDP_EVT_RUH_IMPLICIT_RU_CHANGE;
		e->flags = FDPEF_LV;
		e->rgid = cpu_to_le16(0);
		e->ruhid = victim_line->ruhid;
	}

	/* update line status */
	mark_line_free(ssd, &ppa);
	
	return 0;
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
	uint64_t curlat = 0, maxlat = 0;
	int r;

	wl_update_stats(ssd, start_lpn, end_lpn);

	if (dtype != NVME_DIRECTIVE_DATA_PLACEMENT || !nvme_parse_pid(ns, pid, &ph, &rg)) {
		if (dtype == NVME_DIRECTIVE_DATA_PLACEMENT) {
			// Invalid Placement Identifier 
			NvmeRuHandle *ruh = &ssd->ruhs[0];
			if (ruh->event_filter >> nvme_fdp_evf_shifts[FDP_EVT_INVALID_PID] & 0x1) {
				NvmeFdpEvent *e = ftl_fdp_alloc_event(ssd, &endgrp->fdp.host_events);
				e->type = FDP_EVT_INVALID_PID;
				e->flags = FDPEF_PIV | FDPEF_NSIDV;
				e->pid = cpu_to_le16(pid);
				e->nsid = cpu_to_le32(ns->id);
			}
		}
		// use for Filebech workload 
		ph = wl_select_ruh(ssd);
		rg = 0;
	}

	if (ph >= spp->nruh) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	// FIXME
	// update EnduranceGroup hbmw/mbmw 
	written_bytes = (uint64_t) len * spp->secsz;
	nvme_fdp_stat_inc(&endgrp->fdp.hbmw, written_bytes);
	nvme_fdp_stat_inc(&endgrp->fdp.mbmw, written_bytes);

	// FIXME
	// per_ruh_WAF
	//nvme_fdp_stat_inc(&ssd->ruhs[ph].hostWrite, written_bytes);
	//nvme_fdp_stat_inc(&ssd->ruhs[ph].GCWrite, written_bytes);
	nvme_fdp_stat_inc(&ssd->ruhs[ph].hostWrite, end_lpn - start_lpn + 1);

	if (end_lpn >= spp->tt_pgs) {
		ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
	}

	/* should gc */
	while (should_gc_high(ssd)) {
		/* perform GC here until !should_gc(ssd) */
		r = do_gc(ssd, true);
		if (r == -1)
			break;
	}

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		ppa = get_maptbl_ent(ssd, lpn);
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(ssd, &ppa);
			set_rmap_ent(ssd, INVALID_LPN, &ppa);
		}

		/* new write */
		ppa = get_new_page(ssd, ph);
		/* update maptbl */
		set_maptbl_ent(ssd, lpn, &ppa);
		/* update rmap */
		set_rmap_ent(ssd, lpn, &ppa);

		mark_page_valid(ssd, &ppa);

		/* need to advance the write pointer here */
		ssd_advance_write_pointer(ssd, ph);
		
		struct nand_cmd swr;
		swr.type = USER_IO;
		swr.cmd = NAND_WRITE;
		swr.stime = req->stime;
		/* get latency statistics */
		curlat = ssd_advance_status(ssd, &ppa, &swr);
		maxlat = (curlat > maxlat) ? curlat : maxlat;

		/* lpnCount */
		ssd->lpnCount[start_lpn] += 1;

		/* hostWrite */
		ssd->hostWrite += 1;
	}

	// When the RU exhausts its allocated LBAs, 
	// reset ru->ruamw to ruh->ruamw ;
	NvmeRuHandle *ruh = NULL;
	NvmeReclaimUnit *ru = NULL;
	ruh = &ssd->ruhs[ph];
	ru = &ruh->rus[0];
	while (len) {
		if (len < ru->ruamw) {
			ru->ruamw -= len;
			break;
		}
		len -= ru->ruamw;
		ru->ruamw = ruh->ruamw;
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
	struct ssdparams *spp = &ssd->sp;
	NvmeRequest *req = NULL;
	uint64_t lat = 0;
	int rc;
	int i;

	/* lpnCount */
	bool workload_ended = false;
	uint64_t start_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	uint64_t last_io_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
	
	/* WAF */
	bool is_first = false;
	double WAF = 0.0;
	WAFData = fopen("/home/cpslab/WAFData.csv", "w");
	if (WAFData == NULL) {
		ftl_err("Failed to open WAFData.csv\n");
	}
	fprintf(WAFData, "time(s), hostWrite, GCWrite, WAF\n");

	// FIXME
	/* per_ruh_WAF */
	ruhstat = fopen("/home/cpslab/ruhstat.csv", "w");
	if (ruhstat == NULL) {
		ftl_err("Failed to open ruhstat.csv\n");
	}
	fprintf(ruhstat, "ruhid, hostwrite, GCWrite, WAF\n");

	while (!*(ssd->dataplane_started_ptr)) {
		usleep(100000);
	}

	ssd->to_ftl = n->to_ftl;
	ssd->to_poller = n->to_poller;

	while(1) {

		for(i = 1; i <= n->nr_pollers; i++) {
			if( !ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]) )
				continue;

			/* lpnCount */
			last_io_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
			
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

			/* WAF */
			uint64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
			uint64_t timestamp = current_time - start_time;

			if (timestamp >= 1000000000ULL) {
				WAF = ssd->hostWrite > 0 ?
									(double) (ssd->hostWrite + ssd->GCWrite) / (double) ssd->hostWrite : 0;
				double current = (double) current_time / 1000000000.0;

				if (!is_first) {
					femu_log("workload timestamp: %7.2f\n", current);
					is_first = true;
				}

				fprintf(WAFData, "%7.2f,%9lu,%9lu,%8.5f\n", current, ssd->hostWrite, ssd->GCWrite, WAF); 
				fflush(WAFData);

				struct workload_stats *wl = &ssd->wl_stats;
				uint64_t seq_pct = wl->total_writes > 0 
								? wl->seq_writes * 100 / wl->total_writes : 0;
				femu_log("wl: seq=%lu rand=%lu overwrite=%lu seq_pct=%lu policy=%d\n", wl->seq_writes, wl->rand_writes, wl->overwrite_cnt, seq_pct, spp->ruh_placement_policy);

				start_time = current_time;
			}

			/* clean one line if needed (in the background) */
			if (should_gc(ssd)) {
				do_gc(ssd, false);
			}
		}

		/* lpnCount */
		if (!workload_ended) {
			uint64_t check_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
			/* 1 minutes */
			if ((check_time - last_io_time) > 60000000000ULL) {
				workload_ended = true; 
				
				FILE *accessCount = fopen("/home/cpslab/lpnCount.csv", "w");
				if (accessCount == NULL)
					ftl_err("Failed to open lpnCount.csv\n");

				for (uint64_t j = 0; j < ssd->sp.tt_pgs; j++) {
					fprintf(accessCount, "%7lu, %7lu\n", j, ssd->lpnCount[j]);
				}
				fflush(accessCount);
				fclose(accessCount);
				femu_log("lpnCount successfully recorded !\n");

				/* per_ruh WAF */
				//fprintf(ruhstat, "ruhid,ruht,host_writes,gc_writes,gc_count,valid_pages,WAF\n");
				fprintf(ruhstat, "ruhid,ruht,host_writes(pages),gc_writes(pages),gc_count,valid_pages,WAF\n");
				for (int ruhid = 0; ruhid < spp->nruh; ruhid++) {
					NvmeRuHandle *ruh = &ssd->ruhs[ruhid];
					double ruh_waf = ruh->hostWrite > 0 
						? (double)(ruh->hostWrite + ruh->GCWrite) / ruh->hostWrite 
						: 0.0;
					const char *type_str = (ruh->ruht == NVME_RUHT_INITIALLY_ISOLATED)
										? "II" : "PI";
					fprintf(ruhstat, "%d,%s,%lu,%lu,%lu,%lu,%.2f\n",
								ruhid, type_str,
								ruh->hostWrite, ruh->GCWrite,
								ruh->gc_count, ruh->valid_pages,
								ruh_waf);
				} 
				fflush(ruhstat);
				fclose(ruhstat);
				femu_log("ruh write stats successfully recorded !\n");

			}

		} 
	}

	fclose(WAFData);

	return NULL;
}
