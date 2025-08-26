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

int nvme_register_fdpssd(FemuCtrl *n)
{
	n->ext_ops = (FemuExtCtrlOps) {
		.state		= NULL,
		.init		= fdp_init,
		.exit		= NULL,
		.rw_check_req	= NULL,
		.admin_cmd	= NULL, 
		.io_cmd		= NULL,
		.get_log	= NULL,
	};

	return 0;
}


