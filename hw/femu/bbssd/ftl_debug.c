#include "ftl.h"

/*
 * \brief 检查SSD参数配置的正确性
 */
bool check_ssd_param(struct ssd *ssd) {
    
    /* 检查所有的结构都完成了malloc */
    assert(ssd);
    assert(ssd->slc);
    assert(ssd->qlc);
    assert(ssd->slc->sp);
    assert(ssd->qlc->sp)

    /* 检查逻辑空间是否小于QLC物理空间 */
    if (ssd->qlc->sp.tt_pgs <= TT_LPNS)
    {
        femu_debug("Err: qlc pages: %u, total lpns: %u...\n", ssd->qlc->sp.tt_pgs, TT_LPNS);
        return false;
    }
    float op = (ssd->qlc->sp.tt_pgs - TT_LPNS) / (TT_LPNS * 1.0);
    femu_debug("ssd op space: %.2f\n", op);

    return true;
}

/*
 * \brief 打印ssd的配置信息
 */
void show_ssd_config(struct ssd *ssd)
{
    return;
}