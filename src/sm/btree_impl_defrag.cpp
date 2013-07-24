/*
 * (c) Copyright 2011-2013, Hewlett-Packard Development Company, LP
 */

#include "w_defines.h"

/**
 * Implementation of tree defrag/reorg functions in btree_impl.h.
 * Separated from btree_impl.cpp.
 */

#define SM_SOURCE
#define BTREE_C

#include "sm_int_2.h"
#include "sm_base.h"
#include "btree_p.h"
#include "btree_impl.h"
#include "crash.h"
#include "w_key.h"
#include "xct.h"
#include "page_bf_inline.h"

rc_t btree_impl::_sx_defrag_tree(
    volid_t vol, snum_t store,
    uint16_t inpage_defrag_ghost_threshold,
    uint16_t inpage_defrag_usage_threshold,
    bool does_adopt,
    bool does_merge)
{
    FUNC(btree_impl::_sx_defrag_tree);
    sys_xct_section_t sxs;
    W_DO(sxs.check_error_on_start());
    rc_t ret = _ux_defrag_tree_core (vol, store,
        inpage_defrag_ghost_threshold,
        inpage_defrag_usage_threshold,
        does_adopt, does_merge
    );
    W_DO (sxs.end_sys_xct (ret));
    return ret;
}

rc_t btree_impl::_ux_defrag_tree_core(
    volid_t vol, snum_t store,
    uint16_t /*inpage_defrag_ghost_threshold*/,
    uint16_t /*inpage_defrag_usage_threshold*/,
    bool /*does_adopt*/,
    bool /*does_merge*/)
{
    // TODO implement
    // this should use the improved tree-walk-through jira ticket:60 "Tree walk-through without more than 2 pages latched" (originally trac ticket:62)
    btree_p page;
    W_DO (page.fix_root(vol, store, LATCH_SH));
    
    return RCOK;
}

rc_t btree_impl::_sx_defrag_page(btree_p &page)
{
    FUNC(btree_impl::_sx_defrag_page);
    sys_xct_section_t sxs (true); // this will emit a single log record
    W_DO(sxs.check_error_on_start());
    rc_t ret = _ux_defrag_page_core (page);
    W_DO (sxs.end_sys_xct (ret));
    return ret;
}
rc_t btree_impl::_ux_defrag_page_core(btree_p &page)
{
    w_assert1 (xct()->is_sys_xct());
    W_DO (page.defrag());
    return RCOK;
}
