#include "../nvme.h"
#include "./ftl.h"

static void fdp_init_ctrl_str(FemuCtrl *n)
{
	static int fsid_vfdp = 0;
	const char *vfdpssd_mn = "FEMU FDP-SSD Controller";
	const char *vfdpssd_sn = "vSSD";

	// FIXME: fsid_vfdp is not sure whether supported fdp mode 
	nvme_set_ctrl_name(n, vfdpssd_mn, vfdpssd_sn, &fsid_vfdp);
}

/* fdpssd -> fdp-mode ssd */
static void fdp_init(FemuCtrl *n, Error **errp)
{
	struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

	fdp_init_ctrl_str(n);

	ssd->dataplane_started_ptr = &n->dataplane_started;
	ssd->ssdname = (char *)n->devname;
	femu_debug("Starting FEMU in FDP mode ...\n");
	fdp_ssd_init(n);
}

// FIXME: 
static void fdp_flip(FemuCtrl *n, NvmeCmd *cmd)
{

	struct ssd *ssd = n->ssd;
	int64_t cdw10 = le64_to_cpu(cmd->cdw10);

	switch (cdw10) {
		case FEMU_ENABLE_GC_DELAY:
			ssd->sp.enable_gc_delay = true;
			femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
			break;
		case FEMU_DISABLE_GC_DELAY:
			ssd->sp.enable_gc_delay = false;
			femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
			break;
		case FEMU_ENABLE_DELAY_EMU:
			ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
			ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
			ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
			ssd->sp.ch_xfer_lat = 0;
			femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
			break;
		case FEMU_DISABLE_DELAY_EMU:
			ssd->sp.pg_rd_lat = 0;
			ssd->sp.pg_wr_lat = 0;
			ssd->sp.blk_er_lat = 0;
			ssd->sp.ch_xfer_lat = 0;
			femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
			break;
		case FEMU_RESET_ACCT:
			n->nr_tt_ios = 0;
			n->nr_tt_late_ios = 0;
			femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
					n->nr_tt_late_ios, n->nr_tt_ios);
			break;
		case FEMU_ENABLE_LOG:
			n->print_log = true;
			femu_log("%s,Log print [Enabled]!\n", n->devname);
			break;
		case FEMU_DISABLE_LOG:
			n->print_log = false;
			femu_log("%s,Log print [Disabled]!\n", n->devname);
			break;
		default:
			printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
	}
}

// FIXME: 
static uint16_t fdp_dma_read(FemuCtrl *n, uint8_t *ptr, 
							uint32_t len, NvmeCmd *cmd) 
{
	uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
	uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
	
	return dma_read_prp(n, ptr, len, prp1, prp2);
}

// FIXME:
static uint16_t fdp_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
	switch (cmd->opcode) {
		case NVME_ADM_CMD_FEMU_FLIP: 
			fdp_flip(n, cmd);
			return NVME_SUCCESS;
		default:
			return NVME_INVALID_OPCODE | NVME_DNR;
	}
}

// FIXME:
static uint16_t fdp_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, 
		NvmeRequest *req) 
{
	return nvme_rw(n, ns, cmd, req);
}

static inline uint16_t nvme_make_pid(NvmeNamespace *ns, uint16_t rg, uint16_t ph) 
{
	uint16_t rgif = ns->endgrp->fdp.rgif;
	
	if (!rgif) {
		return ph;
	}

	return (rg << (16 - rgif)) | ph;
}

// FIXME : nvme fdp status
static uint16_t nvme_io_mgmt_recv_ruhs(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, size_t len) 
{
	unsigned int nruhsd;
	uint16_t rg, ph, *ruhid;
	uint16_t ret;
	size_t trans_len;
	uint8_t *buf = NULL;
	NvmeEnduranceGroup *endgrp = ns->endgrp;
	NvmeRuhStatus *hdr;
	NvmeRuhStatusDescr *ruhsd;

	if (!endgrp->fdp.enabled)
		return NVME_FDP_DISABLED | NVME_DNR;
	
	/* Only a single RG is supported */
	nruhsd = endgrp->fdp.nruh * endgrp->fdp.nrg;
	trans_len = sizeof(NvmeRuhStatus) + nruhsd *sizeof(NvmeRuhStatusDescr);
	buf = g_malloc0(trans_len);

	trans_len = MIN(trans_len, len);

	hdr = (NvmeRuhStatus *)buf;
	ruhsd = (NvmeRuhStatusDescr *)(buf + sizeof(NvmeRuhStatus));

	hdr->nruhsd = cpu_to_le16(nruhsd);

	ruhid = ns->fdp.phs;

	// FIXME
	for (ph = 0; ph < ns->fdp.nphs; ph++, ruhid++) {
		NvmeRuHandle *ruh = &endgrp->fdp.ruhs[*ruhid];

		for (rg = 0; rg < endgrp->fdp.nrg; rg++, ruhsd++) {
			uint16_t pid = nvme_make_pid(ns, rg, ph);
		
			ruhsd->pid = cpu_to_le16(pid);
			ruhsd->ruhid = *ruhid;
			ruhsd->earutr = 0;
			ruhsd->ruamw = cpu_to_le64(ruh->rus[rg].ruamw);
		}
	}
	ret = fdp_dma_read(n, (uint8_t *)buf, trans_len, cmd);

	if( ret ) {
		return ret;
	}
	return NVME_SUCCESS;

}

static uint16_t nvme_io_mgmt_recv(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd) 
{
	//NvmeCmd *cmd = &req->cmd;
	uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
	uint32_t numd = le32_to_cpu(cmd->cdw11);
	uint8_t mo = (cdw10 & 0xff);
	size_t len = (numd + 1) << 2;

	switch (mo) {
		case NVME_IOMR_MO_NOP:
			return 0;
		case NVME_IOMR_MO_RUH_STATUS:
			return nvme_io_mgmt_recv_ruhs(n, ns, cmd, len);
		default:
			return NVME_INVALID_FIELD | NVME_DNR;
	};
}

// FIXME:
static uint16_t fdp_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, 
		NvmeRequest *req)
{
	switch(cmd->opcode) {
		case NVME_CMD_READ:
		case NVME_CMD_WRITE:
			return fdp_nvme_rw(n, ns, cmd, req);
		case NVME_CMD_IO_MGMT_RECV:
			/* NVME_CMD_IO_MGMT_RECV = 0x12 */
			/* nvme-cli fdp status */
			return nvme_io_mgmt_recv(n, ns, cmd);
		case NVME_CMD_IO_MGMT_SEND:
			/* NVME_CMD_IO_MGMT_SEND = 0x1d */
			/* nvme-cli fdp ruh update */
		default:
			return NVME_INVALID_OPCODE | NVME_DNR;
	}
}

// FIXME: 
static uint16_t fdp_stats(FemuCtrl *n, uint32_t buf_len,
						uint64_t off, NvmeCmd *cmd) 
{
	NvmeNamespace *ns = n->namespaces;
	NvmeEnduranceGroup *endgrp = ns->endgrp;
	NvmeFdpLog log;
	uint32_t trans_len;
	uint16_t ret;

	if ( off >= sizeof(NvmeFdpLog) ) {
		femu_debug("invalid access\n");
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	if ( !endgrp->fdp.enabled ) {
		femu_debug("FDP disabled\n");
		return NVME_FDP_DISABLED | NVME_DNR;
	}

	trans_len = MIN(sizeof(log) - off, buf_len);

	log.hbmw[0] = cpu_to_le64(endgrp->fdp.hbmw);
	log.mbmw[0] = cpu_to_le64(endgrp->fdp.mbmw);
	log.mbe[0] = cpu_to_le64(endgrp->fdp.mbe);

	ret = fdp_dma_read(n, (uint8_t *)&log + off, trans_len, cmd);
	if( ret ) {
		return ret;
	}
	return NVME_SUCCESS;
}

// FIXME:
static uint16_t fdp_confs(FemuCtrl *n, uint32_t buf_len,
							uint64_t off, NvmeCmd *cmd)
{
	uint16_t ret;
	uint8_t *buf = NULL;
	uint32_t log_size, trans_len;
	size_t nruh, fdp_descr_size;
	
	FdpCtrlParams params = n->fdp_params;

	NvmeNamespace *ns = n->namespaces;
	NvmeEnduranceGroup *endgrp = ns->endgrp;

	NvmeFdpDescrHdr *hdr = NULL;
	NvmeRuhDescr *ruhd = NULL;
	NvmeFdpConfsHdr *log = NULL;

	if( !endgrp->fdp.enabled ) {
		femu_debug("FDP disabled\n");
		return NVME_FDP_DISABLED | NVME_DNR;
	}

	nruh = endgrp->fdp.nruh;
	fdp_descr_size = ROUND_UP(sizeof(NvmeFdpDescrHdr) + nruh * sizeof(NvmeRuhDescr), 8);
	log_size = sizeof(NvmeFdpConfsHdr) + fdp_descr_size;

	if( off >= log_size ) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	trans_len = MIN(log_size - off, buf_len);
	/* log buffer allocation */
	buf = g_malloc0(log_size);
	log = (NvmeFdpConfsHdr *)buf;
	hdr = (NvmeFdpDescrHdr *)(log + 1);
	ruhd = (NvmeRuhDescr *)(buf + sizeof(*log) + sizeof(*hdr));

	log->num_confs = cpu_to_le16(0);
	log->size = cpu_to_le32(log_size);
	
	hdr->descr_size = cpu_to_le16(fdp_descr_size);

	/* written to log */
	if( endgrp->fdp.enabled ) {
		hdr->fdpa = FIELD_DP8(hdr->fdpa, FDPA, VALID, 1);
		hdr->fdpa = FIELD_DP8(hdr->fdpa, FDPA, RGIF, endgrp->fdp.rgif);
		hdr->nrg = cpu_to_le16(endgrp->fdp.nrg); 
		hdr->nruh = cpu_to_le16(endgrp->fdp.nruh);
		hdr->maxpids = cpu_to_le16(NVME_FDP_MAXPIDS - 1);
		hdr->nnss = cpu_to_le32(n->num_namespaces);
		hdr->runs = cpu_to_le64(endgrp->fdp.runs);

		for( int i = 0; i < nruh; i++ ) {
			ruhd->ruht = params.ruh_type;	
			ruhd++;
		}
	}

	ret = fdp_dma_read(n, (uint8_t *)buf + off, trans_len, cmd);
	if( ret ) {
		return ret;	
	}
	return NVME_SUCCESS;
} 

// FIXME:
static uint16_t fdp_ruh_usage(FemuCtrl *n, uint32_t len,
							uint64_t off, NvmeCmd *cmd) 
{
	NvmeNamespace *ns = n->namespaces;
	NvmeEnduranceGroup *endgrp = ns->endgrp;
	//struct NvmeRuHandle *ruh = NULL;
	struct ssd *ssd = n->ssd;
	size_t nruh;
	uint32_t log_size, trans_len;
	uint8_t *buf = NULL;
	uint16_t ret;

	// FDP support
	if (!endgrp->fdp.enabled) {
		return NVME_FDP_DISABLED | NVME_DNR;
	}

	nruh = endgrp->fdp.nruh;
	log_size = sizeof(NvmeRuhStatus) + nruh * sizeof(NvmeRuhStatusDescr);

	if (off >= log_size) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	trans_len = MIN(log_size - off, len);
	buf = g_malloc0(log_size);
	NvmeRuhStatus *ruh_status = (NvmeRuhStatus *)buf;
	ruh_status->nruhsd = cpu_to_le16(nruh);
	
	NvmeRuhStatusDescr *ruhd = (NvmeRuhStatusDescr *)(buf + sizeof(NvmeRuhStatus));

	for (int i = 0; i < nruh; i++) {
		//ruh = &ssd->ruhs[i];
		ruhd[i].ruhid = cpu_to_le16(i);
		// FIXME
		ruhd[i].earutr = cpu_to_le64(ssd->lm.free_line_cnt * ssd->sp.secs_per_line);
		ruhd[i].ruamw = cpu_to_le64(0);
	}

	ret = fdp_dma_read(n, (uint8_t *)buf + off, trans_len, cmd);

	if (ret) {
		return ret;
	}

	g_free(buf);
	
	return NVME_SUCCESS;
}

// FIXME: 
static uint16_t nvme_fdp_events(FemuCtrl *n, uint32_t len,
								uint64_t off, NvmeCmd *cmd) 
{
	NvmeEnduranceGroup *endgrp = n->namespaces->endgrp;
	bool host_events = (le32_to_cpu(cmd->cdw10) >> 8) & 0x1;
	uint32_t log_size, trans_len;
	uint16_t ret;
	NvmeFdpEventBuffer *ebuf;
	NvmeFdpEventsLog *elog = NULL;
	NvmeFdpEvent *event;
	
	if (!endgrp->fdp.enabled) {
		return NVME_FDP_DISABLED | NVME_DNR;
	}

	if (host_events) {
		ebuf = &endgrp->fdp.host_events;
	} else {
		ebuf = &endgrp->fdp.ctrl_events;
	}

	log_size = sizeof(NvmeFdpEventsLog) + ebuf->nelems * sizeof(NvmeFdpEvent);
	trans_len = MIN(log_size - off, len);
	elog = g_malloc0(log_size);
	elog->num_events = cpu_to_le32(ebuf->nelems);
	event = (NvmeFdpEvent *)(elog + 1);

	if (ebuf->nelems && ebuf->start == ebuf->next) {
        unsigned int nelems = (NVME_FDP_MAX_EVENTS - ebuf->start);
        memcpy(event, &ebuf->events[ebuf->start],
               sizeof(NvmeFdpEvent) * nelems);
        memcpy(event + nelems, ebuf->events,
               sizeof(NvmeFdpEvent) * ebuf->next);
    } else if (ebuf->start < ebuf->next) {
        memcpy(event, &ebuf->events[ebuf->start],
               sizeof(NvmeFdpEvent) * (ebuf->next - ebuf->start));
    }

	ret = fdp_dma_read(n, (uint8_t *)elog + off, trans_len, cmd);
	g_free(elog);
	if ( ret ) {
		return ret;
	}

	return NVME_SUCCESS;
}

// FIXME: 
static uint16_t fdp_get_log(FemuCtrl *n, NvmeCmd *cmd) 
{
	uint32_t dw10 = le32_to_cpu(cmd->cdw10);
	uint32_t dw11 = le32_to_cpu(cmd->cdw11);
	uint32_t dw12 = le32_to_cpu(cmd->cdw12);
	uint32_t dw13 = le32_to_cpu(cmd->cdw13);
	uint16_t lid = dw10 & 0xffff;

	uint32_t numdl, numdu, len;
	uint64_t off, lpol, lpou;

	/* numdl: Number of Dwords Lower */
	/* numdu: Number of Dwords upper */
	/* lpol : Log Page Offset Lower */
	/* lpou : Log Page Offset Upper */
	numdl = (dw10 >> 16);
	numdu = (dw11 & 0xffff);
	lpol = dw12;
	lpou = dw13;

	len = (((numdu << 16) | numdl) + 1) << 2;
	off = (lpou << 32ULL) | lpol;

	switch (lid) {
		case NVME_LOG_FDP_STATS:
			return fdp_stats(n, len, off, cmd);

		case NVME_LOG_FDP_CONFS:
			return fdp_confs(n, len, off, cmd);

		case NVME_LOG_FDP_RUH_USAGE:
			return fdp_ruh_usage(n, len, off, cmd);

		case NVME_LOG_FDP_EVENTS:
			// FIXME:
			return nvme_fdp_events(n, len, off, cmd);

		default: 
			return NVME_INVALID_OPCODE | NVME_DNR;
	}
}

// FIXME:
int nvme_register_fdpssd(FemuCtrl *n)
{
	n->ext_ops = (FemuExtCtrlOps) {
		.state			= NULL,
		.init			= fdp_init,
		.exit			= NULL,
		.rw_check_req	= NULL,
		.admin_cmd		= fdp_admin_cmd, 
		.io_cmd			= fdp_io_cmd,
		.get_log		= fdp_get_log,
	};

	return 0;
}


