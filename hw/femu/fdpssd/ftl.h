#include "../nvme.h" 

#define INVALID_PPA (~(0ULL))
#define INVALID_LPN (~(0ULL))
#define UNMAPPED_PPA (~(0ULL))

enum {
    NAND_READ = 0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
};

// Reclaim Uint Handle Type
enum {
    INIT_ISOLATED = 0,
    PERSIST_ISOLATED = 1,
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
    int ruh_type;       // 1) Initially Isolated, 2) Persistently Isolated
};

typedef struct line {
    int id;	// block id
    int ipc;	// invalid page count in this line
    int vpc;	// valid page count in this line 
}line;

struct reclaim_unit {
    // FIXME: to be fix
    line *line;
};

struct reclaim_handle {
    // FIXME: to be fix
    struct reclaim_unit *unit;
    int type;
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
