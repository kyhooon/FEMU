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

enum {
    SEC_FREE = 0,
    PG_FREE = 0
};

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
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
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

// FIXME: 
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

typedef struct line {
    int id;	// block id
    int ipc;	// invalid page count in this line
    int vpc;	// valid page count in this line 
}line;

// FIXME:
struct line_mgmt {
    struct line *lines;

    // QTAILQ_HEAD(free_line_list, line) free_line_list;
    // QTAILQ_HEAD(full_line_list, line) full_line_list;

    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

// FIXME: 
struct reclaim_unit {
};

// FIXME: RG
struct reclaim_group {
    struct line_mgmt *lm;
};

typedef struct reclaim_handle {
    // FIXME: to be fix
    struct write_pointer wp;
    int type;
}ruh;

// FIXME:
struct ssd {
    // reclaim group
    struct reclaim_group *gps;

    struct write_pointer wp;

    struct ssdparams sp;
    struct ssd_channel *ch;
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
