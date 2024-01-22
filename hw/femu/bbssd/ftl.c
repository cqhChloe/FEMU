#include "ftl.h"

#include <stdarg.h>
#include <time.h> // for generate_length

// #define FEMU_DEBUG_FTL
const char *femu_log_file_name = "/home/chenqihui/femu.log";
// const char *femu_log_file_name = "/home/wangshuai/femu.log";
FILE *femu_log_file = NULL;

static void *ftl_thread(void *arg);

static void ftl_print_time(struct ssd *ssd);
static void ftl_print_para(struct ssd *ssd);

/*
 * \brief 对于给定的LPN，随机生成其物理字节数
 * \details TODO: 需要后续完善
 * \details 1. 用齐夫分布模拟LPN之间压缩率的分布
 * （在论文里解释：因为fio、filebench等测试软件没有真实数据读写）
 */
static inline uint32_t generate_length(void)
{
    srand(time(NULL)); 
    int len = rand() % 4096;
    return len;
}

static inline bool should_gc(struct ssd_region *region)
{
    return (region->lm.free_line_cnt <= region->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd_region *region)
{
    return (region->lm.free_line_cnt <= region->sp.gc_thres_lines_high);
}

static inline struct ftl_mptl_slc_entry *get_slc_maptbl_ent(struct ssd *ssd, uint64_t lcn)
{
    return &ssd->maptbl->slc_l2p[lcn];
}

static inline struct ftl_mptl_qlc_entry *get_qlc_maptbl_ent(struct ssd *ssd, uint64_t lcn)
{
    return &ssd->maptbl->qlc_l2p[lcn];
}

// unfinished
// TODO: 参数加一个bitmap，标记每个LPN是否有效
static inline void set_qlc_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->qlc->sp.tt_pgs);
    ssd->maptbl->qlc_l2p->ppa[lpn % QLC_CHUNK_SIZE] = *ppa;
}

/*
 * \brief 设置SLC部分的L2P表以及压缩率
 * \param lpn 待设置的LPN
 * \param ppa 对应的ppa
 * \param nbyte 压缩后的字节数-1
 */
static inline void set_slc_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa, uint32_t nbytes)
{
    struct ftl_mptl_slc_entry *entry = &ssd->maptbl->slc_l2p[lpn / QLC_CHUNK_SIZE];
    ftl_assert(lpn < TT_LPNS);
    entry->ppa[lpn % QLC_CHUNK_SIZE] = *ppa;
    entry->nbytes[lpn % QLC_CHUNK_SIZE] = nbytes;
}

/*
 * \brief 标记slc映射表的chunk状态(dirty or clean)
 */
static inline void set_slc_maptbl_ent_state(struct ssd *ssd, uint64_t lpn, bool is_clean)
{
    struct ftl_mptl_slc_entry *entry = &ssd->maptbl->slc_l2p[lpn / QLC_CHUNK_SIZE];
    ftl_assert(lpn < TT_LPNS);
    entry->is_clean = is_clean;
}

static uint64_t ppa2pgidx(struct ssd_region *region, struct ppa *ppa)
{
    struct ssdparams *spp = &region->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd_region *region, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(region, ppa);

    return region->rmap[pgidx];
}

/* 
 * \brief 设置QLC的反向映射
 * \param chunk_id QLC物理页的所有page反向映射都是这个chunk_id
 * \param valid 如果为false，所有page的反向映射都是INVALID；true: 反向映射都是chunk_id
 */
static inline void set_qlc_rmap(struct ssd *ssd, int chunk_id,  bool valid)
{
    struct ssd_region *region = ssd->qlc;
    struct ftl_mptl_qlc_entry *entry = get_qlc_maptbl_ent(ssd, chunk_id);

    struct ppa ppa;
    uint64_t pgidx = 0;

    for (int i = 0; i < entry->len; ++i)
    {
        ppa.ppa = entry->ppa[i].ppa;
        pgidx = ppa2pgidx(region, &ppa);
        
        region->rmap[pgidx] = valid ? chunk_id : INVALID_LPN;
    }
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd_region *region, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(region, ppa);

    if (region->nand_type == SLC_NAND)
    {
        region->rmap[pgidx] = lpn;
    }
    else if (region->nand_type == QLC_NAND)
    {
        // TODO:
    }
    else
    {
        ftl_flog("[%s]: invaled nand type: %d\n", __FUNCTION__, region->nand_type);
    }
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd_region *region)
{
    struct ssdparams *spp = &region->sp;
    struct line_mgmt *lm = &region->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd_region *region)
{
    struct write_pointer *wpp = &region->wp;
    struct line_mgmt *lm = &region->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd_region *region)
{
    struct line_mgmt *lm = &region->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left !!!!\n" );
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd_region *region)
{
    struct ssdparams *spp = &region->sp;
    struct write_pointer *wpp = &region->wp;
    struct line_mgmt *lm = &region->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(region);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd_region *region)
{
    struct write_pointer *wpp = &region->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp, int nand_type)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    ftl_assert(is_power_of_2(spp->luns_per_ch));
    ftl_assert(is_power_of_2(spp->nchs));

    ftl_assert((spp->secs_per_pg * spp->secsz) == NAND_PAGE_SIZE); // 保证page size正确
    
    
    if (nand_type == QLC_NAND)
    {
        ftl_assert(TT_LPNS < spp->tt_pgs); // 保证有op空间
    }
}

static void ssd_init_params(struct ssdparams *spp, int nand_type)
{
    if (nand_type == SLC_NAND) {
        spp->secsz = 512;
        spp->secs_per_pg = 8;
        spp->pgs_per_blk = 256;
        spp->blks_per_pl = 16; /* ? GB */
        spp->pls_per_lun = 1;
        spp->luns_per_ch = 8;
        spp->nchs = 2;
        spp->ttchks = 100;

        spp->pg_rd_lat = SLC_NAND_READ_LATENCY;
        spp->pg_wr_lat = SLC_NAND_PROG_LATENCY;
        spp->blk_er_lat = SLC_NAND_ERASE_LATENCY;
        spp->ch_xfer_lat = 0;        
    } else if (nand_type == QLC_NAND) {
        spp->secsz = 512;
        spp->secs_per_pg = 8;
        spp->pgs_per_blk = 256;
        spp->blks_per_pl = 256; /* 16GB */
        spp->pls_per_lun = 1;
        spp->luns_per_ch = 8;
        spp->nchs = 8;

        spp->pg_rd_lat = QLC_NAND_READ_LATENCY;
        spp->pg_wr_lat = QLC_NAND_PROG_LATENCY;
        spp->blk_er_lat = QLC_NAND_ERASE_LATENCY;
        spp->ch_xfer_lat = 0;        
    } else {
        ftl_err("No such init region in ssd_init_params !!!!\n");
    }

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
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    check_params(spp, nand_type);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

// static void ssd_init_maptbl(struct ssd *ssd)
// {
//     ssd->maptbl = g_malloc0(sizeof(struct ppa) * TT_LPNS);
//     for (int i = 0; i < TT_LPNS; i++) {
//         ssd->maptbl[i].ppa = UNMAPPED_PPA;
//     }
    
//     return;
// }

static void ssd_init_maptbl(struct ssd *ssd)
{
    /* 分配maptable结构体 */
    ssd->maptbl = g_malloc0(sizeof(struct ftl_mapping_table));
    
    /* 分配slc部分的映射表并完成初始化 */
    ssd->maptbl->slc_l2p = g_malloc0(sizeof(struct ftl_mptl_slc_entry) * TT_CHUNKS);
    for (int i = 0; i < TT_CHUNKS; ++i)
    {
        ssd->maptbl->slc_l2p[i].is_clean = true;
        for (int j = 0; j < QLC_CHUNK_SIZE; ++j)
        {
            ssd->maptbl->slc_l2p[i].ppa[j].ppa = UNMAPPED_PPA;
        }
    }

    /* 分配qlc部分的映射表并完成初始化 */
    ssd->maptbl->qlc_l2p = g_malloc0(sizeof(struct ftl_mptl_qlc_entry)* TT_CHUNKS);
    for (int i = 0; i < TT_CHUNKS; ++i)
    {
        ssd->maptbl->qlc_l2p[i].valid_bitmap = 0;
    }
    
    return;
}


static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssd_region *slc = ssd->slc;
    struct ssdparams *slc_spp = &slc->sp;

    struct ssd_region *qlc = ssd->qlc;
    struct ssdparams *qlc_spp = &qlc->sp;

    slc->rmap = g_malloc0(sizeof(uint64_t) * slc_spp->tt_pgs);
    for (int i = 0; i < slc_spp->tt_pgs; i++) {
        slc->rmap[i] = INVALID_LPN;
    }


    // uint32_t tt_qlc_chunks = (qlc_spp->tt_pgs + QLC_CHUNK_SIZE - 1) / QLC_CHUNK_SIZE;
    qlc->rmap = g_malloc0(sizeof(uint64_t) * qlc_spp->tt_pgs);
    for (int i = 0; i < qlc_spp->tt_pgs; i++) {
        qlc->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssd_region *slc = NULL;
    struct ssd_region *qlc = NULL;

    /* 分配SLC和QLC结构体空间 */
    if (ssd->slc == NULL)
    {
        ssd->slc = g_malloc0(sizeof(struct ssd_region));
        ssd->slc->nand_type = SLC_NAND;
    }
    if (ssd->qlc == NULL)
    {
        ssd->qlc = g_malloc0(sizeof(struct ssd_region));
        ssd->qlc->nand_type = QLC_NAND;
    }

    slc = ssd->slc;
    qlc = ssd->qlc;    

    struct ssdparams *slc_spp = &slc->sp;
    struct ssdparams *qlc_spp = &qlc->sp;

    ftl_assert(ssd);

     /* Open log文件 */
    if (femu_log_file == NULL)
    {
        femu_log_file = fopen(femu_log_file_name, "w");
    }
    assert(femu_log_file);

    /* 打印 */
    ftl_flog("[FEMU_START] ");

    ssd_init_params(slc_spp, SLC_NAND);
    ssd_init_params(qlc_spp, QLC_NAND);

    ftl_print_time(ssd);
    ftl_print_para(ssd);

    /* initialize ssd internal layout architecture */
    slc->ch = g_malloc0(sizeof(struct ssd_channel) * slc_spp->nchs);
    for (int i = 0; i < slc_spp->nchs; i++) {
        ssd_init_ch(&slc->ch[i], slc_spp);
    }

    qlc->ch = g_malloc0(sizeof(struct ssd_channel) * qlc_spp->nchs);
    for (int i = 0; i < qlc_spp->nchs; i++) {
        ssd_init_ch(&qlc->ch[i], qlc_spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(slc);
    ssd_init_lines(qlc);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(slc);
    ssd_init_write_pointer(qlc);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd_region *region, struct ppa *ppa)
{
    struct ssdparams *spp = &region->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd_region *region, uint64_t lpn)
{
    return (lpn < region->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd_region *region, struct ppa *ppa)
{
    return &(region->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd_region *region, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(region, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd_region *region, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(region, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd_region *region, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(region, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd_region *region, struct ppa *ppa)
{
    return &(region->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd_region *region, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(region, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd_region *region, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &region->sp;
    struct nand_lun *lun = get_lun(region, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/*TODO: complete*/
#if 0
static void mark_qlc_chunk_invalid(struct ssd *ssd, int chunk_id)
{
    struct ssd_region *region = ssd->qlc;
    struct ssdparams *spp = &region->sp;
    struct ftl_mptl_qlc_entry *entry = get_qlc_maptbl_ent(ssd, chunk_id);

    struct ppa ppa;
    struct nand_page *pg = NULL;
    struct nand_block *blk = NULL;
    struct line *line = NULL;
    bool was_full_line = false;

    /* 标记chunk的每个ppa无效 */
    for (int i = 0; i < entry->len; ++i)
    {
        struct line_mgmt* lm = &ssd->qlc->lm;
        ppa.ppa = entry->ppa[i].ppa;

        // 标记page无效
        pg = get_pg(region, &ppa);
        pg->status = PG_INVALID;

        // 更新block的有效页数量
        blk = get_blk(region, &ppa);
        ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
        blk->ipc++;
        ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
        blk->vpc--;

        line = get_line(region, &ppa);
        ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
        if (line->vpc == spp->pgs_per_line) {
            ftl_assert(line->ipc == 0);
            was_full_line = true;
        }
        line->ipc++;
        ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
        /* Adjust the position of the victime line in the pq under over-writes */
        if (line->pos) {
            /* Note that line->vpc will be updated by this call */
            pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
        } else {
            line->vpc--;

            if (was_full_line) {
                /* move line: "full" -> "victim" */
                QTAILQ_REMOVE(&lm->full_line_list, line, entry);
                lm->full_line_cnt--;
                pqueue_insert(lm->victim_line_pq, line);
                lm->victim_line_cnt++;
            }
        }
    }
}
#endif

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd_region *region, struct ppa *ppa)
{
    struct line_mgmt *lm = &region->lm;
    struct ssdparams *spp = &region->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(region, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(region, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(region, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd_region *region, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(region, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(region, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < region->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(region, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < region->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd_region *region, struct ppa *ppa)
{
    struct ssdparams *spp = &region->sp;
    struct nand_block *blk = get_blk(region, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd_region *region, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (region->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(region, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, int gc_mode)
{
    // struct ssd_region *cleared_region = NULL;    
    // gc mode == slc->qlc, old_ppa in slc, new ppa in qlc
    // if (gc_mode == GC_SLC_TO_QLC) {
    //     cleared_region = ssd->slc;
    // } else if (gc_mode == GC_QLC) {
    //     cleared_region = ssd->qlc;
    // } else {
    //     // 报错
    // }
    struct ssd_region *written_region = ssd->qlc;

    // uint64_t lpn = get_rmap_ent(cleared_region, old_ppa); 
    struct ppa new_ppa;
    struct nand_lun *new_lun;

    ftl_assert(valid_lpn(cleared_region, lpn));
    new_ppa = get_new_page(written_region);
    /* update maptbl */
    // set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    // set_rmap_ent(written_region, lpn, &new_ppa);

    mark_page_valid(written_region, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(written_region);

    if (written_region->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(written_region, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(written_region, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd_region *region, bool force)
{
    struct line_mgmt *lm = &region->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < region->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}


/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa, int gc_mode)
{
    struct ssd_region *region = NULL;    
    if (gc_mode == GC_QLC) {
        region = ssd->qlc;
    } else if (gc_mode == GC_SLC_TO_QLC){
        region = ssd->slc;
    } else {
        // ftl_err("Unsupported GC-mode %d when clean_one_block\n", gc_mode);
    }
    struct ssdparams *spp = &region->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(region, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(region, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa, gc_mode);
            cnt++;
        }
    }

    ftl_assert(get_blk(region, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd_region *region, struct ppa *ppa)
{
    struct line_mgmt *lm = &region->lm;
    struct line *line = get_line(region, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

// TODO: flush SLC datas when qlc gc
static void copy_back_valid_data(struct ssd *ssd, struct line *victim_line, int gc_mode)
{    
    int ch, lun;
    struct nand_lun *lunp = NULL;
    struct ssd_region *region = NULL;
    struct ppa ppa;
    struct ssdparams *spp = &region->sp;    
    
    if (gc_mode == GC_QLC) {
        region = ssd->qlc;
    } else if (gc_mode == GC_SLC_TO_QLC){
        region = ssd->slc;
    } else {
        ftl_err("Unsupported GC-mode %d when copy_back_valid_data", gc_mode);
    }   

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);
               
    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            
            lunp = get_lun(region, &ppa);
            clean_one_block(ssd, &ppa, gc_mode);
            mark_block_free(region, &ppa);            
            
            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(region, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }
    /* update line status */
    mark_line_free(region, &ppa);
}

static int do_gc_qlc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;

    victim_line = select_victim_line(ssd->qlc, force);

    if (!victim_line) {
        return -1;
    }
    copy_back_valid_data(ssd, victim_line, GC_QLC);

    return -1;
}

static int do_gc_slc(struct ssd *ssd, bool force)
{
    int r = -1;
    struct line *victim_line = NULL;

    /* QLC GC */
    while (should_gc_high(ssd->qlc))
    {
        r = do_gc_qlc(ssd, true);
        if (r == -1)
            break;
    }

    /* SLC GC */
    victim_line = select_victim_line(ssd->slc, force);
    if (!victim_line) {
        return -1;
    }

    copy_back_valid_data(ssd, victim_line, GC_SLC_TO_QLC);
    
    return 0;
}

static uint32_t max_aligned_size(void) {

    return NAND_PAGE_SIZE;
}

static uint32_t calculate_aligned_size(uint32_t gen_len[]) {
    int cnt = 0;
    for(int i = 0; i < QLC_CHUNK_SIZE; i ++) {
        cnt += gen_len[i];
    }
    return cnt / QLC_CHUNK_SIZE;
}


static uint64_t qlc_read(struct ssd *ssd, uint64_t chunk_id, uint64_t bitmap, uint64_t stime)
{
    struct ssdparams *spp = &ssd->qlc->sp;
    struct ppa ppa;
    int lens = QLC_CHUNK_SIZE;
    struct ftl_mptl_qlc_entry *entry = NULL;
    uint64_t start_lpn = chunk_id * QLC_CHUNK_SIZE;
    uint64_t sublat, maxlat = 0;

    if (start_lpn + lens - 1 >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->qlc->sp.tt_pgs);
    }
     
    for (int idx = 0; idx < QLC_CHUNK_SIZE; idx ++){
        if((bitmap & (1 << idx)) != 0) {
            entry = get_qlc_maptbl_ent(ssd, chunk_id);
            ppa = entry->ppa[idx];
            if (!mapped_ppa(&ppa)) {
                printf("%s,chunk(%" PRId64 ") [%d]not mapped to valid ppa\n", ssd->ssdname, chunk_id, idx);
                continue;                
            }
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = stime;
            sublat = ssd_advance_status(ssd->qlc, &ppa, &srd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
    }
    return maxlat;
}

/*
 * \brief qlc写函数
 * \param chunk_id 写入chunk id
 * \param bitmap 实际写入的LPN位图
 * \param 请求的起始时间
 */
static uint64_t qlc_write(struct ssd *ssd, uint64_t chunk_id, uint64_t bitmap, uint64_t stime)
{
    // int offset_in_chunk = 0;
    uint64_t curlat = 0;
    uint64_t maxlat = 0;
    int r = 0;
    struct ftl_mptl_qlc_entry *entry = NULL;
    struct ppa ppa;
    // bool need_read = false;
    int lens = QLC_CHUNK_SIZE;
    int lpn = chunk_id * QLC_CHUNK_SIZE;
    int residual_check = 0;// 检查一个页是否写满了,页写满了就推ssd写指针
    int cnt_chunk_len = 0; // 计chunk内总物理页数
    uint32_t aligned_size, max_size; //对齐的两种单元
    uint64_t alignment = 0;

    /* 判断地址合法性 */
    if (lpn + lens - 1 >= TT_LPNS) // 超出地址范围
    {
        ftl_flog("[Error] %s: lpn=%lu, lens=%u, TT_LPNS=%d\n", __FUNCTION__, lpn, lens, TT_LPNS);

        return 50;
    }
    if ((lpn / QLC_CHUNK_SIZE) != ((lpn + lens - 1) / QLC_CHUNK_SIZE)) // 跨越了chunk
    {
        ftl_flog("[Error] %s: lpn=%lu, lens=%u, QLC_CHUNK_SIZE=%d\n", __FUNCTION__, lpn, lens, QLC_CHUNK_SIZE);
        return 50;
    }

    /* 处理gc */
    while (should_gc_high(ssd->qlc))
    {
        r = do_gc_qlc(ssd, true);
        if (r == -1) {
            ftl_flog("QLC GC failure");
        }
    }

    /* 得到chunk entry*/
    entry = get_qlc_maptbl_ent(ssd, chunk_id);

    /*
     * 新写入： 101011  ->  010100
     * 原本：   110000      010000
     */
    /* 判断是否需要读取（新写入的 和 旧的 部分重叠），获取需要读取的长度？页号 */
    // 需要读取的：old_bitmap & ~(new_map) == 0 不需要读， 否则需要读
    uint64_t old_bitmap = entry->valid_bitmap;
    uint64_t new_bitmap = bitmap;
    uint64_t need_read = old_bitmap & (~new_bitmap);

    /* 如果需要读，处理读 (更新硬件延迟) */
    if (need_read != 0)
    {
        curlat = qlc_read(ssd, chunk_id, need_read, stime);
    }
    for(int i = 0; i < lens; i++) {
        entry->nbytes[i] = generate_length();
    }

    // 计算对齐单元的大小
    max_size = max_aligned_size(); // 待实现
    aligned_size = calculate_aligned_size(entry->nbytes); // 待实现

    /* 写新的数据 old_bitmap | new_bitmap LPN数量 */
    for (int i = 0; i < lens; ++i)
    {
        ppa = entry->ppa[i];
        if (entry->valid_bitmap != 0) {
            // 设置旧chunk无效
            mark_page_invalid(ssd->qlc, &ppa); // 需要修改？
            // 反向映射更新（置无效），全部都是chunkid
            set_rmap_ent(ssd->qlc, INVALID_LPN, &ppa);
        }
        /* FIXME:加入压缩拼接的逻辑 */
        ppa = get_new_page(ssd->qlc);

        if(entry->nbytes[i] > aligned_size) {
            residual_check += max_size;           
            alignment |= (1 << i);
        } else {
            residual_check += aligned_size;
            alignment |= (0 << i);
        }
        // 记录maptbl[lcn] = sppn,nbyetes[i] = gen_len[i],avg_nbyte,max_nbyte,chunk的物理页数len, bitmap里设置对齐方式
        set_qlc_maptbl_ent(ssd, chunk_id, &ppa);
        set_rmap_ent(ssd->qlc, lpn, &ppa);
        mark_page_valid(ssd->qlc, &ppa);

        if(residual_check >= NAND_PAGE_SIZE) {
            cnt_chunk_len ++;
            ssd_advance_write_pointer(ssd->qlc);
            residual_check %= NAND_PAGE_SIZE;

            // 计算latency
            struct nand_cmd swr;
            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = stime;
            /* get latency statistics */
            curlat = ssd_advance_status(ssd->qlc, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat; 
        }
    } 
    // => 多少个PPA
    entry->alignment_bitmap = alignment;
    entry->avg_nbyte = aligned_size;
    entry->max_nbyte = max_size;
    // offset_in_chunk = (lpn + i) % QLC_CHUNK_SIZE;

    /* 更新映射表 */

#if 0
    qlc_l2p_entry = get_qlc_maptbl_ent(ssd, chunk_id);
    if (mapped_ppa(&qlc_l2p_entry->ppa[offset_in_chunk]))
    {
        mark_qlc_chunk_invalid(ssd, chunk_id);
        set_qlc_rmap(ssd, chunk_id, false);
    }

    uint64_t curlat = 0, maxlat = 0;
    struct ssd_region* qlc = ssd->qlc;
    int residual_check = 0;// 检查一个页是否写满了,页写满了就推ssd写指针
    int cnt_chunk_len = 0; // 计chunk内总物理页数
    struct ppa ppa;
    uint32_t aligned_size, max_size;

    // 计算对齐单元的大小
    int* gen_len = generate_length();
    max_size = max_aligned_size(); // 待实现
    aligned_size = calculate_aligned_size(gen_len); // 待实现
    
    for(int i = 0; i < QLC_CHUNK_SIZE; i++) {
        ppa = get_new_page(ssd->qlc);
        // 记录maptbl[lcn] = sppn,nbyetes[i] = gen_len[i],avg_nbyte,max_nbyte,chunk的物理页数len
        set_qlc_maptbl_ent(ssd, chunk_id, &ppa, gen_len[i], aligned_size, max_size);
        set_rmap_ent(qlc, lpn, &ppa);
        mark_page_valid(qlc, &ppa);
        if(gen_len[i] > aligned_size) {
            residual_check += max_size;
        } else {
            residual_check += aligned_size;
        }

        if(residual_check >= NAND_PAGE_SIZE) {
            cnt_chunk_len ++;
            ssd_advance_write_pointer(ssd->qlc);
            residual_check %= NAND_PAGE_SIZE;

            // 计算latency
            struct nand_cmd swr;
            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = req->stime;
            /* get latency statistics */
            curlat = ssd_advance_status(qlc, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }
       
    }
    if(residual_check) {
        cnt_chunk_len ++;
    }
    ssd->maptbl->qlc_l2p->len = cnt_chunk_len;
#endif 

    return maxlat;    
}

/*
 * \brief slc写函数
 * \param lpn待写入的LPN
 * \param stime 请求的开始时间
 * \return 写请求的延迟
 */
static uint64_t slc_write(struct ssd *ssd, uint64_t lpn, uint64_t stime)
{
    struct ftl_mptl_slc_entry *entry = NULL;
    uint64_t lcn = lpn / QLC_CHUNK_SIZE;
    int offset_in_chunk = lpn % QLC_CHUNK_SIZE;
    uint32_t nbytes = 0; // LPN压缩后的字节数（体现压缩率）
    struct ppa ppa;      // 存放物理地址
    int r = 0;           // 调用其他函数的返回值
    int lat;             // 延迟，本函数的返回值

    /* 处理GC */
    while (should_gc_high(ssd->slc))
    {
        r = do_gc_slc(ssd, true);
        if(r == -1) {
            ftl_flog("GC fail when slc write");
        }
    }

    entry = get_slc_maptbl_ent(ssd, lcn);   // 获取slc映射条目（chunk）
    if (mapped_ppa(&entry->ppa[offset_in_chunk]))
    {
        ppa = entry->ppa[offset_in_chunk];

        /* 更新旧的物理页信息 */
        mark_page_invalid(ssd->slc, &ppa);
        set_rmap_ent(ssd->slc, INVALID_LPN, &ppa);
    }

    /* 从slc获取新的ppa */
    ppa = get_new_page(ssd->slc);

    /* 更新slc映射表 */
    set_slc_maptbl_ent(ssd, lpn, &ppa, nbytes); // FIXME: 压缩率需要计算

    /* 标记chunk dirty */
    set_slc_maptbl_ent_state(ssd, lpn, false);

    /* 更新反向映射表 */
    set_rmap_ent(ssd->slc, lpn, &ppa);

    /* 标记页面有效 */
    mark_page_valid(ssd->slc, &ppa);

    /* 更新slc部分的写指针 */
    ssd_advance_write_pointer(ssd->slc);

    /* 计算写延迟 */
    struct nand_cmd swr;
    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = stime;

    lat = ssd_advance_status(ssd->slc, &ppa, &swr);

    return lat;
}
static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *qlc_spp = &ssd->qlc->sp;
    uint64_t lpn = 0;
    uint64_t chunk_id = 0;
    uint64_t curlat = 0;
    uint64_t maxlat = 0;
    uint64_t valid_map = 0;

    /* 将请求的长度转化为4KB LPN */
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    uint64_t start_lpn = lba / qlc_spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / qlc_spp->secs_per_pg;

    /* 判断地址范围的合法性 */
    if (end_lpn >= TT_LPNS)
    {
        ftl_flog("[Error] %s: start_lpn=%lu, end_lpn=%lu, TT_LPNS=%d\n", __FUNCTION__, start_lpn, end_lpn, TT_LPNS);
        return 50;
    }

    /* 处理写请求 */
    for (lpn = start_lpn; lpn <= end_lpn; ) 
    {
        chunk_id = lpn / QLC_CHUNK_SIZE;
        /* 如果lpn足够一个chunk，直接写QLC */
        if ((lpn % QLC_CHUNK_SIZE == 0) && ((lpn + QLC_CHUNK_SIZE - 1) <= end_lpn))
        {
            valid_map = 1; // 设定valid map为1
            curlat = qlc_write(ssd, chunk_id, valid_map, req->stime); /* 10001 */
            maxlat = (curlat > maxlat) ? curlat : maxlat;
            lpn += QLC_CHUNK_SIZE; // 处理下一个chunk
        }
        /* 不足一个chunk的LPN写入SLC */
        else
        {
            curlat = slc_write(ssd, lpn, req->stime);

            maxlat = (curlat > maxlat) ? curlat : maxlat;

            lpn += 1; // 处理下一个lpn
        }
    }

    return maxlat;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa slc_ppa;
    struct ppa qlc_ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    uint64_t chunk_id;
    uint64_t offset_within_chunk;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        // 去slc maptbl中找lpn,找到了去slc_read
        chunk_id = lpn / QLC_CHUNK_SIZE;
        offset_within_chunk = lpn % QLC_CHUNK_SIZE;
        slc_ppa = get_slc_maptbl_ent(ssd, chunk_id);
        
        if (mapped_ppa(&slc_ppa) && valid_ppa(ssd->slc, &slc_ppa)) {
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;
            sublat = ssd_advance_status(ssd, &slc_ppa, &srd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
            continue;
        }

        qlc_ppa = get_qlc_maptbl_ent(ssd, chunk_id);
                // offset计算物理位置
        if (!mapped_ppa(&qlc_ppa) || !valid_ppa(ssd, &qlc_ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &qlc_ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}
#if 0
static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    return 50;
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}
#endif


#if 0
static uint64_t slc_ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->slc->sp;
    uint64_t len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    struct ssd_region *written_region = ssd->slc;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, spp->tt_pgs);
    }

    while (should_gc_high(ssd->slc)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true, GC_SLC_TO_QLC);
        if (r == -1)
            break;
    }

    // To do: Write requests only to the slc region
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(written_region, &ppa);
            set_rmap_ent(written_region, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(written_region);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(written_region, lpn, &ppa);

        mark_page_valid(written_region, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(written_region);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(written_region, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }


    return maxlat;
}


static uint64_t qlc_ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->slc->sp;
    uint64_t len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    struct ssd_region *written_region = ssd->slc;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, spp->tt_pgs);
    }

    while (should_gc_high(ssd->slc)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true, GC_SLC_TO_QLC);
        if (r == -1)
            break;
    }

    while (should_gc_high(ssd->qlc)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true, GC_QLC);
        if (r == -1)
            break;
    }

    // To do: Write requests only to the slc region
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(written_region, &ppa);
            set_rmap_ent(written_region, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(written_region);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(written_region, lpn, &ppa);

        mark_page_valid(written_region, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(written_region);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(written_region, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}
#endif 

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            // if (should_gc(ssd->slc)) {
            //     do_gc(ssd, false, GC_SLC_TO_QLC);
            // }
            // if (should_gc(ssd->qlc)) {
            //     do_gc(ssd, false, GC_QLC);
            // }
        }
    }

    return NULL;
}

/*
 * 在`~/femu.log`中打印日志信息
 */
__attribute__ ((format (printf, 1, 2)))
void ftl_flog(const char *format, ...)
{
    if (femu_log_file == NULL)
    {
        return;
    }

    va_list args;
    
    va_start(args, format);
    vfprintf(femu_log_file, format, args);
    fflush(femu_log_file);
    va_end(args);
}

static void ftl_print_time(struct ssd *ssd)
{
    // 获取当前日历时间（从1970年1月1日UTC至今的秒数）
    time_t current_time = time(NULL);

    // 将日历时间转换为本地时间结构体
    struct tm *local_time = localtime(&current_time);

    // 格式化输出日期和时间
    char buffer[128];
    memset(buffer, 0, sizeof(buffer));
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", local_time);
    
    ftl_flog("%s\n", buffer);

    return;   
}

static void ftl_print_para(struct ssd *ssd)
{
    assert(ssd != NULL);
    assert(ssd->slc);
    assert(ssd->qlc);

    struct ssdparams *slc_sp = &ssd->slc->sp;
    struct ssdparams *qlc_sp = &ssd->qlc->sp;

    ftl_flog("--------------param------------\n");
    ftl_flog("[SLC] tt_pgs = %d, size = %d(MB)\n", slc_sp->tt_pgs, slc_sp->tt_pgs * (NAND_PAGE_SIZE / 1024) / 1024);
    ftl_flog("[QLC] tt_pgs = %d, size = %d(MB)", qlc_sp->tt_pgs, qlc_sp->tt_pgs * (NAND_PAGE_SIZE / 1024) / 1024);
    ftl_flog("op = %.2f%%\n", ((100.0 * (qlc_sp->tt_pgs - TT_LPNS)) / TT_LPNS));

    return;
}