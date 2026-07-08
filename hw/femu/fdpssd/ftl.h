#include "../nvme.h" 

#define INVALID_PPA (~(0ULL))
#define INVALID_LPN (~(0ULL))
#define UNMAPPED_PPA (~(0ULL))

/* Thresholds for workload-aware sequential/random classification */
#define WL_SEQ_THRESHOLD_PCT	70
#define WL_RAND_THRESHOLD_PCT	30

/* Sliding window size: reset workload stats every N writes for adaptability */
#define WL_WINDOW_SIZE			10000
/* If overwrite% of writes in the window exceeds this, treat as hot workload */
#define WL_HOT_THRESHOLD_PCT	20

enum {
	FDP_POLICY_STATIC_HALF		= 0,
	FDP_POLICY_RATIO_SPLIT		= 1,
	FDP_POLICY_WORKLOAD_AWARE	= 2,
};

enum {
	FEMU_ENABLE_GC_DELAY = 1,
	FEMU_DISABLE_GC_DELAY = 2,

	FEMU_ENABLE_DELAY_EMU = 3,
	FEMU_DISABLE_DELAY_EMU = 4, 

	FEMU_RESET_ACCT = 5,
	FEMU_ENABLE_LOG = 6,
	FEMU_DISABLE_LOG = 7,
};

enum {
	NAND_READ = 0,
	NAND_WRITE = 1,
	NAND_ERASE = 2,

	NAND_READ_LATENCY = 40000,
	NAND_PROG_LATENCY = 200000,
	NAND_ERASE_LATENCY = 2000000,
};

#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
	union {
		struct {
			uint64_t blk : BLK_BITS;
			uint64_t pg  : PG_BITS;
			uint64_t sec : SEC_BITS;
			uint64_t pl  : PL_BITS;
			uint64_t lun : LUN_BITS;
			uint64_t ch  : CH_BITS;
			uint64_t rsv : 1;
		} g;

		uint64_t ppa;
	};
};

enum {
	USER_IO = 0,
	GC_IO = 1,
};

enum {
	SEC_FREE = 0,
	SEC_INVALID = 1,
	SEC_VALID = 2,

	PG_FREE = 0,
	PG_INVALID = 1,
	PG_VALID = 2
};

typedef struct line {
	int id;	// block id
	int ipc;	// invalid page count in this line
	int vpc;	// valid page count in this line 
	QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
	/* position in the priority queue for victim lines */
	size_t pos;

	// FIXME
	int ruht;	/* init = 0, ii = 1, pi = 2 */
	int ruhid;	/* init = -1, */
}line;

typedef int nand_sec_status_t;

struct nand_page {
	nand_sec_status_t *sec;    
	int nsecs;
	int status;
};

struct nand_block {
	struct nand_page *pg;
	int npgs;
	int ipc; /* invalid page count */
	int vpc; /* valid page count */
	int erase_cnt;
	int wp; /* current write pointer */
};

struct nand_plane {
	struct nand_block *blk;
	int nblks;
};

struct nand_lun {
	//pool *pool;
	struct nand_plane *pl;
	int npls;
	uint64_t next_lun_avail_time;
	bool busy;
	uint64_t gc_endtime;
};

struct ssd_channel {
	struct nand_lun *lun;
	int nluns;
	uint64_t next_ch_avail_time;
	bool busy;
	uint64_t gc_endtime;
};

struct ssdparams {
	int secsz;        /* sector size in bytes */
	int secs_per_pg;  /* # of sectors per page */
	int pgs_per_blk;  /* # of NAND pages per block */
	int blks_per_pl;  /* # of blocks per plane */
	int pls_per_lun;  /* # of planes per LUN (Die) */
	int luns_per_ch;  /* # of LUNs per channel */
	int nchs;         /* # of channels in the SSD */

	int pg_rd_lat;    /* NAND page read latency in nanoseconds */
	int pg_wr_lat;    /* NAND page program latency in nanoseconds */
	int blk_er_lat;   /* NAND block erase latency in nanoseconds */
	int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
					   * this defines the channel bandwith
					   */

	double gc_thres_pcent;
	int gc_thres_lines;
	double gc_thres_pcent_high;
	int gc_thres_lines_high;
	bool enable_gc_delay;

	/* below are all calculated values */
	int secs_per_blk; /* # of sectors per block */
	int secs_per_pl;  /* # of sectors per plane */
	int secs_per_lun; /* # of sectors per LUN */
	int secs_per_ch;  /* # of sectors per channel */
	int tt_secs;      /* # of sectors in the SSD */

	int pgs_per_pl;   /* # of pages per plane */
	int pgs_per_lun;  /* # of pages per LUN (Die) */
	int pgs_per_ch;   /* # of pages per channel */
	int tt_pgs;       /* total # of pages in the SSD */

	int blks_per_lun; /* # of blocks per LUN */
	int blks_per_ch;  /* # of blocks per channel */
	int tt_blks;      /* total # of blocks in the SSD */

	int secs_per_line;
	int pgs_per_line;
	int blks_per_line;
	int tt_lines;

	int pls_per_ch;   /* # of planes per channel */
	int tt_pls;       /* total # of planes in the SSD */

	int tt_luns;      /* total # of LUNs in the SSD */

	// FDP
	int nru;            // total # of Reclaim Unit in the SSD
	int nrg;            // total # of Reclaim Group in the SSD
	int nruh;           // total # of Reclaim Unit Handle in the SSD
	int ruh_type;       // Initially Isolated (1), Persistently Isolated (2)

	/* Mixed II/PI placement */
	int ruh_placement_policy;	/* FDP_POLICY_* */
	int ii_ruh_cnt;				/* # of ii-type RUHs (index 0..ii_ruh_cnt-1) */
};

// FIXME: 
struct write_pointer {
	struct line *curline;
	int ch;
	int lun;
	int pg;
	int blk;
	int pl;

	/* ruh type */
	int type;	/* ii = 1, pi = 2 */
};

struct line_mgmt {
	struct line *lines;
	/* free line list, we only need to maintain a list of blk numbers */
	QTAILQ_HEAD(free_line_list, line) free_line_list;
	pqueue_t *victim_line_pq;
	//QTAILQ_HEAD(victim_line_list, line) victim_line_list;
	QTAILQ_HEAD(full_line_list, line) full_line_list;
	int tt_lines;
	int free_line_cnt;
	int victim_line_cnt;
	int full_line_cnt;
};

struct nand_cmd {
	int type;
	int cmd;
	int64_t stime;	/* Coperd: request arrival time */
};

/* 
 * Workload Characterization statistics collected in the FTL thread 
 * 
 * Updated on every write; used by policy 2 (Workload-Aware) for automatic 
 * RUH selection and periodically flushed to wlstat.csv 
 */ 
struct workload_stats {
	uint64_t seq_writes;		/* pages written in sequential runs */
	uint64_t rand_writes;		/* pages written with non-contiguous LBAs */
	uint64_t total_writes;		/* total pages written (seq + rand) */ 
	uint64_t overwrite_cnt;		/* writes that invalidated an existing mapping */
	uint64_t last_write_lpn;	/* last LPN written (sequential detection) */
	bool	 last_lpn_valid;

	/* round-robin counters for RUH assignment within the II and PI subsets */
	// ii_rr selects among ruhs[0 .. ii_ruh_cnt - 1]
	// pi_rr selects among ruhs[ii_ruh_cnt .. nruh - 1]
	uint64_t ii_rr;
	uint64_t pi_rr;

	/* Result of the most recent sequential-detection check
	 * wl_update_stats() and wl_select_ruh() */
	bool last_is_sequential;

	/* Hot/cold classification (per-request, set by wl_update_stats) */
	uint64_t hot_writes;		/* writes that overwrote an existing LPN mapping */
	uint64_t cold_writes;		/* writes to LPNs with no prior mapping */
	uint64_t window_writes;		/* write count within current sliding window */
	bool     last_is_overwrite;	/* true if the last write hit a live LPN */
};

struct ssd {

	NvmeRuHandle *ruhs;

	// Back pointer to the FemuCtrl structure 
	FemuCtrl *ctrl;

	char *ssdname;
	struct ssdparams sp;
	struct ssd_channel *ch;
	struct ppa *maptbl;	/* page level mapping table */
	uint64_t *rmap;	/* reverse mapptbl, assume it's stored in OOB */
	bool *dataplane_started_ptr;

	/* II, PI write pointer */
	struct write_pointer *wp;
	/* ruh_index <-> wp_index mapping */
	//int *ruhmap;

	struct line_mgmt lm;

	/* lockless ring for communication with NVMe IO thread */
	struct rte_ring **to_ftl;
	struct rte_ring **to_poller;

	/* WAF */
	uint64_t hostWrite;
	uint64_t GCWrite;

	/* lpnCount */
	uint64_t *lpnCount;

	/* Workload Characterization (policy 2 and wlstat logging) */
	struct workload_stats wl_stats;

	QemuThread ftl_thread;
};

void fdp_ssd_init(FemuCtrl *n);

#ifdef FEMU_DEBUG_FTL 
#define ftl_debug(fmt, ...) \
	do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else 
#define ftl_debug(fmt, ...) \
	do { } while (0);
#endif

#define ftl_err(fmt, ...) \
	do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
	do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)

/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else 
#define ftl_assert(expression)
#endif
