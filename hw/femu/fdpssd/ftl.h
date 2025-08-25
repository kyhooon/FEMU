#include "../nvme.h" 

struct ssdparams {
	int pg_rd_lat;		// NAND page read latency in nanoseconds 
	int pg_wr_lat;		// NAND page program latency in nanoseconds
	int blk_er_lat;		// NAND block erase latency in nanosecond
	int ch_xfer_lat;	// Channel transfer latency for one page in nanoseconds

	bool enable_gc_delay;
};

struct ssd {
	struct ssdparams sp;

	char *ssdname;
	bool *dataplane_started_ptr;
};

void fdp_ssd_init(FemuCtrl *n);

#ifdef FEMU_DEBUG_FTL 
#define ftl_debug(fmt, ...) \
	do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else 
#define ftl_debug(fmt, ...) \
	do { } while (0);
#endif
