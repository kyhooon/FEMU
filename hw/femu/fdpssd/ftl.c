#include "ftl.h"

struct fdp_feature {
	int nrgr;
};

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n) 
{
	return;
}

void fdp_ssd_init(FemuCtrl *n) 
{
	struct ssd *ssd = n->ssd;
	struct ssdparams *spp = &ssd->sp;

	// FIXME
	// ftl_assert(ssd);

	ssd_init_params(spp, n);

	return;
}
