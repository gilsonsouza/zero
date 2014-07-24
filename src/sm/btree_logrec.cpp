/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

/**
 * Logging and its UNDO/REDO code for BTrees.
 * Separated from logrec.cpp.
 */

#include "w_defines.h"

#define SM_SOURCE
#define LOGREC_C
#include "sm_int_2.h"
#include "logdef_gen.cpp"

#include "btree_page_h.h"
#include "btree_impl.h"
#include "lock.h"
#include "log.h"
#include "vec_t.h"
#include "tls.h"
#include "block_alloc.h"
#include "restart.h"

/**
 * Page buffers used while Single-Page-Recovery as scratch space.
 * \ingroup Single-Page-Recovery
 */
DECLARE_TLS(block_alloc<generic_page>, scratch_space_pool);
/**
 * Automatically deletes generic_page obtained from scratch_space_pool.
 * \ingroup Single-Page-Recovery
 */
struct SprScratchSpace {
    SprScratchSpace(volid_t vol, snum_t store, shpid_t pid) {
        p = new (*scratch_space_pool) generic_page();
        ::memset(p, 0, sizeof(generic_page));
        p->tag = t_btree_p;
        p->pid = lpid_t(stid_t(vol, store), pid);
        p->lsn = lsn_t::null;
    }
    ~SprScratchSpace() { scratch_space_pool->destroy_object(p); }
    generic_page* p;
};

struct btree_insert_t {
    shpid_t     root_shpid;
    uint16_t    klen;
    uint16_t    elen;
    char        data[logrec_t::max_data_sz - sizeof(shpid_t) - 2*sizeof(int16_t)];

    btree_insert_t(const btree_page_h& page, const w_keystr_t& key,
                   const cvec_t& el);
    int size()        { return sizeof(shpid_t) + 2*sizeof(int16_t) + klen + elen; }
};

btree_insert_t::btree_insert_t(
    const btree_page_h&   _page, 
    const w_keystr_t&     key, 
    const cvec_t&         el)
    : klen(key.get_length_as_keystr()), elen(el.size())
{
    root_shpid = _page.root().page;
    w_assert1((size_t)(klen + elen) < sizeof(data));
    key.serialize_as_keystr(data);
    el.copy_to(data + klen);
}

btree_insert_log::btree_insert_log(
    const btree_page_h& page, 
    const w_keystr_t&   key,
    const cvec_t&       el)
{
    fill(&page.pid(), page.tag(),
         (new (_data) btree_insert_t(page, key, el))->size());
}

void 
btree_insert_log::undo(fixable_page_h* page) {
    w_assert9(page == 0);
    btree_insert_t* dp = (btree_insert_t*) data();

    lpid_t root_pid (header._vid, header._snum, dp->root_shpid);
    w_keystr_t key;
    key.construct_from_keystr(dp->data, dp->klen);

    // ***LOGICAL*** don't grab locks during undo
    rc_t rc = smlevel_2::bt->remove_as_undo(header._vid.vol, header._snum, key); 
    if(rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

void
btree_insert_log::redo(fixable_page_h* page) {
    borrowed_btree_page_h bp(page);
    btree_insert_t* dp = (btree_insert_t*) data();
    
    w_assert1(bp.is_leaf());
    w_keystr_t key;
    vec_t el;
    key.construct_from_keystr(dp->data, dp->klen);
    el.put(dp->data + dp->klen, dp->elen);

    // PHYSICAL redo
    // see btree_impl::_ux_insert()
    // at the point we called log_btree_insert,
    // we already made sure the page has a ghost
    // record for the key that is enough spacious.
    // so, we just replace the record!
    w_rc_t rc = bp.replace_ghost(key, el);
    if(rc.is_error()) { // can't happen. wtf?
        W_FATAL_MSG(fcINTERNAL, << "btree_insert_log::redo " );
    }
}

btree_insert_nonghost_log::btree_insert_nonghost_log(
    const btree_page_h &page, const w_keystr_t &key, const cvec_t &el) {
    fill(&page.pid(), page.tag(), (new (_data) btree_insert_t(page, key, el))->size());
}
void btree_insert_nonghost_log::undo(fixable_page_h* page) {
    reinterpret_cast<btree_insert_log*>(this)->undo(page); // same as btree_insert
}

void btree_insert_nonghost_log::redo(fixable_page_h* page) {
    borrowed_btree_page_h bp(page);
    btree_insert_t* dp = reinterpret_cast<btree_insert_t*>(data());

    w_assert1(bp.is_leaf());
    w_keystr_t key;
    vec_t el;
    key.construct_from_keystr(dp->data, dp->klen);
    el.put(dp->data + dp->klen, dp->elen);
    bp.insert_nonghost(key, el);
}

struct btree_update_t {
    shpid_t     _root_shpid;
    uint16_t    _klen;
    uint16_t    _old_elen;
    uint16_t    _new_elen;
    char        _data[logrec_t::max_data_sz - sizeof(shpid_t) - 3*sizeof(int16_t)];

    btree_update_t(const btree_page_h& page, const w_keystr_t& key,
                   const char* old_el, int old_elen, const cvec_t& new_el) {
        _root_shpid = page.btree_root();
        _klen       = key.get_length_as_keystr();
        _old_elen   = old_elen;
        _new_elen   = new_el.size();
        key.serialize_as_keystr(_data);
        ::memcpy (_data + _klen, old_el, old_elen);
        new_el.copy_to(_data + _klen + _old_elen);
    }
    int size()        { return sizeof(shpid_t) + 3*sizeof(int16_t) + _klen + _old_elen + _new_elen; }
};

btree_update_log::btree_update_log(
    const btree_page_h&   page, 
    const w_keystr_t&     key,
    const char* old_el, int old_elen, const cvec_t& new_el)
{
    fill(&page.pid(), page.tag(),
         (new (_data) btree_update_t(page, key, old_el, old_elen, new_el))->size());
}


void 
btree_update_log::undo(fixable_page_h*)
{
    btree_update_t* dp = (btree_update_t*) data();
    
    lpid_t root_pid (header._vid, header._snum, dp->_root_shpid);

    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    vec_t old_el;
    old_el.put(dp->_data + dp->_klen, dp->_old_elen);

    // ***LOGICAL*** don't grab locks during undo
    rc_t rc = smlevel_2::bt->update_as_undo(header._vid.vol, header._snum, key, old_el); 
    if(rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

void
btree_update_log::redo(fixable_page_h* page)
{
    borrowed_btree_page_h bp(page);
    btree_update_t* dp = (btree_update_t*) data();
    
    w_assert1(bp.is_leaf());
    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    vec_t old_el;
    old_el.put(dp->_data + dp->_klen, dp->_old_elen);
    vec_t new_el;
    new_el.put(dp->_data + dp->_klen + dp->_old_elen, dp->_new_elen);

    // PHYSICAL redo
    slotid_t       slot;
    bool           found;
    bp.search(key, found, slot);
    if (!found) {
        W_FATAL_MSG(fcINTERNAL, << "btree_update_log::redo(): not found");
        return;
    }
    w_rc_t rc = bp.replace_el_nolog(slot, new_el);
    if(rc.is_error()) { // can't happen. wtf?
        W_FATAL_MSG(fcINTERNAL, << "btree_update_log::redo(): couldn't replace");
    }
}

struct btree_overwrite_t {
    shpid_t     _root_shpid;
    uint16_t    _klen;
    uint16_t    _offset;
    uint16_t    _elen;
    char        _data[logrec_t::max_data_sz - sizeof(shpid_t) - 3*sizeof(int16_t)];

    btree_overwrite_t(const btree_page_h& page, const w_keystr_t& key,
            const char* old_el, const char *new_el, size_t offset, size_t elen) {
        _root_shpid = page.btree_root();
        _klen       = key.get_length_as_keystr();
        _offset     = offset;
        _elen       = elen;
        key.serialize_as_keystr(_data);
        ::memcpy (_data + _klen, old_el + offset, elen);
        ::memcpy (_data + _klen + elen, new_el, elen);
    }
    int size()        { return sizeof(shpid_t) + 3*sizeof(int16_t) + _klen + _elen * 2; }
};


btree_overwrite_log::btree_overwrite_log (const btree_page_h& page, const w_keystr_t& key,
                                          const char* old_el, const char *new_el, size_t offset, size_t elen) {
    fill(&page.pid(), page.tag(),
         (new (_data) btree_overwrite_t(page, key, old_el, new_el, offset, elen))->size());
}

void btree_overwrite_log::undo(fixable_page_h*)
{
    btree_overwrite_t* dp = (btree_overwrite_t*) data();
    
    lpid_t root_pid (header._vid, header._snum, dp->_root_shpid);

    uint16_t elen = dp->_elen;
    uint16_t offset = dp->_offset;
    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    const char* old_el = dp->_data + dp->_klen;

    // ***LOGICAL*** don't grab locks during undo
    rc_t rc = smlevel_2::bt->overwrite_as_undo(header._vid.vol, header._snum, key, old_el, offset, elen); 
    if(rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

void btree_overwrite_log::redo(fixable_page_h* page)
{
    borrowed_btree_page_h bp(page);
    btree_overwrite_t* dp = (btree_overwrite_t*) data();
    
    w_assert1(bp.is_leaf());

    uint16_t elen = dp->_elen;
    uint16_t offset = dp->_offset;
    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    const char* new_el = dp->_data + dp->_klen + elen;

    // PHYSICAL redo
    slotid_t       slot;
    bool           found;
    bp.search(key, found, slot);
    if (!found) {
        W_FATAL_MSG(fcINTERNAL, << "btree_overwrite_log::redo(): not found");
        return;
    }

#if W_DEBUG_LEVEL>0
    const char* old_el = dp->_data + dp->_klen;
    smsize_t cur_elen;
    bool ghost;
    const char* cur_el = bp.element(slot, cur_elen, ghost);
    w_assert1(!ghost);
    w_assert1(cur_elen >= offset + elen);
    w_assert1(::memcmp(old_el, cur_el + offset, elen) == 0);
#endif //W_DEBUG_LEVEL>0

    bp.overwrite_el_nolog(slot, offset, new_el, elen);
}

struct btree_ghost_t {
    shpid_t       root_shpid;
    uint16_t      cnt;
    uint16_t      prefix_offset;
    size_t        total_data_size;
    // list of [offset], and then list of [length, string-data WITHOUT prefix]
    // this is analogous to BTree page structure on purpose.
    // by doing so, we can guarantee the total size is <data_sz.
    // because one log should be coming from just one page.
    char          slot_data[logrec_t::max_data_sz - sizeof(shpid_t)
                        - sizeof(uint16_t) * 2 - sizeof(size_t)];

    btree_ghost_t(const btree_page_h& p, const vector<slotid_t>& slots);
    w_keystr_t get_key (size_t i) const;
    int size() { return sizeof(shpid_t) + sizeof(uint16_t) * 2 + sizeof(size_t) + total_data_size; }
};
btree_ghost_t::btree_ghost_t(const btree_page_h& p, const vector<slotid_t>& slots)
{
    root_shpid = p.root().page;
    cnt = slots.size();
    uint16_t *offsets = reinterpret_cast<uint16_t*>(slot_data);
    char *current = slot_data + sizeof (uint16_t) * slots.size();

    // the first data is prefix
    {
        uint16_t prefix_len = p.get_prefix_length();
        prefix_offset = (current - slot_data);
        // *reinterpret_cast<uint16_t*>(current) = prefix_len; this causes Bus Error on solaris! so, instead:
        ::memcpy(current, &prefix_len, sizeof(uint16_t));
        if (prefix_len > 0) {
            ::memcpy(current + sizeof(uint16_t), p.get_prefix_key(), prefix_len);
        }
        current += sizeof(uint16_t) + prefix_len;
    }
    
     for (size_t i = 0; i < slots.size(); ++i) {
        size_t len;
        w_assert3(p.is_leaf()); // ghost exists only in leaf
        const char* key = p._leaf_key_noprefix(slots[i], len);
        offsets[i] = (current - slot_data);
        // *reinterpret_cast<uint16_t*>(current) = len; this causes Bus Error on solaris! so, instead:
        uint16_t len_u16 = (uint16_t) len;
        ::memcpy(current, &len_u16, sizeof(uint16_t));
        ::memcpy(current + sizeof(uint16_t), key, len);
        current += sizeof(uint16_t) + len;
    }
    total_data_size = current - slot_data;
    w_assert0(logrec_t::max_data_sz >= sizeof(shpid_t) + sizeof(uint16_t) * 2 + sizeof(size_t) + total_data_size);
}
w_keystr_t btree_ghost_t::get_key (size_t i) const {
    w_keystr_t result;
    uint16_t prefix_len;
    // = *reinterpret_cast<const uint16_t*>(slot_data + prefix_offset); this causes Bus Error on solaris
    ::memcpy(&prefix_len, slot_data + prefix_offset, sizeof(uint16_t));
    w_assert1 (prefix_offset < sizeof(slot_data));
    w_assert1 (prefix_len < sizeof(slot_data));
    const char *prefix_key = slot_data + prefix_offset + sizeof(uint16_t);
    uint16_t offset = reinterpret_cast<const uint16_t*>(slot_data)[i];
    w_assert1 (offset < sizeof(slot_data));
    uint16_t len;
    // = *reinterpret_cast<const uint16_t*>(slot_data + offset); this causes Bus Error on solaris
    ::memcpy(&len, slot_data + offset, sizeof(uint16_t));
    w_assert1 (len < sizeof(slot_data));
    const char *key = slot_data + offset + sizeof(uint16_t);
    result.construct_from_keystr(prefix_key, prefix_len, key, len);
    return result;
}

btree_ghost_mark_log::btree_ghost_mark_log(const btree_page_h& p,
                                           const vector<slotid_t>& slots)
{
    fill(&p.pid(), p.tag(), (new (data()) btree_ghost_t(p, slots))->size());
}

void 
btree_ghost_mark_log::undo(fixable_page_h*)
{
    // UNDO of ghost marking is to get the record back to regular state
    btree_ghost_t* dp = (btree_ghost_t*) data();
    lpid_t root_pid (header._vid, header._snum, dp->root_shpid);

    for (size_t i = 0; i < dp->cnt; ++i) {
        w_keystr_t key (dp->get_key(i));
        rc_t rc = smlevel_2::bt->undo_ghost_mark(header._vid.vol, header._snum, key);
        if(rc.is_error()) {
            cerr << " key=" << key << endl << " rc =" << rc << endl;
            W_FATAL(rc.err_num());
        }
    }
}

void
btree_ghost_mark_log::redo(fixable_page_h *page)
{
    // REDO is physical. mark the record as ghost again.
    w_assert1(page);
    borrowed_btree_page_h bp(page);
    w_assert1(bp.is_leaf());
    btree_ghost_t* dp = (btree_ghost_t*) data();
    for (size_t i = 0; i < dp->cnt; ++i) {
        w_keystr_t key (dp->get_key(i));
        w_assert2(bp.fence_contains(key));
        bool found;
        slotid_t slot;
        bp.search(key, found, slot);
        if (false == restart_m::use_redo_full_logging_recovery()) 
        {
            // If doing page driven REDO, page_rebalance initialized the
            // target page (foster child).
            if (!found) {
                cerr << " key=" << key << endl << " not found in btree_ghost_mark_log::redo" << endl;
                w_assert1(false); // something unexpected, but can go on.
            }
            else
                bp.mark_ghost(slot);
        }
        else
        {
            if (!found) 
            {        
// TODO(Restart)...            
                DBGOUT3( << "&&&& btree_ghost_mark_log::redo - page driven with full logging, key not found in page but it is okay");
            }
        }
    }
}

btree_ghost_reclaim_log::btree_ghost_reclaim_log(const btree_page_h& p,
                                                 const vector<slotid_t>& slots)
{
    // ghost reclaim is single-log system transaction. so, use data_ssx()
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_ghost_t(p, slots))->size());
    w_assert0(is_single_sys_xct());
}

void
btree_ghost_reclaim_log::redo(fixable_page_h* page)
{
    // REDO is to defrag it again
    borrowed_btree_page_h bp(page);
    // TODO actually should reclaim only logged entries because
    // locked entries might have been avoided.
    // (but in that case shouldn't defragging the page itself be avoided?)
    rc_t rc = btree_impl::_sx_defrag_page(bp);
    if (rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}


struct btree_ghost_reserve_t {
    uint16_t      klen;
    uint16_t      element_length;
    char          data[logrec_t::max_data_sz - sizeof(uint16_t) * 2];

    btree_ghost_reserve_t(const w_keystr_t& key,
                          int element_length);
    int size() { return sizeof(uint16_t) * 2 + klen; }
};

btree_ghost_reserve_t::btree_ghost_reserve_t(const w_keystr_t& key, int elem_length)
    : klen (key.get_length_as_keystr()), element_length (elem_length)
{
    key.serialize_as_keystr(data);
}

btree_ghost_reserve_log::btree_ghost_reserve_log (
    const btree_page_h& p, const w_keystr_t& key, int element_length) {
    // ghost creation is single-log system transaction. so, use data_ssx()
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_ghost_reserve_t(key, element_length))->size());
    w_assert0(is_single_sys_xct());
}

void btree_ghost_reserve_log::redo(fixable_page_h* page) {
    // REDO is to physically make the ghost record
    borrowed_btree_page_h bp(page);
    // ghost creation is single-log system transaction. so, use data_ssx()
    btree_ghost_reserve_t* dp = (btree_ghost_reserve_t*) data_ssx();

    // PHYSICAL redo.
    w_assert1(bp.is_leaf());
    bp.reserve_ghost(dp->data, dp->klen, dp->element_length);
    w_assert3(bp.is_consistent(true, true));
}

/**
 * A \b multi-page \b SSX log record for \b btree_norec_alloc.
 * This log is totally \b self-contained, so no WOD assumed.
 */
struct btree_norec_alloc_t : public multi_page_log_t {
    btree_norec_alloc_t(const btree_page_h &p,
        shpid_t new_page_id, const w_keystr_t& fence, const w_keystr_t& chain_fence_high);
    shpid_t     _root_pid, _foster_pid;       // +4+4 => 8
    lsn_t       _foster_emlsn;                // +8   => 16
    uint16_t    _fence_len, _chain_high_len;  // +2+2 => 20
    int16_t     _btree_level;                 // +2   => 22
    /** fence key and chain-high key. */
    char        _data[logrec_t::max_data_sz - sizeof(multi_page_log_t) - 22];

    int      size() const {
        return sizeof(multi_page_log_t) + 22 + _fence_len + _chain_high_len;
    }
};

btree_norec_alloc_t::btree_norec_alloc_t(const btree_page_h &p,
        shpid_t new_page_id, const w_keystr_t& fence, const w_keystr_t& chain_fence_high)
    : multi_page_log_t(new_page_id) {
    w_assert1 (g_xct()->is_single_log_sys_xct());
    w_assert1 (new_page_id != p.btree_root());
    w_assert1 (p.latch_mode() != LATCH_NL);

    _root_pid       = p.btree_root();
    _foster_pid     = p.get_foster();
    _foster_emlsn   = p.get_foster_emlsn();
    _fence_len      = (uint16_t) fence.get_length_as_keystr();
    _chain_high_len = (uint16_t) chain_fence_high.get_length_as_keystr();
    _btree_level    = (int16_t) p.level();
    w_assert1(size() < logrec_t::max_data_sz);

    fence.serialize_as_keystr(_data);
    chain_fence_high.serialize_as_keystr(_data + _fence_len);
}

btree_norec_alloc_log::btree_norec_alloc_log(const btree_page_h &p, const btree_page_h &,
    shpid_t new_page_id, const w_keystr_t& fence, const w_keystr_t& chain_fence_high) {
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_norec_alloc_t(p,
        new_page_id, fence, chain_fence_high))->size());
}

void btree_norec_alloc_log::redo(fixable_page_h* p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_norec_alloc_t *dp = reinterpret_cast<btree_norec_alloc_t*>(data_ssx());

    const lsn_t &new_lsn = lsn_ck();
    w_keystr_t fence, chain_high;
    fence.construct_from_keystr(dp->_data, dp->_fence_len);
    chain_high.construct_from_keystr(dp->_data + dp->_fence_len, dp->_chain_high_len);

    shpid_t target_pid = p->pid().page;
    DBGOUT3 (<< *this << ": new_lsn=" << new_lsn
        << ", target_pid=" << target_pid << ", bp.lsn=" << bp.lsn());
    if (target_pid == header._shpid) {
        // we are recovering "page", which is foster-parent.
        bp.accept_empty_child(new_lsn, dp->_page2_pid, true /*from redo*/);
    } else {
        // we are recovering "page2", which is foster-child.
        w_assert0(target_pid == dp->_page2_pid);
        // This log is also a page-allocation log, so redo the page allocation.
        W_COERCE(io_m::redo_alloc_a_page(p->pid().vol(), dp->_page2_pid));
        lpid_t pid(header._vid, header._snum, dp->_page2_pid);
        // initialize as an empty child:
        bp.format_steal(new_lsn, pid, dp->_root_pid, dp->_btree_level, 0, lsn_t::null,
                        dp->_foster_pid, dp->_foster_emlsn, fence, fence, chain_high, false);
    }
}

// logs for Merge/Rebalance/De-Adopt
// see jira ticket:39 "Node removal and rebalancing" (originally trac ticket:39) for detailed spec

/**
 * A \b multi-page \b SSX log record for \b btree_foster_merge.
 * This log is \b NOT-self-contained, so it assumes WOD (foster-child is deleted later).
 */
struct btree_foster_merge_t : multi_page_log_t {
    shpid_t         _foster_pid0;        // +4 => 4, foster page ID (destination page)
    lsn_t           _foster_pid0_emlsn;  // +8 => 12, foster emlsn (destination page)
    uint16_t        _high_len;           // +2 => 14, high key length    
    uint16_t        _chain_high_len;     // +2 => 16, chain_high key length        
    // _data contains high and chain_high
    char            _data[logrec_t::max_data_sz - sizeof(multi_page_log_t) - 16];

    btree_foster_merge_t(shpid_t page2_id,
        const w_keystr_t& high, const w_keystr_t& chain_high,
        shpid_t foster_pid0, lsn_t foster_pid0_emlsn);
    int size() const { return sizeof(multi_page_log_t) + 16 + _high_len + _chain_high_len; }
};

btree_foster_merge_t::btree_foster_merge_t(shpid_t page2_id,
        const w_keystr_t& high,        // high (foster) of destination
        const w_keystr_t& chain_high,  // high fence of all foster nodes
        shpid_t foster_pid0,           // foster page id in destination page
        lsn_t foster_pid0_emlsn)       // foster emlsn in destination page   
    : multi_page_log_t(page2_id) {

    w_assert1(size() < logrec_t::max_data_sz);

    _foster_pid0   = foster_pid0;
    _foster_pid0_emlsn = foster_pid0_emlsn;

    // Figure out the size of each data field
    _high_len = (uint16_t)high.get_length_as_keystr();
    _chain_high_len = (uint16_t)chain_high.get_length_as_keystr();

    // Put all data fields into _data
    high.serialize_as_keystr(_data);
    chain_high.serialize_as_keystr(_data + _high_len);
}

btree_foster_merge_log::btree_foster_merge_log (const btree_page_h& p, // destination
    const btree_page_h& p2,           // source
    const w_keystr_t& high,           // high (foster) of destination
    const w_keystr_t& chain_high,     // high fence of all foster nodes
    shpid_t foster_pid0,              // foster page id in destination page
    lsn_t foster_pid0_emlsn) {        // foster emlsn in destination page   
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_foster_merge_t(p2.pid().page,
        high, chain_high, foster_pid0, foster_pid0_emlsn))->size());
}

void btree_foster_merge_log::redo(fixable_page_h* p) {
    // TODO(Restart)...
    // See detail comments in btree_foster_rebalance_log::redo

    // Two pages are involved:
    // first page: destination, foster parent in this case
    // second page: source, foster child in this case

    // REDO is to merge it again.
    // WOD: "page" is the data source (foster child), which is written later.
    borrowed_btree_page_h bp(p);
    btree_foster_merge_t *dp = reinterpret_cast<btree_foster_merge_t*>(data_ssx());

    // WOD: "page" is the data source, which is written later.
    shpid_t target_pid = p->pid().page;

    bool recovering_dest = (target_pid == shpid());   // true: recover foster parent
                                                      // false: recovery foster child
    shpid_t another_pid = recovering_dest ? dp->_page2_pid : shpid();
    w_assert0(recovering_dest || target_pid == dp->_page2_pid);

    if (true == restart_m::use_redo_full_logging_recovery())
    {
        // TODO(Restart)... milestone 2
        // If using full logging REDO recovery, the merge log record (system txn)
        // occurs before all the actual record move log records

        // For the merge REDO, the REDO is to delete records from 
        // source (foster child) and insert them into destination (foster parent).
        // Do not initialize the target page (fost parent) because it is an existing
        // page containing records, but we do need to reset the high fence and chain_high
        // for the destination (foster parent) page before the actual record movements

        w_keystr_t high_key, chain_high_key;

        // Get fence key information from log record (dp)
        // The _data field in log record:
        // high key + chain_high key
        // Use key lengh to offset into the _data field
        high_key.construct_from_keystr(dp->_data,        // high (foster) key is for the destination page
                                       dp->_high_len);
        chain_high_key.construct_from_keystr(dp->_data + // chain_high is the same for all foster nodes
                                       dp->_high_len, dp->_chain_high_len);

        if (recovering_dest)
        {
            // Recover the destination (foster parent) page, do not initialize the foster 
            // parent page because it contains valid records, set the new high key and 
            // chain_high_key 
            // The following full logging (delete/insert) should insert records from
            // the source (foster child) page

            // Calling init_fence_keys to set the fence keys of the source page
            W_COERCE(bp.init_fence_keys(false /* set_low */, high_key,         // Do not reset the low fence key
                                        true /* set_high */, high_key,         // Reset the high key of the destination page
                                        true /* set_chain */, chain_high_key,  // Reset the chain_high
                                        false /* set_pid0*/, 0,               // No change in destination page of non-leaf pid
                                        false /* set_emlsn*/, lsn_t::null,    // No change in destination  page of non-leaf emlsn
                                        true /* set_foster*/, dp->_foster_pid0,               // Destination  page foster pid
                                        true /* set_foster_emlsn*/, dp->_foster_pid0_emlsn));  // Destination page foster emlsn
        }
        else
        {
            // Recover the source (foster child) page, we are only deleting records
            // from the source page, and will delete the page in a page-merge call
            // no need to set the new fence keys
        }
        return;
    }

    if (p->is_bufferpool_managed()) {
        // Page is buffer pool managed, so fix_direct should be safe.
        btree_page_h another;
        W_COERCE(another.fix_direct(bp.vol(), another_pid, LATCH_EX));
        if (recovering_dest) {
            // we are recovering "page", which is foster-parent (dest).
            if (another.lsn() >= lsn_ck())
            {
                // If we get here, caller was not from page driven REDO phase but "page2" (src) has
                // been recovered already, this is breaking WOD rule and cannot continue

                w_assert1(false == p->is_recovery_access());

                DBGOUT3 (<< "btree_foster_merge_log::redo: caller from page driven REDO, source(foster child) has been recovered");
                W_FATAL_MSG(fcINTERNAL, << "btree_foster_merge_log::redo - WOD cannot be followed, abort the operation");
            }
            else
            {
                // thanks to WOD, "page2" (src) is also assured to be not recovered yet.
                w_assert0(another.lsn() < lsn_ck());
                btree_impl::_ux_merge_foster_apply_parent(bp /*dest*/, another /*src*/);
                another.set_dirty();
                another.update_initial_and_last_lsn(lsn_ck());
                W_COERCE(another.set_to_be_deleted(false));
            }
        } else {
            // we are recovering "page2", which is foster-child (src).
            // in this case, foster-parent(dest) may or may not be written yet.
            if (another.lsn() >= lsn_ck()) {
                // if page (destination) is already durable/recovered,
                // we just delete the foster child and done.
                W_COERCE(bp.set_to_be_deleted(false));
            } else {
                // dest is also old, so we are recovering both.
                btree_impl::_ux_merge_foster_apply_parent(another /*dest*/, bp /*src*/);
                another.set_dirty();
                another.update_initial_and_last_lsn(lsn_ck());
            }
        }
    } else {
        // Cannot be in full logging mode
        w_assert1(false == restart_m::use_redo_full_logging_recovery());

        // Minimal logging while the page is not buffer pool managed, no dependency on
        // write-order-dependency

        // TODO(Restart)...
        // See detail comments in btree_foster_rebalance_log::redo

// TODO(Restart)... page rebalance is not working in multiple test cases, either record not found or fence key comparision error
//                          no test case covering page merge yet

        // this is in Single-Page-Recovery. we use scratch space for another page because bufferpool frame
        // for another page is probably too recent for Single-Page-Recovery.
        DBGOUT1(<< "merge Single-Page-Recovery! another=" << another_pid);
        if (!recovering_dest) {
            // source page is simply deleted, so we don't have to do anything here
            DBGOUT1(<< "recovering source page in merge... nothing to do");
            return;
        }
        DBGOUT1(<< "recovering dest page in merge. recursive Single-Page-Recovery!");
        SprScratchSpace frame(bp.vol(), bp.store(), another_pid);
        // Here, we do "time travel", recursively invoking Single-Page-Recovery.
        lsn_t another_previous_lsn = recovering_dest ? dp->_page2_prv : page_prev_lsn();
        btree_page_h another;
        another.fix_nonbufferpool_page(frame.p);
        W_COERCE(smlevel_0::log->recover_single_page(another, another_previous_lsn));
        btree_impl::_ux_merge_foster_apply_parent(bp, another);
    }
}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_rebalance.
 * This log is \b NOT-self-contained, so it assumes WOD (foster-parent is written later).
 */
struct btree_foster_rebalance_t : multi_page_log_t {
    int32_t         _move_count;         // +4 => 4
    shpid_t         _new_pid0;           // +4 => 8, non-leaf node only  
    lsn_t           _new_pid0_emlsn;     // +8 => 16, non-leaf node only
    uint16_t        _fence_len;          // +2 => 18, fence key length
    uint16_t        _high_len;           // +2 => 20, high key length    
    uint16_t        _chain_high_len;     // +2 => 22, chain_high key length        
    
    // _data contains fence, high and chain_high
    char            _data[logrec_t::max_data_sz - sizeof(multi_page_log_t) - 22];

    btree_foster_rebalance_t(shpid_t page2_id, int32_t move_count,
            const w_keystr_t& fence, shpid_t new_pid0, lsn_t new_pid0_emlsn,
            const w_keystr_t& high, const w_keystr_t& chain_high);
    int size() const { return sizeof(multi_page_log_t) + 22 + _fence_len + _high_len + _chain_high_len; }
};

btree_foster_rebalance_t::btree_foster_rebalance_t(shpid_t page2_id, int32_t move_count,
        const w_keystr_t& fence,       // low fence of destination, also high (foster) of source
        shpid_t new_pid0, lsn_t new_pid0_emlsn,
        const w_keystr_t& high,        // high (foster) of destination
        const w_keystr_t& chain_high)  // high fence of all foster nodes
    : multi_page_log_t(page2_id) {
    _move_count = move_count;
    _new_pid0   = new_pid0;
    _new_pid0_emlsn = new_pid0_emlsn;

    w_assert1(size() < logrec_t::max_data_sz);

    // Figure out the size of each data field
    _fence_len = (uint16_t)fence.get_length_as_keystr();
    _high_len = (uint16_t)high.get_length_as_keystr();
    _chain_high_len = (uint16_t)chain_high.get_length_as_keystr();

    // Put all data fields into _data
    fence.serialize_as_keystr(_data);
    high.serialize_as_keystr(_data + _fence_len);
    chain_high.serialize_as_keystr(_data + _fence_len + _high_len);
}

btree_foster_rebalance_log::btree_foster_rebalance_log (const btree_page_h& p,
    const btree_page_h &p2, int32_t move_count, const w_keystr_t& fence,  // low fence of destination, also high (foster) of source
    shpid_t new_pid0, lsn_t new_pid0_emlsn,
    const w_keystr_t& high,          // high (foster) of destination
    const w_keystr_t& chain_high) {  // high fence of all foster nodes
    // p - destination
    // p2 - source
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_foster_rebalance_t(p2.pid().page,
        move_count, fence, new_pid0, new_pid0_emlsn, high, chain_high))->size());
}

void btree_foster_rebalance_log::redo(fixable_page_h* p) {
    // TODO(Restart)...
    // The problem is that we are relying on "page2" (src, another) to contain the
    // pre-rebalance image, therefore we can move records from 'another' (src) into 'bp' (dest).
    // If 'another' (src) has been recovered already (post-rebalance image), it does not 
    // contain all the pre-rebalance records, we lost the records we need to move into 
    // the source page at this point, we cannot perform REDO with the post-image on source page.
    // Possible solutions:
    // 1. Record the page dependencies (Write Order Dependency) during Log Analysis,
    //       and recover the pages in the correct order.  This solution becomes messy in a hurry
    //       because the dependency gets complex when we have situations such as foster chain,
    //       multiple adoptions, multiple splits/merges, etc.  Also when REDOing the destination page,
    //       we need to recover the source page to the pre-rebalance/pre-merge image first, 
    //       recover the destination page, and then finish the recovery for the source page.
    //       If there are multiple rebalance and/or merge operations on the same source page,
    //       the recovery becomes tricky.
    // 2. Have the rebalance/merge log records containing all record movement information,
    //       the size of the log records become large and potentially expand to multiple log 
    //       records.  Both rebalance and merge are system transactions while we only
    //       supoort single log record system transaction currently, it requires extra handling
    //       in Recovery logic to deal with multi-log system transactions, e.g. rolling forward.
    // 3. Disable the minimal logging for rebalance and merge operations, in other words,
    //       log records for btree_foster_rebalance_log and btree_foster_merge_log
    //       (currently one log record covers the entire operation) will not handle the actual
    //       record movements anymore, instead we will use full logging to generate deletion/insertion 
    //       log records for all record movements.  What would be generated:
    //       1. System transaction log record - contain information to set the destination page fence keys, 
    //                                               including low and high fence keys and chain_high_key (if it 
    //                                               exists in source page form a foster chain)
    //                                               also the source page new high fence key which is the same as
    //                                               the destination page's low fence key
    //       2. Delete/Insert pair of log records for every record movement
    // 4. Multiple recovery steps, in REDO operation, recover the source or destination page to 
    //     immediatelly before the current load balance operation (pre-image), and then perform
    //     the REDO operation on the target page (either source or destination).
    //     The challenge in this solution is to handle complex operation, e.g. multiple load balance
    //     operations on the same page before system crash.

    // TODO(Restart)...
    // In milestone 2:
    //    If restart_m::use_redo_full_logging_recovery() is on, use solution #3 which disables 
    //        the minimal logging and uses full logging instead, therefore btree_foster_rebalance_log
    //        and btree_foster_merge_log log records are used to set page fence keys only.
    //        In this code path, we will use full logging for all page rebalance and page merge 
    //        operations, including both recovery REDO and normal page corruption Single-Page-Recovery
    //        operations.
    //    If restart_m::use_redo_page_recovery() is on, use solution #4 which is using minimum 
    //        logging.  No separate full logging for record movements.
    //        Recovery REDO mark the page as not-bufferpool-managed before calling Single-Page-Recovery
    //        so the page-rebalance/page-merge REDO functions use code path for #4.  Recovery REDO
    //        marks the page as bufferpool-managed after Single-Page-Recovery finished.
    //
    // Long-term solution (what it should be implemented eventually)...
    // 1. Use minimal logging (one system transaction for the entire operation).
    // 2. Page rebalance and page merge operations should share one generic system transaction
    //    log record (load-balance) instead of two different log records, while it is a multi-page single
    //    log system transaction, the first page is the destination page and the second page is the 
    //    source page.
    // 3. The destination page is not always empty, because for a merge operation, the destination
    //    page (foster parent) is not empty.  For the split operation, the destination page (foster child)
    //    is always empty.
    // 4. The system transaction 'load-balance' log record should be used for 3 types of operations:
    //     1. Page split: currently btree_foster_rebalance_log
    //     2. Page merge: currently btree_foster_merge_log
    //     3. Load balance among two existing pages: does not exist currently

    borrowed_btree_page_h bp(p);
    btree_foster_rebalance_t *dp = reinterpret_cast<btree_foster_rebalance_t*>(data_ssx());
    w_keystr_t fence;
    fence.construct_from_keystr(dp->_data, dp->_fence_len);

    // WOD: "page2" is the data source, which is written later.
    const shpid_t target_pid = p->pid().page;
    const shpid_t page_id = shpid();
    const shpid_t page2_id = dp->_page2_pid;
    const lsn_t  &redo_lsn = lsn_ck();
    DBGOUT3 (<< *this << ": redo_lsn=" << redo_lsn << ", bp.lsn=" << bp.lsn());
    w_assert1(bp.lsn() < redo_lsn);

    bool recovering_dest = (target_pid == page_id);             // 'p' is the page we are trying to recover: target
    shpid_t another_pid = recovering_dest ? page2_id : page_id; // another_pid: data source
                                                                // if 'p' is foster child, then another_pid is foster parent
                                                                // if 'p' is foster parent, then another_pid is foster child
                                                                // In the normal Single-Page-Recovery case with WOD, 
                                                                // another_pid (foster parent)
                                                                // must be recovered after foster child

    // recovering_dest == true: recovery foster child, assume foster parent has not been recovered
    // recovering_dest == false: recover foster parent, assumed foster child has been recovered
    w_assert0(recovering_dest || target_pid == page2_id);

    if (true == restart_m::use_redo_full_logging_recovery()) 
    {
        // TODO(Restart)... milestone 2
        // Using full logging for b-tree rebalance operation, the rebalance log record (system txn)
        // during REDO occurs before all the actual record move log records
        //
        // Even if called by the regular Single-Page-Recovery due to page corruption (not from system recovery)
        // because the page driven flag was on, full logging was taken for page rebalance
        // and merge operations, we cannot use the minimal logging code path for regular Single-Page-Recovery either

        w_keystr_t high_key, chain_high_key;

        // Get fence key information from log record (dp)
        // The _data field in log record:
        // fence key + high key + chain_high key
        // Use key lengh to offset into the _data field
        fence.construct_from_keystr(dp->_data, dp->_fence_len);    // fence key is the low fence of destination page
                                                                   // also the high (foster) of the source page
        high_key.construct_from_keystr(dp->_data + dp->_fence_len, // high (foster) key is for the destination page
                                       dp->_high_len);
        chain_high_key.construct_from_keystr(dp->_data + dp->_fence_len + // chain_high is the same for all foster nodes
                                       dp->_high_len, dp->_chain_high_len);

        if (recovering_dest)
        {
            // Recover the destination (foster child) page, regardless whether the foster parent has been: 
            // recovered (no WOD) - parent_page.lsn() >=  redo_lsn
            // not recovered (WOD) - parent_page.lsn() <  redo_lsn
           
            if (bp.is_leaf())
            {
                DBGOUT3( << "&&&& Foster child page is a leaf page");
            }
            else
            {
                DBGOUT3( << "&&&& Foster child page is a non-leaf page");            
            }
// TODO(Restart)... 
            DBGOUT3( << "&&&& btree_foster_rebalance_log: recover foster child page, initialize it to an empty page, foster child pid: " 
                     << bp.pid() << ", number of records (should be zero): " << bp.nrecs());

            // In a page split operation (this one), the assumption is the destination page is a 
            // newly allocated  page, therefore it is safe initialize the destination page (foster child) to 
            // an empty page.
            // The following full logging (delete/insert) whould insert records into the empty destination  page
            // If there are existing records in the destination page, the initialization would erase them

            // Calling format_steal to initialize the destination page, no stealing
            W_COERCE(bp.format_steal(bp.lsn(), bp.pid(),
                            bp.btree_root(), bp.level(),
                            (bp.is_leaf())? 0 : dp->_new_pid0,                 // leaf has no pid0
                            (bp.is_leaf())? lsn_t::null : dp->_new_pid0_emlsn, // leaf has no emlsn
                            bp.get_foster(), bp.get_foster_emlsn(),    // if destination (foster child) has foster child itself (foster chain)
                            fence,            // Low fence of the destination page
                            high_key,         // High fence of the destination, valid only if three is a foster chain
                            chain_high_key,   // Chain_high_fence
                            false,            // don't log the page_img_format record
                            NULL, 0, 0,       // no steal from src_1
                            NULL, 0, 0,       // no steal from src_2
                            false,            // src_2
                            false,            // No full logging
                            false             // No logging
                            ));
        }
        else
        {
            // Recover the source (foster parent) page, do not initialize the foster parent page because 
            // it contains valid records, set the new high key and chain_high_key 
            // The following full logging (delete/insert) should remove records from 
            // the source (foster parent) page

// TODO(Restart)...             
            DBGOUT3( << "&&&& btree_foster_rebalance_log: recover foster parent page, do not initialize the page, foster parent pid: " << bp.pid());
            DBGOUT3( << "&&&& Page had " << bp.nrecs() << " records");       

            // Calling init_fence_keys to set the fence keys of the source page
            W_COERCE(bp.init_fence_keys(false /* set_low */, fence,                // Do not reset the low fence key
                                        true /* set_high */, fence,                // Reset the high key of the source page to fence key
                                        true /* set_chain */, chain_high_key,      // Reset the chain_high although it should be the same
                                        false /* set_pid0*/, 0,                   // No change in source page of non-leaf pid
                                        false /* set_emlsn*/, lsn_t::null,        // No change in source page of non-leaf emlsn
                                        false /* set_foster*/, 0,                  // No change in source page of foster pid
                                        false /* set_foster_emlsn*/, lsn_t::null)); // No change in source page of foster emlsn
                                        
        }

        // Return now to skip the optimal code path
        return;
    }


    // Two pages are involved:
    // first page: destination, foster child in this case
    // second page: source, foster parent in this case

    // If not full logging REDO, use the optimized code path (no full logging)
    if (p->is_bufferpool_managed()) {
        // If page is buffer pool managed (not from Recovery REDO), fix_direct should be safe.
        btree_page_h another;
        W_COERCE(another.fix_direct(bp.vol(), another_pid, LATCH_EX));
        if (recovering_dest) {
            // we are recovering "page", which is foster-child (dest).
            DBGOUT3 (<< "Recovering 'page'. page2.lsn=" << another.lsn());
            if (another.lsn() >= redo_lsn)
            {               
                w_assert1(false == p->is_recovery_access());
                // If we get here, this is not a page driven REDO therefore minimal logging, 
                // but "page2" (src) has been fully recovered already, it is breaking WOD rule and cannot continue

                DBGOUT3 (<< "btree_foster_rebalance_log::redo: caller did not have page driven REDO, source (foster parent) has been recovered");               
                W_FATAL_MSG(fcINTERNAL, << "btree_foster_rebalance_log::redo - WOD cannot be followed, abort the operation");
            }
            else
            {
                // Normal Single-Page-Recovery operation (not from Recovery) or from Recovery but "page2" (src) 
                // has not been recovered yet.
                // thanks to WOD, "page2" (src) is also assured to be not recovered yet.

                w_assert0(another.lsn() < redo_lsn);
                W_COERCE(btree_impl::_ux_rebalance_foster_apply(another/*src*/, bp /*dest*/, dp->_move_count,
                                                fence, dp->_new_pid0, dp->_new_pid0_emlsn));
                another.set_dirty();
                another.update_initial_and_last_lsn(redo_lsn);
            }
        } else {
            // we are recovering "page2", which is foster-parent (src).
            DBGOUT3 (<< "Recovering 'page2'. page.lsn=" << another.lsn());
            if (another.lsn() >= redo_lsn) {
                // if page (destination) is already durable/recovered, we create a dummy scratch
                // space which will be thrown away after recovering "page2".
                w_keystr_t high, chain_high;
                bp.copy_fence_high_key(high);
                bp.copy_chain_fence_high_key(chain_high);
                generic_page scratch_page;
                scratch_page.tag = t_btree_p;
                btree_page_h scratch_p;
                scratch_p.fix_nonbufferpool_page(&scratch_page);
                // initialize as an empty child:
                scratch_p.format_steal(lsn_t::null, another.pid(), bp.btree_root(), bp.level(),
                                    0, lsn_t::null, 0, lsn_t::null,
                                    high, chain_high, chain_high, false);
                W_COERCE(btree_impl::_ux_rebalance_foster_apply(bp /*src*/, scratch_p /*dest*/, dp->_move_count,
                                                fence, dp->_new_pid0, dp->_new_pid0_emlsn));
            } else {
                // dest is also old, so we are recovering both.
                W_COERCE(btree_impl::_ux_rebalance_foster_apply(bp /*src*/, another /*dest*/, dp->_move_count,
                                                fence, dp->_new_pid0, dp->_new_pid0_emlsn));
                another.set_dirty();
                another.update_initial_and_last_lsn(redo_lsn);
            }

        }
    } else {
        // Cannot be in full logging mode
        w_assert1(false == restart_m::use_redo_full_logging_recovery());

        // Minimal logging while the page is not buffer pool managed, no dependency on
        // write-order-dependency
        
        // If we are here during Recovery, we are in restart_m::use_redo_page_recovery() 
        // mode (minimal logging) via Single-Page-Recovery.       
        // The REDO logic (caller) marks the page as not bufferpool_managed before calling 
        // Single-Page-Recovery and marks the page as bufferpool_managed after Single-Page-Recovery.
        
        // For recoverying destination or destination page, the following logic recovers the
        // source page to immediately before the page rebalance operation using a scratch page (pre-image),
        // so the source page has all the original records, and then move records from source to destination. 
        // Because this is a page rebalance operation, the destination page starts as an empty page.
        //
        // The logic probably works well with normal Single Page Recovery (do we have 
        // sufficient test?)
        // but does it work well with all scenarios from Recovery REDO?  Especially if
        // we have complex operations, such as multiple rebalance and merge operations
        // on the same page before system crash, etc.  If we are doing log scan driven REDO then
        // we are okay using this logic because we are recovery based on one log record at a time,
        // but when using page driven REDO (one page at a time) then we might have issues if
        // there are multiple load balance operations on the same page before system crash,
        // the pre-image and record movements might not be what we expected.
        
// TODO(Restart)... not working in multiple test cases, either record not found or fence key comparision error

        // this is in Single-Page-Recovery. we use scratch space for another page because bufferpool frame
        // for another page is probably too latest for Single-Page-Recovery.
        SprScratchSpace frame(bp.vol(), bp.store(), another_pid);
        // Here, we do "time travel", recursively invoking Single-Page-Recovery.
        lsn_t another_previous_lsn = recovering_dest ? dp->_page2_prv : page_prev_lsn();
        DBGOUT1(<< "SPR for rebalance. recursive Single-Page-Recovery! another=" << another_pid <<
            ", another_previous_lsn=" << another_previous_lsn);
        btree_page_h another;
        another.fix_nonbufferpool_page(frame.p);
        W_COERCE(smlevel_0::log->recover_single_page(another, another_previous_lsn));
        W_COERCE(btree_impl::_ux_rebalance_foster_apply(
            recovering_dest ? another : bp, recovering_dest ? bp : another,
            dp->_move_count, fence, dp->_new_pid0, dp->_new_pid0_emlsn));
    }
}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_rebalance_norec.
 * This log is totally \b self-contained, so no WOD assumed.
 */
struct btree_foster_rebalance_norec_t : multi_page_log_t {
    int16_t       _fence_len; // +2 -> 2
    char          _data[logrec_t::max_data_sz - sizeof(multi_page_log_t) - 2];

    btree_foster_rebalance_norec_t(const btree_page_h& p,
        const w_keystr_t& fence) : multi_page_log_t(p.get_foster()) {
        w_assert1 (g_xct()->is_single_log_sys_xct());
        w_assert1 (p.latch_mode() == LATCH_EX);
        _fence_len = fence.get_length_as_keystr();
        fence.serialize_as_keystr(_data);
    }
    int size() const { return sizeof(multi_page_log_t) + 2 + _fence_len; }
};

btree_foster_rebalance_norec_log::btree_foster_rebalance_norec_log(
    const btree_page_h &p, const btree_page_h &, const w_keystr_t& fence) {
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_foster_rebalance_norec_t(
        p, fence))->size());
}
void btree_foster_rebalance_norec_log::redo(fixable_page_h* p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_foster_rebalance_norec_t *dp =
        reinterpret_cast<btree_foster_rebalance_norec_t*>(data_ssx());

    w_keystr_t fence, chain_high;
    fence.construct_from_keystr(dp->_data, dp->_fence_len);
    bp.copy_chain_fence_high_key(chain_high);

    shpid_t target_pid = p->pid().page;
    if (target_pid == header._shpid) {
        // we are recovering "page", which is foster-parent.
        W_COERCE(bp.norecord_split(bp.get_foster(), bp.get_foster_emlsn(),
                                   fence, chain_high));
    } else {
        // we are recovering "page2", which is foster-child.
        w_assert0(target_pid == dp->_page2_pid);
        w_assert1(bp.nrecs() == 0); // this should happen only during page split.

        w_keystr_t high;
        bp.copy_fence_high_key(high);
        w_keystr_len_t prefix_len = fence.common_leading_bytes(high);
        W_COERCE(bp.replace_fence_rec_nolog_may_defrag(fence, high, chain_high, prefix_len));
    }
}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_adopt.
 * This log is totally \b self-contained, so no WOD assumed.
 */
struct btree_foster_adopt_t : multi_page_log_t {
    lsn_t   _new_child_emlsn;   // +8
    shpid_t _new_child_pid;     // +4
    int16_t _new_child_key_len; // +2
    char    _data[logrec_t::max_data_sz - sizeof(multi_page_log_t) - 14];

    btree_foster_adopt_t(shpid_t page2_id, shpid_t new_child_pid,
                         lsn_t new_child_emlsn, const w_keystr_t& new_child_key);
    int size() const { return sizeof(multi_page_log_t) + 14 + _new_child_key_len; }
};
btree_foster_adopt_t::btree_foster_adopt_t(shpid_t page2_id, shpid_t new_child_pid,
                        lsn_t new_child_emlsn, const w_keystr_t& new_child_key)
    : multi_page_log_t(page2_id), _new_child_emlsn(new_child_emlsn),
    _new_child_pid (new_child_pid) {
    _new_child_key_len = new_child_key.get_length_as_keystr();
    new_child_key.serialize_as_keystr(_data);
}

btree_foster_adopt_log::btree_foster_adopt_log (const btree_page_h& p, const btree_page_h& p2,
    shpid_t new_child_pid, lsn_t new_child_emlsn, const w_keystr_t& new_child_key) {
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_foster_adopt_t(
        p2.pid().page, new_child_pid, new_child_emlsn, new_child_key))->size());
}
void btree_foster_adopt_log::redo(fixable_page_h* p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_foster_adopt_t *dp = reinterpret_cast<btree_foster_adopt_t*>(data_ssx());

    w_keystr_t new_child_key;
    new_child_key.construct_from_keystr(dp->_data, dp->_new_child_key_len);

    shpid_t target_pid = p->pid().page;
    DBGOUT3 (<< *this << " target_pid=" << target_pid << ", new_child_pid="
        << dp->_new_child_pid << ", new_child_key=" << new_child_key);
    if (target_pid == header._shpid) {
        // we are recovering "page", which is real-parent.
        btree_impl::_ux_adopt_foster_apply_parent(bp, dp->_new_child_pid,
                                                  dp->_new_child_emlsn, new_child_key);
    } else {
        // we are recovering "page2", which is real-child.
        w_assert0(target_pid == dp->_page2_pid);
        btree_impl::_ux_adopt_foster_apply_child(bp);
    }
}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_deadopt.
 * This log is totally \b self-contained, so no WOD assumed.
 */
struct btree_foster_deadopt_t : multi_page_log_t {
    shpid_t     _deadopted_pid;         // +4
    int32_t     _foster_slot;           // +4
    lsn_t       _deadopted_emlsn;       // +8
    uint16_t    _low_len, _high_len;    // +2+2
    char        _data[logrec_t::max_data_sz - sizeof(multi_page_log_t) + 20];

    btree_foster_deadopt_t(shpid_t page2_id, shpid_t deadopted_pid, lsn_t deadopted_emlsn,
    int32_t foster_slot, const w_keystr_t &low, const w_keystr_t &high);
    int size() const { return sizeof(multi_page_log_t) + 12 + _low_len + _high_len ; }
};
btree_foster_deadopt_t::btree_foster_deadopt_t(shpid_t page2_id, shpid_t deadopted_pid,
    lsn_t deadopted_emlsn, int32_t foster_slot, const w_keystr_t &low, const w_keystr_t &high)
    : multi_page_log_t(page2_id) {
    _deadopted_pid = deadopted_pid;
    _foster_slot = foster_slot;
    _deadopted_emlsn = deadopted_emlsn;
    _low_len = low.get_length_as_keystr();
    _high_len = high.get_length_as_keystr();
    low.serialize_as_keystr(_data);
    high.serialize_as_keystr(_data + _low_len);
}

btree_foster_deadopt_log::btree_foster_deadopt_log (
    const btree_page_h& p, const btree_page_h& p2,
    shpid_t deadopted_pid, lsn_t deadopted_emlsn, int32_t foster_slot,
    const w_keystr_t &low, const w_keystr_t &high) {
    w_assert1(p.is_node());
    fill(&p.pid(), p.tag(), (new (data_ssx()) btree_foster_deadopt_t(p2.pid().page,
        deadopted_pid, deadopted_emlsn, foster_slot, low, high))->size());
}

void btree_foster_deadopt_log::redo(fixable_page_h* p) {
    // apply changes on real-parent again. no write-order dependency with foster-parent
    borrowed_btree_page_h bp(p);
    btree_foster_deadopt_t *dp = reinterpret_cast<btree_foster_deadopt_t*>(data_ssx());

    shpid_t target_pid = p->pid().page;
    if (target_pid == header._shpid) {
        // we are recovering "page", which is real-parent.
        w_assert1(dp->_foster_slot >= 0 && dp->_foster_slot < bp.nrecs());
        btree_impl::_ux_deadopt_foster_apply_real_parent(bp, dp->_deadopted_pid,
                                                         dp->_foster_slot);
    } else {
        // we are recovering "page2", which is foster-parent.
        w_assert0(target_pid == dp->_page2_pid);
        w_keystr_t low_key, high_key;
        low_key.construct_from_keystr(dp->_data, dp->_low_len);
        high_key.construct_from_keystr(dp->_data + dp->_low_len, dp->_high_len);
        btree_impl::_ux_deadopt_foster_apply_foster_parent(bp, dp->_deadopted_pid,
                                        dp->_deadopted_emlsn, low_key, high_key);
    }
}
