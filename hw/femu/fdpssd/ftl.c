#include "ftl.h"

// FIXME:
static void ssd_init_write_pointer(struct ssd *ssd)
{
	//struct write_pointer *wpp = &ssd->wp;
	// struct line_mgmt *lm = &ssd->lm;
	// 
	return;
}

// FIXME:
// static void ssd_init_lines(struct ssd *ssd) 
// {
	//struct ssdparams *spp = &ssd->sp;
	//struct reclaim_group *gps = ssd->gps;
	//struct line_mgmt lm;
	//struct line *line;

	//for(int i = 0; i < spp->nrg; i++) {
		//lm = gps[i].lm;
		//lm.lines = g_malloc0(sizeof(struct line) * spp->tt_lines);
		//for(int j = 0; j < lm.tt_lines; j++) {
		//	line = &lm.lines[j];
		//	line->id = j;
		//	line->ipc = 0;
		//	line->vpc = 0;
		//	lm.free_line_cnt++;
		//}
		//lm.victim_line_cnt = 0;
		//lm.full_line_cnt = 0;
	//}

	//return;
//}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n) 
{
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

	// FDP feature
	spp->nru = n->fdp_params.nr_ru;
	spp->nrg = n->fdp_params.nr_rg;
	spp->nruh = n->fdp_params.nr_ruh;
	spp->ruh_type = n->fdp_params.ruh_type;
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

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp) 
{
	ch->nluns = spp->luns_per_ch;
	ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
	for(int i = 0; i < ch->nluns; i++) {
		ssd_init_nand_lun(&ch->lun[i], spp);
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
	/* initialize all reclaim group */
	ssd->gps = g_malloc0(sizeof(struct reclaim_group) * spp->nrg);

	/* initialize maptbl */
	// ssd_init_maptbl(ssd);
	
	/* initialize all the lines */
	// ssd_init_lines(ssd);

	ssd_init_write_pointer(ssd);

	// FIXME
	// initalize all lines with RUH type
	if(spp->ruh_type == INIT_ISOLATED) 
		return;
	else 
		return;

	return;
}
