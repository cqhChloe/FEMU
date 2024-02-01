#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"

#define FEMU_DEBUG_FTL

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

/* 一些配置，可以配置做不同的实验 */
#define SSD_SIZE_MB     (2048)                  // SSD的逻辑空间大小，单位：MB（决定了映射表的大小, 需要和run-blackbox.sh保持一致）
#define PAGE_SIZE_SHIFT (12)
#define NAND_PAGE_SIZE  (1 << PAGE_SIZE_SHIFT)  // 4096字节
#define SLC_CHUNK_SIZE  (1)                     // SLC以4KiB为粒度映射 (1个page)
#define QLC_CHUNK_SIZE  (16)                    // QLC以64KiB为粒度映射（16个page），最大支持64

/* 可计算的配置 */
#define TT_LPNS ((SSD_SIZE_MB) * (1024 / (NAND_PAGE_SIZE / 1024)))  // FTL需要维护的LPN数量
#define TT_CHUNKS ((TT_LPNS + QLC_CHUNK_SIZE -1 ) / QLC_CHUNK_SIZE)

/* NAND cell type */
enum NAND_CELL_TYPE {
    SLC_NAND = 0,
    MLC_NAND = 1,
    TLC_NAND = 2,
    QLC_NAND = 3,
};

/* Operation */
enum NAND_OP {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,
};

/* Operation Latency */
enum NAND_OP_LATS {
    SLC_NAND_READ_LATENCY = 40000,
    SLC_NAND_PROG_LATENCY = 200000,
    SLC_NAND_ERASE_LATENCY = 2000000,

    QLC_NAND_READ_LATENCY = 40000,
    QLC_NAND_PROG_LATENCY = 200000,
    QLC_NAND_ERASE_LATENCY = 2000000,
};

/* IO source */
enum {
    USER_IO = 0,
    GC_IO = 1,
};

/* NAND STATE */
enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2,
    PG_CLEAN = 3    // 用来标记SLC的page
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,

    FEMU_SHOW_CONFIG = 8,
};

enum {
    GC_SLC_TO_QLC = 1,
    GC_QLC = 2,
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
    int ttchks;

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
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
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
    int64_t stime; /* Coperd: request arrival time */
};

/* different regions may consist of different NAND media */
struct ssd_region {
    int nand_type;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct write_pointer wp;
    struct line_mgmt lm;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse maptbl, assume it's stored in OOB */
};

struct ftl_mptl_slc_entry {
    struct ppa ppa[QLC_CHUNK_SIZE];     // 每个LPN的物理位置
    uint32_t nbytes[QLC_CHUNK_SIZE];    // 每个LPN压缩后的字节数
    bool is_clean;
};

struct ftl_mptl_qlc_entry {
    struct ppa ppa[QLC_CHUNK_SIZE];            // chunk的物理地址, 可能只有部分有效的
    uint32_t nbytes[QLC_CHUNK_SIZE];           // 每个page的实际长度 
    uint32_t avg_nbyte : PAGE_SIZE_SHIFT;      // [0-4095]每个page的平均长度（截取长度）, 长度等于【这个值+1】
    uint32_t max_nbyte : PAGE_SIZE_SHIFT;
    uint32_t len : (32 - 2 * PAGE_SIZE_SHIFT); // chunk的物理页数
    uint64_t valid_bitmap;                     // 标记每个LPN是否有效。比如中间的一些LPN用户可能没有写过 
    uint64_t alignment_bitmap;
};

struct ftl_mapping_table {
    struct ftl_mptl_slc_entry *slc_l2p;
    struct ftl_mptl_qlc_entry *qlc_l2p;
};

struct ftl_statistics {
    uint64_t user_read_cnt;
    uint64_t slc_read_cnt;
    uint64_t qlc_read_cnt;
};

struct ssd {
    char *ssdname;
    struct ftl_statistics *stat;
    struct ftl_mapping_table *maptbl;
    struct ssd_region *slc;
    struct ssd_region *qlc;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
};

void ssd_init(FemuCtrl *n);

#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
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


void ftl_flog(const char *format, ...);
bool check_ssd_param(struct ssd *ssd);
void show_ssd_config(struct ssd *ssd);

#endif
