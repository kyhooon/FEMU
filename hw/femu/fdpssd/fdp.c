#include "../nvme.h"

int nvme_register_fdpssd(FemuCtrl *n)
{
	n->ext_ops = (FemuExtCtrlOps) {
		.state		= NULL,
		.init		= NULL,
		.exit		= NULL,
		.rw_check_req	= NULL,
		.admin_cmd	= NULL, 
		.io_cmd		= NULL,
		.get_log	= NULL,
	};

	return 0;
}
